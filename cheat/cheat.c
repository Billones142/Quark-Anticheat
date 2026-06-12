#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <pid> <address_hex> <new_value>\n", argv[0]);
        fprintf(stderr, "Example: %s 12345 0x7ffd5a34bc10 9999\n", argv[0]);
        return 1;
    }
    
    pid_t pid = (pid_t)atoi(argv[1]);
    
    // Parse hex address
    unsigned long long address;
    if (sscanf(argv[2], "%llx", &address) != 1) {
        // Try parsing with 0x prefix if omitted or included
        if (sscanf(argv[2], "0x%llx", &address) != 1) {
            fprintf(stderr, "Error: Invalid address format. Must be hexadecimal (e.g. 0x555555556010 or 555555556010).\n");
            return 1;
        }
    }
    
    int new_value = atoi(argv[3]);
    
    // Open process memory
    char mem_path[64];
    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    
    printf("[CHEAT] Opening %s for writing...\n", mem_path);
    int fd = open(mem_path, O_RDWR);
    if (fd == -1) {
        perror("[CHEAT] Error: Failed to open /proc/PID/mem. Are you sure the PID is correct and you have permission?");
        fprintf(stderr, "[CHEAT] Hint: The target process must permit debugging (prctl PR_SET_PTRACER) if Yama ptrace_scope is enabled.\n");
        return 1;
    }
    
    // Seek to the target address
    if (lseek(fd, (off_t)address, SEEK_SET) == (off_t)-1) {
        perror("[CHEAT] Error: Failed to seek to address");
        close(fd);
        return 1;
    }
    
    // Write the new value (4 bytes integer)
    printf("[CHEAT] Writing new value %d to address 0x%llX...\n", new_value, address);
    ssize_t bytes_written = write(fd, &new_value, sizeof(int));
    if (bytes_written != sizeof(int)) {
        perror("[CHEAT] Error: Failed to write to memory");
        close(fd);
        return 1;
    }
    
    printf("[CHEAT] SUCCESS! Wrote %zd bytes. Target variable updated.\n", bytes_written);
    
    close(fd);
    return 0;
}
