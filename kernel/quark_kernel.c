/*
 * Quark Anticheat - Real Kernel Module (C)
 * 
 * This file implements the Ring 0 component of Quark Anticheat.
 * It uses kretprobes to dynamically hook ptrace_may_access, blocking
 * unauthorized attempts to read or write the memory of protected processes
 * (such as writing to /proc/PID/mem or attaching via ptrace).
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/kprobes.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/uidgid.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <linux/slab.h>
#include <crypto/hash.h>
#include <crypto/sig.h>
#include <net/sock.h>
#ifdef CONFIG_X86_64
#include <asm/cpufeature.h>
#endif

// Auto-generated, see tooling/keys/generate-testing-key.sh (quark_pubkey.h, public,
// committed) and the top-level Makefile (quark_cli_hash.h, regenerated every build
// from the actually-compiled quark_cli binary, not committed).
#include "quark_pubkey.h"
#include "quark_cli_hash.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martinez Alarcon, Gabriel Sebastian & Merino De Rui, Stefano Nahuel");
MODULE_DESCRIPTION("Quark Anticheat - Ring 0 Security Module (kretprobes & Netlink)");
MODULE_VERSION("0.4.0");

#define QUARK_KERNEL_VERSION "0.4.0"

#define NETLINK_QUARK 31  // Custom netlink protocol number
#define MAX_PROTECTED_PROCESSES 16
#define QUARK_MAX_SIG_LEN 72     // max DER-encoded ECDSA P-256 signature size
#define QUARK_MAX_EXE_SIZE (1 << 20)  // 1MB cap on the exe file we'll hash

// Wire format for commands 1 (protect) and 2 (unprotect): authenticated and
// replay-protected, since either one changes real protection state for a PID.
// The signature covers {command, pid, nonce} -- see quark_verify_signature() and
// quark_nl_recv_msg()'s signed_buf construction.
struct quark_netlink_request {
    s32 pid;
    u64 nonce;                     // must be strictly greater than the last accepted one
    u16 sig_len;                   // actual DER signature length, <= QUARK_MAX_SIG_LEN
    u8  sig[QUARK_MAX_SIG_LEN];
};

// Reply to a "protect PID" request, sent back to the requesting quark_cli over
// the same Netlink socket. Lets userspace (and, through the daemon/SDK chain,
// the protected game itself) know whether protection actually took effect,
// and whether this .ko was built with the VM-detection bypass compiled in --
// a testing build must never be able to pass itself off as a full release build.
struct quark_protect_response {
    u8   ok;                 // 1 = protection registered, 0 = refused
    u8   is_testing_build;   // 1 = built with QUARK_TESTING_BUILD
    u8   reserved[2];
    char version[16];        // QUARK_KERNEL_VERSION, NUL-padded
};

// Returns true if this kernel is running as a hypervisor guest (KVM, VMware,
// VirtualBox, Hyper-V, Xen HVM, ...), via the CPUID "hypervisor present" bit
// (leaf 1, ECX bit 31) that all of those set for the guest. Checked in Ring 0
// so it can't be spoofed by anything running in userspace.
static bool quark_running_in_vm(void) {
#ifdef CONFIG_X86_64
    return boot_cpu_has(X86_FEATURE_HYPERVISOR);
#else
    return false;
#endif
}

static u64 last_nonce = 0;
static DEFINE_SPINLOCK(quark_nonce_lock);

// Computes SHA-256 of `data` (`len` bytes) into `digest` (32 bytes).
static int quark_sha256(const void *data, size_t len, u8 digest[32]) {
    struct crypto_shash *tfm;
    struct shash_desc *desc;
    int ret;

    tfm = crypto_alloc_shash("sha256", 0, 0);
    if (IS_ERR(tfm))
        return PTR_ERR(tfm);

    desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
    if (!desc) {
        crypto_free_shash(tfm);
        return -ENOMEM;
    }
    desc->tfm = tfm;

    ret = crypto_shash_digest(desc, data, len, digest);

    kfree(desc);
    crypto_free_shash(tfm);
    return ret;
}

// Verifies an ECDSA P-256 signature (`sig`, DER-encoded, `sig_len` bytes) over the
// SHA-256 digest of `signed_data` (`signed_len` bytes), against the embedded
// quark_pubkey (kernel/quark_pubkey.h). Returns true only on a fully valid signature.
//
// Algorithm name/key/signature format confirmed empirically against this project's
// actual kernel (crypto_alloc_sig("x962(ecdsa-nist-p256)", ...) wants the raw 65-byte
// SEC1 uncompressed public key point, and a standard DER-encoded ECDSA signature --
// exactly what OpenSSL's EVP API produces, no custom encoding needed on the signer
// side). The bare "ecdsa-nist-p256" name (no x962 wrapper) rejects both of those.
static bool quark_verify_signature(const void *signed_data, size_t signed_len,
                                    const u8 *sig, u16 sig_len) {
    struct crypto_sig *tfm;
    u8 digest[32];
    int ret;

    if (sig_len == 0 || sig_len > QUARK_MAX_SIG_LEN)
        return false;

    if (quark_sha256(signed_data, signed_len, digest) != 0)
        return false;

    tfm = crypto_alloc_sig("x962(ecdsa-nist-p256)", 0, 0);
    if (IS_ERR(tfm))
        return false;

    if (crypto_sig_set_pubkey(tfm, quark_pubkey, sizeof(quark_pubkey)) != 0) {
        crypto_free_sig(tfm);
        return false;
    }

    ret = crypto_sig_verify(tfm, sig, sig_len, digest, sizeof(digest));
    crypto_free_sig(tfm);

    return ret == 0;
}

// Rejects any nonce that isn't strictly greater than the last one that passed full
// authentication -- defeats replay of a captured, validly-signed request. Resets on
// module reload, same as protected_pids (a reload is already a trust-boundary reset).
static bool quark_nonce_is_fresh(u64 nonce) {
    bool fresh;
    unsigned long flags;

    spin_lock_irqsave(&quark_nonce_lock, flags);
    fresh = nonce > last_nonce;
    spin_unlock_irqrestore(&quark_nonce_lock, flags);
    return fresh;
}

// Only ever called after a request has fully passed signature verification --
// otherwise an attacker could burn nonces with garbage signatures.
static void quark_nonce_advance(u64 nonce) {
    unsigned long flags;

    spin_lock_irqsave(&quark_nonce_lock, flags);
    if (nonce > last_nonce)
        last_nonce = nonce;
    spin_unlock_irqrestore(&quark_nonce_lock, flags);
}

// Reads up to QUARK_MAX_EXE_SIZE bytes of `file` and SHA-256s them into `digest`.
static int quark_hash_file(struct file *file, u8 digest[32]) {
    loff_t size = i_size_read(file_inode(file));
    loff_t pos = 0;
    void *buf;
    ssize_t n;
    int ret;

    if (size <= 0 || size > QUARK_MAX_EXE_SIZE)
        return -EFBIG;

    buf = kvmalloc(size, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    n = kernel_read(file, buf, size, &pos);
    if (n != size) {
        kvfree(buf);
        return (n < 0) ? (int)n : -EIO;
    }

    ret = quark_sha256(buf, size, digest);
    kvfree(buf);
    return ret;
}

// Defense-in-depth alongside the signature check: confirms the process that actually
// sent this netlink message -- per the kernel-verified sender credentials in
// NETLINK_CREDS(skb), NOT the self-reported nlmsg_pid header field, which has no
// kernel-enforced binding to the real sender and isn't usable for identity -- is (a)
// running as root, since quark_cli only ever runs via `sudo`, and (b) really is the
// compiled quark_cli binary, by content hash, not just some other root process.
//
// Neither check alone is sufficient (root can already do plenty on its own; a hash
// check alone doesn't stop someone invoking the real quark_cli with attacker-chosen
// arguments): quark_verify_signature() above is what actually proves *authority* to
// change protection state. This is additional friction on top of that, not a
// replacement for it.
static bool quark_check_sender(const struct sk_buff *skb) {
    struct scm_creds *creds = NETLINK_CREDS(skb);
    char path[32];
    struct file *exe_file;
    u8 digest[32];
    bool ok = false;

    if (!uid_eq(creds->uid, GLOBAL_ROOT_UID)) {
        pr_err("[QUARK-KERNEL] Rejecting netlink command from non-root uid %u (pid %u)\n",
               __kuid_val(creds->uid), creds->pid);
        return false;
    }

    // Several more "obvious" ways to get a task's exe file from a pid
    // (find_task_by_vpid, get_task_exe_file, get_mm_exe_file -- all confirmed via
    // failed builds: modpost reports them undefined) aren't actually exported for
    // out-of-tree modules on this kernel. filp_open() on /proc/<pid>/exe is: it's one
    // of the most fundamental, universally-exported VFS primitives, and /proc/<pid>/exe
    // is already a magic symlink the kernel resolves straight to the same file
    // task->mm->exe_file points at -- no task_struct/mm_struct walking needed.
    snprintf(path, sizeof(path), "/proc/%u/exe", creds->pid);
    exe_file = filp_open(path, O_RDONLY, 0);

    if (IS_ERR(exe_file)) {
        pr_err("[QUARK-KERNEL] Rejecting netlink command: could not open exe of pid %u (%ld)\n",
               creds->pid, PTR_ERR(exe_file));
        return false;
    }

    if (quark_hash_file(exe_file, digest) == 0 &&
        memcmp(digest, quark_cli_expected_sha256, sizeof(digest)) == 0) {
        ok = true;
    } else {
        pr_err("[QUARK-KERNEL] Rejecting netlink command: sender pid %u's exe does not "
               "match the expected quark_cli\n", creds->pid);
    }

    fput(exe_file);
    return ok;
}

static struct sock *nl_socket = NULL;
static pid_t protected_pids[MAX_PROTECTED_PROCESSES];
static int protected_pids_count = 0;
static DEFINE_SPINLOCK(quark_lock);

// The one pid (tgid) exempted from the protected-process access block below, set via
// the authenticated command 3 (quark_daemon registers its own pid once at startup so
// its monitor thread can read the memory of processes it registers as protected).
// -1 (no real pid) until registered. Deliberately a single slot, not a list: there is
// only ever one legitimate daemon.
static pid_t trusted_monitor_pid = -1;

// Helper to check if a PID is currently protected
static bool is_pid_protected(pid_t pid) {
    int i;
    bool found = false;
    unsigned long flags;

    spin_lock_irqsave(&quark_lock, flags);
    for (i = 0; i < protected_pids_count; i++) {
        if (protected_pids[i] == pid) {
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&quark_lock, flags);
    return found;
}

// Add a PID to the protected list
static void add_protected_pid(pid_t pid) {
    unsigned long flags;
    spin_lock_irqsave(&quark_lock, flags);
    if (protected_pids_count < MAX_PROTECTED_PROCESSES) {
        int i;
        bool exists = false;
        for (i = 0; i < protected_pids_count; i++) {
            if (protected_pids[i] == pid) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            protected_pids[protected_pids_count++] = pid;
            pr_info("[QUARK-KERNEL] Process %d is now protected by Quark.\n", pid);
        }
    } else {
        pr_warn("[QUARK-KERNEL] Protected PID table full, cannot protect PID %d\n", pid);
    }
    spin_unlock_irqrestore(&quark_lock, flags);
}

// Attempts to protect a PID, subject to the VM check below. Returns true if
// protection was actually registered.
//
// Release builds (QUARK_TESTING_BUILD not defined) refuse outright when a
// hypervisor is detected -- this branch's #else is the only place that ever
// calls add_protected_pid() after a positive quark_running_in_vm(), so a
// release .ko simply does not contain a code path that would let it protect
// a PID inside a VM, regardless of anything userspace sends it.
static bool quark_protect_pid(pid_t pid) {
    if (quark_running_in_vm()) {
#ifdef QUARK_TESTING_BUILD
        pr_warn("[QUARK-KERNEL] TESTING BUILD: hypervisor detected, VM check bypassed for PID %d\n", pid);
#else
        pr_err("[QUARK-KERNEL] Refusing to protect PID %d: hypervisor detected (this is a release build)\n", pid);
        return false;
#endif
    }
    add_protected_pid(pid);
    return true;
}

// Sends a quark_protect_response back to the requesting quark_cli over the same
// Netlink socket. Reused for command 3 (register trusted monitor) too -- both are
// simple ok/nack requests, no need for a second response struct.
static void quark_send_protect_response(u32 dest_pid, bool ok) {
    struct sk_buff *skb_out;
    struct nlmsghdr *nlh;
    struct quark_protect_response resp;
    int msg_size = sizeof(resp);

    memset(&resp, 0, sizeof(resp));
    resp.ok = ok ? 1 : 0;
#ifdef QUARK_TESTING_BUILD
    resp.is_testing_build = 1;
#else
    resp.is_testing_build = 0;
#endif
    strscpy(resp.version, QUARK_KERNEL_VERSION, sizeof(resp.version));

    skb_out = nlmsg_new(msg_size, GFP_KERNEL);
    if (!skb_out) {
        pr_err("[QUARK-KERNEL] Failed to allocate response skb for pid %d\n", dest_pid);
        return;
    }

    nlh = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, msg_size, 0);
    if (!nlh) {
        pr_err("[QUARK-KERNEL] Failed to build response nlmsghdr for pid %d\n", dest_pid);
        kfree_skb(skb_out);
        return;
    }
    memcpy(nlmsg_data(nlh), &resp, msg_size);

    if (nlmsg_unicast(nl_socket, skb_out, dest_pid) < 0) {
        pr_warn("[QUARK-KERNEL] Failed to send protect response to pid %d\n", dest_pid);
    }
}

// Remove a PID from the protected list
static void remove_protected_pid(pid_t pid) {
    int i;
    unsigned long flags;
    spin_lock_irqsave(&quark_lock, flags);
    for (i = 0; i < protected_pids_count; i++) {
        if (protected_pids[i] == pid) {
            protected_pids[i] = protected_pids[protected_pids_count - 1];
            protected_pids_count--;
            pr_info("[QUARK-KERNEL] Process %d removed from Quark protection.\n", pid);
            break;
        }
    }
    spin_unlock_irqrestore(&quark_lock, flags);
}

// Saved from ptrace_may_access(task, mode)'s arguments in the entry handler and read
// back in the return handler. We keep the access mode too, not just the target task:
// only PTRACE_MODE_ATTACH can actually read another process's memory (/proc/<pid>/mem,
// process_vm_readv/writev, ptrace attach), while PTRACE_MODE_READ alone is harmless
// metadata (/proc/<pid>/{cmdline,maps,status}) that ps/htop/journald read constantly.
// Blocking every mode flooded dmesg with alerts for those routine reads (see
// docs/vm_test_environment.md) and needlessly broke process listings for protected
// games; only the ATTACH case is a real threat.
struct quark_pma_call {
    struct task_struct *task;
    unsigned int mode;
};

/*
 * Entry handler: runs before ptrace_may_access executes. Under x86_64 the first two
 * arguments are in DI (task) and SI (mode).
 */
