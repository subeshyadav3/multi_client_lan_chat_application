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
#include "../shared/protocol.h"
#include "../shared/constants.h"
#include "logger.h"
#include "room.h"

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
static volatile bool running = true;
static int total_messages = 0, total_privmsgs = 0, total_files = 0;
static char admin_user[64] = "admin";
static char admin_pass[128] = "";

/* ── user accounts ── */
typedef struct UserAccount {
    char username[MAX_USERNAME];
    char password[128];
    bool active;
    struct UserAccount *next;
} UserAccount;

static UserAccount *user_list = NULL;
static pthread_mutex_t user_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static FileTransfer *transfer_find(const char *sender, const char *filename) {
    pthread_mutex_lock(&transfer_mutex);
    for (FileTransfer *t = transfer_list; t; t = t->next) {
        if (strcmp(t->sender, sender) == 0 && strcmp(t->filename, filename) == 0) {
            pthread_mutex_unlock(&transfer_mutex);
            return t;
        }
    }
    pthread_mutex_unlock(&transfer_mutex);
    return NULL;
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

static void load_users(void) {
    FILE *f = fopen("config/users.cred", "r");
    if (!f) {
        log_message("INFO", "No config/users.cred found – admin must create users");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0]) continue;
        char *pw = strchr(line, ':');
        if (!pw) continue;
        *pw = 0; pw++;
        UserAccount *u = calloc(1, sizeof(UserAccount));
        strncpy(u->username, line, MAX_USERNAME-1);
        strncpy(u->password, pw, sizeof(u->password)-1);
        u->active = true;
        u->next = user_list;
        user_list = u;
    }
    fclose(f);
    {
        int n = 0;
        for (UserAccount *u = user_list; u; u = u->next) n++;
        log_message("INFO", "Loaded %d user accounts", n);
    }
}

static void save_users(void) {
    FILE *f = fopen("config/users.cred", "w");
    if (!f) return;
    for (UserAccount *u = user_list; u; u = u->next) {
        if (u->active)
            fprintf(f, "%s:%s\n", u->username, u->password);
    }
    fclose(f);
}

static UserAccount *user_find(const char *name) {
    for (UserAccount *u = user_list; u; u = u->next)
        if (strcmp(u->username, name) == 0) return u;
    return NULL;
}

static bool user_validate(const char *name, const char *pass) {
    UserAccount *u = user_find(name);
    return u && u->active && strcmp(u->password, pass) == 0;
}

static bool user_create(const char *name, const char *pass) {
    if (!name || !name[0] || !pass || !pass[0]) return false;
    if (strlen(name) >= MAX_USERNAME) return false;
    if (user_find(name)) return false;
    UserAccount *u = calloc(1, sizeof(UserAccount));
    strncpy(u->username, name, MAX_USERNAME-1);
    strncpy(u->password, pass, sizeof(u->password)-1);
    u->active = true;
    u->next = user_list;
    user_list = u;
    save_users();
    return true;
}

static bool user_delete(const char *name) {
    for (UserAccount **pp = &user_list; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->username, name) == 0) {
            UserAccount *tmp = *pp;
            *pp = (*pp)->next;
            free(tmp);
            save_users();
            return true;
        }
    }
    return false;
}

static bool user_reset_pass(const char *name, const char *newpass) {
    UserAccount *u = user_find(name);
    if (!u) return false;
    strncpy(u->password, newpass, sizeof(u->password)-1);
    save_users();
    return true;
}

static void load_admin_creds(void) {
    FILE *f = fopen("config/admin.cred", "r");
    if (!f) {
        log_message("INFO", "No config/admin.cred found, admin=admin (no password)");
        return;
    }
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = 0;
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            char *nl2 = strchr(colon+1, '\n'); if (nl2) *nl2 = 0;
            strncpy(admin_user, line, sizeof(admin_user)-1);
            strncpy(admin_pass, colon+1, sizeof(admin_pass)-1);
            log_message("INFO", "Admin creds loaded: user=%s", admin_user);
        }
    }
    fclose(f);
}

