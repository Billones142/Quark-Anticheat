// Quark Anticheat - BPF-LSM hooks & eBPF telemetry.
//
// Complements kernel/quark_kernel.c's ptrace_may_access kretprobe (the primary,
// authoritative memory/ptrace block -- see docs/features.md §1.1). This object adds
// what a kretprobe on one function can't:
//
//   - Real LSM hooks (BPF-LSM: eBPF programs attached to security_* hooks). An
//     out-of-tree .ko can't register classic LSM hooks -- security_add_hooks() is
//     __init and unexported -- so this is the only way to get one. Used here to:
//       (a) block loading BPF tracing-class programs while any pid is protected,
//           since bpf_probe_read_user() from a kprobe/uprobe reaches a game's memory
//           without ever calling ptrace_may_access, bypassing the kretprobe entirely.
//       (b) stop anyone but the daemon from touching Quark's own maps/programs, or
//           from ptracing the daemon itself (a gap the kretprobe doesn't cover, since
//           the daemon's own pid is never in protected_pids).
//   - Low-overhead telemetry (tracepoints + non-enforcing LSM hooks) for everything
//     else worth knowing about while a process is protected: process_vm_* attempts,
//     PROT_EXEC mprotect on a protected task, signals sent to a protected task,
//     kernel module loads, and exec.
//
// The protected-pid set here (quark_protected) is a *mirror*, not a second source of
// truth: the daemon only ever writes to it after the kernel module's signed netlink
// exchange already returned ok=1 (see quark_daemon/src/bpf.rs). Authority to decide
// who's protected still lives entirely in kernel/quark_kernel.c.
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "quark_events.h"

char LICENSE[] SEC("license") = "GPL";

// PTRACE_MODE_* aren't in a UAPI header (include/linux/ptrace.h is kernel-internal);
// values have been stable since their introduction and are only used here to label or
// filter telemetry events, never to make an enforcement decision.
#define QUARK_PTRACE_MODE_READ    0x01
#define QUARK_PTRACE_MODE_ATTACH  0x02
#define QUARK_PTRACE_MODE_NOAUDIT 0x04

// Not available from vmlinux.h (it's a libc/UAPI errno macro, not a kernel type),
// and pulling in <linux/errno.h> here fights with vmlinux.h's own type definitions --
// the standard libbpf-bootstrap-style workaround is just defining what's used.
#ifndef EPERM
#define EPERM 1
#endif

#define QUARK_MAX_OWN_MAPS  8
#define QUARK_MAX_OWN_PROGS 16
#define QUARK_MAX_OWN_LINKS 16

// Mirrors kernel/quark_kernel.c's protected_pids set: key is the protected tgid,
// value is unused (presence is the signal). Same MAX_PROTECTED_PROCESSES=16 cap.
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 16);
    __type(key, __u32);
    __type(value, __u8);
} quark_protected SEC(".maps");

// Single-entry config blob, written by the daemon at startup (quark_daemon/src/
// bpf.rs, load_and_attach()). daemon_tgid and the map/prog ids are written *before*
// the programs below are attached, so there's no window where quark_bpf_map/
// quark_bpf_prog run without knowing what to protect. Link ids only exist once
// attached, so they're filled in right after (by the daemon, which quark_bpf_map
// still lets through).
struct quark_config {
    __u32 daemon_tgid;
    __u32 map_ids[QUARK_MAX_OWN_MAPS];
    __u32 map_id_count;
    __u32 prog_ids[QUARK_MAX_OWN_PROGS];
    __u32 prog_id_count;
    __u32 link_ids[QUARK_MAX_OWN_LINKS];
    __u32 link_id_count;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct quark_config);
} quark_config SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 256 * 1024);
} quark_events SEC(".maps");

// Count of entries currently in quark_protected, kept in lockstep by the daemon
// alongside its quark_protected map writes (bpf.rs's protect()/unprotect()). Lets the
// hot, system-wide-firing programs below (exec, module load) skip straight past a
// ringbuf reservation when nothing is protected, instead of that being the common
// case on every process exec on the box.
volatile __u32 quark_protected_count = 0;

static __always_inline bool is_protected(__u32 tgid) {
    return bpf_map_lookup_elem(&quark_protected, &tgid) != NULL;
}

static __always_inline struct quark_config *get_config(void) {
    __u32 key = 0;
    return bpf_map_lookup_elem(&quark_config, &key);
}

