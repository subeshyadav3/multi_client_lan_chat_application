#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include "../shared/protocol.h"
#include "../shared/constants.h"
#include "../shared/sha256.h"
#include "logger.h"
#include "room.h"

/* ── File upload permission token system ── */
#define MAX_CONCURRENT_UPLOADS 5
#define UPLOAD_TIMEOUT_SEC 30
#define TOKEN_LEN 16
#define ROOM_HISTORY_MAX 50

typedef struct {
    char token[TOKEN_LEN + 1];
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char recipient[MAX_USERNAME];
    long size;
    time_t started_at;
    bool active;
} UploadSlot;

static UploadSlot upload_slots[MAX_CONCURRENT_UPLOADS];
static pthread_mutex_t upload_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t server_running = 1;

/* Deactivate upload slots whose grant has gone stale without data. */
static void upload_expire_stale(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&upload_mutex);
    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
        if (upload_slots[i].active &&
            (now - upload_slots[i].started_at) > UPLOAD_TIMEOUT_SEC) {
            upload_slots[i].active = false;
        }
    }
    pthread_mutex_unlock(&upload_mutex);
}

typedef struct Client {
    int sockfd;
    struct sockaddr_in addr;
    char username[MAX_USERNAME];
    int status;
    time_t login_time;
    pthread_t thread;
    bool active;
    bool is_admin;
    char current_room[MAX_ROOM_NAME];
    struct Client *next;
} Client;

static Client *client_list = NULL;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
static int server_sock = -1;

static int total_messages = 0, total_privmsgs = 0, total_files = 0;
static char admin_user[64] = "admin";
static char admin_pass[128] = "";
static char admin_pass_hash[65] = "";

/* ── user accounts ── */
typedef struct UserAccount {
    char username[MAX_USERNAME];
    char password[128];
    bool active;
    struct UserAccount *next;
} UserAccount;

static UserAccount *user_list = NULL;
static pthread_mutex_t user_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── room message history (late joiners see recent messages) ── */
typedef struct RoomMessage {
    char *text;
    struct RoomMessage *next;
} RoomMessage;

typedef struct {
    char room_name[MAX_ROOM_NAME];
    RoomMessage *head;
    RoomMessage *tail;
    int count;
    pthread_mutex_t lock;
} RoomHistory;

static RoomHistory room_histories[MAX_ROOMS];
static int room_history_count = 0;
static void history_init(void) {
    room_history_count = 0;
    memset(room_histories, 0, sizeof(room_histories));
}

static RoomHistory *history_for_room(const char *room) {
    for (int i = 0; i < room_history_count; i++) {
        if (strcmp(room_histories[i].room_name, room) == 0)
            return &room_histories[i];
    }
    if (room_history_count >= MAX_ROOMS) return NULL;
    RoomHistory *h = &room_histories[room_history_count++];
    strncpy(h->room_name, room, MAX_ROOM_NAME - 1);
    h->head = h->tail = NULL;
    h->count = 0;
    pthread_mutex_init(&h->lock, NULL);
    return h;
}

static void history_add(const char *room, const char *line) {
    RoomHistory *h = history_for_room(room);
    if (!h) return;
    pthread_mutex_lock(&h->lock);
    RoomMessage *msg = calloc(1, sizeof(RoomMessage));
    if (!msg) { pthread_mutex_unlock(&h->lock); return; }
    msg->text = strdup(line);

    if (h->tail) h->tail->next = msg;
    else         h->head = msg;
    h->tail = msg;
    h->count++;

    while (h->count > ROOM_HISTORY_MAX) {
        RoomMessage *old = h->head;
        h->head = old->next;
        if (!h->head) h->tail = NULL;
        free(old->text);
        free(old);
        h->count--;
    }
    pthread_mutex_unlock(&h->lock);
}

static void history_replay(int sockfd, const char *room) {
    RoomHistory *h = history_for_room(room);
    if (!h) return;
    pthread_mutex_lock(&h->lock);
    for (RoomMessage *m = h->head; m; m = m->next) {
        send(sockfd, m->text, strlen(m->text), 0);
    }
    pthread_mutex_unlock(&h->lock);
}

/* ── active file transfer tracking ── */
typedef struct FileTransfer {
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char recipient[MAX_USERNAME]; /* empty = broadcast */
    long size;
    struct FileTransfer *next;
} FileTransfer;

static FileTransfer *transfer_list = NULL;
static pthread_mutex_t transfer_mutex = PTHREAD_MUTEX_INITIALIZER;

static void transfer_add(const char *sender, const char *filename, long size, const char *recipient) {
    pthread_mutex_lock(&transfer_mutex);
    FileTransfer *t = calloc(1, sizeof(FileTransfer));
    if (t) {
        strncpy(t->sender, sender, MAX_USERNAME - 1);
        strncpy(t->filename, filename, MAX_FILENAME - 1);
        strncpy(t->recipient, recipient ? recipient : "", MAX_USERNAME - 1);
        t->size = size;
        t->next = transfer_list;
        transfer_list = t;
    }
    pthread_mutex_unlock(&transfer_mutex);
}

