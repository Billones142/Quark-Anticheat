# Quark Anticheat × Beyond All Reason — VM Test Environment

This document records how the local QEMU/KVM test VM was set up and used to validate
Quark Anticheat against a real build of the RecoilEngine (the engine Beyond All Reason
runs on), and how to reproduce or repeat the test.

## 1. Environment

- **VM**: local QEMU/KVM, reachable at `192.168.122.203` (root SSH).
- **OS**: Nobara Linux 43 (KDE Plasma), kernel `7.0.9-200.nobara.fc43.x86_64` — this is
  the project's own stated reference distro (see `docs/contexto_proyecto.md`).
- **Disk**: 23G root partition (`/dev/vda2`) — tight for a full C++23 engine build; see
  §4 for the space issues this caused and how they were resolved.
- **User accounts**: `quark` (regular desktop user, owns `~/Quark-Anticheat` and
  `~/RecoilEngine`, has the KDE session) and `root` (used for kernel module load/unload
  and daemon; SSH key access already configured before this session started).
- **Host machine** (this machine, Arch Linux) was used only to explore the RecoilEngine
  source via `git`/`gh api` and, later, to cross-build the AI plugins in a Fedora 44
  Docker container (§6) — no package installation or building happened directly on the
  bare host OS itself for anything destined to run on the VM, only in disposable
  containers or the VM itself.

## 2. What was built, and why

### 2.1 Quark Anticheat components (unmodified)
Built directly from the `Dev` branch of this repo, already present on the VM at
`/home/quark/Quark-Anticheat` (its stale `main`-branch clone was switched to `Dev` and
fast-forwarded first — `git fetch origin && git checkout Dev && git pull --ff-only`):

- `kernel/quark_kernel.ko` — `make -C kernel` (needs `kernel-devel`/`kernel-headers`
  matching the running kernel; both were already installed on the VM).
- `quark_daemon/target/release/quark_daemon` — `cargo build --release`.
- `quark_daemon/quark_cli`, `cheat/cheat` — `gcc -Wall -Wextra -O2 ...` (see root
  `Makefile`).

### 2.2 RecoilEngine, patched
Cloned fresh from `https://github.com/beyond-all-reason/RecoilEngine.git` (with
`--recurse-submodules`) into `~/RecoilEngine` on the VM. Patched with a **3-file, ~10-line
change** (see the approved plan for exact diffs) to link the Quark SDK into the engine
and call `quark_sdk_init()` once at startup:

- `rts/System/QuarkAnticheat/quark_sdk.h` / `.c` — verbatim copy of `sdk/quark_sdk.{h,c}`
  from this repo, placed inside the engine tree so it builds as part of it.
- `rts/System/CMakeLists.txt` — added `quark_sdk.c` to `sources_engine_System_common`
  (the source list shared by `engine-legacy`, `engine-headless`, and `engine-dedicated`).
- `rts/System/Main.cpp` — `#include "System/QuarkAnticheat/quark_sdk.h"` and a
  `quark_sdk_init()` call in `Run()`, both gated `#ifdef __linux__`. Fails open (logs and
  continues) if the daemon isn't running, matching the SDK's existing behavior.

Built with:
```
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAI_TYPES=NONE -DBUILD_TESTING=OFF
cmake --build . --target engine-legacy -j 5
cmake --build . --target basecontent -j 5   # cursors.sdz, maphelper.sdz, bitmaps.sdz, springcontent.sdz
```
`AI_TYPES=NONE` and skipping `test`/full `install` were deliberate: the goal was the
smallest build that produces a working `engine-legacy` binary (`build/spring`), not a
full distribution build. AI support was added later, in a separate build (§6), once the
project's own scope grew to include a live AI-vs-AI skirmish.

### 2.3 Real Beyond All Reason game + map content
The bare RecoilEngine has no game rules or units — "Beyond All Reason" is a separate
content package fetched via the same `pr-downloader` tool BAR's own lobby uses. Built the
standalone CLI (`cmake --build . --target pr-downloader_cli`, output at
`build/tools/pr-downloader/src/pr-downloader`) and fetched:

