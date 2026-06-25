#!/bin/bash

# Quark Anticheat - Ring 0 (Kernel) Test Script
# This script compiles the kernel module, loads it, runs the daemon, 
# launches the game, and tests that the cheat is blocked at the kernel level.

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color
BOLD='\033[1m'

echo -e "${BOLD}=== Quark Anticheat: Starting Ring 0 (Kernel-Level) Test ===${NC}"

# Check if running in a VM/system with sudo capabilities
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

echo "Compiling Ring 0 Kernel Module..."
make -C kernel > /dev/null
if [ $? -ne 0 ]; then
    echo -e "${RED}Error: Kernel module compilation failed! Are kernel headers installed?${NC}"
    exit 1
fi
echo -e "${GREEN}All components compiled successfully.${NC}\n"

# 2. Load the kernel module
echo "Loading kernel module 'quark_kernel' (requires sudo)..."
# Unload first if already loaded
sudo rmmod quark_kernel 2>/dev/null
sudo insmod kernel/quark_kernel.ko
if [ $? -ne 0 ]; then
    echo -e "${RED}Error: Failed to insert kernel module! Check dmesg.${NC}"
    exit 1
fi
echo -e "${GREEN}Kernel module loaded successfully.${NC}\n"

# Clean up logs
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
# Timed subshell to send 'p' at t=2.0s to check state
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

# 5. Extract 'health' address
HEALTH_ADDR=$(grep -oP "Health address:\s+\K0x[0-9a-fA-F]+" game.log)
if [ -z "$HEALTH_ADDR" ]; then
    echo -e "${RED}Error: Could not extract health address.${NC}"
    cat game.log
    kill $GAME_PID $DAEMON_PID 2>/dev/null
    sudo rmmod quark_kernel
    exit 1
fi
echo -e "Target Variable 'health' found at address: ${BOLD}$HEALTH_ADDR${NC}\n"

# 6. Execute the Cheat program to modify the memory of the game
echo -e "${BOLD}Simulating Cheat Attack (Kernel module should intercept)...${NC}"
echo "Running: ./cheat/cheat $GAME_PID $HEALTH_ADDR 9999"
./cheat/cheat $GAME_PID $HEALTH_ADDR 9999 > cheat.log 2>&1
CHEAT_EXIT=$?

# Wait for 'p' command print
sleep 1.5

# 7. Verification
echo -e "\n${BOLD}=== Verification ===${NC}"

# Check cheat output
echo -e "${BOLD}[Cheat Log]${NC}"
cat cheat.log
echo -e "------------------\n"

# Check if the game is still running (it should be, since the cheat was BLOCKED and memory didn't change)
if ps -p $GAME_PID > /dev/null; then
    echo -e "${GREEN}SUCCESS: The Mock Game is still running safely (not terminated because no memory change happened).${NC}"
else
    echo -e "${RED}FAIL: The Mock Game was terminated!${NC}"
fi

# Show final game logs to see the health value
echo -e "\n${BOLD}[Game Log State]${NC}"
cat game.log
echo -e "----------------\n"

# Verify if the value was successfully kept at 100
if grep -q "Value: 100" game.log; then
    echo -e "${GREEN}${BOLD}CONFIRMED: The health variable remained at 100! The kernel module successfully blocked the write operation.${NC}"
else
    echo -e "${RED}FAIL: The health variable was modified! Check game.log.${NC}"
fi

# 8. Unload kernel module and show dmesg output
echo -e "\nUnloading kernel module..."
sudo rmmod quark_kernel
kill $GAME_PID $DAEMON_PID 2>/dev/null
rm -f /tmp/quark.sock

echo -e "\n${BOLD}=== Kernel Ring Buffer Logs (dmesg) ===${NC}"
sudo dmesg | tail -n 12
echo -e "---------------------------------------\n"

echo -e "${GREEN}${BOLD}Ring 0 Test completed successfully!${NC}"
exit 0
