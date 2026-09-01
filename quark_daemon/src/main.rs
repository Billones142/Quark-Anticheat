use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::Path;
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

// First file descriptor systemd hands to a socket-activated service, per the
// sd_listen_fds(3) protocol (fds 0/1/2 are stdio, activation fds start at 3).
const SD_LISTEN_FDS_START: i32 = 3;

/// Picks up the listening socket from systemd if this process was launched via
/// socket activation (LISTEN_FDS/LISTEN_PID set by systemd, matching how
/// `docker.socket` starts `dockerd` on demand): the socket already exists and
/// is already bound before we even start, so the daemon only runs while
/// something is actually talking to it. Returns None if not socket-activated,
/// so `main` can fall back to binding the socket itself (e.g. for manual
/// testing without systemd).
fn systemd_activated_listener() -> Option<UnixListener> {
    let listen_pid = std::env::var("LISTEN_PID").ok()?;
    if listen_pid.parse::<u32>().ok()? != std::process::id() {
        // Not meant for us (e.g. inherited by a child process by mistake).
        return None;
    }

    let listen_fds: i32 = std::env::var("LISTEN_FDS").ok()?.parse().ok()?;
    if listen_fds < 1 {
        return None;
    }

    // SAFETY: systemd guarantees fd SD_LISTEN_FDS_START is a valid, already
    // connect()-able AF_UNIX SOCK_STREAM socket when it sets LISTEN_FDS/
    // LISTEN_PID this way (see systemd.socket(5) / sd_listen_fds(3)); we only
    // reach here after checking LISTEN_PID matches our own pid.
    Some(unsafe { UnixListener::from_raw_fd(SD_LISTEN_FDS_START) })
}

// Command IDs
const CMD_REGISTER_GAME: u32 = 1;
const CMD_REGISTER_VAR: u32 = 2;
const CMD_UPDATE_VAR: u32 = 3;

#[derive(Debug, Clone)]
struct MonitoredVar {
    address: u64,
    size: u32,
    name: String,
    expected_value: u64,
}

struct QuarkState {
    pid: i32,
    variables: HashMap<u64, MonitoredVar>,
    is_active: bool,
}

fn bytes_to_u64(buf: &[u8], size: u32) -> u64 {
    match size {
        1 => buf[0] as u64,
        2 => {
            let mut array = [0u8; 2];
            array.copy_from_slice(&buf[..2]);
            u16::from_ne_bytes(array) as u64
        }
        4 => {
            let mut array = [0u8; 4];
            array.copy_from_slice(&buf[..4]);
            u32::from_ne_bytes(array) as u64
        }
        8 => {
            let mut array = [0u8; 8];
            array.copy_from_slice(&buf[..8]);
            u64::from_ne_bytes(array)
        }
        _ => 0,
    }
}

// SO_PEERCRED is kernel-verified: it reports the real pid/uid/gid of whoever is on
// the other end of this specific connected socket, unlike anything a client could
// put in its own payload. std's UnixStream::peer_cred() would be the natural way to
// get this, but it's still gated behind the unstable peer_credentials_unix_socket
// feature on this toolchain, so it's read directly via libc instead.
fn get_peer_pid(stream: &UnixStream) -> Option<i32> {
    let mut cred = libc::ucred {
        pid: 0,
        uid: 0,
        gid: 0,
    };
    let mut len = std::mem::size_of::<libc::ucred>() as libc::socklen_t;

    let ret = unsafe {
        libc::getsockopt(
            stream.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            &mut cred as *mut libc::ucred as *mut libc::c_void,
            &mut len,
        )
    };

    if ret == 0 && len == std::mem::size_of::<libc::ucred>() as libc::socklen_t {
        Some(cred.pid)
    } else {
        None
    }
}

