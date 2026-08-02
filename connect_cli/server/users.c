/* users.c - user account storage.
 *
 * Accounts live in a linked list (user_list) guarded by user_mutex and
 * are saved to / loaded from the file config/users.cred.
 *
 * Passwords are NEVER stored in plain text inside this module: we always
 * work with the SHA-256 hex digest of the password. user_create and
 * user_reset_pass hash before storing, and user_validate hashes the
 * submitted password and compares digests.
 *
 * THREAD NOTE: the public functions below assume the caller already holds
 * user_mutex (see the callers in handlers.c / server.c). This keeps the
 * locking in one obvious place.
 */
#include "server.h"

/* The account list and the lock that protects it. */
pthread_mutex_t user_mutex = PTHREAD_MUTEX_INITIALIZER;
UserAccount *user_list = NULL;

/* ------------------------------------------------------------------ */
/* Small helpers (internal)                                           */
/* ------------------------------------------------------------------ */

/* True if an account with this username already exists. */
static bool user_exists(const char *username) {
    if (!username || !username[0]) return false;
    for (UserAccount *u = user_list; u; u = u->next) {
        if (strcmp(u->username, username) == 0) return true;
    }
    return false;
}

/* Is `s` a 64-character hex string (i.e. already a SHA-256 digest)?
 * We use this while loading the file: stored digests we keep as-is,
 * plain-text passwords we hash before saving (this upgrades the file). */
static bool is_hex64(const char *s) {
    if (!s || strlen(s) != 64) return false;
    for (const char *p = s; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F')))
            return false;
    }
    return true;
}

/* Create an account storing the password EXACTLY as given. "Plain" here
 * means "no hashing right now" - the caller decides what string to pass.
 * Used both to store a pre-computed digest and as the base for hashing. */
static bool user_create_plain(const char *username, const char *password) {
    if (!username || !username[0] || !password || user_exists(username)) return false;
    UserAccount *u = calloc(1, sizeof(UserAccount));
    if (!u) return false;
    strncpy(u->username, username, MAX_USERNAME - 1);
    strncpy(u->password, password, sizeof(u->password) - 1);
    u->active = true;
    u->next = user_list;   /* insert at the head */
    user_list = u;
    return true;
}

/* ------------------------------------------------------------------ */
/* Public account operations                                          */
/* ------------------------------------------------------------------ */

/* Create a new account, storing the SHA-256 digest of `password`. */
bool user_create(const char *username, const char *password) {
    char hash[65];
    sha256_hex(password, hash);
    return user_create_plain(username, hash);
}

/* Reset an account's password to the digest of `password`. */
bool user_reset_pass(const char *username, const char *password) {
    char hash[65];
    sha256_hex(password, hash);
    for (UserAccount *u = user_list; u; u = u->next) {
        if (strcmp(u->username, username) == 0) {
            strncpy(u->password, hash, sizeof(u->password) - 1);
            return true;
        }
    }
    return false;
}

/* Check a login: true only if the account exists and the SHA-256 of the
 * submitted password matches the stored digest. */
bool user_validate(const char *username, const char *password) {
    char hash[65];
    sha256_hex(password, hash);
    for (UserAccount *u = user_list; u; u = u->next) {
        if (strcmp(u->username, username) == 0) {
            return strcmp(u->password, hash) == 0;
        }
    }
    return false;
}

/* Remove an account by username. Returns 1 if removed, 0 if not found. */
int account_remove(const char *username) {
    UserAccount **pp = &user_list;
    while (*pp) {
        if (strcmp((*pp)->username, username) == 0) {
            UserAccount *t = *pp;
            *pp = (*pp)->next;   /* unlink */
            free(t);
            return 1;
        }
        pp = &((*pp)->next);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Persistence (config/users.cred)                                   */
/* ------------------------------------------------------------------ */

/* Write every account to config/users.cred as  username:passwordDigest
 * one per line. Called whenever an admin changes the account list. */
void users_save(void) {
    FILE *f = fopen("config/users.cred", "w");
    if (!f) return;   /* if we can't write, there is nothing we can do */
    for (UserAccount *u = user_list; u; u = u->next) {
        fprintf(f, "%s:%s\n", u->username, u->password);
    }
    fclose(f);
}

/* Read config/users.cred into the account list at startup.
 * Lines already holding a half-digest stay as-is; plain-text passwords
 * are hashed so the file is upgraded to digests automatically. */
void users_load(void) {
    FILE *f = fopen("config/users.cred", "r");
    if (!f) {
        log_message("INFO", "No config/users.cred found - admin must create users");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;   /* drop newline */
        if (!line[0]) continue;                            /* skip empty */
        char *colon = strchr(line, ':');
        if (!colon) continue;   /* malformed line: skip it */
        *colon = 0;             /* split into name : password */
        if (is_hex64(colon + 1)) user_create_plain(line, colon + 1);
        else                     user_create(line, colon + 1);
    }
    fclose(f);
}
