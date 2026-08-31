#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/netlink.h>

#define NETLINK_QUARK 31

// Mirrors kernel/quark_kernel.c's struct quark_protect_response.
struct quark_protect_response {
    uint8_t ok;
    uint8_t is_testing_build;
    uint8_t reserved[2];
    char version[16];
};

// The kernel replies over the same Netlink socket for command 1 (protect).
// This is a local round trip to a just-loaded module, so a short timeout is
// plenty; if it fires, quark_daemon (our only caller) must not hang waiting
// on us -- it services one client connection at a time.
#define QUARK_CLI_RECV_TIMEOUT_SEC 3

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

    if (command == 1) {
        struct timeval tv;
        tv.tv_sec = QUARK_CLI_RECV_TIMEOUT_SEC;
        tv.tv_usec = 0;
        if (setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            perror("[QUARK-CLI] Warning: failed to set recv timeout");
        }
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
    
    printf("[QUARK-CLI] Sending command %d for PID %d to kernel...\n", command, target_pid);
    
    if (sendto(sock_fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        perror("[QUARK-CLI] Error sending netlink message. Is the kernel module 'quark_kernel' loaded?");
        free(nlh);
        close(sock_fd);
        return 1;
    }
    
    printf("[QUARK-CLI] Command sent successfully.\n");
    free(nlh);

    if (command != 1) {
        // Unprotect stays fire-and-forget: nothing waits on it.
        close(sock_fd);
        return 0;
    }

    // Protect: wait for the kernel's ok/testing-build/version reply. Printed
    // as parseable "KEY:VALUE" lines so quark_daemon can scrape them out of
    // our already-captured stdout without a separate IPC channel.
    uint8_t resp_buf[NLMSG_SPACE(sizeof(struct quark_protect_response))];
    ssize_t n = recv(sock_fd, resp_buf, sizeof(resp_buf), 0);
    close(sock_fd);

    if (n < 0) {
        perror("[QUARK-CLI] Error waiting for kernel response (timed out or module not loaded?)");
        return 1;
    }

    struct nlmsghdr *resp_nlh = (struct nlmsghdr *)resp_buf;
    if (n < (ssize_t)NLMSG_SPACE(sizeof(struct quark_protect_response)) ||
        resp_nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct quark_protect_response))) {
        fprintf(stderr, "[QUARK-CLI] Malformed response from kernel\n");
        return 1;
    }

    struct quark_protect_response resp;
    memcpy(&resp, NLMSG_DATA(resp_nlh), sizeof(resp));
    resp.version[sizeof(resp.version) - 1] = '\0';

    printf("QUARK_OK:%d\n", resp.ok);
    printf("QUARK_VERSION:%s\n", resp.version);
    printf("QUARK_TESTING_BUILD:%d\n", resp.is_testing_build);

    return resp.ok ? 0 : 1;
}
