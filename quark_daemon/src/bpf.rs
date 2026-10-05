//! Userspace side of Quark's BPF-LSM hooks and eBPF telemetry (src/bpf/quark.bpf.c).
//!
//! The kernel module (kernel/quark_kernel.c) remains the authority on which pids are
//! protected; `QuarkBpf::protect()` is only ever called after the kernel module has
//! already acked a protect request, mirroring that decision into the BPF programs'
//! `quark_protected` map.

use std::collections::HashSet;
use std::mem::MaybeUninit;
use std::os::fd::AsFd;
use std::sync::Mutex;
use std::thread;
use std::time::Duration;

use libbpf_rs::skel::{OpenSkel, Skel, SkelBuilder};
use libbpf_rs::{MapCore, MapFlags, OpenObject, Program, RingBufferBuilder};

mod skel {
    include!(concat!(env!("OUT_DIR"), "/quark.skel.rs"));
}
use skel::{QuarkSkel, QuarkSkelBuilder};

// Must match QUARK_MAX_OWN_{MAPS,PROGS,LINKS} in quark.bpf.c (the generated
// types::quark_config array lengths enforce this at compile time).
const MAX_OWN_MAPS: usize = 8;
const MAX_OWN_PROGS: usize = 16;
const MAX_OWN_LINKS: usize = 16;

// Mirror of enum quark_event_type in src/bpf/quark_events.h.
const EVENT_EXEC: u32 = 1;
const EVENT_PTRACE_ATTEMPT: u32 = 2;
const EVENT_PROCESS_VM_ACCESS: u32 = 3;
const EVENT_EXEC_MPROTECT: u32 = 4;
const EVENT_TASK_KILL: u32 = 5;
const EVENT_MODULE_LOAD: u32 = 6;
const EVENT_BPF_PROG_DENIED: u32 = 7;
const EVENT_BPF_OBJ_DENIED: u32 = 8;
const EVENT_DAEMON_ACCESS_DENIED: u32 = 9;

const COMM_LEN: usize = 16;
const MODNAME_LEN: usize = 56;
/// size_of::<struct quark_event>() in quark_events.h: u64 + 4*u32 + comm + modname.
const EVENT_SIZE: usize = 8 + 4 * 4 + COMM_LEN + MODNAME_LEN;

/// Decoded `struct quark_event` (src/bpf/quark_events.h).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QuarkEvent {
    pub timestamp_ns: u64,
    pub kind: u32,
    pub pid: u32,
    pub target_pid: u32,
    pub aux: u32,
    pub comm: String,
    pub modname: String,
}