- **Game**: `bar:git:8448c1d0faf89f3b883d65f99be36954c2555042` — "Beyond all Reason
  test-11420" — via `--download-game`. **Careful**: the generic `bar:test` rapid tag on
  `repos.springrts.com` actually resolves to a *different* game ("Balanced Annihilation
  Reloaded", an unrelated mod that happens to share the `bar` repo namespace) — use a
  specific `bar:git:<hash>` tag for real Beyond All Reason. The correct hash was found via
  `curl -s https://repos.springrts.com/bar/versions.gz | gunzip | grep 'Beyond all Reason'`.
- **Map**: "Techno Lands Final 26.1" (1.4MB, small) via `--download-map`, found via the
  springfiles JSON API (`curl -s 'https://springfiles.springrts.com/json.php?category=map'`).

Both were written with `--filesystem-writepath /home/quark/RecoilEngine/build` (must run
as the `quark` user, not root, or content lands in `/root/.spring` instead). The engine's
archive scanner picks up `build/maps/`, `build/packages/`, `build/base/` and
`cont/{base,fonts}` automatically once `SPRING_DATADIR` includes both `build/` and
`cont/` (see §5, the font-loading fix).

## 3. Kernel module + daemon — running state

### 3.1 Kernel module
Still a manual step — it isn't DKMS-packaged (a known, documented limitation), so it
doesn't survive a reboot:
```bash
# as root
insmod /home/quark/Quark-Anticheat/kernel/quark_kernel.ko
```
Check with `lsmod | grep quark` before assuming it's loaded.

### 3.2 Daemon — socket-activated (systemd), not a manually-started process
Originally the daemon was just started by hand (`nohup ... &`) — but that has real
problems: it doesn't survive a reboot either, it was observed dying mid-session with no
VM reboot involved (most likely reaped by systemd-logind's session cleanup when the SSH
session that launched it ended — `setsid nohup ... & disown` avoided that but is still a
manual step to remember), and its Unix socket defaulted to root-only permissions
(`chmod 777 /tmp/quark.sock` was needed by hand every time, or the unprivileged `quark`
user's game process couldn't even connect).

Fixed properly with **systemd socket activation** — the same mechanism Docker uses for
`docker.socket` → `dockerd`: systemd owns and pre-creates `/tmp/quark.sock` (with the
right permissions declared up front, `SocketMode=0666`), and only starts
`quark_daemon` the first time something actually connects to it. It then keeps running
(same as `dockerd` does) rather than stopping again after that connection closes.

This required one small addition to `quark_daemon/src/main.rs`
(`systemd_activated_listener()`): on startup, check `LISTEN_PID`/`LISTEN_FDS` (the
`sd_listen_fds(3)` protocol) and, if set and matching our own pid, wrap the pre-opened fd
`3` as a `UnixListener` instead of binding a new socket ourselves. Falls back to the old
self-bind behavior if those env vars aren't set (e.g. running it by hand for a quick
test, same as before). No new dependency needed — the whole thing is ~25 lines against
plain `std`.

Unit files: `quark_daemon/systemd/quark.socket` + `quark_daemon/systemd/quark.service`
in this repo (paths in `quark.service` are placeholders — the VM's deployed copies under
`/etc/systemd/system/` point at `/home/quark/Quark-Anticheat`). Installed with:
```bash
# as root, after `cargo build --release --manifest-path quark_daemon/Cargo.toml`
cp quark_daemon/systemd/quark.socket quark_daemon/systemd/quark.service /etc/systemd/system/
# then edit ExecStart=/WorkingDirectory= in quark.service to match where this repo lives
systemctl daemon-reload
systemctl enable --now quark.socket   # NOT quark.service directly -- the socket unit
                                       # is what should be enabled; it starts the
                                       # service on demand
```
Verify with `systemctl status quark.socket` (should show `active (listening)`) and
`systemctl status quark.service` (should show `inactive (dead)` until something actually
connects — confirmed by launching the patched engine and watching it flip to
`active (running)`). Daemon logs now go to the journal
(`journalctl -u quark.service`) instead of a manually-redirected file.

The kernel module still needs the manual `insmod` step above — systemd starting the
daemon doesn't imply the kernel module is loaded, and the daemon has no way to load it
itself (that would need root capabilities it doesn't currently request, and is out of
scope here).

## 4. Problems hit on the VM and how they were resolved