fn handle_client(mut stream: UnixStream) -> std::io::Result<()> {
    println!("[QUARK-DAEMON] Client connected.");

    // /tmp/quark.sock is world read/write (game processes need to reach it), so the
    // pid a client *claims* in CMD_REGISTER_GAME's payload can't be trusted -- a
    // malicious local process could claim someone else's real, already-protected pid
    // and then just disconnect, tricking this function's own cleanup path into
    // unprotecting the victim. peer_cred() is kernel-verified (SO_PEERCRED under the
    // hood) and can't be spoofed by the client, so it -- not the payload -- is the
    // only pid ever actually used against the kernel module below.
    let real_pid: Option<i32> = get_peer_pid(&stream);
    if real_pid.is_none() {
        eprintln!("[QUARK-DAEMON] Could not determine the real pid of the connecting client (peer_cred failed); refusing to register it.");
    }

    let state: Arc<Mutex<Option<QuarkState>>> = Arc::new(Mutex::new(None));
    let state_clone = Arc::clone(&state);

    let mut header_buf = [0u8; 8];
    
    loop {
        if let Err(e) = stream.read_exact(&mut header_buf) {
            if e.kind() == std::io::ErrorKind::UnexpectedEof {
                println!("[QUARK-DAEMON] Client disconnected (EOF).");
                break;
            }
            return Err(e);
        }
        
        let command = u32::from_ne_bytes([header_buf[0], header_buf[1], header_buf[2], header_buf[3]]);
        let payload_len = u32::from_ne_bytes([header_buf[4], header_buf[5], header_buf[6], header_buf[7]]);
        
        let mut payload = vec![0u8; payload_len as usize];
        stream.read_exact(&mut payload)?;
        
        match command {
            CMD_REGISTER_GAME => {
                if payload.len() < 4 {
                    println!("[QUARK-DAEMON] Invalid payload size for REGISTER_GAME");
                    continue;
                }
                let claimed_pid = i32::from_ne_bytes([payload[0], payload[1], payload[2], payload[3]]);

                // Authority comes from peer_cred() alone -- see the comment where
                // real_pid is computed above. The payload's pid is only ever used for
                // an anomaly log line.
                let pid = match real_pid {
                    Some(p) => p,
                    None => {
                        eprintln!("[QUARK-DAEMON] Refusing REGISTER_GAME: no verified peer pid for this connection.");
                        if let Err(e) = stream.write_all(&[0u8; 18]) {
                            eprintln!("[QUARK-DAEMON] Failed to send registration nack: {:?}", e);
                            return Err(e);
                        }
                        break;
                    }
                };
                if claimed_pid != pid {
                    eprintln!(
                        "[QUARK-DAEMON] NOTE: client on pid {} claimed pid {} in its REGISTER_GAME payload -- ignoring the claim, using the real connection pid.",
                        pid, claimed_pid
                    );
                }
                println!("[QUARK-DAEMON] Registering game process with PID: {}", pid);

                // 1. Tell kernel module to PROTECT this PID
                println!("[QUARK-DAEMON] Registering PID {} to Ring 0 Module...", pid);
                let output = Command::new("sudo")
                    .args(&["./quark_daemon/quark_cli", "1", &pid.to_string()])
                    .output();
                // quark_cli exits non-zero on any netlink failure (e.g. the kernel
                // module isn't loaded, or it refused because it detected a VM), so
                // its exit status is the actual source of truth for whether Ring 0
                // protection is active for this PID -- not just whether the daemon
                // itself is reachable. On success it also prints QUARK_VERSION: and
                // QUARK_TESTING_BUILD: lines we scrape out of its stdout below.
                let mut kernel_version = String::from("unknown");
                let mut kernel_testing_build = false;
                let kernel_ok = match &output {
                    Ok(out) => {
                        let stdout_str = String::from_utf8_lossy(&out.stdout);
                        print!("[QUARK-DAEMON] Kernel Registration output: {}", stdout_str);
                        for line in stdout_str.lines() {
                            if let Some(v) = line.strip_prefix("QUARK_VERSION:") {
                                kernel_version = v.trim().to_string();
                            } else if let Some(v) = line.strip_prefix("QUARK_TESTING_BUILD:") {
                                kernel_testing_build = v.trim() == "1";
                            }
                        }
                        if !out.status.success() {
                            let stderr_str = String::from_utf8_lossy(&out.stderr);
                            eprintln!("[QUARK-DAEMON] Kernel Registration stderr: {}", stderr_str);
                        }
                        out.status.success()
                    }
                    Err(e) => {
                        eprintln!("[QUARK-DAEMON] Failed to execute quark_cli: {:?}", e);
                        false
                    }
                };

                if !kernel_ok {
                    eprintln!(
                        "[QUARK-DAEMON] Refusing to confirm protection for PID {}: kernel module registration failed.",
                        pid
                    );
                }

                if kernel_testing_build {
                    println!(
                        "[QUARK-DAEMON] NOTE: kernel module for PID {} is a TESTING build (VM check bypassed).",
                        pid
                    );
                }

                // Tell the client whether Ring 0 protection is actually active (and
                // what build reported it), so quark_sdk_init() can refuse to let the
                // game run unprotected instead of assuming success just because the
                // daemon answered, and so games/servers can later refuse to trust a
                // testing build even when it reports "active".
                let mut ack = [0u8; 18];
                ack[0] = if kernel_ok { 1 } else { 0 };
                ack[1] = if kernel_testing_build { 1 } else { 0 };
                let version_bytes = kernel_version.as_bytes();
                let copy_len = version_bytes.len().min(16);
                ack[2..2 + copy_len].copy_from_slice(&version_bytes[..copy_len]);
                if let Err(e) = stream.write_all(&ack) {
                    eprintln!("[QUARK-DAEMON] Failed to send registration ack: {:?}", e);
                    return Err(e);
                }

                if !kernel_ok {
                    // Nothing left to supervise for this connection: the client's
                    // watchdog will see the socket close and refuse to continue.
                    break;
                }

                let mut lock = state.lock().unwrap();
                *lock = Some(QuarkState {
                    pid,
                    variables: HashMap::new(),
                    is_active: true,
                });

                // Spawn the monitoring thread (runs in user-space as redundancy / logging)
                let state_for_thread = Arc::clone(&state);
                thread::spawn(move || {
                    if let Err(e) = run_monitor_loop(state_for_thread) {
                        println!("[QUARK-MONITOR] Thread terminated with error: {:?}", e);
                    }
                });
            }
            CMD_REGISTER_VAR => {
                if payload.len() < 44 {
                    println!("[QUARK-DAEMON] Invalid payload size for REGISTER_VAR");
                    continue;
                }
                
                let address = u64::from_ne_bytes([
                    payload[0], payload[1], payload[2], payload[3],
                    payload[4], payload[5], payload[6], payload[7]
                ]);
                let size = u32::from_ne_bytes([payload[8], payload[9], payload[10], payload[11]]);
                
                let name_bytes = &payload[12..44];
                let name = String::from_utf8_lossy(name_bytes)
                    .trim_matches('\0')
                    .to_string();
                
                println!(
                    "[QUARK-DAEMON] Registering target variable: '{}' at Address: 0x{:X} (size: {} bytes)",
                    name, address, size
                );
                
                let mut lock = state.lock().unwrap();
                if let Some(ref mut s) = *lock {
                    // Try to read initial value
                    let mem_path = format!("/proc/{}/mem", s.pid);
                    let initial_value = match File::open(&mem_path) {
                        Ok(mut f) => {
                            if f.seek(SeekFrom::Start(address)).is_ok() {
                                let mut buf = vec![0u8; size as usize];
                                if f.read_exact(&mut buf).is_ok() {
                                    let val = bytes_to_u64(&buf, size);
                                    println!("[QUARK-DAEMON] Read initial value for '{}': {}", name, val);
                                    val
                                } else {
                                    eprintln!(
                                        "[QUARK-DAEMON] Warning: opened {} but failed to read {} bytes at 0x{:X} for '{}' -- falling back to expected_value=0.",
                                        mem_path, size, address, name
                                    );
                                    0
                                }
                            } else {
                                eprintln!(
                                    "[QUARK-DAEMON] Warning: opened {} but failed to seek to 0x{:X} for '{}' -- falling back to expected_value=0.",
                                    mem_path, address, name
                                );
                                0
                            }
                        }
                        Err(e) => {
                            // Same root cause as run_monitor_loop()'s EACCES case below if
                            // this is PermissionDenied: this daemon process itself couldn't
                            // read a PID it just registered as protected. Non-fatal here --
                            // expected_value just starts at 0 instead of the real value,
                            // which self-corrects on the game's next quark_sdk_update_var()
                            // call -- but worth logging since it silently masked a real bug
                            // (the kernel blocking even root, fixed in quark_kernel.c) before.
                            eprintln!(
                                "[QUARK-DAEMON] Warning: could not open {} to read '{}''s initial value ({:?}) -- falling back to expected_value=0.",
                                mem_path, name, e
                            );
                            0
                        }
                    };
                    
                    s.variables.insert(address, MonitoredVar {
                        address,
                        size,
                        name,
                        expected_value: initial_value,
                    });
                }
            }
            CMD_UPDATE_VAR => {
                if payload.len() < 16 {
                    println!("[QUARK-DAEMON] Invalid payload size for UPDATE_VAR");
                    continue;
                }
                let address = u64::from_ne_bytes([
                    payload[0], payload[1], payload[2], payload[3],
                    payload[4], payload[5], payload[6], payload[7]
                ]);
                let new_value = u64::from_ne_bytes([
                    payload[8], payload[9], payload[10], payload[11],
                    payload[12], payload[13], payload[14], payload[15]
                ]);
                
                let mut lock = state.lock().unwrap();
                if let Some(ref mut s) = *lock {
                    if let Some(var) = s.variables.get_mut(&address) {
                        var.expected_value = new_value;
                    }
                }
            }
            _ => {
                println!("[QUARK-DAEMON] Unknown command: {}", command);
            }
        }
    }
    
    // Cleanup: Unprotect the PID from the kernel module
    let mut lock = state_clone.lock().unwrap();
    if let Some(ref mut s) = *lock {
        s.is_active = false;
        let pid = s.pid;
        println!("[QUARK-DAEMON] Unregistering PID {} from Ring 0 Module...", pid);
        let _ = Command::new("sudo")
            .args(&["./quark_daemon/quark_cli", "2", &pid.to_string()])
            .status();
        println!("[QUARK-DAEMON] Monitoring session ended for PID {}.", pid);
    }
    
    Ok(())
}