static void *handle_client(void *arg);
static void client_add(Client *c);
static void client_remove(int fd);
static Client *client_find(const char *username);
static void broadcast_room(const char *room, const char *msg, Client *except);
static void broadcast(const char *msg, Client *except);
static void send_to_user(const char *username, const char *msg);
static void get_timestamp(char *buf, size_t len);
static void server_shutdown(void);
static void signal_handler(int sig);

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    logger_init("logs");
    room_init();
    load_admin_creds();
    load_users();

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        exit(1);
    }

    listen(server_sock, 10);
    printf("[SERVER] Listening on port %d\n", PORT);
    log_message("INFO", "Server started on port %d", PORT);

    while (running) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int new_sock = accept(server_sock, (struct sockaddr *)&cli_addr, &cli_len);
        if (new_sock < 0) {
            if (errno == EINTR || !running) break;
            continue;
        }

        Client *c = calloc(1, sizeof(Client));
        c->sockfd = new_sock;
        c->addr = cli_addr;
        c->active = true;
        strncpy(c->current_room, "general", MAX_ROOM_NAME-1);

        client_add(c);
        pthread_create(&c->thread, NULL, handle_client, c);
        pthread_detach(c->thread);
    }

    server_shutdown();
    return 0;
}

static void *handle_client(void *arg) {
    Client *c = (Client*)arg;
    char buf[BUFFER_SIZE];
    char line[BUFFER_SIZE];
    size_t line_len = 0;

    while (c->active && running) {
        ssize_t n = recv(c->sockfd, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';
                char cmd[64] = {0}, arg1[256] = {0}, arg2[MAX_MESSAGE] = {0}, arg3[256] = {0}, arg4[256] = {0};
                int parts = sscanf(line, "%63[^|]|%255[^|]|%2047[^|]|%255[^|]|%255[^\n]", cmd, arg1, arg2, arg3, arg4);

                char ts[32]; get_timestamp(ts, sizeof(ts));

                if (strcmp(cmd, "LOGIN") == 0 && parts >= 3) {
                    /* LOGIN|username|password — always require password */
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
                        if (strcmp(arg2, admin_pass) == 0) {
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
                        /* Regular user – validate against user accounts. */
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
                        char ok[128]; snprintf(ok, sizeof(ok), "LOGIN_OK|%s\n", arg1);
                        send(c->sockfd, ok, strlen(ok), 0);
                        char notify[256]; snprintf(notify, sizeof(notify), "NOTIFY|%s has joined the chat.\n", arg1);
                        broadcast(notify, NULL);
                        log_message("INFO", "User '%s' logged in from %s", arg1, inet_ntoa(c->addr.sin_addr));
                    }
                } else if (strcmp(cmd, "PUBLIC") == 0 && parts >= 2) {
                    char msg[MAX_MESSAGE + 512];
                    snprintf(msg, sizeof(msg), "PUBLIC|%s|%s|%s|%s\n", c->current_room, c->username, arg2, ts);
                    broadcast_room(c->current_room, msg, NULL);
                    total_messages++;
                    log_message("MSG", "[%s] %s: %s", c->current_room, c->username, arg2);
                } else if (strcmp(cmd, "PRIVATE") == 0 && parts >= 3) {
                    char msg[MAX_MESSAGE + 512];
                    snprintf(msg, sizeof(msg), "PRIVATE|%s|%s|%s|%s\n", c->username, arg1, arg2, ts);
                    send_to_user(arg1, msg);
                    char self[MAX_MESSAGE + 256];
                    snprintf(self, sizeof(self), "PRIVATE|%s|%s|%s|%s\n", c->username, arg1, arg2, ts);
                    send_to_user(c->username, self);
                    total_privmsgs++;
                    log_message("PRIV", "%s -> %s: %s", c->username, arg1, arg2);
                } else if (strcmp(cmd, "TYPING") == 0 && parts >= 1) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "TYPING|%s|%s\n", arg1, c->username);
                    broadcast(msg, c);
                } else if (strcmp(cmd, "JOIN") == 0 && parts >= 2) {
                    /* JOIN|room  or  JOIN|room|password */
                    const char *room_name = arg1;
                    const char *password = (parts >= 3) ? arg2 : "";

                    if (room_is_protected(room_name) && !c->is_admin) {
                        if (!password[0] || !room_check_password(room_name, password)) {
                            char err[256];
                            snprintf(err, sizeof(err), "JOIN_FAIL|Room '%s' requires password\n", room_name);
                            send(c->sockfd, err, strlen(err), 0);
                        } else {
                            strncpy(c->current_room, room_name, MAX_ROOM_NAME-1);
                            c->current_room[MAX_ROOM_NAME-1] = '\0';
                            char ok[128];
                            snprintf(ok, sizeof(ok), "JOIN_OK|%s\n", room_name);
                            send(c->sockfd, ok, strlen(ok), 0);
                            char msg[256];
                            snprintf(msg, sizeof(msg), "NOTIFY|%s joined room %s.\n", c->username, room_name);
                            broadcast(msg, NULL);
                        }
                    } else {
                        strncpy(c->current_room, room_name, MAX_ROOM_NAME-1);
                        c->current_room[MAX_ROOM_NAME-1] = '\0';
                        char ok[128];
                        snprintf(ok, sizeof(ok), "JOIN_OK|%s\n", room_name);
                        send(c->sockfd, ok, strlen(ok), 0);
                        char msg[256];
                        snprintf(msg, sizeof(msg), "NOTIFY|%s joined room %s.\n", c->username, room_name);
                        broadcast(msg, NULL);
                    }
                } else if (strcmp(cmd, "LEAVE") == 0 && parts >= 2) {
                    strncpy(c->current_room, "general", MAX_ROOM_NAME-1);
                    c->current_room[MAX_ROOM_NAME-1] = '\0';
                    char msg[256];
                    snprintf(msg, sizeof(msg), "NOTIFY|%s left room %s.\n", c->username, arg1);
                    broadcast(msg, NULL);
                } else if (strcmp(cmd, "CREATE") == 0 && parts >= 2) {
                    room_create(arg1);
                    char msg[256];
                    snprintf(msg, sizeof(msg), "NOTIFY|Room '%s' created by %s.\n", arg1, c->username);
                    broadcast(msg, NULL);
                } else if (strcmp(cmd, "CREATE_ROOM") == 0 && parts >= 2) {
                    /* CREATE_ROOM|name|title|desc|password */
                    const char *rn = arg1;
                    const char *rt = (parts >= 3) ? arg2 : "";
                    const char *rd = (parts >= 4) ? arg3 : "";
                    const char *rp = (parts >= 5) ? arg4 : "";
                    if (room_create_extended(rn, rt, rd, rp, c->username)) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "NOTIFY|Room '%s' created by %s.\n", rn, c->username);
                        broadcast(msg, NULL);
                        char ok[128];
                        snprintf(ok, sizeof(ok), "ROOM_CREATED|%s\n", rn);
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
                        /* Move users in deleted room back to general */
                        pthread_mutex_lock(&client_mutex);
                        for (Client *p = client_list; p; p = p->next) {
                            if (p->active && strcmp(p->current_room, arg1) == 0) {
                                strncpy(p->current_room, "general", MAX_ROOM_NAME-1);
                            }
                        }
                        pthread_mutex_unlock(&client_mutex);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Cannot delete room '%s' (not found or not authorized)\n", arg1);
                        send(c->sockfd, err, strlen(err), 0);
                    }
                } else if (strcmp(cmd, "UPDATE_ROOM") == 0 && parts >= 4) {
                    /* UPDATE_ROOM|name|field|value */
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
                } else if (strcmp(cmd, "STATS") == 0 && c->is_admin) {
                    int users = 0;
                    pthread_mutex_lock(&client_mutex);
                    for (Client *p = client_list; p; p = p->next) {
                        if (p->active) users++;
                    }
                    pthread_mutex_unlock(&client_mutex);
                    char msg[256];
                    snprintf(msg, sizeof(msg), "ANNOUNCE|Stats: Users=%d Messages=%d PMs=%d Files=%d\n",
                        users, total_messages, total_privmsgs, total_files);
                    send(c->sockfd, msg, strlen(msg), 0);
                } else if (strcmp(cmd, "KICK") == 0 && parts >= 2 && c->is_admin) {
                    pthread_mutex_lock(&client_mutex);
                    Client *target = client_find(arg1);
                    if (target) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "KICK|%s\n", arg2);
                        send(target->sockfd, msg, strlen(msg), 0);
                        target->active = false;
                    }
                    pthread_mutex_unlock(&client_mutex);
                } else if (strcmp(cmd, "ANNOUNCE") == 0 && parts >= 2 && c->is_admin) {
                    char msg[MAX_MESSAGE + 256];
                    snprintf(msg, sizeof(msg), "ANNOUNCE|%s|%s|%s\n", "Server", arg1, ts);
                    broadcast(msg, NULL);
                    log_message("CTRL", "Announcement: %s", arg1);
                } else if (strcmp(cmd, "FILE_OFFER") == 0 && parts >= 4) {
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
                    char *p1 = strchr(line, '|');
                    if (p1) {
                        char *p2 = strchr(p1 + 1, '|');
                        if (p2) {
                            *p2 = '\0';
                            const char *fname = p1 + 1;
                            const char *b64 = p2 + 1;
                            FileTransfer *t = transfer_find(c->username, fname);
                            if (t) {
                                char fwd[BUFFER_SIZE + 64];
                                snprintf(fwd, sizeof(fwd), "FILE_DATA|%s|%s|%s\n", c->username, fname, b64);
                                if (t->recipient[0]) send_to_user(t->recipient, fwd);
                                else broadcast(fwd, c);
                            }
                            *p2 = '|';
                        }
                    }
                } else if (strcmp(cmd, "FILE_END") == 0 && parts >= 2) {
                    FileTransfer *t = transfer_find(c->username, arg1);
                    if (t) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "FILE_END|%s|%s\n", c->username, arg1);
                        if (t->recipient[0]) send_to_user(t->recipient, msg);
                        else broadcast(msg, c);
                        transfer_remove(c->username, arg1);
                    }
                } else if (strcmp(cmd, "FILE_REJECT") == 0 && parts >= 3) {
                    FileTransfer *t = transfer_find(arg1, arg2);
                    if (t && strcmp(t->recipient, c->username) == 0) {
                        char msg[512];
                        snprintf(msg, sizeof(msg), "FILE_REJECT|%s|%s|%s\n", c->username, arg2, arg3);
                        send_to_user(arg1, msg);
                        transfer_remove(arg1, arg2);
                        log_message("FILE", "%s rejected file '%s' from %s: %s", c->username, arg2, arg1, arg3);
                    }
                } else if (strcmp(cmd, "CREATE_USER") == 0 && parts >= 3 && c->is_admin) {
                    pthread_mutex_lock(&user_mutex);
                    if (user_create(arg1, arg2)) {
                        char ok[256];
                        snprintf(ok, sizeof(ok), "ANNOUNCE|User '%s' created.\n", arg1);
                        send(c->sockfd, ok, strlen(ok), 0);
                        log_message("INFO", "Admin created user '%s'", arg1);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|Cannot create user '%s' (exists/invalid)\n", arg1);
                        send(c->sockfd, err, strlen(err), 0);
                    }
                    pthread_mutex_unlock(&user_mutex);
                } else if (strcmp(cmd, "DELETE_USER") == 0 && parts >= 2 && c->is_admin) {
                    /* Also disconnect if online */
                    pthread_mutex_lock(&client_mutex);
                    Client *target = client_find(arg1);
                    if (target) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "KICK|Your account has been deleted.\n");
                        send(target->sockfd, msg, strlen(msg), 0);
                        target->active = false;
                    }
                    pthread_mutex_unlock(&client_mutex);
                    pthread_mutex_lock(&user_mutex);
                    if (user_delete(arg1)) {
                        char ok[256];
                        snprintf(ok, sizeof(ok), "ANNOUNCE|User '%s' deleted.\n", arg1);
                        send(c->sockfd, ok, strlen(ok), 0);
                        log_message("INFO", "Admin deleted user '%s'", arg1);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|User '%s' not found\n", arg1);
                        send(c->sockfd, err, strlen(err), 0);
                    }
                    pthread_mutex_unlock(&user_mutex);
                } else if (strcmp(cmd, "RESET_PASS") == 0 && parts >= 3 && c->is_admin) {
                    pthread_mutex_lock(&user_mutex);
                    if (user_reset_pass(arg1, arg2)) {
                        char ok[256];
                        snprintf(ok, sizeof(ok), "ANNOUNCE|Password reset for '%s'.\n", arg1);
                        send(c->sockfd, ok, strlen(ok), 0);
                        log_message("INFO", "Admin reset password for '%s'", arg1);
                    } else {
                        char err[256];
                        snprintf(err, sizeof(err), "ERROR|User '%s' not found\n", arg1);
                        send(c->sockfd, err, strlen(err), 0);
                    }
                    pthread_mutex_unlock(&user_mutex);
                } else if (strcmp(cmd, "LIST_ACCOUNTS") == 0 && c->is_admin) {
                    char buf[2048] = {0};
                    pthread_mutex_lock(&user_mutex);
                    for (UserAccount *u = user_list; u; u = u->next) {
                        if (!u->active) continue;
                        if (buf[0]) strncat(buf, ", ", sizeof(buf)-strlen(buf)-1);
                        strncat(buf, u->username, sizeof(buf)-strlen(buf)-1);
                    }
                    pthread_mutex_unlock(&user_mutex);
                    char out[4096];
                    snprintf(out, sizeof(out), "ANNOUNCE|Registered users: %s\n", buf[0] ? buf : "(none)");
                    send(c->sockfd, out, strlen(out), 0);
                } else if (strcmp(cmd, "STATUS") == 0 && parts >= 2) {
                    c->status = atoi(arg1);
                } else if (strcmp(cmd, "LOGOUT") == 0) {
                    break;
                }
                line_len = 0;
            } else {
                if (line_len < sizeof(line)-1) line[line_len++] = buf[i];
            }
        }
    }

    c->active = false;
    if (c->username[0]) {
        char notify[256];
        snprintf(notify, sizeof(notify), "NOTIFY|%s has left the chat.\n", c->username);
        broadcast(notify, NULL);
        log_message("INFO", "User '%s' disconnected", c->username);
    }
    close(c->sockfd);
    client_remove(c->sockfd);
    return NULL;
}

