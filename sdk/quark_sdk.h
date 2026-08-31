#ifndef QUARK_SDK_H
#define QUARK_SDK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes the Quark SDK by connecting to the Quark Daemon socket
 * and registering the current game process PID.
 *
 * The daemon is asked to confirm that Ring 0 (kernel module) protection was
 * actually registered for this PID -- a reachable daemon is not by itself
 * proof of protection (e.g. the kernel module may not be loaded). This call
 * only succeeds once that confirmation is received.
 *
 * On success, also starts a background watchdog thread that keeps checking
 * the daemon connection is still alive. If the daemon disconnects later
 * (killed, crashed, etc.), the watchdog terminates this process immediately
 * (process exit, not a normal shutdown) rather than letting the game keep
 * running unsupervised.
 *
 * Returns 0 on success, -1 on failure (daemon not running at startup, or
 * reachable but unable to confirm kernel-level protection; in either case
 * the caller should treat the game as unprotected and act accordingly,
 * e.g. refuse to start).
 */
int quark_sdk_init(void);

/**
 * Registers a variable with the Quark Daemon to be monitored.
 * @param address Pointer to the variable to monitor.
 * @param size Size of the variable in bytes (e.g. 1, 2, 4, 8).
 * @param name A human-readable identifier for the variable (max 31 chars).
 * Returns 0 on success, -1 on failure.
 */
int quark_sdk_register_var(void *address, uint32_t size, const char *name);

/**
 * Notifies the Quark Daemon of a legitimate update to a registered variable.
 * @param address Pointer to the updated variable.
 * @param new_value The new value of the variable (cast to uint64_t).
 * Returns 0 on success, -1 on failure.
 */
int quark_sdk_update_var(void *address, uint64_t new_value);

/**
 * Reports whether Quark protection is currently active for this process,
 * i.e. quark_sdk_init() previously confirmed kernel-level registration and
 * the daemon connection is still up (the watchdog thread terminates the
 * process immediately if that connection drops, so as long as the process
 * is alive this stays accurate without needing to re-check the daemon).
 * Returns 1 if active, 0 otherwise (never initialized, init failed, or
 * quark_sdk_close() was called).
 */
int quark_sdk_is_active(void);

/**
 * Reports whether the kernel module currently protecting this process is a
 * TESTING build -- i.e. it was compiled with the VM-detection bypass and
 * would (unlike a release build) accept running inside a hypervisor guest.
 * A testing build should never be trusted as equivalent to real protection
 * by anything that matters (e.g. a game server deciding whether to accept a
 * client into a match that requires anticheat).
 * Returns 1 if this is a testing build, 0 otherwise (including if Quark
 * protection isn't active at all).
 */
int quark_sdk_is_testing_build(void);

/**
 * Copies the kernel module's version string (as reported by the daemon
 * during registration) into buf.
 * @param buf Destination buffer.
 * @param buf_size Size of buf; the copied string is always NUL-terminated.
 * Returns 0 on success, -1 if Quark protection isn't active (buf is left
 * empty) or buf/buf_size are invalid.
 */
int quark_sdk_get_version(char *buf, size_t buf_size);

/**
 * Closes the Quark SDK connection and cleans up resources.
 */
void quark_sdk_close(void);

#ifdef __cplusplus
}
#endif

#endif // QUARK_SDK_H
