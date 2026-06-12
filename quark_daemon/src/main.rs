use std::collections::HashMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::Path;
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

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
    
    // We will initialize the state when we receive CMD_REGISTER_GAME
    let state: Arc<Mutex<Option<QuarkState>>> = Arc::new(Mutex::new(None));
    let state_clone = Arc::clone(&state);
    
    let mut header_buf = [0u8; 8];
    
    loop {
        // Read packet header (command: u32, payload_len: u32)
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
                
                let mut lock = state.lock().unwrap();
                *lock = Some(QuarkState {
                    pid,
                    variables: HashMap::new(),
                    is_active: true,
                });
                
                // Spawn the monitoring thread
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
                
                // Extract 32 bytes for name
                let name_bytes = &payload[12..44];
                let name = String::from_utf8_lossy(name_bytes)
                    .trim_matches('\0')
                    .to_string();
                
                println!(
                    "[QUARK-DAEMON] Registering target variable: '{}' at Address: 0x{:X} (size: {} bytes)",
                    name, address, size
                );
                
                // Let's read the initial value from the game's memory
                let mut lock = state.lock().unwrap();
                if let Some(ref mut s) = *lock {
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
                                    println!("[QUARK-DAEMON] Failed to read memory for initial value");
                                    0
                                }
                            } else {
                                println!("[QUARK-DAEMON] Failed to seek to address 0x{:X}", address);
                                0
                            }
                        }
                        Err(e) => {
                            println!("[QUARK-DAEMON] Failed to open {}: {:?}", mem_path, e);
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
                        // Let's update the cache
                        var.expected_value = new_value;
                        // Optional debug print (uncomment if needed, but keeping it clean)
                        // println!("[QUARK-DAEMON] Expected value of '{}' updated to {}", var.name, new_value);
                    }
                }
            }
            _ => {
                println!("[QUARK-DAEMON] Unknown command: {}", command);
            }
        }
    }
    
    // Cleanup
    let mut lock = state_clone.lock().unwrap();
    if let Some(ref mut s) = *lock {
        s.is_active = false;
        println!("[QUARK-DAEMON] Monitoring session ended for PID {}.", s.pid);
    }
    
    Ok(())
}

fn run_monitor_loop(state: Arc<Mutex<Option<QuarkState>>>) -> std::io::Result<()> {
    // Wait a brief moment for registration of variables
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
            println!("[QUARK-MONITOR] Error: Could not open {}: {:?}", mem_path, e);
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
                        let kill_res = Command::new("kill")
                            .args(&["-9", &pid.to_string()])
                            .status();
                        match kill_res {
                            Ok(status) => println!("[QUARK ACTION] Process terminated. Kill status: {}", status),
                            Err(e) => println!("[QUARK ACTION] Failed to terminate process: {:?}", e),
                        }
                        
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
    
    if Path::new(socket_path).exists() {
        let _ = std::fs::remove_file(socket_path);
    }
    
    let listener = match UnixListener::bind(socket_path) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("Failed to bind socket: {:?}", e);
            std::process::exit(1);
        }
    };
    
    println!("==================================================");
    println!("🛡️🛡️  QUARK ANTICHEAT DAEMON ACTIVE  🛡️🛡️");
    println!("Listening on Unix socket: {}", socket_path);
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
