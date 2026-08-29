#!/bin/bash
#
# capture.sh - Evidence capture harness for the Quark Anticheat comparative results.
#
# Runs the mock game (game_target/game) under three protection modes and retains every
# artifact needed to cite observed behaviour in a paper. Unlike run_test*.sh, which print
# a pass/fail verdict and discard their logs, this keeps all raw output on disk and emits
# one CSV row per repetition.
#
#   MODES
#     none  no daemon, no kernel module    -> attacker expected to succeed
#     A     daemon only                    -> write lands, daemon detects, SIGKILL
#     B     daemon + kernel module loaded  -> open() denied, write never lands
#
#   USAGE
#     ./capture.sh <none|A|B> [reps]        one mode
#     ./capture.sh all                      none x5, A x15, B x10 (default plan)
#
#   OUTPUT
#     captures/<timestamp>/environment.txt   methodology facts (kernel, distro, CPU, versions)
#     captures/<timestamp>/results.csv       one row per repetition
#     captures/<timestamp>/<mode>.<n>.*      raw daemon/game/cheat/dmesg output per rep
#     captures/<timestamp>/summary.txt       per-mode aggregate, latency stats for Mode A
#
# NO SOURCE CHANGES REQUIRED. Latency measured here is end-to-end (attacker's write
# returns -> game process reaped), which includes the daemon's poll delay, the alert
# print, the spawn of /bin/kill, signal delivery, and reaping. It cannot be decomposed
# into detection vs mitigation without timestamping cheat.c and main.rs.
#
# Run this in the test VM. It loads/unloads kernel modules and sends SIGKILL.

set -u

# ---------------------------------------------------------------- configuration

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO" || exit 1

GAME_BIN=./game_target/game
CHEAT_BIN=./cheat/cheat
DAEMON_BIN=./quark_daemon/target/release/quark_daemon
MODULE=kernel/quark_kernel.ko
MODULE_NAME=quark_kernel

CHEAT_VALUE=9999          # attacker's target value; game starts health at 100
GAME_SETTLE=1.2           # seconds to wait for the game to print its address
DAEMON_SETTLE=0.6         # seconds to wait for the daemon to bind its socket
REAP_TIMEOUT=5            # seconds to wait for the game to die before calling it alive
POST_ATTACK_PRINT=1.5     # seconds to wait for the game's 'p' output after the attack

RUN_DIR="captures/$(date +%Y%m%d-%H%M%S)"

# ---------------------------------------------------------------- preflight

die() { echo "ERROR: $*" >&2; exit 1; }

for b in "$GAME_BIN" "$CHEAT_BIN" "$DAEMON_BIN"; do
    [ -x "$b" ] || die "missing $b -- run 'make' first"
done

command -v bc >/dev/null || die "bc not installed (needed for latency arithmetic)"

MODE_ARG="${1:-}"
[ -n "$MODE_ARG" ] || die "usage: $0 <none|A|B|all> [reps]"

if [ "$MODE_ARG" = B ] || [ "$MODE_ARG" = all ]; then
    [ -f "$MODULE" ] || die "missing $MODULE -- run 'make -C kernel' first"
fi

# The daemon shells out to `sudo ./quark_daemon/quark_cli` with a RELATIVE path
# (quark_daemon/src/main.rs:117), so it must be started from the repo root -- which is
# where we are. It also needs a live sudo timestamp, which expires around 5 minutes and
# would otherwise die mid-run and silently downgrade Mode B to Mode A.
echo "Priming sudo (needed for insmod and for the daemon's quark_cli call)..."
sudo -v || die "sudo required"
( while true; do sudo -n true 2>/dev/null; sleep 45; done ) &
SUDO_KEEPALIVE=$!

cleanup() {
    kill "$SUDO_KEEPALIVE" 2>/dev/null
    pkill -f "$DAEMON_BIN" 2>/dev/null
    sudo rmmod "$MODULE_NAME" 2>/dev/null
    rm -f /tmp/quark.sock
}
trap cleanup EXIT INT TERM