static void transfer_remove(const char *sender, const char *filename) {
    pthread_mutex_lock(&transfer_mutex);
    FileTransfer **pp = &transfer_list;
    while (*pp) {
        if (strcmp((*pp)->sender, sender) == 0 && strcmp((*pp)->filename, filename) == 0) {
            FileTransfer *tmp = *pp;
            *pp = (*pp)->next;
            free(tmp);
            break;
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&transfer_mutex);
}

/* Caller must hold transfer_mutex; returns true and fills a snapshot. */
static bool transfer_find(const char *sender, const char *filename, FileTransfer *out) {
    for (FileTransfer *t = transfer_list; t; t = t->next) {
        if (strcmp(t->sender, sender) == 0 && strcmp(t->filename, filename) == 0) {
            if (out) { memcpy(out, t, sizeof(FileTransfer)); out->next = NULL; }
            return true;
        }
    }
    return false;
}

static void sanitize_filename(char *dst, size_t dst_len, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '/' || c == '\\' || c == '|' || c == '\n' || c == '\r' || c == '\0') continue;
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
    if (j == 0) strncpy(dst, "file", dst_len - 1);
}

static void get_timestamp(char *buf, size_t len) {
    time_t t = time(NULL);
    strftime(buf, len, "%I:%M %p", localtime(&t));
}
/* Caller must hold user_mutex. */
static int account_remove(const char *username) {
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
static bool user_create(const char *username, const char *password) {
    char hash[65];
    sha256_hex(password, hash);
    return user_create_plain(username, hash);
}

static bool user_reset_pass(const char *username, const char *password) {
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

static bool user_validate(const char *username, const char *password) {
    char hash[65];
    sha256_hex(password, hash);
    for (UserAccount *u = user_list; u; u = u->next) {
        if (strcmp(u->username, username) == 0) {
            return strcmp(u->password, hash) == 0;
        }
    }
    return false;
}

/* Rewrite config/users.cred with hashed passwords. Caller must hold user_mutex. */
static void save_users(void) {
    FILE *f = fopen("config/users.cred", "w");
    if (!f) return;
    for (UserAccount *u = user_list; u; u = u->next) {
        fprintf(f, "%s:%s\n", u->username, u->password);
    }
    fclose(f);
}

/* Caller must hold user_mutex. */
static void load_users(void) {
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
/* Send data to a client socket with error checking; marks inactive on failure. */
static bool safe_send(Client *c, const char *msg, size_t len) {
    if (!c || !c->active || !msg || len == 0) return false;
    ssize_t n = send(c->sockfd, msg, len, 0);
    if (n <= 0) {
        c->active = false;
        return false;
    }
    return true;
}

static void broadcast_room(const char *room, const char *msg, Client *except) {
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c != except && c->active && strcmp(c->current_room, room) == 0) {
            safe_send(c, msg, strlen(msg));
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

static void broadcast(const char *msg, Client *except) {
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c != except && c->active) {
            safe_send(c, msg, strlen(msg));
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

static void send_to_user(const char *username, const char *msg) {
    if (!username || !msg) return;
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c->active && strcmp(c->username, username) == 0) {
            safe_send(c, msg, strlen(msg));
            break;
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

/* Caller must hold client_mutex. */
static Client *client_find(const char *username) {
    if (!username) return NULL;
    for (Client *c = client_list; c; c = c->next) {
        if (strcmp(c->username, username) == 0) return c;
    }
    return NULL;
}

static void client_remove(Client *target) {
    pthread_mutex_lock(&client_mutex);
    Client **pp = &client_list;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            break;
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&client_mutex);
    if (target->sockfd >= 0) close(target->sockfd);
    free(target);
}

static void broadcast_user_list(void) {
    char userlist[MAX_MESSAGE] = {0};
    pthread_mutex_lock(&client_mutex);
    for (Client *p = client_list; p; p = p->next) {
        if (!p->active) continue;
        if (userlist[0]) strncat(userlist, ",", sizeof(userlist) - strlen(userlist) - 1);
        strncat(userlist, p->username, sizeof(userlist) - strlen(userlist) - 1);
        strncat(userlist, ":1", sizeof(userlist) - strlen(userlist) - 1);
    }
    pthread_mutex_unlock(&client_mutex);
    char out[MAX_MESSAGE + 64];
    snprintf(out, sizeof(out), "USERS|%s\n", userlist);
    broadcast(out, NULL);
}

static void broadcast_room_list(void) {
    char rooms[MAX_MESSAGE] = {0};
    room_list(rooms, sizeof(rooms));
    char out[MAX_MESSAGE + 64];
    snprintf(out, sizeof(out), "ROOMS|%s\n", rooms);
    broadcast(out, NULL);
}

static void send_status_to(Client *c) {
    int online = 0;
    pthread_mutex_lock(&client_mutex);
    for (Client *p = client_list; p; p = p->next) if (p->active) online++;
    pthread_mutex_unlock(&client_mutex);
    char out[512];
    snprintf(out, sizeof(out),
        "STATUS|Online users: %d | Messages: %d | Private msgs: %d | Files offered: %d\n",
        online, total_messages, total_privmsgs, total_files);
    send(c->sockfd, out, strlen(out), 0);
}

/* Switch a client into `room`, replay history, confirm, and tell the others. */
static void finish_join(Client *c, const char *room) {
    strncpy(c->current_room, room, MAX_ROOM_NAME - 1);
    c->current_room[MAX_ROOM_NAME - 1] = '\0';
    history_replay(c->sockfd, room);
    char ok[128];
    snprintf(ok, sizeof(ok), "JOIN_OK|%s\n", room);
    send(c->sockfd, ok, strlen(ok), 0);
    char msg[256];
    snprintf(msg, sizeof(msg), "NOTIFY|%s joined the room.\n", c->username);
    broadcast_room(room, msg, c);
}
static void *handle_client(void *arg) {
    Client *c = (Client*)arg;
    char buf[BUFFER_SIZE];
    char line[BUFFER_SIZE];
    size_t line_len = 0;

    while (c->active && server_running) {
        ssize_t n = recv(c->sockfd, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';
                line_len = 0;

                /* Split on '|', preserving empty fields. */
                char *tokens[5] = {NULL};
                int parts = 0;
                {
                    char copy[BUFFER_SIZE];
                    strncpy(copy, line, sizeof(copy) - 1);
                    copy[sizeof(copy) - 1] = '\0';
                    char *p = copy;
                    while (parts < 5) {
                        char *pipe = strchr(p, '|');
                        if (pipe) *pipe = '\0';
                        tokens[parts] = strdup(p);
                        parts++;
                        if (pipe) p = pipe + 1;
                        else break;
                    }
                }
                char cmd[64] = {0}, arg1[256] = {0}, arg2[MAX_MESSAGE] = {0}, arg3[256] = {0}, arg4[256] = {0};
                if (parts >= 1 && tokens[0]) strncpy(cmd,  tokens[0], sizeof(cmd)-1);
                if (parts >= 2 && tokens[1]) strncpy(arg1, tokens[1], sizeof(arg1)-1);
                if (parts >= 3 && tokens[2]) strncpy(arg2, tokens[2], sizeof(arg2)-1);
                if (parts >= 4 && tokens[3]) strncpy(arg3, tokens[3], sizeof(arg3)-1);
                if (parts >= 5 && tokens[4]) strncpy(arg4, tokens[4], sizeof(arg4)-1);
                for (int ti = 0; ti < parts; ti++) free(tokens[ti]);

                char ts[32]; get_timestamp(ts, sizeof(ts));

                if (strcmp(cmd, "LOGIN") == 0 && parts >= 3) {
                    bool accepted = false;
                    bool is_admin_user = (strcmp(arg1, admin_user) == 0);
                    pthread_mutex_lock(&client_mutex);
                    bool duplicate = (client_find(arg1) != NULL);
                    pthread_mutex_unlock(&client_mutex);
                    if (duplicate) {
                        char err[256];
                        snprintf(err, sizeof(err), "LOGIN_FAIL|User already logged in\n");
                        send(c->sockfd, err, strlen(err), 0);
                    } else if (is_admin_user) {
                        char ah[65];
                        sha256_hex(arg2, ah);
                        if (strcmp(ah, admin_pass_hash) == 0) {
                            pthread_mutex_lock(&client_mutex);
                            if (client_find(arg1) == NULL) {
                                strncpy(c->username, arg1, MAX_USERNAME-1);
                                c->username[MAX_USERNAME-1] = '\0';
                                c->is_admin = true;
                                accepted = true;
                            }
                            pthread_mutex_unlock(&client_mutex);
                        } else {
                            char err[256];
                            snprintf(err, sizeof(err), "LOGIN_FAIL|Invalid admin password\n");
                            send(c->sockfd, err, strlen(err), 0);
                        }
                    } else {
                        pthread_mutex_lock(&user_mutex);
                        bool valid = user_validate(arg1, arg2);
                        pthread_mutex_unlock(&user_mutex);
                        if (valid) {
                            pthread_mutex_lock(&client_mutex);
                            if (client_find(arg1) == NULL) {
                                strncpy(c->username, arg1, MAX_USERNAME-1);
                                c->username[MAX_USERNAME-1] = '\0';
                                accepted = true;
                            }
                            pthread_mutex_unlock(&client_mutex);
                        } else {
                            char err[256];
                            snprintf(err, sizeof(err), "LOGIN_FAIL|Invalid username or password\n");
                            send(c->sockfd, err, strlen(err), 0);
                        }
                    }
                    if (accepted) {
                        c->active = true;
                        strncpy(c->current_room, "general", MAX_ROOM_NAME-1);
                        char ok[128]; snprintf(ok, sizeof(ok), "LOGIN_OK|%s\n", arg1);
                        send(c->sockfd, ok, strlen(ok), 0);
                        history_replay(c->sockfd, "general");
                        broadcast_user_list();
                        broadcast_room_list();
                        log_message("INFO", "User '%s' logged in from %s", arg1, inet_ntoa(c->addr.sin_addr));
                    }
                } else if (strcmp(cmd, "PUBLIC") == 0 && parts >= 2) {
                    char msg[MAX_MESSAGE + 512];
                    snprintf(msg, sizeof(msg), "PUBLIC|%s|%s|%s|%s\n", c->current_room, c->username, arg2, ts);
                    history_add(c->current_room, msg);
                    broadcast_room(c->current_room, msg, NULL);
                    total_messages++;
                    log_message("MSG", "[%s] %s: %s", c->current_room, c->username, arg2);
                } else if (strcmp(cmd, "PRIVATE") == 0 && parts >= 3) {
                    char msg[MAX_MESSAGE + 512];
                    snprintf(msg, sizeof(msg), "PRIVATE|%s|%s|%s|%s\n", c->username, arg1, arg2, ts);
                    send_to_user(arg1, msg);
                    send_to_user(c->username, msg);
                    total_privmsgs++;
                    log_message("PRIV", "%s -> %s: %s", c->username, arg1, arg2);
                } else if (strcmp(cmd, "TYPING") == 0 && parts >= 1) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "TYPING|%s|%s\n", c->current_room, c->username);
                    broadcast_room(c->current_room, msg, c);
                } else if (strcmp(cmd, "JOIN") == 0 && parts >= 2) {
                    const char *room_name = arg1;
                    const char *password = (parts >= 3) ? arg2 : "";
                    if (!room_exists(room_name)) {
                        char err[256];
                        snprintf(err, sizeof(err), "JOIN_FAIL|Room '%s' does not exist\n", room_name);
                        send(c->sockfd, err, strlen(err), 0);
                    } else if (room_is_protected(room_name) && !c->is_admin) {
                        if (!room_has_access(c->username, room_name)) {
                            if (!password[0] || !room_check_password(room_name, password)) {
                                char err[256];
                                snprintf(err, sizeof(err), "JOIN_FAIL|Room '%s' requires password\n", room_name);
                                send(c->sockfd, err, strlen(err), 0);
                            } else {
                                room_grant_access(c->username, room_name);
                                finish_join(c, room_name);
                            }
                        } else {
                            finish_join(c, room_name);
                        }
                    } else {
                        finish_join(c, room_name);
                    }
                } else if (strcmp(cmd, "LEAVE") == 0 && parts >= 2) {
                    char prev_room[MAX_ROOM_NAME];
                    strncpy(prev_room, c->current_room, sizeof(prev_room)-1);
                    strncpy(c->current_room, "general", MAX_ROOM_NAME-1);
                    c->current_room[MAX_ROOM_NAME-1] = '\0';
                    char msg[256];
                    snprintf(msg, sizeof(msg), "NOTIFY|%s left room %s.\n", c->username, prev_room);
                    broadcast_room(prev_room, msg, NULL);
                } else if (strcmp(cmd, "CREATE") == 0 && parts >= 2) {
                    if (room_create(arg1)) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "NOTIFY|Room '%s' created by %s.\n", arg1, c->username);
                        broadcast(msg, NULL);
                        broadcast_room_list();
                    }
                } else if (strcmp(cmd, "CREATE_ROOM") == 0 && parts >= 2) {
                    const char *rn = arg1;
                    const char *rt = (parts >= 3) ? arg2 : "";
                    const char *rd = (parts >= 4) ? arg3 : "";
                    const char *rp = (parts >= 5) ? arg4 : "";
                    if (room_create_extended(rn, rt, rd, rp, c->username)) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "NOTIFY|Room '%s' created by %s.\n", rn, c->username);
                        broadcast(msg, NULL);
                        broadcast_room_list();
                        char ok[128]; snprintf(ok, sizeof(ok), "ROOM_CREATED|%s\n", rn);
                        send(c->sockfd, ok, strlen(ok), 0);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Room '%s' already exists or invalid\n", rn);
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "DELETE_ROOM") == 0 && parts >= 2) {
                    if (room_delete(arg1, c->username)) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "NOTIFY|Room '%s' deleted by %s.\n", arg1, c->username);
                        broadcast(msg, NULL);
                        pthread_mutex_lock(&client_mutex);
                        for (Client *p = client_list; p; p = p->next) {
                            if (p->active && strcmp(p->current_room, arg1) == 0) {
                                strncpy(p->current_room, "general", MAX_ROOM_NAME-1);
                            }
                        }
                        pthread_mutex_unlock(&client_mutex);
                        broadcast_room_list();
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Cannot delete room '%s' (not found or not authorized)\n", arg1);
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "UPDATE_ROOM") == 0 && parts >= 4) {
                    if (room_update_field(arg1, arg2, arg3)) {
                        char ok[128];
                        snprintf(ok, sizeof(ok), "NOTIFY|Room '%s' updated.\n", arg1);
                        broadcast(ok, NULL);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Could not update room\n");
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "LIST_USERS") == 0) {
                    char userlist[MAX_MESSAGE] = {0};
                    pthread_mutex_lock(&client_mutex);
                    for (Client *p = client_list; p; p = p->next) {
                        if (p->active) {
                            if (strlen(userlist) > 0) strncat(userlist, ",", sizeof(userlist) - strlen(userlist) - 1);
                            strncat(userlist, p->username, sizeof(userlist) - strlen(userlist) - 1);
                            strncat(userlist, ":1", sizeof(userlist) - strlen(userlist) - 1);
                        }
                    }
                    pthread_mutex_unlock(&client_mutex);
                    char out[MAX_MESSAGE + 64];
                    snprintf(out, sizeof(out), "USERS|%s\n", userlist);
                    send(c->sockfd, out, strlen(out), 0);
                } else if (strcmp(cmd, "LIST_ROOMS") == 0) {
                    char roomstr[MAX_MESSAGE] = {0};
                    room_list(roomstr, sizeof(roomstr));
                    char out[MAX_MESSAGE + 64];
                    snprintf(out, sizeof(out), "ROOMS|%s\n", roomstr);
                    send(c->sockfd, out, strlen(out), 0);
                } else if (strcmp(cmd, "WHO") == 0 && parts >= 2) {
                    const char *room = arg1;
                    if (!room_exists(room)) {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Room '%s' does not exist\n", room);
                        send(c->sockfd, err, strlen(err), 0);
                    } else {
                        char who[MAX_MESSAGE] = {0};
                        pthread_mutex_lock(&client_mutex);
                        for (Client *p = client_list; p; p = p->next) {
                            if (p->active && strcmp(p->current_room, room) == 0) {
                                if (who[0]) strncat(who, ", ", sizeof(who) - strlen(who) - 1);
                                strncat(who, p->username, sizeof(who) - strlen(who) - 1);
                            }
                        }
                        pthread_mutex_unlock(&client_mutex);
                        char out[MAX_MESSAGE + 128];
                        if (who[0])
                            snprintf(out, sizeof(out), "STATUS|Users in #%s: %s\n", room, who);
                        else
                            snprintf(out, sizeof(out), "STATUS|No one else is in #%s\n", room);
                        send(c->sockfd, out, strlen(out), 0);
                    }
                } else if (strcmp(cmd, "HISTORY") == 0) {
                    /* Replay this client's current-room history (no cross-room leak). */
                    history_replay(c->sockfd, c->current_room);
                } else if (strcmp(cmd, "STATS") == 0) {
                    if (c->is_admin) {
                        send_status_to(c);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Only admin can request stats\n");
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "ANNOUNCE") == 0 && parts >= 2) {
                    if (c->is_admin) {
                        char msg[MAX_MESSAGE + 256];
                        snprintf(msg, sizeof(msg), "ANNOUNCE|%s|%s|%s\n", c->username, arg1, ts);
                        broadcast(msg, NULL);
                        log_message("CTRL", "Announcement by admin: %s", arg1);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Only admin can announce\n");
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "KICK") == 0 && parts >= 3) {
                    if (c->is_admin) {
                        Client *target = NULL;
                        pthread_mutex_lock(&client_mutex);
                        target = client_find(arg1);
                        if (target && target->active) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "KICK|%s\n", arg2);
                            send(target->sockfd, msg, strlen(msg), 0);
                            target->active = false;
                            shutdown(target->sockfd, SHUT_RDWR);
                        }
                        pthread_mutex_unlock(&client_mutex);
                        char notify[256];
                        snprintf(notify, sizeof(notify), "NOTIFY|%s was kicked: %s\n", arg1, arg2);
                        broadcast(notify, NULL);
                        broadcast_user_list();
                        log_message("CTRL", "Admin kicked %s: %s", arg1, arg2);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Only admin can kick\n");
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "CREATE_USER") == 0 && parts >= 3) {
                    if (c->is_admin) {
                        pthread_mutex_lock(&user_mutex);
                        bool created = user_create(arg1, arg2);
                        if (created) save_users();
                        pthread_mutex_unlock(&user_mutex);
                        if (created) {
                            char ok[256];
                            snprintf(ok, sizeof(ok), "ANNOUNCE|User '%s' created.\n", arg1);
                            send(c->sockfd, ok, strlen(ok), 0);
                            log_message("INFO", "Admin created user '%s'", arg1);
                        } else {
                            char err[256];
                            snprintf(err, sizeof(err), "ERROR|Cannot create user '%s' (exists/invalid)\n", arg1);
                            send(c->sockfd, err, strlen(err), 0);
                        }
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Only admin can create users\n");
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "DELETE_USER") == 0 && parts >= 2) {
                    if (c->is_admin) {
                        pthread_mutex_lock(&client_mutex);
                        Client *target = client_find(arg1);
                        if (target && target->active) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "KICK|Your account has been deleted.\n");
                            send(target->sockfd, msg, strlen(msg), 0);
                            target->active = false;
                            shutdown(target->sockfd, SHUT_RDWR);
                        }
                        pthread_mutex_unlock(&client_mutex);
                        pthread_mutex_lock(&user_mutex);
                        bool removed = account_remove(arg1);
                        if (removed) save_users();
                        pthread_mutex_unlock(&user_mutex);
                        if (removed) {
                            char ok[256];
                            snprintf(ok, sizeof(ok), "ANNOUNCE|User '%s' deleted.\n", arg1);
                            broadcast(ok, NULL);
                            broadcast_user_list();
                            log_message("INFO", "Admin deleted user '%s'", arg1);
                        } else {
                            char err[256];
                            snprintf(err, sizeof(err), "ERROR|Cannot delete user '%s' (not found)\n", arg1);
                            send(c->sockfd, err, strlen(err), 0);
                        }
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Only admin can delete users\n");
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "RESET_PASS") == 0 && parts >= 3) {
                    if (c->is_admin) {
                        pthread_mutex_lock(&user_mutex);
                        bool ok = user_reset_pass(arg1, arg2);
                        if (ok) save_users();
                        pthread_mutex_unlock(&user_mutex);
                        if (ok) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "ANNOUNCE|Password reset for '%s'.\n", arg1);
                            send(c->sockfd, msg, strlen(msg), 0);
                            log_message("INFO", "Admin reset password for '%s'", arg1);
                        } else {
                            char err[256];
                            snprintf(err, sizeof(err), "ERROR|Cannot reset password for '%s' (not found)\n", arg1);
                            send(c->sockfd, err, strlen(err), 0);
                        }
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Only admin can reset passwords\n");
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "LIST_ACCOUNTS") == 0) {
                    if (c->is_admin) {
                        char list[4096] = {0};
                        pthread_mutex_lock(&user_mutex);
                        for (UserAccount *u = user_list; u; u = u->next) {
                            if (list[0]) strncat(list, ",", sizeof(list) - strlen(list) - 1);
                            strncat(list, u->username, sizeof(list) - strlen(list) - 1);
                        }
                        pthread_mutex_unlock(&user_mutex);
                        char out[4096 + 64];
                        snprintf(out, sizeof(out), "ACCOUNT_LIST|%s\n", list);
                        send(c->sockfd, out, strlen(out), 0);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Only admin can list accounts\n");
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "FILE_REQUEST") == 0 && parts >= 3) {
                    /* FILE_REQUEST|filename|size|target   (target empty = broadcast to room) */
                    const char *raw = arg1;
                    long fsize = atol(arg2);
                    char target[MAX_USERNAME] = {0};
                    strncpy(target, arg3, sizeof(target)-1);
                    char safe_name[MAX_FILENAME];
                    sanitize_filename(safe_name, sizeof(safe_name), raw);

                    if (fsize > MAX_FILE_SIZE) {
                        char err[256];
                        snprintf(err, sizeof(err), "FILE_DENIED|%s|%s|File too large\n", safe_name, c->username);
                        send(c->sockfd, err, strlen(err), 0);
                    } else {
                        int slot = -1;
                        pthread_mutex_lock(&upload_mutex);
                        for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
                            if (!upload_slots[i].active) { slot = i; break; }
                        }
                        pthread_mutex_unlock(&upload_mutex);
                        if (slot < 0) {
                            char err[256];
                            snprintf(err, sizeof(err), "FILE_DENIED|%s|%s|Too many concurrent uploads\n", safe_name, c->username);
                            send(c->sockfd, err, strlen(err), 0);
                        } else {
                            char token[TOKEN_LEN + 1] = {0};
                            unsigned h = (unsigned)(time(NULL) ^ (uintptr_t)c ^ (unsigned)rand());
                            for (int i = 0; i < TOKEN_LEN; i++) {
                                int r = (h >> (i * 2)) & 0xf;
                                token[i] = "0123456789abcdef"[r % 16];
                                h = h * 1103515245u + 12345u;
                            }
                            token[TOKEN_LEN] = '\0';

                            pthread_mutex_lock(&upload_mutex);
                            upload_slots[slot].active = true;
                            strncpy(upload_slots[slot].token, token, TOKEN_LEN);
                            strncpy(upload_slots[slot].sender, c->username, MAX_USERNAME - 1);
                            strncpy(upload_slots[slot].filename, safe_name, MAX_FILENAME - 1);
                            strncpy(upload_slots[slot].recipient, target, MAX_USERNAME - 1);
                            upload_slots[slot].size = fsize;
                            upload_slots[slot].started_at = time(NULL);
                            pthread_mutex_unlock(&upload_mutex);

                            total_files++;
                            transfer_add(c->username, safe_name, fsize, target);

                            char grant[512];
                            snprintf(grant, sizeof(grant), "FILE_GRANTED|%s|%s|%s|%ld\n",
                                c->username, safe_name, token, fsize);
                            send(c->sockfd, grant, strlen(grant), 0);

                            char fwd[512];
                            snprintf(fwd, sizeof(fwd), "FILE_OFFER|%s|%s|%ld|%s\n",
                                c->username, safe_name, fsize, target);
                            if (target[0]) send_to_user(target, fwd);
                            else broadcast(fwd, c);

                            log_message("FILE", "%s GRANTED token %s for '%s' (%ld bytes) slot=%d",
                                c->username, token, safe_name, fsize, slot);
                        }
                    }
                } else if (strcmp(cmd, "FILE_OFFER") == 0 && parts >= 3) {
                    total_files++;
                    char safe_name[MAX_FILENAME];
                    sanitize_filename(safe_name, sizeof(safe_name), arg1);
                    transfer_add(c->username, safe_name, atol(arg2), arg3);
                    char fwd[512];
                    snprintf(fwd, sizeof(fwd), "FILE_OFFER|%s|%s|%s|%s\n", c->username, safe_name, arg2, arg3);
                    if (strlen(arg3) > 0) send_to_user(arg3, fwd);
                    else broadcast(fwd, c);
                    log_message("FILE", "%s offered file '%s' (%s bytes) to %s", c->username, safe_name, arg2, arg3[0] ? arg3 : "all");
                } else if (strcmp(cmd, "FILE_DATA") == 0) {
                    /* Format: FILE_DATA|filename|token|base64
                     * Parse from the raw line (base64 may be large) by locating
                     * the first two pipes, mirroring the reference client. */
                    char *p1 = strchr(line, '|');
                    if (p1) {
                        char *p2 = strchr(p1 + 1, '|');
                        if (p2) {
                            *p2 = '\0';
                            const char *fname = p1 + 1;
                            const char *b64 = p2 + 1;
                            FileTransfer tf;
                            bool found = false;
                            pthread_mutex_lock(&transfer_mutex);
                            found = transfer_find(c->username, fname, &tf);
                            pthread_mutex_unlock(&transfer_mutex);
                            if (found) {
                                /* Validate upload token embedded after filename. */
                                char token_match[TOKEN_LEN + 1] = {0};
                                const char *base64_data = b64;
                                bool valid_token = false;
                                char *token_delim = strchr(b64, '|');
                                if (token_delim) {
                                    size_t tok_len = token_delim - b64;
                                    if (tok_len > TOKEN_LEN) tok_len = TOKEN_LEN;
                                    strncpy(token_match, b64, tok_len);
                                    token_match[tok_len] = '\0';
                                    base64_data = token_delim + 1;
                                    pthread_mutex_lock(&upload_mutex);
                                    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
                                        if (upload_slots[i].active &&
                                            strcmp(upload_slots[i].sender, c->username) == 0 &&
                                            strcmp(upload_slots[i].filename, fname) == 0 &&
                                            strcmp(upload_slots[i].token, token_match) == 0) {
                                            valid_token = true;
                                            upload_slots[i].started_at = time(NULL);
                                            break;
                                        }
                                    }
                                    pthread_mutex_unlock(&upload_mutex);
                                } else {
                                    valid_token = true; /* backward-compatible */
                                }
                                if (valid_token) {
                                    char fwd[BUFFER_SIZE + 64];
                                    snprintf(fwd, sizeof(fwd), "FILE_DATA|%s|%s|%s\n", c->username, fname, base64_data);
                                    if (tf.recipient[0]) send_to_user(tf.recipient, fwd);
                                    else broadcast(fwd, c);
                                }
                            }
                        }
                    }
                } else if (strcmp(cmd, "FILE_END") == 0 && parts >= 2) {
                    FileTransfer tf;
                    bool found = false;
                    pthread_mutex_lock(&transfer_mutex);
                    found = transfer_find(c->username, arg1, &tf);
                    pthread_mutex_unlock(&transfer_mutex);
                    if (found) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "FILE_END|%s|%s\n", c->username, arg1);
                        if (tf.recipient[0]) send_to_user(tf.recipient, msg);
                        else broadcast(msg, c);
                        transfer_remove(c->username, arg1);
                        pthread_mutex_lock(&upload_mutex);
                        for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
                            if (upload_slots[i].active &&
                                strcmp(upload_slots[i].sender, c->username) == 0 &&
                                strcmp(upload_slots[i].filename, arg1) == 0) {
                                upload_slots[i].active = false;
                                break;
                            }
                        }
                        pthread_mutex_unlock(&upload_mutex);
                        log_message("FILE", "File '%s' from %s completed", arg1, c->username);
                    }
                } else if (strcmp(cmd, "FILE_ACCEPT") == 0 && parts >= 2) {
                    FileTransfer tf;
                    bool found = false;
                    pthread_mutex_lock(&transfer_mutex);
                    found = transfer_find(arg1, arg2, &tf);
                    pthread_mutex_unlock(&transfer_mutex);
                    /* Accepting works for a targeted recipient OR any user when
                       the file was offered to the room (broadcast). */
                    if (found && (tf.recipient[0] == 0 || strcmp(tf.recipient, c->username) == 0)) {
                        char msg[512];
                        snprintf(msg, sizeof(msg), "FILE_ACCEPT|%s|%s\n", c->username, arg2);
                        send_to_user(arg1, msg);
                        log_message("FILE", "%s accepted file '%s' from %s", c->username, arg2, arg1);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|No active file offer from '%s' named '%s'\n", arg1, arg2);
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "FILE_REJECT") == 0 && parts >= 3) {
                    FileTransfer tf;
                    bool found = false;
                    pthread_mutex_lock(&transfer_mutex);
                    found = transfer_find(arg1, arg2, &tf);
                    pthread_mutex_unlock(&transfer_mutex);
                    if (found && (tf.recipient[0] == 0 || strcmp(tf.recipient, c->username) == 0)) {
                        char msg[512];
                        snprintf(msg, sizeof(msg), "FILE_REJECT|%s|%s|%s\n", c->username, arg2, arg3);
                        send_to_user(arg1, msg);
                        transfer_remove(arg1, arg2);
                        pthread_mutex_lock(&upload_mutex);
                        for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
                            if (upload_slots[i].active &&
                                strcmp(upload_slots[i].sender, arg1) == 0 &&
                                strcmp(upload_slots[i].filename, arg2) == 0) {
                                upload_slots[i].active = false;
                                break;
                            }
                        }
                        pthread_mutex_unlock(&upload_mutex);
                        log_message("FILE", "%s rejected file '%s' from %s: %s", c->username, arg2, arg1, arg3);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|No active file offer from '%s' named '%s'\n", arg1, arg2);
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "LOGOUT") == 0) {
                    c->active = false;
                    break;
                }
                /* end of command dispatch */
            } else {
                if (line_len < sizeof(line)-1) {
                    line[line_len++] = buf[i];
                }
            }
        }
    }

    /* Client disconnected: clean up. */
    char uname[MAX_USERNAME] = {0};
    char last_room[MAX_ROOM_NAME] = {0};
    strncpy(uname, c->username, MAX_USERNAME-1);
    strncpy(last_room, c->current_room, MAX_ROOM_NAME-1);
    bool was_online = c->active || uname[0];
    (void)was_online;
    client_remove(c);
    /* Release this client's upload slots and pending transfers. */
    pthread_mutex_lock(&upload_mutex);
    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
        if (upload_slots[i].active && strcmp(upload_slots[i].sender, uname) == 0)
            upload_slots[i].active = false;
    }
    pthread_mutex_unlock(&upload_mutex);
    pthread_mutex_lock(&transfer_mutex);
    {
        FileTransfer **pp = &transfer_list;
        while (*pp) {
            if (strcmp((*pp)->sender, uname) == 0) {
                FileTransfer *tmp = *pp;
                *pp = (*pp)->next;
                free(tmp);
            } else {
                pp = &((*pp)->next);
            }
        }
    }
    pthread_mutex_unlock(&transfer_mutex);
    if (uname[0]) {
        broadcast_user_list();
        if (last_room[0] && strcmp(last_room, "general") != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "NOTIFY|%s disconnected.\n", uname);
            broadcast_room(last_room, msg, NULL);
        }
        log_message("INFO", "User '%s' disconnected", uname);
    }
    return NULL;
}
static void server_shutdown(void) {
    server_running = 0;
    if (server_sock >= 0) close(server_sock);
    pthread_mutex_lock(&client_mutex);
    while (client_list) {
        Client *tmp = client_list;
        client_list = client_list->next;
        shutdown(tmp->sockfd, SHUT_RDWR);
        close(tmp->sockfd);
        free(tmp);
    }
    pthread_mutex_unlock(&client_mutex);
    logger_close();
    room_destroy();
    room_access_clear();
}