The VM's package repos (`nobara`, a rolling release) were mid-transition from Fedora 43
to Fedora 44 packages when this session started — **not caused by this work**, but it
directly blocked several build steps, so fixing it became part of the process:

1. **`cmake` install failed GPG verification** — the `nobara` repo served an
   `fc44`-tagged `cmake` package, but the local repo config only trusted the `fc41`–`fc43`
   signing keys (the `fc44` key file already existed locally, just wasn't referenced).
   Fixed by adding `file:///etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-44-primary` to
   `/etc/yum.repos.d/nobara.repo`'s `gpgkey=` list — trusting an already-vendored key, not
   disabling verification.
2. **`boost-devel` / `libcurl-devel` installs pulled a nearly-full-system upgrade** (up to
   ~2988 packages, ~5GB download) — because the repo mixing meant almost any dependency
   chain touched packages only available at `fc44`. Confirmed with the user before running
   anything this size. Root cause of #4 below.
3. **`mesa-libGL-devel` pulled in a broken 32-bit multilib conflict** from a third repo
   (`nobara-pikaos-additional` vs `nobara-updates` shipping different `mesa-filesystem`
   i686 versions) — worked around by installing `libglvnd-devel` instead (same headers,
   no i686 pull-in).
4. **After the `libcurl-devel` fix, the KDE greeter (`plasmalogin`/`kwin_wayland`)
   crashed** — VM screen went black. Root cause:
   `kwin_wayland: symbol lookup error: /lib64/libkwin.so.6: undefined symbol:
   _ZN12ScreenLocker7KSldApp14inhibitSuspendEv`. `kwin` had been pulled to `fc44` by the
   `libcurl-devel` transaction's dependency resolution, but `kscreenlocker` (which
   provides that symbol) had not — leftover version skew from a *partial* sync. Fixed by
   `dnf upgrade -y kscreenlocker`, then (once it became clear dozens of other
   `kf6-*`/`plasma-*`/`mesa-*` packages were still on `fc43`) a scoped
   `dnf upgrade -y 'kf6-*' 'plasma-*' 'kde-*' 'kwin*' 'kdecoration*' 'mesa-*' 'qt6-*'
   'libwayland-*' 'xorg-x11-server-Xwayland' 'polkit-kde*' 'kscreenlocker' 'libglvnd*'`
   (127 packages, ~620MB) rather than a blanket `dnf upgrade -y` on the first attempt, to
   keep disk/time cost down and stay scoped to what actually mattered. `systemctl restart
   plasmalogin.service` afterward brought the greeter back.
5. **Disk filled up mid-upgrade twice** (`/var/cache/libdnf5` reached 5.6G from repeated
   failed-transaction downloads) — `dnf clean packages` recovered it each time; also
   removed the unused old kernel (`7.0.0`, superseded by the running `7.0.9`) for ~430MB.
   A later `dnf upgrade -y` (full sync, explicitly approved by the user after the
   classifier blocked an unscoped attempt) needed ~3GB net / much more at peak, which the
   VM's 23G disk could not sustain until the cache was cleared first.