mkdir -p "$RUN_DIR" || die "cannot create $RUN_DIR"

# ---------------------------------------------------------------- environment record

{
    echo "# Quark Anticheat capture run"
    echo "date_utc:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "host:            $(hostname)"
    echo
    echo "## Kernel and distro"
    echo "uname_r:         $(uname -r)"
    echo "uname_a:         $(uname -a)"
    echo "os_release:      $(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME")"
    echo "glibc:           $(ldd --version 2>/dev/null | head -1)"
    echo
    echo "## Relevant kernel config"
    for opt in CONFIG_KPROBES CONFIG_KRETPROBES CONFIG_BPF_LSM CONFIG_SECURITY_YAMA; do
        printf '%-24s %s\n' "$opt:" \
            "$(grep -m1 "^$opt=" "/boot/config-$(uname -r)" 2>/dev/null || echo '(not found)')"
    done
    echo "yama_ptrace_scope: $(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 'n/a')"
    echo
    echo "## CPU"
    echo "cpus:            $(nproc)"
    echo "governor:        $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'n/a')"
    echo "model:           $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "mem_total:       $(grep MemTotal /proc/meminfo | awk '{print $2, $3}')"
    echo
    echo "## Toolchain"
    echo "gcc:             $(gcc --version 2>/dev/null | head -1)"
    echo "cargo:           $(cargo --version 2>/dev/null)"
    echo "rustc:           $(rustc --version 2>/dev/null)"
    echo
    echo "## Repository"
    echo "git_commit:      $(git rev-parse HEAD 2>/dev/null)"
    echo "git_branch:      $(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
    echo "git_dirty:       $(git diff --quiet 2>/dev/null && echo no || echo YES)"
    echo
    echo "## Harness parameters"
    echo "cheat_value:     $CHEAT_VALUE"
    echo "reap_timeout_s:  $REAP_TIMEOUT"
    echo "poll_interval:   50ms (quark_daemon/src/main.rs:269, not set by this harness)"
} > "$RUN_DIR/environment.txt"

echo "Environment recorded in $RUN_DIR/environment.txt"

# ---------------------------------------------------------------- helpers

# Wait for a pid to exit, up to $2 seconds, polling every 1ms. `tail --pid` is used
# instead of a busy `kill -0` spin so the harness does not compete with the daemon for
# CPU while the latency measurement is in flight.
wait_for_exit() {
    timeout "$2" tail -s 0.001 --pid="$1" -f /dev/null 2>/dev/null
}

reset_state() {
    pkill -f "$DAEMON_BIN" 2>/dev/null
    sudo rmmod "$MODULE_NAME" 2>/dev/null
    rm -f /tmp/quark.sock
    sleep 0.3
    sudo dmesg -C >/dev/null 2>&1
}

# ---------------------------------------------------------------- one repetition