static int ptrace_may_access_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct quark_pma_call *call = (struct quark_pma_call *)ri->data;
#ifdef CONFIG_X86_64
    call->task = (struct task_struct *)regs->di;
    call->mode = (unsigned int)regs->si;
#else
    call->task = NULL;
    call->mode = 0;
#endif
    return 0;
}

/*
 * Return handler: runs after ptrace_may_access finishes.
 * We inspect if the target task is protected. If so, the caller is not the process
 * itself or the one authenticated trusted monitor, AND this is an attach-level access
 * (the only kind that can read memory), we override the return value (rax) to 0
 * (false/access denied). Read-only metadata accesses are left untouched.
 */
static int ptrace_may_access_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct quark_pma_call *call = (struct quark_pma_call *)ri->data;
    struct task_struct *task = call->task;
    pid_t target_pid;
    pid_t parent_pid;

    if (!task) {
        return 0;
    }

    // task_tgid_vnr(), not task_pid_vnr(): the latter is per-*thread* (each thread
    // has its own distinct kernel pid_t, only a process's leader thread's happens to
    // equal its getpid()/tgid), so it silently broke self-access for any protected
    // process that reads its own memory from a non-leader thread, and would equally
    // have broken a same-process trusted-monitor comparison below (quark_daemon's
    // monitor runs on a background std::thread, a different tid from its own
    // process-wide getpid()). Comparing tgids matches what quark_sdk_init()'s
    // getpid() and quark_daemon's own pid actually mean.
    target_pid = task_tgid_vnr(task);
    parent_pid = task_tgid_vnr(current);

    if (is_pid_protected(target_pid)) {
        // Allow the process to access itself, and allow the one specific pid
        // authenticated as the trusted monitor (command 3, quark_register_monitor())
        // -- quark_daemon's own run_monitor_loop() (quark_daemon/src/main.rs) reads
        // /proc/<pid>/mem to cross-check registered variables, and needs this exact
        // exemption or it's blocked identically to an attacker the instant a PID
        // becomes protected. Deliberately NOT "any root process": this project's own
        // test scripts run quark_daemon *and* the attack-simulating `cheat` tool both
        // as root (ssh session convenience), and a blanket root exemption was tried
        // first here and found, live, to let `cheat` through right along with the
        // daemon -- a real regression, not just a test artifact, since a cheat
        // process elevated to root by any means would have sailed through too.
        // PTRACE_MODE_ATTACH is set for /proc/<pid>/mem, process_vm_*, and ptrace
        // attach -- everything that can read the target's memory. A bare
        // PTRACE_MODE_READ (no ATTACH) is just metadata and is allowed through, so
        // normal tooling keeps working and dmesg isn't flooded. Ratelimited because a
        // real attacker (e.g. Cheat Engine) polls, and the daemon's BPF telemetry is
        // the authoritative per-attempt record anyway (docs/features.md §1.6).
        if (target_pid != parent_pid && parent_pid != trusted_monitor_pid &&
            (call->mode & PTRACE_MODE_ATTACH)) {
            pr_warn_ratelimited("[QUARK-KERNEL ALERT] Blocked memory access to protected process %d by PID %d (ptrace mode 0x%x)!\n",
                                target_pid, parent_pid, call->mode);

#ifdef CONFIG_X86_64
            // Override rax register (return value) to 0 (false)
            regs->ax = 0;
#endif
        }
    }
    return 0;
}

