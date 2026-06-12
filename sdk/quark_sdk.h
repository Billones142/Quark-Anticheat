#ifndef QUARK_SDK_H
#define QUARK_SDK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initializes the Quark SDK by connecting to the Quark Daemon socket
 * and registering the current game process PID.
 * Returns 0 on success, -1 on failure.
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
 * Closes the Quark SDK connection and cleans up resources.
 */
void quark_sdk_close(void);

#ifdef __cplusplus
}
#endif

#endif // QUARK_SDK_H