run_rep() {
    local mode=$1 rep=$2
    local tag="$RUN_DIR/$mode.$rep"

    reset_state

    if [ "$mode" = B ]; then
        sudo insmod "$MODULE" || { echo "insmod failed" >&2; return 1; }
    fi

    local daemon_pid=""
    if [ "$mode" != none ]; then
        "$DAEMON_BIN" > "$tag.daemon" 2>&1 &
        daemon_pid=$!
        sleep "$DAEMON_SETTLE"
        kill -0 "$daemon_pid" 2>/dev/null || { echo "daemon died" >&2; return 1; }
    fi

    # Feed the game a 'p' after the attack so its own post-attack state is on record,
    # then keep stdin open so it does not exit on EOF.
    ( sleep 3.5; echo p; while true; do sleep 1; done ) | "$GAME_BIN" > "$tag.game" 2>&1 &
    local game_pid=$!
    sleep "$GAME_SETTLE"

    local addr
    addr=$(grep -oP 'Health address:\s+\K0x[0-9a-fA-F]+' "$tag.game" | head -1)
    if [ -z "$addr" ]; then
        echo "could not extract health address (rep $rep)" >&2
        kill "$game_pid" 2>/dev/null
        return 1
    fi

    # The daemon registers the game's own getpid() and the kernel protects that PID, so
    # the attacker must target the same one. For `( ... ) | ./game &` bash sets $! to the
    # game itself, but assert it rather than assume -- a stray wrapper process in the
    # pipeline would otherwise silently point the attack at the wrong target.
    local self_pid
    self_pid=$(grep -oP 'PID:\s+\K[0-9]+' "$tag.game" | head -1)
    if [ -n "$self_pid" ] && [ "$self_pid" != "$game_pid" ]; then
        echo "PID mismatch: job=$game_pid game reports $self_pid -- skipping rep $rep" >&2
        kill "$game_pid" 2>/dev/null
        return 1
    fi

    # Randomise where the attack lands inside the daemon's 50ms poll cycle. Without this
    # every repetition fires at the same phase and the latency distribution collapses to
    # a spuriously tight cluster.
    sleep "0.0$(printf '%02d' $((RANDOM % 100)))"

    local t0 t1 cheat_exit alive latency
    t0=$(date +%s%N)
    "$CHEAT_BIN" "$game_pid" "$addr" "$CHEAT_VALUE" > "$tag.cheat" 2>&1
    cheat_exit=$?

    wait_for_exit "$game_pid" "$REAP_TIMEOUT"
    t1=$(date +%s%N)

    if kill -0 "$game_pid" 2>/dev/null; then
        alive=1
        latency=NA
    else
        alive=0
        latency=$(echo "scale=3; ($t1 - $t0) / 1000000" | bc)
    fi

    # Give the surviving game a moment to print its post-attack state.
    [ "$alive" = 1 ] && sleep "$POST_ATTACK_PRINT"

    sudo dmesg > "$tag.dmesg" 2>/dev/null

    # ---- derive the observable facts ----

    # Last health value the game itself printed. The startup banner prints the initial
    # 100, so take the final occurrence, which is the post-attack 'p' output.
    local health
    health=$(grep -oP 'Health address:\s+\S+\s+->\s+Value:\s+\K[0-9]+' "$tag.game" | tail -1)
    health=${health:-NA}

    # Did the write land? Two independent witnesses, either is sufficient:
    #   - the game printed the attacker's value itself
    #   - the daemon read the attacker's value out of the game's memory (Mode A alert)
    local landed=0
    [ "$health" = "$CHEAT_VALUE" ] && landed=1
    grep -qE "Actual Value: +$CHEAT_VALUE" "$tag.daemon" 2>/dev/null && landed=1

    local detected=0
    grep -q 'TAMPERING DETECTED' "$tag.daemon" 2>/dev/null && detected=1

    local denied=0
    grep -q 'Permission denied' "$tag.cheat" 2>/dev/null && denied=1

    # Kernel actually registered the PID. quark_cli reports success as soon as sendto()
    # returns and never reads a reply (quark_daemon/quark_cli.c:70), so its output proves
    # nothing -- only the module's own pr_info does.
    local protected=0
    grep -q "Process $game_pid is now protected by Quark" "$tag.dmesg" 2>/dev/null && protected=1

    local kblocks
    kblocks=$(grep -c 'QUARK-KERNEL ALERT' "$tag.dmesg" 2>/dev/null || echo 0)

    # The mutual-exclusion witness: the daemon's own monitor thread denied by the module.
    local monitor_blocked=0
    grep -q 'Could not open /proc' "$tag.daemon" 2>/dev/null && monitor_blocked=1

    # Validity. A Mode B rep whose PID was never registered has silently degraded to
    # Mode A and must not be averaged in.
    local valid=1
    [ "$mode" = B ] && [ "$protected" = 0 ] && valid=0

    echo "$rep,$mode,$cheat_exit,$denied,$protected,$landed,$detected,$alive,$monitor_blocked,$kblocks,$health,$latency,$valid" \
        >> "$RUN_DIR/results.csv"

    printf '  rep %-3s exit=%s denied=%s landed=%s detected=%s alive=%s monitor_blocked=%s health=%s latency=%s%s\n' \
        "$rep" "$cheat_exit" "$denied" "$landed" "$detected" "$alive" \
        "$monitor_blocked" "$health" "$latency" \
        "$([ "$valid" = 0 ] && echo '  <-- INVALID: PID never protected')"

    kill "$game_pid" 2>/dev/null
    [ -n "$daemon_pid" ] && kill "$daemon_pid" 2>/dev/null
    sudo rmmod "$MODULE_NAME" 2>/dev/null
    rm -f /tmp/quark.sock
    return 0
}