static __always_inline bool is_daemon(__u32 tgid) {
    struct quark_config *cfg = get_config();
    return cfg && cfg->daemon_tgid != 0 && cfg->daemon_tgid == tgid;
}

// `max` is a compile-time constant at every (inlined) call site, which is what lets
// the verifier prove this loop terminates (bounded loops, kernel >= 5.3).
static __always_inline bool id_in(const __u32 *ids, __u32 count, int max, __u32 id) {
    for (int i = 0; i < max; i++) {
        if (i >= count)
            break;
        if (ids[i] == id)
            return true;
    }
    return false;
}

static __always_inline bool is_quark_map_id(__u32 id) {
    struct quark_config *cfg = get_config();
    return cfg && id_in(cfg->map_ids, cfg->map_id_count, QUARK_MAX_OWN_MAPS, id);
}

static __always_inline bool is_quark_prog_id(__u32 id) {
    struct quark_config *cfg = get_config();
    return cfg && id_in(cfg->prog_ids, cfg->prog_id_count, QUARK_MAX_OWN_PROGS, id);
}

static __always_inline bool is_quark_link_id(__u32 id) {
    struct quark_config *cfg = get_config();
    return cfg && id_in(cfg->link_ids, cfg->link_id_count, QUARK_MAX_OWN_LINKS, id);
}

// Reserves and submits a quark_event. `modname` may be NULL (most event types don't
// use it); when non-NULL it's read with bpf_probe_read_kernel_str().
static __always_inline void emit_event(__u32 type, __u32 pid, __u32 target_pid,
                                        __u32 aux, const char *modname) {
    struct quark_event *ev = bpf_ringbuf_reserve(&quark_events, sizeof(*ev), 0);
    if (!ev)
        return;

    ev->timestamp_ns = bpf_ktime_get_ns();
    ev->type = type;
    ev->pid = pid;
    ev->target_pid = target_pid;
    ev->aux = aux;
    bpf_get_current_comm(&ev->comm, sizeof(ev->comm));
    ev->modname[0] = '\0';
    if (modname)
        bpf_probe_read_kernel_str(&ev->modname, sizeof(ev->modname), modname);

    bpf_ringbuf_submit(ev, 0);
}

// --- (a) Deny loading BPF tracing-class programs while anything is protected -----
//
// A kprobe/uprobe/tracing program running bpf_probe_read_user() against a game's
// memory never calls ptrace_may_access, so it's invisible to the kretprobe entirely.
// Non-tracing types (XDP, cgroup/skb, sockops, etc.) are left alone -- they have no
// path to arbitrary process memory.
SEC("lsm/bpf_prog_load")
int BPF_PROG(quark_bpf_prog_load, struct bpf_prog *prog, union bpf_attr *attr,
             struct bpf_token *token, bool kernel) {
    if (quark_protected_count == 0)
        return 0;

    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    if (is_daemon(caller_tgid))
        return 0;

    enum bpf_prog_type type = BPF_CORE_READ(prog, type);
    bool is_tracing_type = type == BPF_PROG_TYPE_KPROBE ||
                            type == BPF_PROG_TYPE_TRACEPOINT ||
                            type == BPF_PROG_TYPE_PERF_EVENT ||
                            type == BPF_PROG_TYPE_RAW_TRACEPOINT ||
                            type == BPF_PROG_TYPE_RAW_TRACEPOINT_WRITABLE ||
                            type == BPF_PROG_TYPE_TRACING ||
                            type == BPF_PROG_TYPE_LSM ||
                            type == BPF_PROG_TYPE_EXT;
    if (!is_tracing_type)
        return 0;

    emit_event(QUARK_EVENT_BPF_PROG_DENIED, caller_tgid, 0, (__u32)type, NULL);
    return -EPERM;
}

// --- (b) Self-protection: Quark's own maps/programs, and the daemon's own pid ----

SEC("lsm/bpf_map")
int BPF_PROG(quark_bpf_map, struct bpf_map *map, fmode_t fmode) {
    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    if (is_daemon(caller_tgid))
        return 0;

    __u32 id = BPF_CORE_READ(map, id);
    if (!is_quark_map_id(id))
        return 0;

    emit_event(QUARK_EVENT_BPF_OBJ_DENIED, caller_tgid, 0, id, NULL);
    return -EPERM;
}

