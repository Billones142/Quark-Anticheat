# Quark Anticheat — Implemented Features & Detection Methods

A single reference for everything Quark actually does today, built directly from the
source (`kernel/quark_kernel.c` v0.4.0, `quark_daemon/`, `sdk/`) — not from the
project's aspirational architecture description in `docs/contexto_proyecto.md`, which
also describes some capabilities (e.g. LSM hooks, eBPF telemetry) that aren't
implemented yet. See "Not yet implemented" at the bottom for that gap.

## 1. Detection methods

### 1.1 Memory/ptrace tampering — Ring 0 (`kernel/quark_kernel.c`)

A `kretprobe` dynamically hooked on `ptrace_may_access` (chosen over an LSM hook or a
static hook because the required internals aren't exported symbols for an out-of-tree
module — see §2.3). On every call:

- If the **target** task is a protected PID (`is_pid_protected()`, tracked in
  `protected_pids[MAX_PROTECTED_PROCESSES=16]`) **and** the caller is neither the
  process itself nor the registered trusted monitor, the kernel overrides the return
  value to `0` (access denied) by patching `regs->ax` directly.
- This blocks both `ptrace()` attaches and reads of `/proc/<pid>/mem` — the two most
  common vectors for external cheats (memory scanners/editors, DLL-injection-style
  trainers, debuggers) on Linux.
- Comparison uses `task_tgid_vnr()` (process-level pid), not `task_pid_vnr()`
  (thread-level) — required so a protected process reading its own memory from a
  non-leader thread, or the daemon's monitor thread (a different tid from its own
  `getpid()`), still match correctly.
- Deliberately **not** a blanket root exemption: an earlier version exempted any root
  caller, which — since this project's own test scripts run both the daemon and the
  `cheat` attack tool as root over the same SSH session — let the attack tool through
  identically to the daemon. Fixed by a single named `trusted_monitor_pid` slot,
  registered explicitly and authenticated the same way as protect/unprotect (§2).

### 1.2 VM/hypervisor detection (`quark_running_in_vm()`)

CPUID "hypervisor present" bit (leaf 1, ECX bit 31), read from Ring 0 so it can't be
spoofed by anything running in userspace. Detects KVM, VMware, VirtualBox, Hyper-V,
Xen HVM, and any other hypervisor that sets the standard bit.

- **Release builds** (no `QUARK_TESTING_BUILD`): `quark_protect_pid()` refuses outright
  when a hypervisor is detected — there is no code path in a release `.ko` that ever
  calls `add_protected_pid()` after a positive VM check, regardless of what userspace
  sends it.
- **Testing builds** (`QUARK_TESTING_BUILD` defined at compile time): bypass the VM
  refusal so the project can be developed/tested inside the QEMU/KVM VM (see
  `docs/vm_test_environment.md`), but every protect response honestly self-reports
  `is_testing_build=1` — see §3 for how this is surfaced up to the game/server.

### 1.3 Application-level variable tampering — userspace (`quark_daemon`)

A per-connection monitor thread (`run_monitor_loop()`) polls `/proc/<pid>/mem` every
50ms for every variable the game registered via `quark_sdk_register_var()`
(health, currency, ammo, etc. — whatever the integrator chooses to watch):

- Compares the live value against an `expected_value` that only changes when the game
  itself calls `quark_sdk_update_var()` to declare a legitimate write.
- Any mismatch is logged as `⚡ [QUARK ALERT] TAMPERING DETECTED!` (variable name,
  address, expected vs. actual value) and the target process is killed immediately
  (`kill -9`) — fail-closed, no warning/grace period.
- This is explicitly supplementary/redundant to the Ring-0 block in §1.1 (a defense
  layer in userspace, in case something reaches the memory some other way), not the
  primary defense.

### 1.4 Daemon liveness (fail-closed watchdog) — SDK side (`sdk/`)

`quark_sdk_init()` starts a background watchdog thread that keeps checking the Unix
socket connection to the daemon is still alive. If the daemon disconnects for any
reason (killed, crashed, kernel module unloaded and daemon exits, etc.), the watchdog
terminates the game process immediately — the game is never allowed to keep running
under the assumption that "no news is good news."

## 2. Control-channel security (kernel ↔ daemon ↔ CLI)