# ---------------------------------------------------------------- negative control

# Same process, same address, same attacker -- only the module differs. This is the
# strongest single piece of evidence that a Mode B denial was Quark's doing and not an
# unrelated OS restriction such as Yama ptrace_scope.
run_negative_control() {
    local tag="$RUN_DIR/negctl"
    echo
    echo "Negative control: identical attack on one live PID, module loaded then unloaded"

    reset_state
    sudo insmod "$MODULE" || return 1
    "$DAEMON_BIN" > "$tag.daemon" 2>&1 &
    local dp=$!
    sleep "$DAEMON_SETTLE"
    ( sleep 30 ) | "$GAME_BIN" > "$tag.game" 2>&1 &
    local gp=$!
    sleep "$GAME_SETTLE"

    local addr
    addr=$(grep -oP 'Health address:\s+\K0x[0-9a-fA-F]+' "$tag.game" | head -1)
    [ -z "$addr" ] && { kill "$gp" "$dp" 2>/dev/null; return 1; }

    echo "  target pid=$gp addr=$addr"
    "$CHEAT_BIN" "$gp" "$addr" "$CHEAT_VALUE" > "$tag.with_module.cheat" 2>&1
    echo "  with module:    exit=$? $(head -c 200 "$tag.with_module.cheat" | tr '\n' ' ')"

    sudo rmmod "$MODULE_NAME"
    sleep 0.2

    "$CHEAT_BIN" "$gp" "$addr" "$CHEAT_VALUE" > "$tag.without_module.cheat" 2>&1
    echo "  without module: exit=$? $(head -c 200 "$tag.without_module.cheat" | tr '\n' ' ')"

    sudo dmesg > "$tag.dmesg" 2>/dev/null
    kill "$gp" "$dp" 2>/dev/null
    rm -f /tmp/quark.sock
}

# ---------------------------------------------------------------- watchdog probe

# Kills the daemon out from under a connected game and records whether the SDK watchdog
# terminates the game. Exit status 137 is what _exit(137) at sdk/quark_sdk.c:95 produces.
run_watchdog_probe() {
    local tag="$RUN_DIR/watchdog"
    echo
    echo "Watchdog probe: kill the daemon under a connected game"

    reset_state
    "$DAEMON_BIN" > "$tag.daemon" 2>&1 &
    local dp=$!
    sleep "$DAEMON_SETTLE"
    ( sleep 60 ) | "$GAME_BIN" > "$tag.game" 2>&1 &
    local gp=$!
    sleep "$GAME_SETTLE"

    if ! grep -q 'Monitored variables registered' "$tag.game"; then
        echo "  game never registered with the daemon -- probe inconclusive"
        kill "$gp" "$dp" 2>/dev/null; return 1
    fi

    local t0 t1
    t0=$(date +%s%N)
    kill -9 "$dp" 2>/dev/null
    wait_for_exit "$gp" 15
    t1=$(date +%s%N)

    if kill -0 "$gp" 2>/dev/null; then
        echo "  game STILL RUNNING after 15s -- watchdog did not fire"
        kill "$gp" 2>/dev/null
    else
        wait "$gp" 2>/dev/null
        local st=$?
        echo "  game exited after $(echo "scale=3; ($t1-$t0)/1000000000" | bc)s, status=$st (137 = watchdog _exit)"
        grep -q 'FATAL: lost connection' "$tag.game" \
            && echo "  fatal message present in game log" \
            || echo "  fatal message NOT in game log"
    fi
    rm -f /tmp/quark.sock
}

