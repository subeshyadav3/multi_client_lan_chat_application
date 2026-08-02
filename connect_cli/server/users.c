/* users.c - user account storage (config/users.cred).
 *
 * Passwords are stored as SHA-256 hex strings. All functions that touch
 * user_list expect the caller to hold user_mutex (except the pure lookups,
 * which are also expected to run under user_mutex).
 */
#include "server.h"

pthread_mutex_t user_mutex = PTHREAD_MUTEX_INITIALIZER;
UserAccount *user_list = NULL;

static bool user_exists(const char *username) {
    if (!username || !username[0]) return false;
    for (UserAccount *u = user_list; u; u = u->next) {
        if (strcmp(u->username, username) == 0) return true;
    }
    return false;
}

/* True if `s` is a 64-char lowercase/uppercase hex string (a stored SHA-256). */
static bool is_hex64(const char *s) {
    if (!s || strlen(s) != 64) return false;
    for (const char *p = s; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F')))
            return false;
    }
    return true;
}

/* Store a password exactly as given (used for pre-hashed entries on load). */
static bool user_create_plain(const char *username, const char *password) {
    if (!username || !username[0] || !password || user_exists(username)) return false;
    UserAccount *u = calloc(1, sizeof(UserAccount));
    if (!u) return false;
    strncpy(u->username, username, MAX_USERNAME - 1);
    strncpy(u->password, password, sizeof(u->password) - 1);
    u->active = true;
    u->next = user_list;
    user_list = u;
    return true;
}

/* Create a user, storing the SHA-256 hash of the password. */
bool user_create(const char *username, const char *password) {
    char hash[65];
    sha256_hex(password, hash);
    return user_create_plain(username, hash);
}

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

int account_remove(const char *username) {
    UserAccount **pp = &user_list;
    while (*pp) {
        if (strcmp((*pp)->username, username) == 0) {
            UserAccount *t = *pp;
            *pp = (*pp)->next;
            free(t);
            return 1;
        }
        pp = &((*pp)->next);
    }
    return 0;
}

/* Rewrite config/users.cred with the (hashed) passwords. */
void users_save(void) {
    FILE *f = fopen("config/users.cred", "w");
    if (!f) return;
    for (UserAccount *u = user_list; u; u = u->next) {
        fprintf(f, "%s:%s\n", u->username, u->password);
    }
    fclose(f);
}

void users_load(void) {
    FILE *f = fopen("config/users.cred", "r");
    if (!f) {
        log_message("INFO", "No config/users.cred found - admin must create users");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0]) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        /* Keep pre-hashed entries; hash plaintext ones (upgrades the file). */
        if (is_hex64(colon + 1)) user_create_plain(line, colon + 1);
        else user_create(line, colon + 1);
    }
    fclose(f);
}
