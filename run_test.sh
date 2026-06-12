#!/bin/bash

# Quark Anticheat - Automated Test Script
# This script compiles the project, launches the components in the background,
# simulates a memory modification cheat, and verifies if Quark terminates the game.

# Colors for nice output
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color
BOLD='\033[1m'

echo -e "${BOLD}=== Quark Anticheat: Starting Automated Test ===${NC}"

# 1. Compile the project
echo "Compiling components..."
make clean > /dev/null
make > /dev/null
if [ $? -ne 0 ]; then
    echo -e "${RED}Error: Compilation failed!${NC}"
    exit 1
fi
echo -e "${GREEN}Compilation successful.${NC}\n"

# Clean up logs and sockets
rm -f daemon.log game.log /tmp/quark.sock

# 2. Start the Quark Daemon in the background
echo "Starting Quark Daemon..."
./quark_daemon/target/release/quark_daemon > daemon.log 2>&1 &
DAEMON_PID=$!
sleep 0.5

# Check if daemon started successfully
if ! ps -p $DAEMON_PID > /dev/null; then
    echo -e "${RED}Error: Quark Daemon failed to start. Check daemon.log:${NC}"
    cat daemon.log
    exit 1
fi
echo -e "Daemon started (PID: $DAEMON_PID).\n"

# 3. Start the Mock Game in the background
echo "Starting Mock Game..."
# We run it in the background, redirecting stdout/stderr to game.log.
# We also pipe a dummy character loop so it doesn't immediately exit due to stdin EOF
(while true; do sleep 1; done) | ./game_target/game > game.log 2>&1 &
GAME_PID=$!
sleep 1.0

# Check if game started successfully
if ! ps -p $GAME_PID > /dev/null; then
    echo -e "${RED}Error: Mock Game failed to start. Check game.log:${NC}"
    cat game.log
    kill $DAEMON_PID 2>/dev/null
    exit 1
fi
echo -e "Mock Game started (PID: $GAME_PID).\n"

# 4. Extract target variable address from Game log
echo "Extracting 'health' variable address from game telemetry..."
HEALTH_ADDR=$(grep -oP "Health address:\s+\K0x[0-9a-fA-F]+" game.log)

if [ -z "$HEALTH_ADDR" ]; then
    echo -e "${RED}Error: Could not extract health variable address from game.log! Content:${NC}"
    cat game.log
    kill $GAME_PID $DAEMON_PID 2>/dev/null
    exit 1
fi
echo -e "Target Variable 'health' found at address: ${BOLD}$HEALTH_ADDR${NC}\n"

# Show initial status from logs
echo -e "${BOLD}--- Initial Logs ---${NC}"
echo -e "${BOLD}[Game Log]${NC}"
cat game.log
echo -e "\n${BOLD}[Daemon Log]${NC}"
cat daemon.log
echo -e "--------------------\n"

# 5. Execute the Cheat program to modify the memory of the game
echo -e "${BOLD}Simulating Cheat Attack...${NC}"
echo "Running: ./cheat/cheat $GAME_PID $HEALTH_ADDR 9999"
./cheat/cheat $GAME_PID $HEALTH_ADDR 9999
sleep 0.5

# 6. Verify if the Game was terminated by Quark
echo -e "\n${BOLD}=== Verification ===${NC}"
if ps -p $GAME_PID > /dev/null; then
    echo -e "${RED}FAIL: The Mock Game (PID: $GAME_PID) is still running! Quark failed to mitigate the cheat.${NC}"
    kill $GAME_PID $DAEMON_PID 2>/dev/null
    exit 1
else
    echo -e "${GREEN}SUCCESS: The Mock Game (PID: $GAME_PID) was successfully terminated!${NC}"
fi

# Show final daemon logs with the alert
echo -e "\n${BOLD}--- Final Quark Daemon Logs ---${NC}"
cat daemon.log
echo -e "--------------------------------\n"

# Cleanup background processes
kill $DAEMON_PID 2>/dev/null
rm -f daemon.log game.log /tmp/quark.sock

echo -e "${GREEN}${BOLD}Test completed successfully!${NC}"
exit 0
