/*
 * Wire format for Quark's BPF-LSM/telemetry ring buffer (quark_events map).
 *
 * Included by both quark.bpf.c (kernel side) and mirrored field-for-field by the
 * #[repr(C)] QuarkEvent struct in bpf.rs (userspace side) -- keep the two in sync
 * by hand, the same way quark_kernel.c's netlink structs are hand-mirrored in
 * quark_cli.c.
 */
#ifndef QUARK_EVENTS_H
#define QUARK_EVENTS_H

enum quark_event_type {
    QUARK_EVENT_EXEC = 1,          // sched_process_exec while any pid is protected
    QUARK_EVENT_PTRACE_ATTEMPT,    // ptrace_access_check against a protected pid
    QUARK_EVENT_PROCESS_VM_ACCESS, // process_vm_readv/writev targeting a protected pid
    QUARK_EVENT_EXEC_MPROTECT,     // PROT_EXEC added to an anonymous mapping in a protected task
    QUARK_EVENT_TASK_KILL,         // signal sent to a protected tgid by a non-self/daemon sender
    QUARK_EVENT_MODULE_LOAD,       // any kernel module loaded while a pid is protected
    QUARK_EVENT_BPF_PROG_DENIED,   // bpf_prog_load blocked by lsm/bpf_prog_load
    QUARK_EVENT_BPF_OBJ_DENIED,    // access to a Quark map/prog/link blocked by lsm/bpf_map, lsm/bpf_prog or lsm/bpf
    QUARK_EVENT_DAEMON_ACCESS_DENIED, // ptrace-level access to the daemon itself denied
};

#define QUARK_COMM_LEN 16
#define QUARK_MODNAME_LEN 56

struct quark_event {
    unsigned long long timestamp_ns;   // bpf_ktime_get_ns() at emission time
    unsigned int type;                 // enum quark_event_type
    unsigned int pid;                  // caller tgid (or exiting/exec'ing tgid for EXEC/MODULE_LOAD)
    unsigned int target_pid;           // protected tgid this event is about (0 if not applicable)
    unsigned int aux;                  // ptrace mode / signal number / bpf_prog_type, per type
    char comm[QUARK_COMM_LEN];         // caller's comm
    char modname[QUARK_MODNAME_LEN];   // module name, MODULE_LOAD only; empty otherwise
};

#endif // QUARK_EVENTS_H
