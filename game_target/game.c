#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include "../sdk/quark_sdk.h"

// Define a macro for PR_SET_PTRACER if not available (some older headers)
#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

// Target variables we want to protect
int health = 100;
int score = 0;

void print_status(void) {
    printf("\n--- [GAME STATE] ---\n");
    printf("PID:            %d\n", getpid());
    printf("Health address: %p -> Value: %d\n", (void*)&health, health);
    printf("Score address:  %p -> Value: %d\n", (void*)&score, score);
    printf("--------------------\n");
}

int main(void) {
    // 1. Enable debugging/memory read by other processes (bypasses Yama ptrace_scope = 1)
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) == -1) {
        perror("[GAME] Warning: prctl(PR_SET_PTRACER) failed. Quark daemon might fail to read memory");
    } else {
        printf("[GAME] prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY) set successfully.\n");
    }
    
    // 2. Initialize Quark SDK
    printf("[GAME] Connecting to Quark Anticheat...\n");
    if (quark_sdk_init() == -1) {
        printf("[GAME] WARNING: Quark Anticheat Daemon not detected. Running without protection.\n");
    } else {
        // 3. Register target variables
        quark_sdk_register_var(&health, sizeof(health), "health");
        quark_sdk_register_var(&score, sizeof(score), "score");
        printf("[GAME] Monitored variables registered with Quark Anticheat Daemon.\n");
    }
    
    print_status();
    
    char choice;
    while (1) {
        printf("\nCommands: [h] Take Damage (-10 Health), [s] Add Score (+100), [p] Print Status, [q] Quit\n");
        printf("Enter command: ");
        fflush(stdout);
        
        // Read input
        if (scanf(" %c", &choice) != 1) {
            break;
        }
        
        if (choice == 'q' || choice == 'Q') {
            printf("[GAME] Exiting...\n");
            break;
        }
        
        switch (choice) {
            case 'h':
            case 'H':
                health -= 10;
                if (health < 0) health = 0;
                printf("[GAME] Player took damage. Health is now: %d\n", health);
                
                // Notify Quark of the legitimate change
                quark_sdk_update_var(&health, health);
                break;
                
            case 's':
            case 'S':
                score += 100;
                printf("[GAME] Player gained points. Score is now: %d\n", score);
                
                // Notify Quark of the legitimate change
                quark_sdk_update_var(&score, score);
                break;
                
            case 'p':
            case 'P':
                print_status();
                break;
                
            default:
                printf("[GAME] Unknown command '%c'.\n", choice);
                break;
        }
    }
    
    // Close Quark connection
    quark_sdk_close();
    return 0;
}
