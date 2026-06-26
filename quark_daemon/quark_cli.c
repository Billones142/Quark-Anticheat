#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NETLINK_QUARK 31

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <1=protect/2=unprotect> <pid>\n", argv[0]);
    return 1;
  }

  int command = atoi(argv[1]);
  int target_pid = atoi(argv[2]);

  if (command != 1 && command != 2) {
    fprintf(stderr, "Invalid command. Use 1 for protect, 2 for unprotect.\n");
    return 1;
  }

  int sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_QUARK);
  if (sock_fd < 0) {
    perror("[QUARK-CLI] Error creating netlink socket");
    return 1;
  }

  struct sockaddr_nl src_addr;
  memset(&src_addr, 0, sizeof(src_addr));
  src_addr.nl_family = AF_NETLINK;
  src_addr.nl_pid = getpid();

  if (bind(sock_fd, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
    perror("[QUARK-CLI] Error binding netlink socket");
    close(sock_fd);
    return 1;
  }

  struct sockaddr_nl dest_addr;
  memset(&dest_addr, 0, sizeof(dest_addr));
  dest_addr.nl_family = AF_NETLINK;
  dest_addr.nl_pid = 0; // Kernel

  // Allocate space for netlink message header + int payload
  struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(sizeof(int)));
  if (!nlh) {
    fprintf(stderr, "[QUARK-CLI] Out of memory\n");
    close(sock_fd);
    return 1;
  }
  memset(nlh, 0, NLMSG_SPACE(sizeof(int)));
  nlh->nlmsg_len = NLMSG_LENGTH(sizeof(int));
  nlh->nlmsg_pid = getpid();
  nlh->nlmsg_flags = 0;
  nlh->nlmsg_type = command;

  *(int *)NLMSG_DATA(nlh) = target_pid;

  printf("[QUARK-CLI] Sending command %d for PID %d to kernel...\n", command,
         target_pid);

  if (sendto(sock_fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&dest_addr,
             sizeof(dest_addr)) < 0) {
    perror("[QUARK-CLI] Error sending netlink message. Is the kernel module "
           "'quark_kernel' loaded?");
    free(nlh);
    close(sock_fd);
    return 1;
  }

  printf("[QUARK-CLI] Command sent successfully.\n");

  free(nlh);
  close(sock_fd);
  return 0;
}
