#!/bin/bash
# Build RecoilEngine (or any of its CMake targets) in a Fedora 44 container that
# matches the test VM's glibc (2.43), so build output can be copied straight to the VM
# without compiling anything there. See ../../docs/vm_test_environment.md section 6.
#
# Usage:
#   ./build.sh [--reconfigure] [-D<CMAKE_VAR>=<value> ...] [--] <target> [<target> ...]
#
# Examples:
#   ./build.sh                              # configure (if needed) + build BARb, C-AIInterface (default)
#   ./build.sh engine-legacy                # build just the engine binary
#   ./build.sh --reconfigure -DAI_TYPES=NATIVE -- BARb CircuitAI C-AIInterface
#
# Output lands in $RECOIL_ENGINE_DIR/build-fedora/ (bind-mounted, so it's on the host
# filesystem too — no `docker cp` needed). Copy what you need to the VM with scp, e.g.:
#   scp build-fedora/AI/Skirmish/BARb/data/libSkirmishAI.so \
#       root@<vm>:/home/quark/RecoilEngine/cont/AI/Skirmish/BARb/stable/
#
# RECOIL_ENGINE_DIR (env var, default: ../../RecoilEngine relative to this script,
# i.e. the RecoilEngine git submodule at the repo root) controls which source
# checkout gets mounted — see docker-compose.yml.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

RECONFIGURE=0
CMAKE_ARGS=()
TARGETS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --reconfigure) RECONFIGURE=1; shift ;;
        --) shift; TARGETS+=("$@"); break ;;
        -D*) CMAKE_ARGS+=("$1"); shift ;;
        *) TARGETS+=("$1"); shift ;;
    esac
done

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=(BARb C-AIInterface)
fi

echo "==> Starting build container (Fedora 44, glibc matches the test VM)"
docker compose up -d --build

echo "==> Trusting bind-mounted source dir for git (container root vs host-owned files)"
docker compose exec build git config --global --add safe.directory /src

CONFIGURE=0
if [[ $RECONFIGURE -eq 1 ]]; then
    CONFIGURE=1
elif ! docker compose exec build test -f /src/build-fedora/CMakeCache.txt; then
    CONFIGURE=1
fi

if [[ $CONFIGURE -eq 1 ]]; then
    echo "==> Configuring (build-fedora/, RelWithDebInfo)"
    docker compose exec build sh -c \
        "mkdir -p /src/build-fedora && cd /src/build-fedora && cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF ${CMAKE_ARGS[*]}"
fi

echo "==> Building: ${TARGETS[*]}"
docker compose exec build sh -c \
    "cd /src/build-fedora && cmake --build . --target ${TARGETS[*]} -j \$(nproc)"

echo "==> Done. Output under \$RECOIL_ENGINE_DIR/build-fedora/"
