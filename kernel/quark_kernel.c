/*
 * Quark Anticheat - Real Kernel Module (C)
 *
 * This file implements the Ring 0 component of Quark Anticheat.
 * It uses kretprobes to dynamically hook ptrace_may_access, blocking
 * unauthorized attempts to read or write the memory of protected processes
 * (such as writing to /proc/PID/mem or attaching via ptrace).
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <net/sock.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martinez Alarcon, Gabriel Sebastian & Merino De Rui, Stefano Nahuel");
MODULE_DESCRIPTION("Quark Anticheat - Ring 0 Security Module (kretprobes & Netlink)");
MODULE_VERSION("0.2.0");

#define NETLINK_QUARK 31 // Custom netlink protocol number
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
    pr_warn("[QUARK-KERNEL] Protected PID table full, cannot protect PID %d\n",
            pid);
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
      pr_info("[QUARK-KERNEL] Process %d removed from Quark protection.\n",
              pid);
      break;
    }
  }
  spin_unlock_irqrestore(&quark_lock, flags);
}

/*
 * Entry handler: runs before ptrace_may_access executes.
 * We extract the first argument (struct task_struct *task) and save it in
 * ri->data. Under x86_64, the first argument is passed in the DI register
 * (regs->di).
 */
static int ptrace_may_access_entry_handler(struct kretprobe_instance *ri,
                                           struct pt_regs *regs) {
#ifdef CONFIG_X86_64
  struct task_struct *task = (struct task_struct *)regs->di;
  *((struct task_struct **)ri->data) = task;
#else
  *((struct task_struct **)ri->data) = NULL;
#endif
  return 0;
}

/*
 * Return handler: runs after ptrace_may_access finishes.
 * We inspect if the target task is protected. If so, and the caller is not
 * the process itself, we override the return value (rax) to 0 (false/access
 * denied).
 */
static int ptrace_may_access_ret_handler(struct kretprobe_instance *ri,
                                         struct pt_regs *regs) {
  struct task_struct *task = *((struct task_struct **)ri->data);
  pid_t target_pid;
  pid_t parent_pid;

  if (!task) {
    return 0;
  }

  target_pid = task_pid_vnr(task);
  parent_pid = task_pid_vnr(current);

  if (is_pid_protected(target_pid)) {
    // Allow the process to access itself, block others
    if (target_pid != parent_pid) {
      pr_warn("[QUARK-KERNEL ALERT] Blocked memory/ptrace access to protected "
              "process %d by PID %d!\n",
              target_pid, parent_pid);

#ifdef CONFIG_X86_64
      // Override rax register (return value) to 0 (false)
      regs->ax = 0;
#endif
    }
  }
  return 0;
}

static struct kretprobe quark_kretprobe = {
    .handler = ptrace_may_access_ret_handler,
    .entry_handler = ptrace_may_access_entry_handler,
    .data_size = sizeof(struct task_struct *),
    .maxactive = 64,
};

/*
 * Netlink receiver callback. Receives commands from userspace daemon.
 */
static void quark_nl_recv_msg(struct sk_buff *skb) {
  struct nlmsghdr *nlh;
  int pid;
  int msg_type;

  nlh = (struct nlmsghdr *)skb->data;
  msg_type = nlh->nlmsg_type;

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

static int __init quark_kernel_init(void) {
  struct netlink_kernel_cfg cfg = {
      .input = quark_nl_recv_msg,
  };
  int ret;

  pr_info("[QUARK-KERNEL] Initializing Quark Anticheat Kernel Module...\n");

  // 1. Create Netlink socket for Daemon communication
  nl_socket = netlink_kernel_create(&init_net, NETLINK_QUARK, &cfg);
  if (!nl_socket) {
    pr_err("[QUARK-KERNEL] Failed to create Netlink socket.\n");
    return -ENOMEM;
  }
  pr_info("[QUARK-KERNEL] Netlink socket created successfully.\n");

  // 2. Register kretprobe
  quark_kretprobe.kp.symbol_name = "ptrace_may_access";
  ret = register_kretprobe(&quark_kretprobe);
  if (ret < 0) {
    pr_err("[QUARK-KERNEL] Failed to register kretprobe on ptrace_may_access: "
           "%d\n",
           ret);
    netlink_kernel_release(nl_socket);
    return ret;
  }

  pr_info("[QUARK-KERNEL] Successfully registered kretprobe on "
          "ptrace_may_access.\n");
  return 0;
}

static void __exit quark_kernel_exit(void) {
  pr_info("[QUARK-KERNEL] Exiting Quark Anticheat Kernel Module...\n");

  // Unregister kretprobe
  unregister_kretprobe(&quark_kretprobe);

  // Release netlink socket
  if (nl_socket) {
    netlink_kernel_release(nl_socket);
  }
}

module_init(quark_kernel_init);
module_exit(quark_kernel_exit);