static struct kretprobe quark_kretprobe = {
    .handler = ptrace_may_access_ret_handler,
    .entry_handler = ptrace_may_access_entry_handler,
    .data_size = sizeof(struct quark_pma_call),
    .maxactive = 64,
};

/*
 * Netlink receiver callback. Receives commands from userspace daemon.
 *
 * Commands 1 (protect), 2 (unprotect), and 3 (register trusted monitor) all change
 * real protection state, so all three go through the same authentication gate:
 * sender must be root running the real quark_cli binary (quark_check_sender), and
 * must present a fresh (quark_nonce_is_fresh), validly-signed (quark_verify_signature)
 * request. Command 3 especially: an unauthenticated version of it would let any local
 * process grant itself read access to every protected process's memory outright.
 */
static void quark_nl_recv_msg(struct sk_buff *skb) {
    struct nlmsghdr *nlh;
    struct quark_netlink_request req;
    int msg_type;
    u8 signed_buf[1 + sizeof(req.pid) + sizeof(req.nonce)];

    nlh = (struct nlmsghdr *)skb->data;
    msg_type = nlh->nlmsg_type;

    if (msg_type != 1 && msg_type != 2 && msg_type != 3) {
        pr_warn("[QUARK-KERNEL] Unknown Netlink message type: %d\n", msg_type);
        return;
    }
    // Commands 1 and 3 are request/response (the caller needs to know whether it
    // actually took effect, not just that the netlink send succeeded); 2 stays
    // fire-and-forget.
#define QUARK_EXPECTS_RESPONSE(t) ((t) == 1 || (t) == 3)

    if (nlmsg_len(nlh) < sizeof(req)) {
        pr_err("[QUARK-KERNEL] Netlink payload too small for command %d\n", msg_type);
        if (QUARK_EXPECTS_RESPONSE(msg_type))
            quark_send_protect_response(nlh->nlmsg_pid, false);
        return;
    }
    memcpy(&req, nlmsg_data(nlh), sizeof(req));

    if (!quark_check_sender(skb)) {
        if (QUARK_EXPECTS_RESPONSE(msg_type))
            quark_send_protect_response(nlh->nlmsg_pid, false);
        return;
    }

    if (!quark_nonce_is_fresh(req.nonce)) {
        pr_err("[QUARK-KERNEL] Rejecting command %d for pid %d: stale/replayed nonce\n",
               msg_type, req.pid);
        if (QUARK_EXPECTS_RESPONSE(msg_type))
            quark_send_protect_response(nlh->nlmsg_pid, false);
        return;
    }

    signed_buf[0] = (u8)msg_type;
    memcpy(&signed_buf[1], &req.pid, sizeof(req.pid));
    memcpy(&signed_buf[1 + sizeof(req.pid)], &req.nonce, sizeof(req.nonce));

    if (!quark_verify_signature(signed_buf, sizeof(signed_buf), req.sig, req.sig_len)) {
        pr_err("[QUARK-KERNEL] Rejecting command %d for pid %d: signature verification failed\n",
               msg_type, req.pid);
        if (QUARK_EXPECTS_RESPONSE(msg_type))
            quark_send_protect_response(nlh->nlmsg_pid, false);
        return;
    }

    quark_nonce_advance(req.nonce);

    switch (msg_type) {
        case 1: { // Command: Protect PID
            bool ok = quark_protect_pid(req.pid);
            quark_send_protect_response(nlh->nlmsg_pid, ok);
            break;
        }
        case 2: // Command: Unprotect PID
            remove_protected_pid(req.pid);
            break;
        case 3: // Command: Register trusted monitor pid
            trusted_monitor_pid = req.pid;
            pr_info("[QUARK-KERNEL] Trusted monitor pid set to %d\n", req.pid);
            quark_send_protect_response(nlh->nlmsg_pid, true);
            break;
    }
}
#undef QUARK_EXPECTS_RESPONSE

