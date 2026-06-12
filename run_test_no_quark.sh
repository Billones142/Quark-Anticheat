#!/bin/bash

# Quark Anticheat - Test Script Without Quark Protection
# This script simulates the cheat attack when the anticheat daemon is NOT running.
# It proves that the memory is tampered successfully and the game continues running.

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color
BOLD='\033[1m'

echo -e "${BOLD}=== Quark Anticheat: Starting Test WITHOUT Quark Protection ===${NC}"

# 1. Compile the project
echo "Compiling components..."
make > /dev/null
if [ $? -ne 0 ]; then
    echo -e "${RED}Error: Compilation failed!${NC}"
    exit 1
fi
echo -e "${GREEN}Compilation successful.${NC}\n"

# Clean up previous logs
rm -f game_no_quark.log

# 2. Start the Mock Game in the background with a timed subshell pipe for stdin
echo "Starting Mock Game (unprotected)..."
(
  # Keep stdin open initially
  sleep 2.0
  # Send 'p' command to print the status after the cheat has run
  echo "p"
  # Keep stdin open so the game doesn't exit on EOF
  while true; do sleep 1; done
) | ./game_target/game > game_no_quark.log 2>&1 &
GAME_PID=$!

# Wait for game initialization
sleep 1.0

# Check if game started successfully
if ! ps -p $GAME_PID > /dev/null; then
    echo -e "${RED}Error: Mock Game failed to start. Check game_no_quark.log:${NC}"
    if [ -f game_no_quark.log ]; then cat game_no_quark.log; fi
    exit 1
fi
echo -e "Mock Game started (PID: $GAME_PID).\n"

# 3. Extract target variable address from Game log
echo "Extracting 'health' variable address..."
HEALTH_ADDR=$(grep -oP "Health address:\s+\K0x[0-9a-fA-F]+" game_no_quark.log)

if [ -z "$HEALTH_ADDR" ]; then
    echo -e "${RED}Error: Could not extract health variable address from game_no_quark.log! Content:${NC}"
    cat game_no_quark.log
    kill $GAME_PID 2>/dev/null
    exit 1
fi
echo -e "Target Variable 'health' found at address: ${BOLD}$HEALTH_ADDR${NC}\n"

# Show initial status from log
echo -e "${BOLD}--- Initial Game Log ---${NC}"
cat game_no_quark.log
echo -e "------------------------\n"

# 4. Execute the Cheat program to modify the memory of the game
echo -e "${BOLD}Simulating Cheat Attack...${NC}"
echo "Running: ./cheat/cheat $GAME_PID $HEALTH_ADDR 9999"
./cheat/cheat $GAME_PID $HEALTH_ADDR 9999
sleep 1.5 # Wait for the subshell to send 'p' at t=2.0s

# 5. Verify if the Game is still running and check the value of health
echo -e "\n${BOLD}=== Verification ===${NC}"
if ps -p $GAME_PID > /dev/null; then
    echo -e "${GREEN}SUCCESS (for the cheat): The Mock Game is still running!${NC}"
    echo -e "This proves that the attack was NOT mitigated because Quark was not active."
else
    echo -e "${RED}FAIL: The Mock Game terminated unexpectedly!${NC}"
    exit 1
fi

# Show final game logs to see the modified health
echo -e "\n${BOLD}--- Final Game Log ---${NC}"
cat game_no_quark.log
echo -e "----------------------\n"

# Verify if the printed value contains 9999
if grep -q "Value: 9999" game_no_quark.log; then
    echo -e "${GREEN}${BOLD}CONFIRMED: The health variable was successfully modified to 9999 in memory!${NC}"
else
    echo -e "${RED}Warning: Could not confirm the value 9999 in the logs. Check game_no_quark.log output above.${NC}"
fi

# Cleanup
echo -e "\nCleaning up background processes..."
kill -9 $GAME_PID 2>/dev/null
rm -f game_no_quark.log

echo -e "${GREEN}${BOLD}Test completed successfully!${NC}"
exit 0