Everything below exists to answer one question: *when the kernel module receives a
"protect PID X" or "unprotect PID X" command over Netlink, how does it know that
request is legitimate?* Four independent, layered checks — closing this was itself a
response to two real local-privilege-escalation-style holes found during development
(any unprivileged process could open its own Netlink socket and unprotect an arbitrary
PID; a client connecting to the world-writable daemon socket could claim someone else's
PID and get the daemon to unprotect it on the client's behalf).

### 2.1 Kernel-verified sender UID (`quark_check_sender()`)

Uses `NETLINK_CREDS(skb)` — the kernel-verified real uid/pid of whoever called
`sendto()` — **not** the self-reported `nlmsg_pid` header field, which has no
kernel-enforced binding to the real sender. Rejects any command whose sender uid isn't
root, since `quark_cli` only ever runs via `sudo`.

### 2.2 Exe-identity check

Using the sender's kernel-verified real pid, `filp_open("/proc/<pid>/exe", ...)`
resolves to the actual binary that sent the command; its content is SHA-256'd
(`quark_hash_file()`, capped at 1MB) and compared against `quark_cli_expected_sha256`,
a constant regenerated at every build from the actually-compiled `quark_daemon/quark_cli`
binary (top-level `Makefile`, `kernel/quark_cli_hash.h`, not committed). Rejects a
root process that isn't literally the real `quark_cli`.

(`find_task_by_vpid`, `get_task_exe_file`, and `get_mm_exe_file` were all tried first
and are not exported for out-of-tree modules on this kernel — `filp_open()` on the
`/proc/<pid>/exe` magic symlink sidesteps `task_struct`/`mm_struct` walking entirely.)

### 2.3 Signed, replay-protected commands (ECDSA P-256)

Commands 1 (protect), 2 (unprotect), and 3 (register trusted monitor) all carry:

- **`nonce`**: current time in nanoseconds. The kernel tracks `last_nonce` and rejects
  any nonce `<= last_nonce`, advancing it only after full signature verification
  succeeds (so garbage-signed messages can't be used to burn/waste nonces). Resets on
  module reload, same trust-boundary reset as `protected_pids` itself.
- **`sig`**: a DER-encoded ECDSA P-256 signature (`quark_cli.c`'s `quark_sign_request()`,
  OpenSSL EVP API) over SHA-256 of `{command, pid, nonce}`, verified in-kernel via
  `crypto_alloc_sig("x962(ecdsa-nist-p256)", ...)` / `crypto_sig_verify()` against a
  compiled-in public key (`kernel/quark_pubkey.h`, generated by
  `tooling/keys/generate-testing-key.sh`; private key never committed, lives at
  `/opt/quark-anticheat/keys/quark_signing_key.pem`, root-only-readable).
- Only commands 1/2/3 exist; **every** state-changing command goes through this same
  gate, including command 3 — an unauthenticated register-trusted-monitor would let any
  local process grant itself read access to every protected process's memory.

### 2.4 Trusted-monitor registration (command 3)

The daemon registers its own pid as `trusted_monitor_pid` once at startup (best-effort,
non-fatal if the kernel module isn't loaded yet). This is the *only* pid, besides a
protected process itself, exempted from the Ring-0 block in §1.1 — see the anti-root-
exemption note there for why this is a single authenticated slot rather than a broader
rule.

## 3. Protection-status attestation

Every protect response (`quark_protect_response`) reports:

- `ok` — whether protection actually took effect (not just whether the netlink send
  succeeded — e.g. it's `0` if a release build refused due to VM detection).
- `is_testing_build` — whether this `.ko` was compiled with the VM-detection bypass.
  A testing build must never be trusted as equivalent to real protection by anything
  that matters, such as a game server deciding whether to admit a client into a
  ranked/anticheat-required match.
- `version` — the kernel module's version string.

This flows daemon → SDK unmodified, exposed to the integrating game via
`quark_sdk_is_active()`, `quark_sdk_is_testing_build()`, and `quark_sdk_get_version()`.

## 4. Developer-facing SDK (`sdk/quark_sdk.h`)

| Function | Purpose |
|---|---|
| `quark_sdk_init()` | Connects to the daemon, confirms actual kernel-level protection is active (not just daemon reachability) before returning success. Starts the liveness watchdog (§1.4). Returns -1 if protection can't be confirmed — the caller should refuse to start rather than run unprotected. |
| `quark_sdk_register_var(addr, size, name)` | Opts a variable (health, currency, ammo, ...) into tamper monitoring (§1.3). |
| `quark_sdk_update_var(addr, new_value)` | Declares a legitimate write to a registered variable so it isn't flagged as tampering. |
| `quark_sdk_is_active()` | Live protection status (accurate without re-querying the daemon, since the watchdog kills the process the instant the connection drops). |
| `quark_sdk_is_testing_build()` | Whether the protecting kernel module is a VM-bypass testing build — see §3. |
| `quark_sdk_get_version()` | Kernel module version string. |
| `quark_sdk_close()` | Clean teardown. |

## 5. Not yet implemented

- **Server-side SDK**: no API yet for a game server to independently challenge/verify a
  connecting client's Quark status (would need its own nonce-challenge + signature
  check, separate from the local daemon protocol above).
- **End-to-end client↔server test**: current validation (`docs/vm_test_environment.md`)
  covers a single protected process against local attack tools; a real two-instance
  client/server game connection hasn't been tested yet.
- **LSM hooks / eBPF telemetry**: described as part of the target architecture in
  `docs/contexto_proyecto.md` (§3) but not present in `kernel/quark_kernel.c` today —
  all in-kernel interception currently goes through the single `ptrace_may_access`
  kretprobe described in §1.1, with no LSM module or eBPF program loaded.
- **Syscall/process monitoring beyond ptrace**: no hooks yet on process creation,
  module loading, or other syscalls sometimes used by cheats (e.g. `process_vm_readv`,
  which reaches the same memory as `/proc/<pid>/mem` but through a different syscall
  than the one currently hooked — worth checking whether it needs the same
  `ptrace_may_access`-style gate or a separate hook).
