# Quark Anticheat — Log Message Reference

Every log message the four components can print, what it means, and where to find it
live. Built directly from the source (grep for `printf`/`println!`/`eprintln!`/`pr_info`/
`pr_warn`/`pr_err`/`perror` across `sdk/`, `quark_daemon/`, `kernel/`) — not from memory,
so line numbers below stay accurate as long as the surrounding code doesn't move.

Where to see each component's output live:
- **Kernel module** → `dmesg` (or `journalctl -k`).
- **Daemon** → `journalctl -u quark.service -f` if running under systemd (see
  `docs/vm_test_environment.md` §3.2), otherwise wherever its stdout/stderr was
  redirected (e.g. `/tmp/daemon.log` when started by hand).
- **SDK** → the *game's* own stdout/stderr (it's linked into the game process, not a
  separate process) — e.g. the patched engine's own log file
  (`~/.config/spring/infolog.txt` for the RecoilEngine integration).
- **`quark_cli`** → invoked internally by the daemon; its own stdout/stderr get folded
  into the daemon's log (`[QUARK-DAEMON] Kernel Registration output: ...`).

## Kernel module (`kernel/quark_kernel.c`)

All kernel-log messages, `pr_info`/`pr_warn`/`pr_err` (dmesg log levels 6/4/3
respectively).