6. **7-Zip couldn't create the `.sdz` content archives** (`Codec Load Error: /bin/7z.so :
   errno=25`, "Unsupported archive type") — the `7zip` RPM installs its zip codec at
   `/usr/libexec/7zip/7z.so`, but the `7z` binary looks for `7z.so` next to itself. Fixed
   with `ln -sf /usr/libexec/7zip/7z.so /usr/bin/7z.so` (packaging bug in that RPM, not
   anything we changed).
7. **Engine crashed on startup**: `Failed to load FontFile "fonts/FreeSansBold.otf", did
   you forget to run make install?` — expected, since only `engine-legacy` +
   `basecontent` were built, not a full `make install`. The engine's `CMakeLists.txt`
   itself documents the intended workaround (`cd build-dir; SPRING_DATADIR=$(pwd)
   ./spring`), but that alone isn't enough because loose source assets like fonts live
   under `cont/`, not `build/`. Fixed by setting
   `SPRING_DATADIR=/home/quark/RecoilEngine/build:/home/quark/RecoilEngine/cont` (colon-
   separated, confirmed against `DataDirLocater::AddDirs`/`SplitColonString` in the
   engine source) in the desktop shortcut's `Exec=` line.

None of these were caused by the Quark SDK patch itself — all were either pre-existing
VM/repo state or generic RecoilEngine build/run prerequisites.

## 5. Desktop shortcut

`~/Escritorio/BeyondAllReason-Quark.desktop` (KDE may show a one-time "trust this
executable" prompt on first launch — normal Plasma behavior):

```ini
[Desktop Entry]
Type=Application
Name=BAR (Quark Protected)
Comment=Beyond All Reason engine (RecoilEngine), patched with the Quark Anticheat SDK
Exec=env SPRING_DATADIR=/home/quark/RecoilEngine/build:/home/quark/RecoilEngine/cont /home/quark/RecoilEngine/build/spring
Path=/home/quark/RecoilEngine/build
Icon=/home/quark/RecoilEngine/rts/RecoilEngine_Icon.svg
Terminal=false
Categories=Game;
```

This launches the patched client only — it does **not** start the daemon or load the
kernel module (those need root; see §3), by design, so the shortcut stays a simple,
double-click "run the protected game" action for the desktop user.

## 6. AI-vs-AI skirmish setup

To have a live, continuously-running game (real economy, real combat, real wreckage) to
test Quark against, rather than an idle empty lobby, two AI-controlled sides were added
using BAR's own production skirmish AI ("BARb"), not a custom scripted behavior — see
rationale below.

### 6.1 Why BARb as-is, not a custom "rush to center" script
The original ask was for AI players that "only send troops to the center of the map...
and give constructor planes so they can salvage the scraps." BARb has no such literal
mode (checked `AI/Skirmish/BARb/data/AIOptions.lua` — only a `profile` difficulty list
and a few performance toggles). Implementing that literally would mean writing a new,
untested BAR-specific Lua gadget (hooking unit creation, issuing periodic `Fight` orders
toward map-center coordinates, scripting constructor aircraft to seek and reclaim
wreckage) — real development work with real risk of bugs, in a codebase not otherwise
touched this session. Given the user's actual goal — a genuinely live, churning game to
validate Quark against — **the user chose to use BARb as-is**: real production AI, will
fight, build, and naturally reclaim wrecks as part of normal play; on a small map (Techno
Lands, already downloaded) the two sides converge and clash without any extra scripting.

### 6.2 Building the AI plugins — cross-built in Docker, not on the VM
BARb (and its required `C-AIInterface` plugin) are native, compiled artifacts — the
engine `dlopen()`s them at runtime — and were **not** included in the original
`engine-legacy` build (`-DAI_TYPES=NONE` was used deliberately in §2.2 to keep that build
minimal). Per the user's instruction to avoid spending the VM's limited CPU/RAM (5 cores,
8GB RAM) on compilation, these were cross-built on the host machine instead, inside a
Fedora 44 Docker container (**not** built bare on the host's own Arch Linux — an
Arch-linked `.so` would not run on the VM's older glibc; confirmed the container's glibc
(`2.43`) exactly matches the VM's before proceeding).

This is now a small, reusable setup rather than ad-hoc `docker run`/`exec` commands —
see `tooling/recoil-fedora-build/` in this repo:

- `Dockerfile` — Fedora 44 + every dependency the engine's native targets need
  (mirrors the package list installed on the VM directly, §4).
- `docker-compose.yml` — bind-mounts a RecoilEngine checkout into the container at
  `/src` (path controlled by the `RECOIL_ENGINE_DIR` env var, default:
  `../../../RecoilEngine`, i.e. a sibling of this repo).
- `build.sh` — configures (once; `--reconfigure` to force) and builds whichever CMake
  targets you name, defaulting to `BARb C-AIInterface`.

```bash
cd tooling/recoil-fedora-build
./build.sh                                        # builds BARb + C-AIInterface (default)
./build.sh engine-legacy                          # or build the engine itself, e.g. for a
                                                   # from-scratch redo of the whole VM setup
