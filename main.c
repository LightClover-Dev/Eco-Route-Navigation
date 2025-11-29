#include <stdio.h>
#include <stdlib.h>
#include "login.h"
#include "adb[1].h"
#include "carbon.c"
#include <unistd.h>
//#include "history.h"
void mainMenu();
void userMenu();

int main() {
    int choice;
    while(1) {
        mainMenu();
        printf("Enter choice: ");
        scanf("%d", &choice);
        getchar(); // consume newline
        switch(choice) {
            case 1:
                registerUser();
                break;
            case 2:
                if(loginUser()) {
                    userMenu();
                }
                break;
            case 3:
                deleteAccount();
                break;
            case 4:
                printf("Exiting program.\n");
                exit(0);
            default:
                printf("Invalid choice. Try again.\n");
        }
    }
    return 0;
}

void mainMenu() {
    printf("\n==== MAIN MENU ====\n");
    printf("1. Register\n");
    printf("2. Login\n");
    printf("3. Delete Account\n");
    printf("4. Exit\n");
}

void userMenu() {
    int choice;
    char username;
    printf("Enter username to continue: ");
    scanf("%s",&username);
    while(1) {
        printf("\n==== USER MENU ====\n");
        printf("1. Shortest Path\n");
        printf("2. Eco Carbon Factor Path\n");
        printf("3. Route History\n");
        printf("4. Logout\n");
        printf("Enter choice: ");
        scanf("%d", &choice);
        getchar(); // consume newline
        switch(choice) {
            case 1:
                ecopath(username);
                break;
            case 2:
                shortp();
                break;
            case 3:
                while (1) {
                    printf("\n======Route History Menu=======\n");
                    printf("1) View Route History\n");
                    printf("2) Delete Route by ID\n");
                    printf("3) View Login History\n");
                    printf("Choice: "); scanf("%d", &choice);
                     if (choice == 1) {
                       // showhistory();
                    } else if (choice == 2) {
                        //deleteuserhistory();
                    } else if (choice == 3) {
                       // viewLoginHistory();
                    } else if (choice == 4) {
                        printf("Logging out...\n");
                        break;
                    } else {
                        printf("Invalid choice.\n");
                    }
                }
            case 4:
                printf("Logged out.\n");
                return;
            default:
                printf("Invalid choice. Try again.\n");
        }
    }
}
