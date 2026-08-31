#!/bin/bash

# Quark Anticheat - Release Kernel Module VM-Refusal Test Script
#
# Companion to run_test_kernel.sh, which builds the TESTING kernel module
# (VM check bypassed) because this test rig is itself a hypervisor guest.
# This script instead builds the RELEASE module and asserts the opposite
# outcome: on a hypervisor guest, the release module must refuse to protect
# anything at all, and the game must observe that refusal (via the existing
# ack/nack fail-closed handshake) instead of silently getting real protection.

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color
BOLD='\033[1m'

echo -e "${BOLD}=== Quark Anticheat: Release Kernel Module VM-Refusal Test ===${NC}"

if ! command -v sudo &> /dev/null; then
    echo -e "${RED}Error: 'sudo' command not found. Root privileges are required to load kernel modules.${NC}"
    exit 1
fi

# 1. Compile everything
echo "Compiling userspace components..."
make clean > /dev/null
make > /dev/null
if [ $? -ne 0 ]; then
    echo -e "${RED}Error: Userspace compilation failed!${NC}"
    exit 1
fi

echo "Compiling Ring 0 Kernel Module (RELEASE build -- no VM-check bypass compiled in)..."
make -C kernel > /dev/null
if [ $? -ne 0 ]; then
    echo -e "${RED}Error: Kernel module compilation failed! Are kernel headers installed?${NC}"
    exit 1
fi
echo -e "${GREEN}All components compiled successfully.${NC}\n"

# 2. Load the kernel module
echo "Loading kernel module 'quark_kernel' (requires sudo)..."
sudo rmmod quark_kernel 2>/dev/null
sudo insmod kernel/quark_kernel.ko
if [ $? -ne 0 ]; then
    echo -e "${RED}Error: Failed to insert kernel module! Check dmesg.${NC}"
    exit 1
fi
echo -e "${GREEN}Kernel module loaded successfully.${NC}\n"

rm -f daemon.log game.log cheat.log

# 3. Start the Quark Daemon in the background
echo "Starting Quark Daemon..."
./quark_daemon/target/release/quark_daemon > daemon.log 2>&1 &
DAEMON_PID=$!
sleep 0.5

if ! ps -p $DAEMON_PID > /dev/null; then
    echo -e "${RED}Error: Daemon failed to start.${NC}"
    cat daemon.log
    sudo rmmod quark_kernel
    exit 1
fi
echo -e "Daemon started (PID: $DAEMON_PID).\n"

# 4. Start the Mock Game in the background
echo "Starting Mock Game..."
(
  sleep 2.0
  echo "p"
  while true; do sleep 1; done
) | ./game_target/game > game.log 2>&1 &
GAME_PID=$!
sleep 1.0

if ! ps -p $GAME_PID > /dev/null; then
    echo -e "${RED}Error: Mock Game failed to start.${NC}"
    cat game.log
    kill $DAEMON_PID 2>/dev/null
    sudo rmmod quark_kernel
    exit 1
fi
echo -e "Mock Game started (PID: $GAME_PID).\n"

# 5. Extract 'health' address (still printed by print_status() regardless of protection state)
HEALTH_ADDR=$(grep -oP "Health address:\s+\K0x[0-9a-fA-F]+" game.log)
if [ -z "$HEALTH_ADDR" ]; then
    echo -e "${RED}Error: Could not extract health address.${NC}"
    cat game.log
    kill $GAME_PID $DAEMON_PID 2>/dev/null
    sudo rmmod quark_kernel
    exit 1
fi
echo -e "Target Variable 'health' found at address: ${BOLD}$HEALTH_ADDR${NC}\n"

# 6. Execute the Cheat program -- expected to SUCCEED, since the release module
# should have refused to register protection for this PID at all.
echo -e "${BOLD}Simulating Cheat Attack (should succeed: release module must have refused protection in this VM)...${NC}"
echo "Running: ./cheat/cheat $GAME_PID $HEALTH_ADDR 9999"
./cheat/cheat $GAME_PID $HEALTH_ADDR 9999 > cheat.log 2>&1

sleep 1.5

# 7. Verification
echo -e "\n${BOLD}=== Verification ===${NC}"

echo -e "${BOLD}[Cheat Log]${NC}"
cat cheat.log
echo -e "------------------\n"

echo -e "${BOLD}[Game Log]${NC}"
cat game.log
echo -e "------------------\n"

PASS=1

if grep -q "FATAL: kernel-level protection was not confirmed" game.log; then
    echo -e "${GREEN}CONFIRMED: quark_sdk_init() reported the fail-closed FATAL message.${NC}"
else
    echo -e "${RED}FAIL: Expected the SDK's FATAL fail-closed message; game.log doesn't contain it.${NC}"
    PASS=0
fi

if grep -q "Value: 9999" game.log; then
    echo -e "${GREEN}CONFIRMED: The health variable WAS modified to 9999 -- no kernel-level protection was active, as expected in a release build running inside a VM.${NC}"
else
    echo -e "${RED}FAIL: health was not modified -- either protection unexpectedly succeeded, or the cheat itself failed.${NC}"
    PASS=0
fi

if ! ps -p $GAME_PID > /dev/null; then
    echo -e "${RED}FAIL: The Mock Game was terminated (no monitoring session should have started at all).${NC}"
    PASS=0
fi

# 8. Unload kernel module and show dmesg output
echo -e "\nUnloading kernel module..."
sudo rmmod quark_kernel
kill $GAME_PID $DAEMON_PID 2>/dev/null
rm -f /tmp/quark.sock

echo -e "\n${BOLD}=== Kernel Ring Buffer Logs (dmesg) ===${NC}"
sudo dmesg | tail -n 12
echo -e "---------------------------------------\n"

if [ "$PASS" -eq 1 ]; then
    echo -e "${GREEN}${BOLD}Release-build VM-refusal test completed successfully!${NC}"
    exit 0
else
    echo -e "${RED}${BOLD}Release-build VM-refusal test FAILED -- see above.${NC}"
    exit 1
fi
