#ifndef AUTH_H
#define AUTH_H
#include "types.h"

/* Login returns 1=success 0=fail */
int  auth_login(const char *username, const char *password);
void auth_logout(void);

/* Returns JSON string of current session (caller frees) */
char *auth_session_json(void);

/* Returns JSON array of all users (admin only) */
char *auth_users_json(void);

/* Add / remove users (admin only) */
int  auth_add_user(const char *username, const char *password, int role);
int  auth_remove_user(const char *username);

/* Initialise default users if file missing */
void auth_init(void);

/* Persist / load */
void auth_save(void);
void auth_load(void);

#endif
