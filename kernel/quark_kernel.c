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
#include <net/sock.h>
#ifdef CONFIG_X86_64
#include <asm/cpufeature.h>
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martinez Alarcon, Gabriel Sebastian & Merino De Rui, Stefano Nahuel");
MODULE_DESCRIPTION("Quark Anticheat - Ring 0 Security Module (kretprobes & Netlink)");
MODULE_VERSION("0.3.0");

#define QUARK_KERNEL_VERSION "0.3.0"

#define NETLINK_QUARK 31  // Custom netlink protocol number
#define MAX_PROTECTED_PROCESSES 16

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

static struct sock *nl_socket = NULL;
static pid_t protected_pids[MAX_PROTECTED_PROCESSES];
static int protected_pids_count = 0;
static DEFINE_SPINLOCK(quark_lock);

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

// Sends a quark_protect_response back to the requesting quark_cli over the
// same Netlink socket.
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

/*
 * Entry handler: runs before ptrace_may_access executes.
 * We extract the first argument (struct task_struct *task) and save it in ri->data.
 * Under x86_64, the first argument is passed in the DI register (regs->di).
 */
static int ptrace_may_access_entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
#ifdef CONFIG_X86_64
    struct task_struct *task = (struct task_struct *)regs->di;
    *((struct task_struct **)ri->data) = task;
#else
    *((struct task_struct **)ri->data) = NULL;
#endif
    return 0;
}

/*
 * Return handler: runs after ptrace_may_access finishes.
 * We inspect if the target task is protected. If so, and the caller is not
 * the process itself, we override the return value (rax) to 0 (false/access denied).
 */
static int ptrace_may_access_ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs) {
    struct task_struct *task = *((struct task_struct **)ri->data);
    pid_t target_pid;
    pid_t parent_pid;

    if (!task) {
        return 0;
    }

    target_pid = task_pid_vnr(task);
    parent_pid = task_pid_vnr(current);

    if (is_pid_protected(target_pid)) {
        // Allow the process to access itself, block others
        if (target_pid != parent_pid) {
            pr_warn("[QUARK-KERNEL ALERT] Blocked memory/ptrace access to protected process %d by PID %d!\n", 
                    target_pid, parent_pid);
            
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
    .data_size = sizeof(struct task_struct *),
    .maxactive = 64,
};

/*
 * Netlink receiver callback. Receives commands from userspace daemon.
 */
static void quark_nl_recv_msg(struct sk_buff *skb) {
    struct nlmsghdr *nlh;
    int pid;
    int msg_type;

    nlh = (struct nlmsghdr *)skb->data;
    msg_type = nlh->nlmsg_type;
    
    if (nlmsg_len(nlh) < sizeof(int)) {
        pr_err("[QUARK-KERNEL] Netlink payload too small\n");
        return;
    }
    
    pid = *(int *)nlmsg_data(nlh);

    switch (msg_type) {
        case 1: { // Command: Protect PID
            bool ok = quark_protect_pid(pid);
            quark_send_protect_response(nlh->nlmsg_pid, ok);
            break;
        }
        case 2: // Command: Unprotect PID
            remove_protected_pid(pid);
            break;
        default:
            pr_warn("[QUARK-KERNEL] Unknown Netlink message type: %d\n", msg_type);
            break;
    }
}

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
