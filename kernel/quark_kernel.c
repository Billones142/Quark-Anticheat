/*
 * Quark Anticheat - Reference Kernel Module (C)
 * 
 * This file serves as a reference implementation of the Ring 0 component 
 * of Quark Anticheat. It uses Linux Security Modules (LSM) hooks to intercept 
 * process memory access (like ptrace or /proc/PID/mem writes) and blocks 
 * unauthorized attempts.
 * 
 * Note: Compiling and loading this module requires kernel headers and root privileges.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/lsm_hooks.h>
#include <linux/security.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martinez Alarcon, Gabriel Sebastian & Merino De Rui, Stefano Nahuel");
MODULE_DESCRIPTION("Quark Anticheat - Ring 0 Security Module (LSM & Netlink)");
MODULE_VERSION("0.1.0");

#define NETLINK_QUARK 31  // Custom netlink protocol number
#define MAX_PROTECTED_PROCESSES 16

static struct sock *nl_socket = NULL;
static pid_t protected_pids[MAX_PROTECTED_PROCESSES];
static int protected_pids_count = 0;
static DEFINE_SPINLOCK(quark_lock);

// Helper to check if a PID is currently protected
static bool is_pid_protected(pid_t pid) {
    int i;
    bool found = false;
    unsigned long flags;

    spin_lock_irqsave(&quark_lock, flags);
    for (i = 0; i < protected_pids_count; i++) {
        if (protected_pids[i] == pid) {
            found = true;
            break;
        }
    }
    spin_unlock_irqrestore(&quark_lock, flags);
    return found;
}

// Add a PID to the protected list
static void add_protected_pid(pid_t pid) {
    unsigned long flags;
    spin_lock_irqsave(&quark_lock, flags);
    if (protected_pids_count < MAX_PROTECTED_PROCESSES) {
        // Check if already present
        int i;
        bool exists = false;
        for (i = 0; i < protected_pids_count; i++) {
            if (protected_pids[i] == pid) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            protected_pids[protected_pids_count++] = pid;
            pr_info("[QUARK-KERNEL] Process %d is now protected by Quark.\n", pid);
        }
    } else {
        pr_warn("[QUARK-KERNEL] Protected PID table full, cannot protect PID %d\n", pid);
    }
    spin_unlock_irqrestore(&quark_lock, flags);
}

// Remove a PID from the protected list
static void remove_protected_pid(pid_t pid) {
    int i;
    unsigned long flags;
    spin_lock_irqsave(&quark_lock, flags);
    for (i = 0; i < protected_pids_count; i++) {
        if (protected_pids[i] == pid) {
            protected_pids[i] = protected_pids[protected_pids_count - 1];
            protected_pids_count--;
            pr_info("[QUARK-KERNEL] Process %d removed from Quark protection.\n", pid);
            break;
        }
    }
    spin_unlock_irqrestore(&quark_lock, flags);
}

/*
 * LSM Hook: Intercepts process attachment or memory access checks.
 * Under Linux, opening /proc/<PID>/mem or calling process_vm_writev/ptrace
 * triggers a security_ptrace_access_check.
 */
static int quark_ptrace_access_check(struct task_struct *child, unsigned int mode) {
    pid_t target_pid = task_pid_vnr(child);
    pid_t parent_pid = task_pid_vnr(current);

    // If the target process is protected by Quark
    if (is_pid_protected(target_pid)) {
        // Allow the process to access its own memory
        if (target_pid == parent_pid) {
            return 0; 
        }

        // Block all other processes from accessing this process's memory/debugging it
        pr_warn("[QUARK-KERNEL ALERT] Blocked access to protected process %d by PID %d (Mode: %u)\n", 
                target_pid, parent_pid, mode);
        
        // Return Access Denied
        return -EACCES; 
    }

    // Default allow for other processes
    return 0; 
}

/*
 * Netlink receiver callback. Receives commands from userspace daemon.
 */
static void quark_nl_recv_msg(struct sk_buff *skb) {
    struct nlmsghdr *nlh;
    int pid;
    int msg_type;

    nlh = (struct nlmsghdr *)skb->data;
    msg_type = nlh->nlmsg_type;
    
    // Payload contains the target PID
    if (nlmsg_len(nlh) < sizeof(int)) {
        pr_err("[QUARK-KERNEL] Netlink payload too small\n");
        return;
    }
    
    pid = *(int *)nlmsg_data(nlh);

    switch (msg_type) {
        case 1: // Command: Protect PID
            add_protected_pid(pid);
            break;
        case 2: // Command: Unprotect PID
            remove_protected_pid(pid);
            break;
        default:
            pr_warn("[QUARK-KERNEL] Unknown Netlink message type: %d\n", msg_type);
            break;
    }
}

// Registering LSM Hooks (Modern Linux Kernel format)
static struct security_hook_list quark_hooks[] __lsm_ro_after_init = {
    LSM_HOOK_INIT(ptrace_access_check, quark_ptrace_access_check),
};

static int __init quark_kernel_init(void) {
    struct netlink_kernel_cfg cfg = {
        .input = quark_nl_recv_msg,
    };

    pr_info("[QUARK-KERNEL] Initializing Quark Anticheat Kernel Module...\n");

    // 1. Create Netlink socket for Daemon communication
    nl_socket = netlink_kernel_create(&init_net, NETLINK_QUARK, &cfg);
    if (!nl_socket) {
        pr_err("[QUARK-KERNEL] Failed to create Netlink socket.\n");
        return -ENOMEM;
    }
    pr_info("[QUARK-KERNEL] Netlink socket created successfully.\n");

    // 2. Register security hooks
    // In a real kernel compilation, the LSM module is initialized during boot. 
    // Here we simulate the registration.
#ifdef CONFIG_SECURITY
    security_add_hooks(quark_hooks, ARRAY_SIZE(quark_hooks), "quark");
    pr_info("[QUARK-KERNEL] LSM security hooks registered successfully.\n");
#else
    pr_warn("[QUARK-KERNEL] CONFIG_SECURITY not enabled, LSM hooks could not be registered.\n");
#endif

    return 0;
}

static void __exit quark_kernel_exit(void) {
    pr_info("[QUARK-KERNEL] Exiting Quark Anticheat Kernel Module...\n");

    // Release netlink socket
    if (nl_socket) {
        netlink_kernel_release(nl_socket);
    }

    // Hooks are typically not removed dynamically in modern LSM architectures to prevent security bypasses,
    // but in standard kernel modules they would be cleaned up if supported.
}

module_init(quark_kernel_init);
module_exit(quark_kernel_exit);