static void signal_handler(int sig) {
    (void)sig;
    server_running = 0;
    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }
}

int main(int argc, char **argv) {
    int port = PORT;
    if (argc > 1) port = atoi(argv[1]);
    if (port <= 0 || port > 65535) port = PORT;

    /* Load admin credentials. */
    FILE *af = fopen("config/admin.cred", "r");
    if (af) {
        char line[256];
        if (fgets(line, sizeof(line), af)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            char *colon = strchr(line, ':');
            if (colon) { *colon = 0; strncpy(admin_user, line, sizeof(admin_user)-1); strncpy(admin_pass, colon+1, sizeof(admin_pass)-1); }
        }
        fclose(af);
        sha256_hex(admin_pass, admin_pass_hash);
    }

    logger_init("logs");
    room_init();
    pthread_mutex_lock(&user_mutex);
    load_users();
    pthread_mutex_unlock(&user_mutex);
    room_access_load();
    history_init();
    srand((unsigned)time(NULL));

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }
    if (listen(server_sock, MAX_CLIENTS) < 0) { perror("listen"); return 1; }

    printf("[SERVER] Listening on port %d\n", port);
    log_message("INFO", "Server started on port %d", port);

    while (server_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_sock, &rfds);
        struct timeval tv = {1, 0};
        int sel = select(server_sock + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) {          /* periodic housekeeping */
            upload_expire_stale();
            continue;
        }
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(server_sock, (struct sockaddr*)&caddr, &clen);
        if (cfd < 0) {
            if (!server_running) break;
            continue;
        }
        Client *c = calloc(1, sizeof(Client));
        if (!c) { close(cfd); continue; }
        c->sockfd = cfd;
        c->addr = caddr;
        c->active = true;
        c->status = 1;
        c->login_time = time(NULL);
        c->current_room[0] = '\0';

        pthread_mutex_lock(&client_mutex);
        c->next = client_list;
        client_list = c;
        pthread_mutex_unlock(&client_mutex);

        if (pthread_create(&c->thread, NULL, handle_client, c) != 0) {
            close(cfd);
            c->active = false;
        } else {
            pthread_detach(c->thread);
        }
    }

    printf("[SERVER] Shutting down...\n");
    server_shutdown();
    return 0;
}












