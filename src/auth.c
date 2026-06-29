#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "auth.h"
#include "types.h"

/* ── Global session state (declared in types.h as extern) ── */
User  g_users[MAX_USERS];
int   g_user_count  = 0;
int   g_logged_in   = 0;
int   g_current_role= ROLE_OPERATOR;
char  g_current_user[32] = "";

#define USERS_FILE "data/users.csv"

/* ── Persist ─────────────────────────────────────────────── */
void auth_save(void) {
    FILE *f = fopen(USERS_FILE, "w");
    if (!f) return;
    fprintf(f, "username,password,role,active\n");
    for (int i = 0; i < g_user_count; i++) {
        User *u = &g_users[i];
        fprintf(f, "%s,%s,%d,%d\n",
            u->username, u->password, u->role, u->active);
    }
    fclose(f);
}

void auth_load(void) {
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) return;
    char line[256]; fgets(line, sizeof(line), f); /* skip header */
    g_user_count = 0;
    while (fgets(line, sizeof(line), f) && g_user_count < MAX_USERS) {
        User *u = &g_users[g_user_count];
        if (sscanf(line, "%31[^,],%63[^,],%d,%d",
            u->username, u->password, &u->role, &u->active) == 4)
            g_user_count++;
    }
    fclose(f);
}

/* ── Init defaults ───────────────────────────────────────── */
void auth_init(void) {
    /* Ensure data directory exists before trying to read/write */
    system("mkdir -p data 2>/dev/null");
    auth_load();
    if (g_user_count == 0) {
        /* seed defaults */
        strncpy(g_users[0].username, "admin",    31);
        strncpy(g_users[0].password, "admin123", 63);
        g_users[0].role   = ROLE_ADMIN;
        g_users[0].active = 1;

        strncpy(g_users[1].username, "operator",  31);
        strncpy(g_users[1].password, "op123",     63);
        g_users[1].role   = ROLE_OPERATOR;
        g_users[1].active = 1;

        g_user_count = 2;
        auth_save();
    }
}

/* ── Login ───────────────────────────────────────────────── */
int auth_login(const char *username, const char *password) {
    for (int i = 0; i < g_user_count; i++) {
        User *u = &g_users[i];
        if (!u->active) continue;
        if (strcmp(u->username, username) == 0 &&
            strcmp(u->password, password) == 0) {
            g_logged_in    = 1;
            g_current_role = u->role;
            strncpy(g_current_user, u->username, 31);
            return 1;
        }
    }
    return 0;
}

void auth_logout(void) {
    g_logged_in = 0;
    g_current_role = ROLE_OPERATOR;
    g_current_user[0] = '\0';
}

/* ── Session JSON ────────────────────────────────────────── */
char *auth_session_json(void) {
    char *buf = malloc(256);
    if (!buf) return NULL;
    snprintf(buf, 256,
        "{\"logged_in\":%s,\"username\":\"%s\",\"role\":%d,\"role_name\":\"%s\"}",
        g_logged_in ? "true" : "false",
        g_current_user,
        g_current_role,
        g_current_role == ROLE_ADMIN ? "admin" : "operator");    return buf;
}

/* ── Users JSON ──────────────────────────────────────────── */
char *auth_users_json(void) {
    char *buf = malloc(2048);
    if (!buf) return NULL;
    int p = snprintf(buf, 2048, "[");
    for (int i = 0; i < g_user_count; i++) {
        User *u = &g_users[i];
        p += snprintf(buf+p, 2048-p,
            "%s{\"username\":\"%s\",\"role\":%d,\"role_name\":\"%s\",\"active\":%d}",
            i ? "," : "",
            u->username,
            u->role,
            u->role == ROLE_ADMIN ? "admin" : "operator",
            u->active);
    }
    snprintf(buf+p, 2048-p, "]");
    return buf;
}

/* ── Add / Remove ────────────────────────────────────────── */
int auth_add_user(const char *username, const char *password, int role) {
    if (g_user_count >= MAX_USERS) return -1;
    /* check duplicate */
    for (int i = 0; i < g_user_count; i++)
        if (strcmp(g_users[i].username, username) == 0) return -2;
    User *u = &g_users[g_user_count++];
    strncpy(u->username, username, 31);
    strncpy(u->password, password, 63);
    u->role   = role;
    u->active = 1;
    auth_save();
    return 0;
}

int auth_remove_user(const char *username) {
    for (int i = 0; i < g_user_count; i++) {
        if (strcmp(g_users[i].username, username) == 0) {
            /* don't allow removing last admin */
            if (g_users[i].role == ROLE_ADMIN) {
                int admins = 0;
                for (int j = 0; j < g_user_count; j++)
                    if (g_users[j].role == ROLE_ADMIN && g_users[j].active) admins++;
                if (admins <= 1) return -2;
            }
            /* shift left */
            memmove(&g_users[i], &g_users[i+1],
                (g_user_count-i-1)*sizeof(User));
            g_user_count--;
            auth_save();
            return 0;
        }
    }
    return -1;
}