SEC("lsm/bpf_prog")
int BPF_PROG(quark_bpf_prog, struct bpf_prog *prog) {
    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    if (is_daemon(caller_tgid))
        return 0;

    __u32 id = BPF_CORE_READ(prog, aux, id);
    if (!is_quark_prog_id(id))
        return 0;

    emit_event(QUARK_EVENT_BPF_OBJ_DENIED, caller_tgid, 0, id, NULL);
    return -EPERM;
}

// Links have no dedicated LSM hook, so this gates the bpf() command itself: without
// it, any root process could BPF_LINK_GET_FD_BY_ID one of Quark's links and
// BPF_LINK_DETACH it, unloading the hooks above. (Map and prog fds obtained by id
// already go through quark_bpf_map / quark_bpf_prog.)
SEC("lsm/bpf")
int BPF_PROG(quark_bpf_cmd, int cmd, union bpf_attr *attr, unsigned int size, bool kernel) {
    if (cmd != BPF_LINK_GET_FD_BY_ID)
        return 0;

    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    if (is_daemon(caller_tgid))
        return 0;

    __u32 id = BPF_CORE_READ(attr, link_id);
    if (!is_quark_link_id(id))
        return 0;

    emit_event(QUARK_EVENT_BPF_OBJ_DENIED, caller_tgid, 0, id, NULL);
    return -EPERM;
}

// --- ptrace: daemon self-protection (enforced) + protected-pid telemetry ---------
//
// Blocking access to a *protected* pid is the kretprobe's job (kernel/quark_kernel.c)
// -- this hook does not duplicate that block, only reports the attempt. Blocking
// access to the *daemon itself* is new: nothing else stops another local process from
// ptracing or pidfd_getfd()-ing the daemon (its own pid is never in protected_pids,
// since command 1 only ever registers game pids).
//
// What this denial actually covers is attach-level access: ptrace attach,
// pidfd_getfd(), /proc/<daemon>/mem, process_vm_*. Read-level /proc entries
// (environ, maps, auxv) stay readable by root regardless: mm_access() ->
// may_access_mm() grants PTRACE_MODE_READ to any perfmon_capable() caller *after*
// ptrace_may_access() (and so this hook) already refused. That's deliberate kernel
// behavior for profilers, it's inlined, and nothing here can override it.
SEC("lsm/ptrace_access_check")
int BPF_PROG(quark_ptrace_access_check, struct task_struct *child, unsigned int mode) {
    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    __u32 target_tgid = BPF_CORE_READ(child, tgid);

    if (caller_tgid == target_tgid)
        return 0;

    // Only attach-level access (ptrace attach, /proc/<pid>/mem, process_vm_*) can read
    // another process's memory; a bare PTRACE_MODE_READ is just metadata (cmdline,
    // maps, status) that ps/htop/journald read constantly. Report only attach, matching
    // the kretprobe's enforcement (kernel/quark_kernel.c), so the daemon log shows real
    // memory-access attempts instead of every process listing.
    bool is_attach = mode & QUARK_PTRACE_MODE_ATTACH;

    // The daemon stays fully locked down (every mode denied, defense in depth), but only
    // a genuine attach attempt is worth a log line.
    if (is_daemon(target_tgid) && !is_daemon(caller_tgid)) {
        if (is_attach)
            emit_event(QUARK_EVENT_DAEMON_ACCESS_DENIED, caller_tgid, target_tgid, mode, NULL);
        return -EPERM;
    }

    if (is_attach && is_protected(target_tgid) && !is_daemon(caller_tgid))
        emit_event(QUARK_EVENT_PTRACE_ATTEMPT, caller_tgid, target_tgid, mode, NULL);

    return 0;
}

// --- process_vm_readv/writev telemetry --------------------------------------------
//
// Both syscalls reach mm_access() -> ptrace_may_access, so the kretprobe already
// blocks them against a protected target; these two events are what let
// docs/features.md §5's open question be answered "yes, same gate, already covered" —
// with proof an attempt actually happened, not just a code-reading argument.
SEC("tp/syscalls/sys_enter_process_vm_readv")
int quark_tp_process_vm_readv(struct trace_event_raw_sys_enter *ctx) {
    __u32 target_pid = (__u32)ctx->args[0];
    if (!is_protected(target_pid))
        return 0;
    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    if (caller_tgid == target_pid || is_daemon(caller_tgid))
        return 0;
    emit_event(QUARK_EVENT_PROCESS_VM_ACCESS, caller_tgid, target_pid, 0, NULL);
    return 0;
}

