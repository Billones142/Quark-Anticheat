CC = gcc
CFLAGS = -Wall -Wextra -O2

# Paths
SDK_DIR = sdk
GAME_DIR = game_target
CHEAT_DIR = cheat
DAEMON_DIR = quark_daemon

.PHONY: all daemon game cheat clean

all: daemon game cheat

daemon:
	@echo "=== Compiling Quark Daemon (Rust) ==="
	cargo build --release --manifest-path $(DAEMON_DIR)/Cargo.toml

game: $(GAME_DIR)/game.c $(SDK_DIR)/quark_sdk.c
	@echo "=== Compiling Game & SDK (C) ==="
	$(CC) $(CFLAGS) -I$(SDK_DIR) $(GAME_DIR)/game.c $(SDK_DIR)/quark_sdk.c -o $(GAME_DIR)/game

cheat: $(CHEAT_DIR)/cheat.c
	@echo "=== Compiling Cheat (C) ==="
	$(CC) $(CFLAGS) $(CHEAT_DIR)/cheat.c -o $(CHEAT_DIR)/cheat

clean:
	@echo "=== Cleaning Build Artifacts ==="
	cargo clean --manifest-path $(DAEMON_DIR)/Cargo.toml
	rm -f $(GAME_DIR)/game
	rm -f $(CHEAT_DIR)/cheat
	rm -f /tmp/quark.sock