static int __init quark_kernel_init(void) {
    struct netlink_kernel_cfg cfg = {
        .input = quark_nl_recv_msg,
    };
    int ret;

    pr_info("[QUARK-KERNEL] Initializing Quark Anticheat Kernel Module...\n");

    // 1. Create Netlink socket for Daemon communication
    nl_socket = netlink_kernel_create(&init_net, NETLINK_QUARK, &cfg);
    if (!nl_socket) {
        pr_err("[QUARK-KERNEL] Failed to create Netlink socket.\n");
        return -ENOMEM;
    }
    pr_info("[QUARK-KERNEL] Netlink socket created successfully.\n");

    // 2. Register kretprobe
    quark_kretprobe.kp.symbol_name = "ptrace_may_access";
    ret = register_kretprobe(&quark_kretprobe);
    if (ret < 0) {
        pr_err("[QUARK-KERNEL] Failed to register kretprobe on ptrace_may_access: %d\n", ret);
        netlink_kernel_release(nl_socket);
        return ret;
    }
    
    pr_info("[QUARK-KERNEL] Successfully registered kretprobe on ptrace_may_access.\n");
    return 0;
}

static void __exit quark_kernel_exit(void) {
    pr_info("[QUARK-KERNEL] Exiting Quark Anticheat Kernel Module...\n");

    // Unregister kretprobe
    unregister_kretprobe(&quark_kretprobe);

    // Release netlink socket
    if (nl_socket) {
        netlink_kernel_release(nl_socket);
    }
}

module_init(quark_kernel_init);
module_exit(quark_kernel_exit);