fn run_monitor_loop(state: Arc<Mutex<Option<QuarkState>>>) -> std::io::Result<()> {
    thread::sleep(Duration::from_millis(100));
    
    let pid = {
        let lock = state.lock().unwrap();
        if let Some(ref s) = *lock {
            s.pid
        } else {
            return Ok(());
        }
    };
    
    let mem_path = format!("/proc/{}/mem", pid);
    println!("[QUARK-MONITOR] Thread started. Monitoring /proc/{}/mem", pid);
    
    let mut mem_file = match File::open(&mem_path) {
        Ok(f) => f,
        Err(e) => {
            if e.kind() == std::io::ErrorKind::PermissionDenied {
                eprintln!(
                    "[QUARK-MONITOR] Warning: Could not open {} (Permission denied). This is \
                     the daemon's own supplementary variable-tamper check failing to read a \
                     process it just registered as protected -- not the game being denied \
                     access to its own memory (that's the Ring-0 kretprobe working as \
                     intended, unrelated to this). Expected cause: this daemon isn't running \
                     as root (quark.service should set User=root; see kernel/quark_kernel.c's \
                     ptrace_may_access_ret_handler, which exempts root callers from the \
                     protected-PID block specifically so this thread can read a client it \
                     registered -- everyone else, root included previously, was blocked \
                     identically to an attacker). This thread exits; core Ring-0 protection \
                     for PID {} is unaffected either way.",
                    mem_path, pid
                );
            } else {
                println!("[QUARK-MONITOR] Warning: Could not open {}: {:?}", mem_path, e);
            }
            return Err(e);
        }
    };
    
    loop {
        thread::sleep(Duration::from_millis(50));
        
        let mut lock = state.lock().unwrap();
        let s = match *lock {
            Some(ref mut s) => s,
            None => break,
        };
        
        if !s.is_active {
            break;
        }
        
        for (&address, var) in s.variables.iter() {
            if mem_file.seek(SeekFrom::Start(address)).is_ok() {
                let mut buf = vec![0u8; var.size as usize];
                if mem_file.read_exact(&mut buf).is_ok() {
                    let actual_value = bytes_to_u64(&buf, var.size);
                    if actual_value != var.expected_value {
                        println!("\n==================================================");
                        println!("⚡⚡ [QUARK ALERT] TAMPERING DETECTED! ⚡⚡");
                        println!("Variable Name:   {}", var.name);
                        println!("Memory Address:  0x{:X}", var.address);
                        println!("Expected Value:  {}", var.expected_value);
                        println!("Actual Value:    {}", actual_value);
                        println!("==================================================");
                        
                        println!("[QUARK ACTION] Terminating target process {} immediately...", pid);
                        let _ = Command::new("kill").args(&["-9", &pid.to_string()]).status();
                        
                        s.is_active = false;
                        break;
                    }
                }
            }
        }
        
        if !s.is_active {
            break;
        }
    }
    
    println!("[QUARK-MONITOR] Thread exiting.");
    Ok(())
}