fn c_str(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

fn u32_at(buf: &[u8], off: usize) -> u32 {
    u32::from_ne_bytes(buf[off..off + 4].try_into().expect("4-byte slice"))
}

/// Parses one ring buffer record. Returns None for a short/garbled record rather
/// than panicking inside the libbpf callback.
pub fn parse_event(buf: &[u8]) -> Option<QuarkEvent> {
    if buf.len() < EVENT_SIZE {
        return None;
    }
    let comm_off = 24;
    let modname_off = comm_off + COMM_LEN;
    Some(QuarkEvent {
        timestamp_ns: u64::from_ne_bytes(buf[0..8].try_into().ok()?),
        kind: u32_at(buf, 8),
        pid: u32_at(buf, 12),
        target_pid: u32_at(buf, 16),
        aux: u32_at(buf, 20),
        comm: c_str(&buf[comm_off..modname_off]),
        modname: c_str(&buf[modname_off..modname_off + MODNAME_LEN]),
    })
}

/// Human-readable name for a BPF_PROG_TYPE_* value, for the ones
/// quark_bpf_prog_load denies.
fn bpf_prog_type_name(t: u32) -> String {
    match t {
        2 => "kprobe".into(),
        5 => "tracepoint".into(),
        7 => "perf_event".into(),
        17 => "raw_tracepoint".into(),
        24 => "raw_tracepoint_writable".into(),
        26 => "tracing".into(),
        28 => "ext".into(),
        29 => "lsm".into(),
        other => format!("type {other}"),
    }
}

/// One `[QUARK-BPF]` log line per event.
pub fn format_event(ev: &QuarkEvent) -> String {
    let who = format!("pid {} ({})", ev.pid, ev.comm);
    match ev.kind {
        EVENT_EXEC => format!("[QUARK-BPF] EXEC: {who}"),
        EVENT_PTRACE_ATTEMPT => format!(
            "[QUARK-BPF] PTRACE_ATTEMPT: {who} -> protected pid {} (mode 0x{:x})",
            ev.target_pid, ev.aux
        ),
        EVENT_PROCESS_VM_ACCESS => format!(
            "[QUARK-BPF] PROCESS_VM_ACCESS: {who} -> protected pid {}",
            ev.target_pid
        ),
        EVENT_EXEC_MPROTECT => format!(
            "[QUARK-BPF] EXEC_MPROTECT: protected {who} made anonymous memory executable (prot 0x{:x})",
            ev.aux
        ),
        EVENT_TASK_KILL => format!(
            "[QUARK-BPF] TASK_KILL: {who} sent signal {} to protected pid {}",
            ev.aux, ev.target_pid
        ),
        EVENT_MODULE_LOAD => format!(
            "[QUARK-BPF] MODULE_LOAD: '{}' loaded by {who} while a process is protected",
            ev.modname
        ),
        EVENT_BPF_PROG_DENIED => format!(
            "[QUARK-BPF] BPF_PROG_DENIED: blocked {who} from loading a {} BPF program",
            bpf_prog_type_name(ev.aux)
        ),
        EVENT_BPF_OBJ_DENIED => format!(
            "[QUARK-BPF] BPF_OBJ_DENIED: blocked {who} from accessing Quark BPF object id {}",
            ev.aux
        ),
        EVENT_DAEMON_ACCESS_DENIED => format!(
            "[QUARK-BPF] DAEMON_ACCESS_DENIED: blocked {who} from ptrace-level access to the daemon (mode 0x{:x})",
            ev.aux
        ),
        other => format!("[QUARK-BPF] unknown event type {other} from {who}"),
    }
}

fn too_many_objects(what: &str, found: usize, max: usize) -> libbpf_rs::Error {
    std::io::Error::new(
        std::io::ErrorKind::InvalidData,
        format!("BPF object has {found} {what}, quark_config only holds {max}"),
    )
    .into()
}

fn write_config(
    skel: &QuarkSkel<'_>,
    cfg: &skel::types::quark_config,
) -> Result<(), libbpf_rs::Error> {
    // SAFETY: quark_config is a plain-old-data #[repr(C)] struct generated from the
    // BPF object's BTF, so viewing it as bytes is sound.
    let bytes = unsafe {
        std::slice::from_raw_parts(
            (cfg as *const skel::types::quark_config).cast::<u8>(),
            std::mem::size_of_val(cfg),
        )
    };
    skel.maps
        .quark_config
        .update(&0u32.to_ne_bytes(), bytes, MapFlags::ANY)
}

struct Inner {
    skel: QuarkSkel<'static>,
    protected: HashSet<u32>,
}

/// Loaded and attached BPF object. Links detach when this is dropped (daemon exit).
pub struct QuarkBpf {
    inner: Mutex<Inner>,
}

impl QuarkBpf {
    /// Loads src/bpf/quark.bpf.c, registers `daemon_tgid` plus Quark's own map/prog
    /// ids as the self-protection config, attaches every program, and starts the
    /// telemetry thread.
    ///
    /// The config is written *before* attaching: once quark_bpf_map is attached,
    /// Quark's own maps are only reachable by `daemon_tgid`.
    pub fn load_and_attach(daemon_tgid: u32) -> Result<Self, libbpf_rs::Error> {
        // Lives as long as the daemon; the skeleton borrows from it.
        let object: &'static mut MaybeUninit<OpenObject> =
            Box::leak(Box::new(MaybeUninit::uninit()));
        let open = QuarkSkelBuilder::default().open(object)?;
        let mut skel = open.load()?;

        let mut cfg = skel::types::quark_config {
            daemon_tgid,
            ..Default::default()
        };
        let map_ids: Vec<u32> = skel
            .object()
            .maps()
            .map(|m| m.info().map(|i| i.info.id))
            .collect::<Result<_, _>>()?;
        let prog_ids: Vec<u32> = skel
            .object()
            .progs()
            .map(|p| Program::id_from_fd(p.as_fd()))
            .collect::<Result<_, _>>()?;
        if map_ids.len() > MAX_OWN_MAPS {
            return Err(too_many_objects("maps", map_ids.len(), MAX_OWN_MAPS));
        }
        if prog_ids.len() > MAX_OWN_PROGS {
            return Err(too_many_objects("programs", prog_ids.len(), MAX_OWN_PROGS));
        }
        cfg.map_ids[..map_ids.len()].copy_from_slice(&map_ids);
        cfg.map_id_count = map_ids.len() as u32;
        cfg.prog_ids[..prog_ids.len()].copy_from_slice(&prog_ids);
        cfg.prog_id_count = prog_ids.len() as u32;

        write_config(&skel, &cfg)?;

        skel.attach()?;

        // Link ids only exist after attach. Found by prog id rather than by walking
        // skel.links field by field, so a new program can't be forgotten here.
        let link_ids: Vec<u32> = libbpf_rs::query::LinkInfoIter::default()
            .filter(|l| prog_ids.contains(&l.prog_id))
            .map(|l| l.id)
            .collect();
        if link_ids.len() > MAX_OWN_LINKS {
            return Err(too_many_objects("links", link_ids.len(), MAX_OWN_LINKS));
        }
        cfg.link_ids[..link_ids.len()].copy_from_slice(&link_ids);
        cfg.link_id_count = link_ids.len() as u32;
        write_config(&skel, &cfg)?;

        let ringbuf = {
            let mut builder = RingBufferBuilder::new();
            builder.add(&skel.maps.quark_events, |data: &[u8]| {
                match parse_event(data) {
                    Some(ev) => println!("{}", format_event(&ev)),
                    None => eprintln!("[QUARK-BPF] Dropped malformed event ({} bytes)", data.len()),
                }
                0
            })?;
            builder.build()?
        };
        thread::spawn(move || {
            loop {
                if let Err(e) = ringbuf.poll(Duration::from_millis(250)) {
                    // EINTR is routine (signals); anything else ends telemetry but
                    // leaves the attached LSM hooks in place.
                    if e.kind() != libbpf_rs::ErrorKind::Interrupted {
                        eprintln!("[QUARK-BPF] Telemetry poll failed, stopping event thread: {e}");
                        break;
                    }
                }
            }
        });

        println!(
            "[QUARK-BPF] BPF-LSM hooks and telemetry attached ({} programs, {} maps, {} links).",
            prog_ids.len(),
            map_ids.len(),
            link_ids.len()
        );
        Ok(QuarkBpf {
            inner: Mutex::new(Inner {
                skel,
                protected: HashSet::new(),
            }),
        })
    }

    /// Mirrors a pid the kernel module has already agreed to protect.
    pub fn protect(&self, tgid: u32) -> Result<(), libbpf_rs::Error> {
        let mut inner = self.inner.lock().unwrap();
        inner
            .skel
            .maps
            .quark_protected
            .update(&tgid.to_ne_bytes(), &[1u8], MapFlags::ANY)?;
        inner.protected.insert(tgid);
        Self::sync_count(&mut inner);
        Ok(())
    }

    /// Removes a pid from the mirror. A missing entry isn't an error: the
    /// sched_process_exit program may already have removed it.
    pub fn unprotect(&self, tgid: u32) {
        let mut inner = self.inner.lock().unwrap();
        if let Err(e) = inner.skel.maps.quark_protected.delete(&tgid.to_ne_bytes())
            && e.kind() != libbpf_rs::ErrorKind::NotFound
        {
            eprintln!("[QUARK-BPF] Warning: failed to remove pid {tgid} from quark_protected: {e}");
        }
        inner.protected.remove(&tgid);
        Self::sync_count(&mut inner);
    }

    // The daemon is the only writer of quark_protected_count. Once a protected pid
    // exits, the BPF side deletes its map entry but leaves the count alone; a count
    // that is briefly too high only keeps the gates closed a little longer, which
    // fails safe.
    fn sync_count(inner: &mut Inner) {
        let count = inner.protected.len() as u32;
        if let Some(bss) = inner.skel.maps.bss_data.as_deref_mut() {
            bss.quark_protected_count = count;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn raw_event(kind: u32, pid: u32, target: u32, aux: u32, comm: &str, modname: &str) -> Vec<u8> {
        let mut buf = Vec::with_capacity(EVENT_SIZE);
        buf.extend_from_slice(&42u64.to_ne_bytes());
        for v in [kind, pid, target, aux] {
            buf.extend_from_slice(&v.to_ne_bytes());
        }
        let mut c = [0u8; COMM_LEN];
        c[..comm.len()].copy_from_slice(comm.as_bytes());
        buf.extend_from_slice(&c);
        let mut m = [0u8; MODNAME_LEN];
        m[..modname.len()].copy_from_slice(modname.as_bytes());
        buf.extend_from_slice(&m);
        buf
    }

    #[test]
    fn parses_event_fields() {
        let ev = parse_event(&raw_event(EVENT_PTRACE_ATTEMPT, 10, 20, 0x11, "cheat", "")).unwrap();
        assert_eq!(
            ev,
            QuarkEvent {
                timestamp_ns: 42,
                kind: EVENT_PTRACE_ATTEMPT,
                pid: 10,
                target_pid: 20,
                aux: 0x11,
                comm: "cheat".into(),
                modname: String::new(),
            }
        );
    }

    #[test]
    fn rejects_short_record() {
        assert!(parse_event(&[0u8; EVENT_SIZE - 1]).is_none());
    }

    #[test]
    fn formats_ptrace_attempt() {
        let ev = parse_event(&raw_event(EVENT_PTRACE_ATTEMPT, 10, 20, 0x11, "cheat", "")).unwrap();
        assert_eq!(
            format_event(&ev),
            "[QUARK-BPF] PTRACE_ATTEMPT: pid 10 (cheat) -> protected pid 20 (mode 0x11)"
        );
    }

    #[test]
    fn formats_denied_bpf_prog_type() {
        let ev = parse_event(&raw_event(EVENT_BPF_PROG_DENIED, 7, 0, 2, "bpftrace", "")).unwrap();
        assert_eq!(
            format_event(&ev),
            "[QUARK-BPF] BPF_PROG_DENIED: blocked pid 7 (bpftrace) from loading a kprobe BPF program"
        );
    }

    #[test]
    fn formats_module_load_name() {
        let ev = parse_event(&raw_event(EVENT_MODULE_LOAD, 1, 0, 0, "insmod", "evil_mod")).unwrap();
        assert_eq!(
            format_event(&ev),
            "[QUARK-BPF] MODULE_LOAD: 'evil_mod' loaded by pid 1 (insmod) while a process is protected"
        );
    }

    #[test]
    fn formats_daemon_access_denied() {
        let ev = parse_event(&raw_event(
            EVENT_DAEMON_ACCESS_DENIED,
            5,
            9,
            0x12,
            "gdb",
            "",
        ))
        .unwrap();
        assert_eq!(
            format_event(&ev),
            "[QUARK-BPF] DAEMON_ACCESS_DENIED: blocked pid 5 (gdb) from ptrace-level access to the daemon (mode 0x12)"
        );
    }

    #[test]
    fn formats_unknown_type() {
        let ev = parse_event(&raw_event(99, 1, 0, 0, "x", "")).unwrap();
        assert_eq!(
            format_event(&ev),
            "[QUARK-BPF] unknown event type 99 from pid 1 (x)"
        );
    }

    // The two tests below need root and `bpf` in /sys/kernel/security/lsm:
    // `sudo -E cargo test -- --ignored --test-threads=1` (each attaches its own copy
    // of the hooks, so they must not overlap).

    /// Verifier acceptance on the running kernel.
    #[test]
    #[ignore]
    fn loads_and_attaches_on_running_kernel() {
        let bpf = QuarkBpf::load_and_attach(std::process::id()).expect("load/attach failed");
        bpf.protect(u32::MAX - 1).expect("protect failed");
        bpf.unprotect(u32::MAX - 1);
    }

    /// Attach-level access to the daemon (here: opening /proc/<pid>/mem) must be
    /// closed to every other process, root included (lsm/ptrace_access_check). This
    /// test process plays the daemon.
    ///
    /// Deliberately doesn't check read-level entries like /proc/<pid>/environ or maps:
    /// mm_access() -> may_access_mm() lets any perfmon_capable() caller through for
    /// PTRACE_MODE_READ even after ptrace_may_access() refused, so root can always read
    /// those. That's kernel policy, not a Quark bug (see quark.bpf.c).
    #[test]
    #[ignore]
    fn daemon_memory_closed_to_other_processes() {
        let own = std::process::id();
        let bpf = QuarkBpf::load_and_attach(own).expect("load/attach failed");

        let cfg = {
            let inner = bpf.inner.lock().unwrap();
            let raw = inner
                .skel
                .maps
                .quark_config
                .lookup(&0u32.to_ne_bytes(), MapFlags::ANY)
                .expect("config lookup failed")
                .expect("config entry missing");
            u32::from_ne_bytes(raw[0..4].try_into().unwrap())
        };
        assert_eq!(cfg, own, "daemon_tgid in quark_config");

        let status = std::process::Command::new("dd")
            .arg(format!("if=/proc/{own}/mem"))
            .args(["bs=1", "count=0"])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .expect("failed to run dd");
        assert!(
            !status.success(),
            "another process could open the daemon's /proc/<pid>/mem"
        );
    }
}
