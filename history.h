#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    char username[100];
    char source[100];
    char destination[100];
    float distance;
    float co2;
} routerecord;

// Save a route record
void savehistory(char username[], char source[], char destination[], float distance, float co2) {
    FILE *fp = fopen("history.txt", "a");
    if (fp == NULL) {
        printf("Error: Unable to open history.txt for writing!\n");
        return;
    }
    fprintf(fp, "%s %s %s %.2f %.2f\n", username, source, destination, distance, co2);
    fclose(fp);
    printf("\n Route saved successfully!\n");
}

// Display all route history
void showhistory() {
    char user[100], src[100], dest[100];
    float dist, co2;
    FILE *fp = fopen("history.txt", "r");
    if (fp == NULL) {
        printf(" No history found.\n");
        return;
    }

    printf("\n===========  All Route History ===========\n");
    int count = 0;
    while (fscanf(fp, "%s %s %s %f %f", user, src, dest, &dist, &co2) == 5) {
        printf("%-10s | %-10s -> %-10s | %.2f km | CO2: %.2f kg\n", user, src, dest, dist, co2);
        count++;
    }
    if (count == 0)
        printf("No route records available.\n");
    fclose(fp);
}

// Show routes for a specific user
void showuserhistory(char username[]) {
    char user[100], src[100], dest[100];
    float dist, co2;
    int found = 0;

    FILE *fp = fopen("history.txt", "r");
    if (fp == NULL) {
        printf(" No history file found.\n");
        return;
    }

    printf("\n===========  Route History for %s ===========\n", username);
    while (fscanf(fp, "%s %s %s %f %f", user, src, dest, &dist, &co2) == 5) {
        if (strcmp(user, username) == 0) {
            printf("%-10s -> %-10s | %.2f km | CO2: %.2f kg\n", src, dest, dist, co2);
            found = 1;
        }
    }
    if (!found)
        printf("No route history found for user %s.\n", username);
    fclose(fp);
}

// Delete all history for a given user
void deleteuserhistory(const char *username) {
    FILE *fp = fopen("history.txt", "r");
    FILE *temp = fopen("temp.txt", "w");
    char user[100], src[100], dest[100];
    float dist, co2;
    int deleted = 0;

    if (!fp || !temp) {
        printf("Error opening file.\n");
        return;
    }

    while (fscanf(fp, "%s %s %s %f %f", user, src, dest, &dist, &co2) == 5) {
        if (strcmp(user, username) != 0)
            fprintf(temp, "%s %s %s %.2f %.2f\n", user, src, dest, dist, co2);
        else
            deleted++;
    }

    fclose(fp);
    fclose(temp);
    remove("history.txt");
    rename("temp.txt", "history.txt");

    if (deleted > 0)
        printf(" Deleted %d route(s) for user %s.\n", deleted, username);
    else
        printf("No records found for user %s.\n", username);
}

// Delete all records (Admin use)
void clearallhistory() {
    FILE *fp = fopen("history.txt", "w");
    if (fp) {
        fclose(fp);
        printf("🧹 All history records cleared successfully!\n");
    } else {
        printf("Error clearing history.\n");
    }
}

// Show top 3 longest routes
void showtoproutes() {
    routerecord routes[100];
    int count = 0;

    FILE *fp = fopen("history.txt", "r");
    if (!fp) {
        printf("No history found.\n");
        return;
    }

    while (fscanf(fp, "%s %s %s %f %f", routes[count].username, routes[count].source,
                  routes[count].destination, &routes[count].distance, &routes[count].co2) == 5) {
        count++;
    }
    fclose(fp);

    // Sort by distance descending
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (routes[j].distance > routes[i].distance) {
                routerecord temp = routes[i];
                routes[i] = routes[j];
                routes[j] = temp;
            }
        }
    }

    printf("\n Top 3 Longest Routes:\n");
    for (int i = 0; i < count && i < 3; i++) {
        printf("%d) %s: %s -> %s | %.2f km | CO2 %.2f kg\n",
               i + 1, routes[i].username, routes[i].source,
               routes[i].destination, routes[i].distance, routes[i].co2);
    }
}

// Show total CO2 and total distance for one user
void showuserstats(char username[]) {
    FILE *fp = fopen("history.txt", "r");
    if (!fp) {
        printf("No history found.\n");
        return;
    }

    char user[100], src[100], dest[100];
    float dist, co2, totalDist = 0, totalCO2 = 0;
    int count = 0;

    while (fscanf(fp, "%s %s %s %f %f", user, src, dest, &dist, &co2) == 5) {
        if (strcmp(user, username) == 0) {
            totalDist += dist;
            totalCO2 += co2;
            count++;
        }
    }
    fclose(fp);

    if (count == 0) {
        printf("No history for %s.\n", username);
        return;
    }

    printf("\n User Summary for %s\n", username);
    printf("Total Trips: %d\nTotal Distance: %.2f km\nTotal CO2: %.2f kg\n", count, totalDist, totalCO2);
}
