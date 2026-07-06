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
                char cmd[64] = {0}, arg1[256] = {0}, arg2[MAX_MESSAGE] = {0}, arg3[256] = {0};
                int parts = sscanf(line, "%63[^|]|%255[^|]|%2047[^|]|%255[^\n]", cmd, arg1, arg2, arg3);

                char ts[32]; get_timestamp(ts, sizeof(ts));

                if (strcmp(cmd, "LOGIN") == 0 && parts >= 2) {
                    bool accepted = false;
                    pthread_mutex_lock(&client_mutex);
                    if (strlen(arg1) == 0 || strlen(arg1) >= MAX_USERNAME || client_find(arg1) != NULL) {
                        char err[256];
                        snprintf(err, sizeof(err), "LOGIN_FAIL|Username already exists or invalid\n");
                        send(c->sockfd, err, strlen(err), 0);
                    } else {
                        strncpy(c->username, arg1, MAX_USERNAME-1);
                        c->username[MAX_USERNAME-1] = '\0';
                        if (strcmp(c->username, "admin") == 0) c->is_admin = true;
                        accepted = true;
                    }
                    pthread_mutex_unlock(&client_mutex);
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
                    broadcast_room(c->current_room, msg, c);
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
                    strncpy(c->current_room, arg1, MAX_ROOM_NAME-1);
                    c->current_room[MAX_ROOM_NAME-1] = '\0';
                    char msg[256];
                    snprintf(msg, sizeof(msg), "NOTIFY|%s joined room %s.\n", c->username, arg1);
                    broadcast(msg, NULL);
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
                    char fwd[512];
                    snprintf(fwd, sizeof(fwd), "FILE_OFFER|%s|%s|%s|%s\n", c->username, arg1, arg2, arg3);
                    if (strlen(arg3) > 0) send_to_user(arg3, fwd);
                    else broadcast(fwd, c);
                    log_message("FILE", "%s offered file '%s' (%s bytes)", c->username, arg1, arg2);
                } else if (strcmp(cmd, "FILE_DATA") == 0 && parts >= 2) {
                    char fwd[BUFFER_SIZE + 64];
                    snprintf(fwd, sizeof(fwd), "FILE_DATA|%s|%s\n", arg1, arg2);
                    broadcast(fwd, NULL);
                } else if (strcmp(cmd, "FILE_ACCEPT") == 0 && parts >= 1) {
                    char msg[256]; snprintf(msg, sizeof(msg), "FILE_ACCEPT|%s\n", arg1);
                    broadcast(msg, NULL);
                } else if (strcmp(cmd, "FILE_END") == 0 && parts >= 1) {
                    char msg[256]; snprintf(msg, sizeof(msg), "FILE_END|%s\n", arg1);
                    broadcast(msg, NULL);
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
