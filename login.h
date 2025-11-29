#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define USER_FILE "users.txt"

// Register a new user
void registerUser() {
    char username[50], password[50];

    printf("Enter username: ");
    scanf("%s", username);
    printf("Enter password: ");
    scanf("%s", password);

    // Check if username already exists
    FILE *fp = fopen(USER_FILE, "r");
    if (fp) {
        char uname[50], pass[50];
        while(fscanf(fp, "%s %s", uname, pass) == 2) {
            if(strcmp(uname, username) == 0) {
                printf("Username already exists!\n");
                fclose(fp);
                return;
            }
        }
        fclose(fp);
    }

    // Append new user
    fp = fopen(USER_FILE, "a");
    if(!fp) {
        printf("Error opening file.\n");
        return;
    }
    fprintf(fp, "%s %s\n", username, password);
    fclose(fp);
    printf("Registration successful!\n");
}

// Login user
int loginUser() {
    char username[50], password[50];
    printf("Enter username: ");
    scanf("%s", username);
    printf("Enter password: ");
    scanf("%s", password);
    
    FILE *fp = fopen(USER_FILE, "r");
    if(!fp) {
        printf("No users found. Please register first.\n");
        return 0;
    }

    char uname[50], pass[50];
    int success = 0;

    while(fscanf(fp, "%s %s", uname, pass) == 2) {
        if(strcmp(uname, username) == 0 && strcmp(pass, password) == 0) {
            success = 1;
            break;
        }
    }
    fclose(fp);

    if(success) {
        printf("Login successful!\n");
        
        // Record login time
        FILE *log = fopen("login_times.txt", "a");
        if(log) {
            time_t now = time(NULL);
            char *timeStr = ctime(&now);   // Converts to readable string
            timeStr[strlen(timeStr)-1] = '\0'; // Remove newline
            fprintf(log, "%s logged in at %s\n", username, timeStr);
            fclose(log);
        }
        return 1;
    } else {
        printf("Login failed!\n");
        return 0;
    }
}


// Delete account
void deleteAccount() {
    char username[50];
    printf("Enter username to delete: ");
    scanf("%s", username);

    FILE *fp = fopen(USER_FILE, "r");
    FILE *temp = fopen("temp.txt", "w");
    if(!fp || !temp) {
        printf("Error opening file.\n");
        return;
    }

    char uname[50], pass[50];
    int found = 0;
    while(fscanf(fp, "%s %s", uname, pass) == 2) {
        if(strcmp(uname, username) != 0) {
            fprintf(temp, "%s %s\n", uname, pass);
        } else {
            found = 1;
        }
    }

    fclose(fp);
    fclose(temp);

    remove(USER_FILE);
    rename("temp.txt", USER_FILE);

    if(found) printf("Account deleted successfully.\n");
    else printf("Username not found.\n");
}