static void client_add(Client *c) {
    pthread_mutex_lock(&client_mutex);
    c->next = client_list;
    client_list = c;
    pthread_mutex_unlock(&client_mutex);
}

static void client_remove(int fd) {
    pthread_mutex_lock(&client_mutex);
    Client **pp = &client_list;
    while (*pp) {
        if ((*pp)->sockfd == fd) {
            Client *tmp = *pp;
            *pp = (*pp)->next;
            free(tmp);
            break;
        }
        pp = &((*pp)->next);
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

static void broadcast_room(const char *room, const char *msg, Client *except) {
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c != except && c->active && strcmp(c->current_room, room) == 0) {
            send(c->sockfd, msg, strlen(msg), 0);
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

static void broadcast(const char *msg, Client *except) {
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c != except && c->active) {
            send(c->sockfd, msg, strlen(msg), 0);
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

static void send_to_user(const char *username, const char *msg) {
    if (!username || !msg) return;
    pthread_mutex_lock(&client_mutex);
    Client *c = client_find(username);
    if (c && c->active) {
        send(c->sockfd, msg, strlen(msg), 0);
    }
    pthread_mutex_unlock(&client_mutex);
}

static void get_timestamp(char *buf, size_t len) {
    time_t t = time(NULL);
    strftime(buf, len, "%I:%M %p", localtime(&t));
}

static void server_shutdown(void) {
    running = false;
    close(server_sock);
    pthread_mutex_lock(&client_mutex);
    while (client_list) {
        Client *tmp = client_list;
        client_list = client_list->next;
        close(tmp->sockfd);
        free(tmp);
    }
    pthread_mutex_unlock(&client_mutex);
    logger_close();
}

static void signal_handler(int sig) {
    (void)sig;
    printf("[SERVER] Shutting down...\n");
    running = false;
    close(server_sock);
    log_message("INFO", "Server shutting down");
    exit(0);
}
