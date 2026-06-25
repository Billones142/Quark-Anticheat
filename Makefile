CC = gcc
CFLAGS = -Wall -Wextra -O2

# Paths
SDK_DIR = sdk
GAME_DIR = game_target
CHEAT_DIR = cheat
DAEMON_DIR = quark_daemon

.PHONY: all daemon game cheat clean lint compile_commands.json

all: daemon game cheat

daemon:
	@echo "=== Compiling Quark Daemon (Rust) ==="
	cargo build --release --manifest-path $(DAEMON_DIR)/Cargo.toml
	@echo "=== Compiling Quark Netlink CLI Client (C) ==="
	$(CC) $(CFLAGS) $(DAEMON_DIR)/quark_cli.c -o $(DAEMON_DIR)/quark_cli

game: $(GAME_DIR)/game.c $(SDK_DIR)/quark_sdk.c
	@echo "=== Compiling Game & SDK (C) ==="
	$(CC) $(CFLAGS) -I$(SDK_DIR) $(GAME_DIR)/game.c $(SDK_DIR)/quark_sdk.c -o $(GAME_DIR)/game

cheat: $(CHEAT_DIR)/cheat.c
	@echo "=== Compiling Cheat (C) ==="
	$(CC) $(CFLAGS) $(CHEAT_DIR)/cheat.c -o $(CHEAT_DIR)/cheat

compile_commands.json:
	@echo "=== Generating compile_commands.json for Kernel Module ==="
	$(MAKE) -C kernel
	python3 /lib/modules/$(shell uname -r)/build/scripts/clang-tools/gen_compile_commands.py -d kernel
	python3 -c "import json; f=open('compile_commands.json','r'); cmds=json.load(f); f.close(); [cmd.update({'command': ' '.join([arg for arg in cmd['command'].split() if not any(x in arg for x in ['-fdiagnostics-show-context', '-fmin-function-alignment', '-fno-allow-store-data-races', '-fzero-init-padding-bits', '-mindirect-branch', '-mpreferred-stack-boundary', '-mrecord-mcount'])])}) for cmd in cmds]; f=open('compile_commands.json','w'); json.dump(cmds, f, indent=2); f.close()"

lint: compile_commands.json
	@echo "=== Linting Userspace C Codebases ==="
	clang-tidy $(SDK_DIR)/quark_sdk.c -- -I$(SDK_DIR)
	clang-tidy $(GAME_DIR)/game.c -- -I$(SDK_DIR)
	clang-tidy $(CHEAT_DIR)/cheat.c --
	clang-tidy $(DAEMON_DIR)/quark_cli.c --
	@echo "=== Linting Kernel Module ==="
	clang-tidy kernel/quark_kernel.c

clean:
	@echo "=== Cleaning Build Artifacts ==="
	cargo clean --manifest-path $(DAEMON_DIR)/Cargo.toml
	rm -f $(DAEMON_DIR)/quark_cli
	rm -f $(GAME_DIR)/game
	rm -f $(CHEAT_DIR)/cheat
	rm -f /tmp/quark.sock
	rm -f compile_commands.json
	$(MAKE) -C kernel clean