./build.sh --reconfigure -DAI_TYPES=NATIVE -- BARb CircuitAI C-AIInterface
```

Output lands in `$RECOIL_ENGINE_DIR/build-fedora/` on the **host**, via the bind mount —
no `docker cp` needed, and the VM never compiles anything, only runs the already-built
`.so` files.

### 6.3 Installing the built AI plugins
Spring/Recoil AI plugins load from a fixed, versioned layout under any configured data
dir — resolved from the engine's own `cmake_install.cmake` rather than guessed:

- Skirmish AI: `<datadir>/AI/Skirmish/<shortName>/<version>/libSkirmishAI.so` +
  `AIInfo.lua` + `AIOptions.lua` (for BARb: `shortName=BARb`, `version=stable`, per
  `AI/Skirmish/BARb/data/AIInfo.lua`).
- AI interface: `<datadir>/AI/Interfaces/<shortName>/<version>/libAIInterface.so` +
  `InterfaceInfo.lua` (for the C interface: `shortName=C`, `version=0.1`).

Installed under `~/RecoilEngine/cont/AI/...` on the VM (one of the two `SPRING_DATADIR`
roots the desktop shortcut already sets, §5 — so no shortcut change needed):

```bash
# from the host, after ./build.sh has produced build-fedora/
scp build-fedora/AI/Skirmish/BARb/data/libSkirmishAI.so \
    root@192.168.122.203:/home/quark/RecoilEngine/cont/AI/Skirmish/BARb/stable/
scp ../../../RecoilEngine/AI/Skirmish/BARb/data/{AIInfo,AIOptions}.lua \
    root@192.168.122.203:/home/quark/RecoilEngine/cont/AI/Skirmish/BARb/stable/
scp build-fedora/AI/Interfaces/C/data/libAIInterface.so \
    root@192.168.122.203:/home/quark/RecoilEngine/cont/AI/Interfaces/C/0.1/
scp ../../../RecoilEngine/AI/Interfaces/C/data/InterfaceInfo.lua \
    root@192.168.122.203:/home/quark/RecoilEngine/cont/AI/Interfaces/C/0.1/