fn main() {
    let socket_path = "/tmp/quark.sock";

    let (listener, activated) = match systemd_activated_listener() {
        Some(l) => (l, true),
        None => {
            // Not socket-activated (no systemd, or launched directly for
            // testing) -- fall back to binding the socket ourselves, same as
            // before.
            if Path::new(socket_path).exists() {
                let _ = std::fs::remove_file(socket_path);
            }

            match UnixListener::bind(socket_path) {
                Ok(l) => (l, false),
                Err(e) => {
                    eprintln!("Failed to bind socket: {:?}", e);
                    std::process::exit(1);
                }
            }
        }
    };

    println!("==================================================");
    println!("🛡️🛡️  QUARK ANTICHEAT DAEMON ACTIVE  🛡️🛡️");
    if activated {
        println!("Listening on Unix socket: {} (systemd socket activation)", socket_path);
    } else {
        println!("Listening on Unix socket: {}", socket_path);
    }
    println!("==================================================");

    // Registers this process's own pid as the kernel module's trusted monitor (see
    // kernel/quark_kernel.c's ptrace_may_access_ret_handler / trusted_monitor_pid) --
    // without it, run_monitor_loop()'s /proc/<pid>/mem reads get blocked by the same
    // kretprobe as an attacker's would, the instant a pid becomes protected, since
    // the kernel has no other way to recognize this specific process as trusted.
    // Best-effort and non-fatal: if the kernel module isn't loaded yet (or this fails
    // for any other reason), the daemon keeps running -- CMD_REGISTER_GAME's own
    // ack/nack already makes "kernel not actually protecting anything" fail closed
    // for clients regardless; this only affects the supplementary monitor thread.
    let own_pid = std::process::id();
    match Command::new("sudo")
        .args(&["./quark_daemon/quark_cli", "3", &own_pid.to_string()])
        .output()
    {
        Ok(out) if out.status.success() => {
            println!("[QUARK-DAEMON] Registered pid {} as trusted monitor with the kernel module.", own_pid);
        }
        Ok(out) => {
            eprintln!(
                "[QUARK-DAEMON] Warning: failed to register as trusted monitor (kernel module not \
                 loaded yet?). The supplementary variable-tamper monitor thread will not be able \
                 to read protected processes' memory until this succeeds -- core Ring-0 protection \
                 is unaffected. quark_cli stderr: {}",
                String::from_utf8_lossy(&out.stderr)
            );
        }
        Err(e) => {
            eprintln!("[QUARK-DAEMON] Warning: failed to run quark_cli to register as trusted monitor: {:?}", e);
        }
    }

    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                if let Err(e) = handle_client(stream) {
                    eprintln!("Error handling client: {:?}", e);
                }
            }
            Err(e) => {
                eprintln!("Connection failed: {:?}", e);
            }
        }
    }
}
