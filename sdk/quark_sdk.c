#include "quark_sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <pthread.h>

#define QUARK_SOCKET_PATH "/tmp/quark.sock"

#define CMD_REGISTER_GAME 1
#define CMD_REGISTER_VAR  2
#define CMD_UPDATE_VAR    3

// How often the watchdog thread checks the daemon connection is still alive.
#define QUARK_WATCHDOG_INTERVAL_SEC 1

static int quark_socket_fd = -1;
static pthread_mutex_t quark_fd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t quark_watchdog_thread;

static int send_packet(uint32_t command, const void *payload, uint32_t payload_len) {
    if (quark_socket_fd == -1) {
        return -1;
    }

    uint8_t header[8];
    // Use native endian since SDK and daemon run on the same system
    memcpy(&header[0], &command, 4);
    memcpy(&header[4], &payload_len, 4);

    if (write(quark_socket_fd, header, 8) != 8) {
        return -1;
    }

    if (payload_len > 0 && payload) {
        if (write(quark_socket_fd, payload, payload_len) != (ssize_t)payload_len) {
            return -1;
        }
    }

    return 0;
}

// Runs for as long as the daemon connection is alive. If it drops (daemon
// killed, crashed, or the connection otherwise closes), this is treated as
// the anticheat's supervision having disappeared: the game must not be
// allowed to keep running unsupervised, so this thread terminates the whole
// process immediately.
static void *quark_watchdog_main(void *arg) {
    (void)arg;

    for (;;) {
        sleep(QUARK_WATCHDOG_INTERVAL_SEC);

        pthread_mutex_lock(&quark_fd_mutex);
        int fd = quark_socket_fd;
        pthread_mutex_unlock(&quark_fd_mutex);

        // quark_sdk_close() was called deliberately; nothing left to watch.
        if (fd == -1) {
            return NULL;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ready = poll(&pfd, 1, 0);
        if (ready < 0) {
            continue; // interrupted by a signal or similar transient error, retry next tick
        }

        int disconnected = 0;
        if (ready > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            disconnected = 1;
        } else if (ready > 0 && (pfd.revents & POLLIN)) {
            // The daemon never sends data to us (fire-and-forget protocol), so
            // the only thing that should ever make the socket readable is the
            // peer closing it. Peek to confirm EOF vs. genuinely unexpected data.
            char peek_buf[1];
            ssize_t n = recv(fd, peek_buf, sizeof(peek_buf), MSG_PEEK | MSG_DONTWAIT);
            if (n == 0) {
                disconnected = 1;
            }
        }

        if (disconnected) {
            fprintf(stderr, "[QUARK-SDK] FATAL: lost connection to the Quark Anticheat daemon. "
                             "Refusing to continue unsupervised.\n");
            fflush(stderr);
            _exit(137);
        }
    }
}

int quark_sdk_init(void) {
    struct sockaddr_un addr;

    quark_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (quark_socket_fd == -1) {
        perror("[QUARK-SDK] socket creation failed");
        return -1;
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, QUARK_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(quark_socket_fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_un)) == -1) {
        perror("[QUARK-SDK] connection to daemon failed");
        close(quark_socket_fd);
        quark_socket_fd = -1;
        return -1;
    }

    pid_t pid = getpid();
    if (send_packet(CMD_REGISTER_GAME, &pid, sizeof(pid_t)) != 0) {
        fprintf(stderr, "[QUARK-SDK] Failed to send game registration\n");
        close(quark_socket_fd);
        quark_socket_fd = -1;
        return -1;
    }

    printf("[QUARK-SDK] Successfully initialized and linked to Quark Daemon (PID: %d)\n", pid);

    if (pthread_create(&quark_watchdog_thread, NULL, quark_watchdog_main, NULL) != 0) {
        perror("[QUARK-SDK] failed to start connection watchdog thread");
        // Non-fatal: we're still connected and registered, just without the
        // "refuse to continue if disconnected" guarantee.
    } else {
        pthread_detach(quark_watchdog_thread);
    }

    return 0;
}

int quark_sdk_register_var(void *address, uint32_t size, const char *name) {
    if (quark_socket_fd == -1) {
        return -1;
    }

    uint8_t payload[44];
    memset(payload, 0, sizeof(payload));

    uint64_t addr_val = (uint64_t)address;

    memcpy(&payload[0], &addr_val, 8);
    memcpy(&payload[8], &size, 4);

    // Copy up to 32 bytes for the variable name
    if (name) {
        strncpy((char *)&payload[12], name, 31);
    }

    if (send_packet(CMD_REGISTER_VAR, payload, 44) != 0) {
        fprintf(stderr, "[QUARK-SDK] Failed to register variable '%s'\n", name ? name : "unknown");
        return -1;
    }

    return 0;
}

int quark_sdk_update_var(void *address, uint64_t new_value) {
    if (quark_socket_fd == -1) {
        return -1;
    }

    uint8_t payload[16];
    uint64_t addr_val = (uint64_t)address;

    memcpy(&payload[0], &addr_val, 8);
    memcpy(&payload[8], &new_value, 8);

    if (send_packet(CMD_UPDATE_VAR, payload, 16) != 0) {
        fprintf(stderr, "[QUARK-SDK] Failed to send update for variable at %p\n", address);
        return -1;
    }

    return 0;
}

void quark_sdk_close(void) {
    pthread_mutex_lock(&quark_fd_mutex);
    if (quark_socket_fd != -1) {
        close(quark_socket_fd);
        quark_socket_fd = -1;
        printf("[QUARK-SDK] Connection closed.\n");
    }
    pthread_mutex_unlock(&quark_fd_mutex);
}
