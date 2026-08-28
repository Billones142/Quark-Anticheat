use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::os::fd::FromRawFd;
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

fn handle_client(mut stream: UnixStream) -> std::io::Result<()> {
    println!("[QUARK-DAEMON] Client connected.");
    
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
                let pid = i32::from_ne_bytes([payload[0], payload[1], payload[2], payload[3]]);
                println!("[QUARK-DAEMON] Registering game process with PID: {}", pid);
                
                // 1. Tell kernel module to PROTECT this PID
                println!("[QUARK-DAEMON] Registering PID {} to Ring 0 Module...", pid);
                let output = Command::new("sudo")
                    .args(&["./quark_daemon/quark_cli", "1", &pid.to_string()])
                    .output();
                match output {
                    Ok(out) => {
                        let stdout_str = String::from_utf8_lossy(&out.stdout);
                        let stderr_str = String::from_utf8_lossy(&out.stderr);
                        print!("[QUARK-DAEMON] Kernel Registration output: {}", stdout_str);
                        if !out.status.success() {
                            eprintln!("[QUARK-DAEMON] Kernel Registration stderr: {}", stderr_str);
                        }
                    }
                    Err(e) => {
                        eprintln!("[QUARK-DAEMON] Failed to execute quark_cli: {:?}", e);
                    }
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
                                    0
                                }
                            } else {
                                0
                            }
                        }
                        Err(_) => {
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
            println!("[QUARK-MONITOR] Warning: Could not open {}: {:?}", mem_path, e);
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