| Message | Level | Meaning |
|---|---|---|
| `[QUARK-KERNEL] Initializing Quark Anticheat Kernel Module...` | info | Module load started (`quark_kernel_init`). |
| `[QUARK-KERNEL] Netlink socket created successfully.` | info | The daemon↔kernel Netlink channel (protocol 31) is up. |
| `[QUARK-KERNEL] Failed to create Netlink socket.` | error | Module load is aborting — Netlink socket creation failed (`-ENOMEM`). Module won't finish loading. |
| `[QUARK-KERNEL] Successfully registered kretprobe on ptrace_may_access.` | info | The actual protection mechanism is armed — `insmod` succeeded end-to-end. |
| `[QUARK-KERNEL] Failed to register kretprobe on ptrace_may_access: %d` | error | Module load is aborting — the kretprobe couldn't attach (symbol not found, kprobes disabled in the running kernel, etc). **No protection is active** if you see this instead of the line above. |
| `[QUARK-KERNEL] Process %d is now protected by Quark.` | info | A PID was added to the protected list (daemon sent Netlink command `1`, usually right after a game calls `quark_sdk_init()`). |
| `[QUARK-KERNEL] Process %d removed from Quark protection.` | info | A PID was removed (Netlink command `2` — daemon sends this when the game's socket connection closes). |
| `[QUARK-KERNEL] Protected PID table full, cannot protect PID %d` | warn | The 16-slot protected-PID table (`MAX_PROTECTED_PROCESSES`) is full; this PID was **not** protected. Needs an `[QUARK-KERNEL] Process ... removed` first, or a kernel-side capacity change. |
| `[QUARK-KERNEL ALERT] Blocked memory/ptrace access to protected process %d by PID %d!` | warn | **The actual detection event** — some other process just tried `ptrace()`/opened `/proc/<pid>/mem` on a protected process and was denied. This is the line to grep for (`dmesg \| grep QUARK-KERNEL`) to see real interception activity. |
| `[QUARK-KERNEL] Netlink payload too small` | error | A malformed Netlink message arrived (payload smaller than one `int`) — ignored. |
| `[QUARK-KERNEL] Unknown Netlink message type: %d` | warn | A Netlink message arrived with a command code that isn't `1` (protect) or `2` (unprotect) — ignored. |
| `[QUARK-KERNEL] Exiting Quark Anticheat Kernel Module...` | info | `rmmod` in progress — kretprobe and Netlink socket are being torn down. |

## Daemon (`quark_daemon/src/main.rs`)

Plain `println!`/`eprintln!` — everything goes to stdout except the two explicitly
marked stderr below. Under systemd both land in the journal regardless.

| Message | Meaning |
|---|---|
| `🛡️🛡️  QUARK ANTICHEAT DAEMON ACTIVE  🛡️🛡️` / `Listening on Unix socket: <path> [(systemd socket activation)]` | Daemon startup banner. The `(systemd socket activation)` suffix tells you whether it inherited the socket from `quark.socket` or bound it itself (see `docs/vm_test_environment.md` §3.2). |
| `Failed to bind socket: <err>` *(stderr)* | Fatal — daemon exits immediately. Almost always "address in use" (a stale `/tmp/quark.sock` from a previous run, or another daemon instance already running) when **not** using systemd socket activation. |
| `[QUARK-DAEMON] Client connected.` | A process opened a connection to the daemon's Unix socket (before it's necessarily sent anything). |
| `[QUARK-DAEMON] Client disconnected (EOF).` | The connected game's socket closed — normal on game exit; triggers the unprotect sequence below. |
| `[QUARK-DAEMON] Registering game process with PID: <pid>` | Received `CMD_REGISTER_GAME` — a game called `quark_sdk_init()` successfully. Kernel registration follows immediately. |
| `[QUARK-DAEMON] Registering PID <pid> to Ring 0 Module...` | About to shell out to `quark_cli` to send the Netlink "protect" command. |
| `[QUARK-DAEMON] Kernel Registration output: <text>` | Captured stdout from that `quark_cli` invocation (normally `[QUARK-CLI] Sending command 1 for PID <pid> to kernel...` + `Command sent successfully.`, see the CLI table below). |
| `[QUARK-DAEMON] Kernel Registration stderr: <text>` *(stderr)* | Captured stderr from `quark_cli` — only printed if that invocation's exit status was non-zero. A real problem: check whether `sudo` needed a password, or whether the kernel module isn't loaded (`quark_cli`'s own `sendto()` failure message would appear here). |
| `[QUARK-DAEMON] Failed to execute quark_cli: <err>` *(stderr)* | Couldn't even launch the `sudo ./quark_daemon/quark_cli ...` subprocess (e.g. wrong working directory — it's a relative path — or `sudo`/the binary missing). Kernel-side protection did **not** happen even though the daemon accepted the connection. |
| `[QUARK-DAEMON] Invalid payload size for REGISTER_GAME` / `..._VAR` / `..._UPDATE_VAR` | A malformed packet arrived on the wire protocol (payload shorter than the command requires) — that command is dropped, connection stays open. Usually means a version mismatch between the SDK and daemon's wire format. |
| `[QUARK-DAEMON] Registering target variable: '<name>' at Address: 0x<hex> (size: <n> bytes)` | Received `CMD_REGISTER_VAR` — only happens if the game actually calls `quark_sdk_register_var()` (not used by the current RecoilEngine integration, which is process-level-only). |
| `[QUARK-DAEMON] Read initial value for '<name>': <val>` | The daemon's own `/proc/<pid>/mem` read succeeded when registering that variable's baseline. If you *don't* see this line after a register, the daemon couldn't read the game's memory at that point (e.g. Yama `ptrace_scope`, or the kernel module already blocking even the daemon itself — see `docs/vm_test_environment.md` §7, this is expected and correct once Ring-0 protection is active). |
| `[QUARK-DAEMON] Unknown command: <n>` | A packet arrived with a command byte the daemon doesn't recognize — dropped. |
| `[QUARK-DAEMON] Unregistering PID <pid> from Ring 0 Module...` | Client disconnected; about to send the Netlink "unprotect" command. |
| `[QUARK-DAEMON] Monitoring session ended for PID <pid>.` | Cleanup for that connection is finished. |
| `[QUARK-MONITOR] Thread started. Monitoring /proc/<pid>/mem` | The userspace-fallback polling thread (checks registered variables every 50ms) started for this PID. Independent of the kernel module. |
| `[QUARK-MONITOR] Warning: Could not open /proc/<pid>/mem: <err>` | This thread couldn't open the target's memory — **expected and correct** once the Ring-0 kretprobe is active (it blocks the daemon too, not just attackers; see the process-level validation results in `docs/vm_test_environment.md` §7). Without the kernel module loaded, this instead means Yama `ptrace_scope` is blocking it. |
| `[QUARK-MONITOR] Thread terminated with error: <err>` | Follow-up to the line above — the thread is exiting because it couldn't do its job. |
| `\n==================================================` / `⚡⚡ [QUARK ALERT] TAMPERING DETECTED! ⚡⚡` / `Variable Name:` / `Memory Address:` / `Expected Value:` / `Actual Value:` / `==================================================` | **The tamper-detected alert block** — a registered variable's live value no longer matches what `quark_sdk_update_var()` last reported. Only reachable via the userspace monitor thread, so only fires when the kernel module *isn't* blocking that memory access in the first place (i.e. it's the fallback path, not the primary Ring-0 one). |
| `[QUARK ACTION] Terminating target process <pid> immediately...` | Immediately follows the alert block — the daemon is about to `kill -9` the game process. |
| `[QUARK-MONITOR] Thread exiting.` | Normal shutdown of the monitor thread (game disconnected, or it just killed the process above). |
| `Error handling client: <err>` *(stderr)* | An I/O error in `handle_client` outside the specific cases above (e.g. a read failed for a reason other than clean EOF). |
| `Connection failed: <err>` *(stderr)* | `listener.incoming()` itself returned an error accepting a new connection — rare. |

## SDK (`sdk/quark_sdk.c`) — printed by the game process, not the daemon

| Message | Meaning |
|---|---|
| `[QUARK-SDK] Successfully initialized and linked to Quark Daemon (PID: <pid>)` | `quark_sdk_init()` succeeded — connected, registered, and (as of the disconnect-watchdog addition) the background watchdog thread was started. This is the line to look for in the *game's* log to confirm it's protected. |
| `[QUARK-SDK] socket creation failed: <err>` *(via `perror`)* | `socket(AF_UNIX, ...)` itself failed — very rare (fd exhaustion, etc). `quark_sdk_init()` returns `-1`; game proceeds unprotected (fail-open by design). |
| `[QUARK-SDK] connection to daemon failed: <err>` *(via `perror`)* | Couldn't `connect()` to `/tmp/quark.sock` — most commonly **the daemon isn't running** ("No such file or directory") or, if it is running, **a socket-permission problem** ("Permission denied" — see the `chmod`/`SocketMode` discussion in `docs/vm_test_environment.md` §3.2). Game proceeds unprotected either way. |
| `[QUARK-SDK] Failed to send game registration` | Connected, but the `CMD_REGISTER_GAME` packet write failed. `quark_sdk_init()` returns `-1`. |
| `[QUARK-SDK] failed to start connection watchdog thread: <err>` *(via `perror`)* | `pthread_create` for the watchdog failed — non-fatal, `quark_sdk_init()` still returns `0` (connected/registered), but the "refuse to continue if the daemon disconnects" guarantee (`docs/vm_test_environment.md` §8) does **not** apply for this run. |
| `[QUARK-SDK] FATAL: lost connection to the Quark Anticheat daemon. Refusing to continue unsupervised.` | **The watchdog firing** — the daemon connection dropped after a successful init (daemon killed/crashed). The process calls `_exit(137)` immediately after printing this; if you see this line, the game is about to disappear within about a second (longer if the engine is heavily CPU-loaded at that moment — the watchdog thread has to get scheduled too). |
| `[QUARK-SDK] Failed to register variable '<name>'` | `quark_sdk_register_var()` failed to send its packet. Not used by the current RecoilEngine integration (process-level-only). |
| `[QUARK-SDK] Failed to send update for variable at <ptr>` | `quark_sdk_update_var()` failed to send its packet. Same — not currently used. |
| `[QUARK-SDK] Connection closed.` | `quark_sdk_close()` was called and actually had an open connection to close. The RecoilEngine integration never calls this (process exit closes the socket naturally), so you won't see it there. |

## `quark_cli` helper (`quark_daemon/quark_cli.c`)

Invoked by the daemon as `sudo ./quark_daemon/quark_cli <1|2> <pid>`; its stdout is
folded into the daemon's `Kernel Registration output:` line, stderr into `Kernel
Registration stderr:` (see the daemon table above).

| Message | Meaning |
|---|---|
| `Usage: <argv0> <1=protect/2=unprotect> <pid>` *(stderr)* | Called with the wrong number of arguments — shouldn't happen from the daemon itself, only if invoked by hand incorrectly. |
| `Invalid command. Use 1 for protect, 2 for unprotect.` *(stderr)* | First argument wasn't `1` or `2`. |
| `[QUARK-CLI] Error creating netlink socket: <err>` *(stderr, via `perror`)* | Couldn't open an `AF_NETLINK`/protocol-31 socket — usually a permissions issue (needs root/`CAP_NET_ADMIN`). |
| `[QUARK-CLI] Error binding netlink socket: <err>` *(stderr, via `perror`)* | The Netlink `bind()` failed. |
| `[QUARK-CLI] Out of memory` *(stderr)* | `malloc()` for the Netlink message buffer failed — essentially never happens. |
| `[QUARK-CLI] Sending command <1\|2> for PID <pid> to kernel...` | About to `sendto()` the Netlink message. |
| `[QUARK-CLI] Error sending netlink message. Is the kernel module 'quark_kernel' loaded? <err>` *(stderr, via `perror`)* | The `sendto()` itself failed — almost always because **`quark_kernel.ko` isn't loaded** (nothing is listening on Netlink protocol 31). Check `lsmod \| grep quark`. |
| `[QUARK-CLI] Command sent successfully.` | The Netlink message was accepted by the kernel (does **not** by itself confirm the kernel module processed it correctly — cross-check against the kernel's own `[QUARK-KERNEL] Process <pid> is now protected` line in `dmesg`). |