ssh root@192.168.122.203 'chown -R quark:quark /home/quark/RecoilEngine/cont/AI'
```

### 6.4 The skirmish start script, and two config problems it exposed
`~/RecoilEngine/build/skirmish_2ai.txt` (Spring start-script format, see
`doc/StartScriptFormat.txt` in the engine source) — one human spectator (`Host`) plus
two `[AI0]`/`[AI1]` sections running `ShortName=BARb`, `Team=0`/`Team=1`, on
`MapName=Techno Lands Final 26.1`, `GameType=Beyond all Reason test-11420-8448c1d` (the
exact game-archive name string, confirmed via
`pr-downloader --dump-sdp <sdp> | grep modinfo` → decompress that pool blob → read its
`name`/`version` fields, since it doesn't match the rapid tag's own label).

Getting a working AI actually took two rounds of fixing:

1. **First attempt**: both AIs failed `EVENT_INIT` (`AI script: 'script/hard/init.as' is
   missing!`, error 201) and were immediately dropped from their teams. BARb's
   `AIOptions.lua` `profile` option defaults to `'hard'`, but every profile except `'dev'`
   is commented out in this build's option list — `'hard'` was never actually wired up.
   Fixed by setting `profile=dev;` (and `game_config=false;`, since the game-side
   override path it also tries first — `LuaRules/Configs/BARb/stable/script/hard/init.as`
   — isn't shipped by this particular BAR test build either) in each AI's `[OPTIONS]`
   block.
2. **Second attempt**: same error, now for `'script/dev/init.as'` instead — because only
   `libSkirmishAI.so` + the two top-level `.lua` files had been copied to the VM (§6.3),
   not BARb's own bundled `data/script/` and `data/config/` subtrees, which the `'dev'`
   profile needs. Fixed by copying `AI/Skirmish/BARb/data/` wholesale (minus the `.so`,
   already placed) to `~/RecoilEngine/cont/AI/Skirmish/BARb/stable/` on the VM.

With both fixes in place, a 23-second timed test run (`timeout 25 ./spring
skirmish_2ai.txt`) completed with **zero** AI errors and **zero** "removed from team"
messages — both sides loaded, analyzed the map, and stayed in control of their team for
the full run.

### 6.5 Final validation — Quark against a live, active game
Reran the same skirmish untimed (`setsid nohup ... &`, so it survives the SSH session
ending) with the daemon + kernel module active:

1. `[QUARK-DAEMON] Registering game process with PID: 13326` — the running skirmish
   itself, not an idle menu.
2. Confirmed alive and actively simulating: `ps` showed **345% CPU** (multi-threaded,
   both BARb instances doing pathfinding/economy/combat AI concurrently) at the moment of
   the attack.
3. `./cheat/cheat 13326 0x400000 1234` → **blocked**: `Permission denied`; `dmesg`:
   `[QUARK-KERNEL ALERT] Blocked memory/ptrace access to protected process 13326 ...`
   (logged for several distinct attacking PIDs across the run — the daemon's own
   redundant monitor thread included).
4. The skirmish process was completely unaffected — still running normally afterward,
   only stopped by an explicit `kill` once validation was done.

This is a strictly stronger result than §7's original test: the same Ring-0 block, now
proven against a real, CPU-heavy, multi-threaded game process actively simulating a real
match, not an idle process sitting at a menu.

### 6.6 Desktop shortcuts (final)
Two shortcuts now exist on `~/Escritorio/`:
- `BeyondAllReason-Quark.desktop` (§5) — plain menu launch.
- `BAR-2xAI-Skirmish-Quark.desktop` — launches straight into the 2×BARb skirmish
  described above (`Exec=... /home/quark/RecoilEngine/build/spring skirmish_2ai.txt`).

## 7. Validation results (process-level protection)

Full round-trip proof, run against the real patched `engine-legacy` binary (not the
project's mock `game_target/game`):

1. Launched patched engine → daemon log: `Registering game process with PID: 8300`,
   kernel log: `Process 8300 is now protected by Quark.`
2. `./cheat/cheat 8300 0x400000 1234` → **blocked**: `Permission denied`; kernel log:
   `[QUARK-KERNEL ALERT] Blocked memory/ptrace access to protected process 8300 by PID
   ...`. The daemon's own root-level `/proc/pid/mem` monitor thread was blocked too,
   confirming the block is total (kernel doesn't special-case root), not just aimed at
   the specific attacker.
3. Negative control: `rmmod quark_kernel`, repeated the identical `cheat` command →
   **succeeded** (`Wrote 4 bytes`) — confirms the block in step 2 was genuinely Quark's
   doing, not an unrelated OS restriction (e.g. Yama `ptrace_scope`, which this engine
   binary never opts out of via `prctl`, unlike the mock game).
4. `insmod` the module again; the engine process was unharmed throughout (never crashed
   or was killed — SIGKILL only happens on a *value* mismatch via the userspace monitor
   path, which isn't in play here since no variables were registered, per the
   process-level-only scope decision).

## 8. Daemon-disconnect watchdog — refuse to continue if supervision drops

Everything above protects the game *while the daemon is up*, but originally said nothing
about the daemon disappearing after a successful connection (killed, crashed) — the game
would just keep running, unprotected, without any indication. Fixed by giving
`quark_sdk_init()` a background watchdog thread (`sdk/quark_sdk.c` /
`quark_watchdog_main`): once connected and registered, it polls the daemon socket every
second (`poll()` for `POLLHUP`/`POLLERR`, falling back to a `MSG_PEEK` `recv()` to
distinguish real EOF from an unexpectedly-readable socket, since the daemon's protocol is
otherwise one-way and never sends anything back) and calls `_exit(137)` the moment the
connection is gone — printing `[QUARK-SDK] FATAL: lost connection to the Quark Anticheat
daemon. Refusing to continue unsupervised.` first. Entirely self-contained in the SDK; no
engine-side change needed (`Main.cpp` is untouched by this).

Thread-safety note: `quark_socket_fd` is now touched from two threads (main +
watchdog), guarded by a small mutex (`quark_fd_mutex`) around the watchdog's reads and
`quark_sdk_close()`'s write — `quark_sdk_init()` itself doesn't need the lock since the
watchdog thread doesn't exist yet at any point during its own fd setup.

**Validated live**: launched the protected skirmish (PID 14365, daemon registered it
normally), then `kill -9`'d the daemon process directly. The engine printed the fatal
message and exited on its own — confirmed via its log file, not just inference from the
process disappearing. Actual wall-clock gap between killing the daemon and the game
actually exiting was ~5.5s here, not the ~1s poll interval — the engine was still deep in
its own CPU-heavy startup/map-loading at that moment (5 cores fully occupied by 2× BARb
initializing), so the watchdog thread itself was scheduling-delayed. This is a best-effort
interval, not a hard real-time guarantee, which is an acceptable trade-off for this
purpose.

The full sequence to reproduce: load the kernel module, `systemctl start quark.socket`
(or just connect — it starts itself, §3.2), launch the protected engine, confirm
`[QUARK-SDK] Successfully initialized...` in its log, then
`systemctl stop quark.service` (or `pkill -9 quark_daemon`) and watch the engine's own
log for the fatal message and process exit within a few seconds.

## 9. Low graphics preset

`~/.config/spring/springsettings.cfg` was set to the cheapest rendering options the
engine exposes (`Shadows=0`, `ShadowMapSize=0`, `GroundDetail=0`, `Water=0`,
`GrassDetail=0`, `GroundDecals=0`, `MaxParticles=500` down from the default 15000,
`AdvModelShading=0`, `NormalMapping=0`, deferred rendering off) — there's no single
"preset" name to set; BAR's own in-game graphics menu just writes these same engine
cvars, so this is the direct equivalent without needing to click through the UI.
Verified the engine accepts all of them (no "unknown key" warnings) and both AI sides
still played normally. This is the standing config now — applies to every future launch
via either desktop shortcut or SSH, no extra flag needed.

## 10. Fixing the spectator/no-spawn bug, and why "2 metal spots + starting buildings" wasn't shippable

**Spectator/no-spawn bug**: the original start script used `StartPosType=0` ("fixed")
without ever setting `StartPosX`/`StartPosZ` for either team. Per
`doc/StartScriptFormat.txt`, those fields are only consulted for `StartPosType=3`
("choose before game") — `0` instead means "use the map's own predefined start-position
markers," and something about that assignment broke for one of the two teams, which
showed up in the real GUI session as one AI stuck as a spectator and the other never
spawning. Fixed by switching to `StartPosType=1` (random, picks from the map's valid
start positions) — confirmed both AIs took control of their teams cleanly afterward.

**A second real bug found along the way**: `luarules/configs/metalSpots.lua` on this map
computes `tonumber(Spring.GetMapOptions().map_metal) * 1.45`, but `map_metal` (declared
in the map's own `mapoptions.lua` with default `4.7`) is only actually populated at
runtime if the start script's `[MAPOPTIONS]` block sets it explicitly — that default is
just for lobby-UI display. Without it, `Spring.GetMapOptions().map_metal` came back
`nil` and metal-spot loading errored out every run
(`attempt to perform arithmetic on a nil value`). Fixed by adding
`[MAPOPTIONS] { map_metal=4.7; map_middlemexes=0; }` to the start script — confirmed the
error is gone. Both fixes are now in `skirmish_2ai.txt` and are genuine improvements
independent of anything else in this section.

**"2 metal spots + pre-placed starting buildings" was attempted and dropped.** The plan
was a small custom Lua gadget (`gadget:AllowUnitCreation` to refuse metal extractors
outside two chosen real spots — verified the exact callin signature against
`rts/Lua/LuaHandleSynced.cpp` first — plus `gadget:Initialize()` spawning a starting
`armlab`/`corlab` for each team via `Spring.CreateUnit`, verified against
`rts/Lua/LuaSyncedCtrl.cpp`), dropped as a loose override file under
`cont/luarules/gadgets/`. It never loaded. Root cause, found in the game's own
`luarules/gadgets.lua` (the gadget manager all BAR gadgets go through):

```lua
local VFSMODE = VFS.ZIP_ONLY
if (Spring.IsDevLuaEnabled()) then
  VFSMODE = VFS.RAW_ONLY
end
```

Gadgets load from the packed game archive **or** from loose files — never both at once.
Switching to raw/loose mode would also make every *real* BAR gadget stop loading (they
only exist inside the archive). Worse, `Spring.IsDevLuaEnabled()` is driven by a manual,
cheat-gated, interactive in-game console command (`/DevLua`) — not something a
start-script can set — so this path was a dead end for automation regardless. Getting a
custom gadget into a real match for real would mean editing the game's packed `.sdp`
manifest and content-addressed pool directly (binary format, real risk of corrupting the
archive) — offered to the user as the actual next step; **declined** in favor of keeping
just the two genuine bug fixes above and dropping the buildings/2-metal-spot feature.
The unused gadget file was removed from the VM (`cont/luarules/gadgets/` no longer
exists there).
