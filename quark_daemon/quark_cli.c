#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/netlink.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>

#define NETLINK_QUARK 31

// Private half of tooling/keys/generate-testing-key.sh's testing keypair. Never
// committed (see .gitignore); must be installed here, root-only-readable, on
// whichever box actually runs quark_cli. Matches kernel/quark_pubkey.h.
#define QUARK_SIGNING_KEY_PATH "/opt/quark-anticheat/keys/quark_signing_key.pem"

#define QUARK_MAX_SIG_LEN 72

// Mirrors kernel/quark_kernel.c's struct quark_netlink_request. Every protect/
// unprotect command is signed (ECDSA P-256 over SHA-256 of {command, pid, nonce})
// and nonce-stamped, so the kernel module only ever honors a command actually
// produced by whoever holds the private key -- not just anything that can open a
// netlink socket to this protocol.
struct quark_netlink_request {
    int32_t pid;
    uint64_t nonce;
    uint16_t sig_len;
    uint8_t sig[QUARK_MAX_SIG_LEN];
};

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

// Builds the {command, pid, nonce} byte layout the kernel module hashes and
// verifies the signature over (kernel/quark_kernel.c's quark_nl_recv_msg) and signs
// it with the testing private key. Returns 0 on success, filling req->sig/sig_len;
// -1 on any failure (key missing/unreadable, OpenSSL error, etc).
static int quark_sign_request(uint8_t command, int32_t pid, uint64_t nonce,
                               struct quark_netlink_request *req) {
    uint8_t signed_buf[1 + sizeof(int32_t) + sizeof(uint64_t)];
    FILE *fp;
    EVP_PKEY *pkey;
    EVP_MD_CTX *mdctx;
    size_t sig_len;
    int ret = -1;

    signed_buf[0] = command;
    memcpy(&signed_buf[1], &pid, sizeof(pid));
    memcpy(&signed_buf[1 + sizeof(pid)], &nonce, sizeof(nonce));

    fp = fopen(QUARK_SIGNING_KEY_PATH, "r");
    if (!fp) {
        perror("[QUARK-CLI] Failed to open signing key (" QUARK_SIGNING_KEY_PATH ")");
        return -1;
    }
    pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);
    if (!pkey) {
        fprintf(stderr, "[QUARK-CLI] Failed to parse signing key: %s\n",
                ERR_reason_error_string(ERR_get_error()));
        return -1;
    }

    mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        EVP_PKEY_free(pkey);
        return -1;
    }

    sig_len = sizeof(req->sig);
    if (EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, pkey) == 1 &&
        EVP_DigestSign(mdctx, req->sig, &sig_len, signed_buf, sizeof(signed_buf)) == 1) {
        req->sig_len = (uint16_t)sig_len;
        ret = 0;
    } else {
        fprintf(stderr, "[QUARK-CLI] Signing failed: %s\n",
                ERR_reason_error_string(ERR_get_error()));
    }

    EVP_MD_CTX_free(mdctx);
    EVP_PKEY_free(pkey);
    return ret;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <1=protect/2=unprotect/3=register-trusted-monitor> <pid>\n", argv[0]);
        return 1;
    }

    int command = atoi(argv[1]);
    int target_pid = atoi(argv[2]);

    if (command != 1 && command != 2 && command != 3) {
        fprintf(stderr, "Invalid command. Use 1 for protect, 2 for unprotect, 3 to register a trusted monitor pid.\n");
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

    if (command == 1 || command == 3) {
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

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t nonce = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    struct quark_netlink_request req;
    memset(&req, 0, sizeof(req));
    req.pid = target_pid;
    req.nonce = nonce;

    if (quark_sign_request((uint8_t)command, target_pid, nonce, &req) != 0) {
        fprintf(stderr, "[QUARK-CLI] Failed to sign request; refusing to send it unsigned.\n");
        close(sock_fd);
        return 1;
    }

    // Allocate space for netlink message header + signed request payload
    struct nlmsghdr *nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(sizeof(req)));
    if (!nlh) {
        fprintf(stderr, "[QUARK-CLI] Out of memory\n");
        close(sock_fd);
        return 1;
    }
    memset(nlh, 0, NLMSG_SPACE(sizeof(req)));
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(req));
    nlh->nlmsg_pid = getpid();
    nlh->nlmsg_flags = 0;
    nlh->nlmsg_type = command;

    memcpy(NLMSG_DATA(nlh), &req, sizeof(req));

    printf("[QUARK-CLI] Sending signed command %d for PID %d to kernel...\n", command, target_pid);
    
    if (sendto(sock_fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        perror("[QUARK-CLI] Error sending netlink message. Is the kernel module 'quark_kernel' loaded?");
        free(nlh);
        close(sock_fd);
        return 1;
    }
    
    printf("[QUARK-CLI] Command sent successfully.\n");
    free(nlh);

    if (command != 1 && command != 3) {
        // Unprotect stays fire-and-forget: nothing waits on it.
        close(sock_fd);
        return 0;
    }

    // Protect and register-trusted-monitor: wait for the kernel's ok/testing-build/
    // version reply. Printed as parseable "KEY:VALUE" lines so quark_daemon can
    // scrape them out of our already-captured stdout without a separate IPC channel.
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