SEC("tp/syscalls/sys_enter_process_vm_writev")
int quark_tp_process_vm_writev(struct trace_event_raw_sys_enter *ctx) {
    __u32 target_pid = (__u32)ctx->args[0];
    if (!is_protected(target_pid))
        return 0;
    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    if (caller_tgid == target_pid || is_daemon(caller_tgid))
        return 0;
    emit_event(QUARK_EVENT_PROCESS_VM_ACCESS, caller_tgid, target_pid, 0, NULL);
    return 0;
}

// --- PROT_EXEC mprotect telemetry (injected-code indicator) -----------------------
//
// Telemetry only, deliberately not enforced: legitimate JITs (including game engine
// script VMs) mprotect(PROT_EXEC) anonymous memory too, so blocking this outright
// would be a real false-positive risk. Left for a human/server-side policy to weigh
// against everything else known about the process.
SEC("lsm/file_mprotect")
int BPF_PROG(quark_file_mprotect, struct vm_area_struct *vma, unsigned long reqprot,
             unsigned long prot) {
    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    if (!is_protected(caller_tgid))
        return 0;

    if (!(reqprot & 0x4)) // PROT_EXEC
        return 0;

    struct file *vm_file = BPF_CORE_READ(vma, vm_file);
    if (vm_file) // file-backed mapping (e.g. a legitimately-mapped .so) -- ignore
        return 0;

    emit_event(QUARK_EVENT_EXEC_MPROTECT, caller_tgid, caller_tgid, (__u32)prot, NULL);
    return 0;
}

// --- Signals to a protected task, from anyone but itself/the daemon ---------------
SEC("lsm/task_kill")
int BPF_PROG(quark_task_kill, struct task_struct *p, struct kernel_siginfo *info,
             int sig, const struct cred *cred) {
    __u32 target_tgid = BPF_CORE_READ(p, tgid);
    if (!is_protected(target_tgid))
        return 0;

    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    if (caller_tgid == target_tgid || is_daemon(caller_tgid))
        return 0;

    emit_event(QUARK_EVENT_TASK_KILL, caller_tgid, target_tgid, (__u32)sig, NULL);
    return 0;
}

// --- Kernel module loads while anything is protected ------------------------------
//
// Purely informational: a kernel-mode cheat is out of reach of any LSM once it's
// loaded, but knowing one loaded at all while a match was in progress is itself a
// useful signal for a server-side policy.
SEC("tp/module/module_load")
int quark_tp_module_load(struct trace_event_raw_module_load *ctx) {
    if (quark_protected_count == 0)
        return 0;
    __u32 caller_tgid = bpf_get_current_pid_tgid() >> 32;
    // `name` is a tracepoint __string() field: __data_loc_name packs a 16-bit
    // byte offset (from the start of ctx) to the actual string, not the string
    // itself -- see Documentation/trace/tracepoint-analysis or any
    // __data_loc-using example in libbpf-bootstrap.
    const char *modname = (const char *)ctx + (ctx->__data_loc_name & 0xffff);
    emit_event(QUARK_EVENT_MODULE_LOAD, caller_tgid, 0, 0, modname);
    return 0;
}

// --- Exec telemetry ----------------------------------------------------------------
SEC("tp/sched/sched_process_exec")
int quark_tp_sched_process_exec(struct trace_event_raw_sched_process_exec *ctx) {
    if (quark_protected_count == 0)
        return 0;
    __u32 tgid = bpf_get_current_pid_tgid() >> 32;
    emit_event(QUARK_EVENT_EXEC, tgid, 0, 0, NULL);
    return 0;
}

// --- Protected-set cleanup on exit --------------------------------------------------
//
// Belt-and-braces against pid reuse: if the daemon's own cleanup (bpf.rs's
// unprotect(), driven by the client socket closing) is ever late or skipped, a
// protected tgid stays keyed in quark_protected only until it actually exits -- a
// freshly reused pid can never inherit stale protected/self-protection status.
SEC("tp/sched/sched_process_exit")
int quark_tp_sched_process_exit(struct trace_event_raw_sched_process_exit *ctx) {
    __u64 pid_tgid = bpf_get_current_pid_tgid();
    __u32 pid = (__u32)pid_tgid;
    __u32 tgid = pid_tgid >> 32;

    if (pid != tgid) // only act on the group leader exiting, not any thread
        return 0;

    if (bpf_map_delete_elem(&quark_protected, &tgid) == 0) {
        if (quark_protected_count > 0)
            quark_protected_count--;
    }
    return 0;
}