# ---------------------------------------------------------------- summary

write_summary() {
    {
        echo "Quark Anticheat capture summary"
        echo "run: $RUN_DIR"
        echo
        awk -F, 'NR>1 && $13==1 {
            n[$2]++
            landed[$2]+=$6; detected[$2]+=$7; alive[$2]+=$8
            denied[$2]+=$4; mon[$2]+=$9
        } END {
            printf "%-6s %5s %8s %9s %8s %8s %10s\n", \
                   "mode","reps","landed","detected","denied","alive","mon_block"
            for (m in n)
                printf "%-6s %5d %8d %9d %8d %8d %10d\n", \
                       m, n[m], landed[m], detected[m], denied[m], alive[m], mon[m]
        }' "$RUN_DIR/results.csv"

        echo
        echo "Mode A end-to-end latency, ms (write returns -> game reaped)"
        awk -F, 'NR>1 && $2=="A" && $12!="NA" && $13==1 {v[n++]=$12+0; s+=$12}
        END {
            if (n==0) { print "  no valid samples"; exit }
            for(i=0;i<n;i++) for(j=i+1;j<n;j++) if(v[j]<v[i]){t=v[i];v[i]=v[j];v[j]=t}
            printf "  n=%d  mean=%.1f  median=%.1f  min=%.1f  max=%.1f\n", \
                   n, s/n, v[int(n/2)], v[0], v[n-1]
            printf "  all: "; for(i=0;i<n;i++) printf "%.1f ", v[i]; print ""
        }' "$RUN_DIR/results.csv"

        echo
        echo "Includes the daemon's poll wait, alert print, /bin/kill spawn, signal"
        echo "delivery and reaping. Not decomposable without instrumenting cheat.c and"
        echo "quark_daemon/src/main.rs. Poll interval is 50ms (main.rs:269)."

        local invalid
        invalid=$(awk -F, 'NR>1 && $13==0' "$RUN_DIR/results.csv" | wc -l)
        [ "$invalid" -gt 0 ] && {
            echo
            echo "WARNING: $invalid repetition(s) excluded -- kernel never confirmed the PID"
            echo "as protected. Usually an expired sudo timestamp. Re-run those."
        }
    } > "$RUN_DIR/summary.txt"

    cat "$RUN_DIR/summary.txt"
}

# ---------------------------------------------------------------- main

echo "rep,mode,cheat_exit,denied,protected,landed,detected,game_alive,monitor_blocked,kernel_blocks,health,latency_ms,valid" \
    > "$RUN_DIR/results.csv"

run_mode() {
    local mode=$1 reps=$2
    echo
    echo "=== Mode $mode, $reps repetition(s) ==="
    for i in $(seq 1 "$reps"); do
        run_rep "$mode" "$i" || echo "  rep $i FAILED to run"
    done
}

case "$MODE_ARG" in
    none|A|B) run_mode "$MODE_ARG" "${2:-10}" ;;
    all)
        run_mode none 5
        run_mode A 15
        run_mode B 10
        run_negative_control
        run_watchdog_probe
        ;;
    *) die "unknown mode '$MODE_ARG' (expected none, A, B, or all)" ;;
esac

echo
write_summary
echo
echo "Artifacts in $RUN_DIR/"
