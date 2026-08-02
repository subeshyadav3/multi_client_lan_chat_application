/* net.c - the connected-client list and everything that delivers bytes.
 *
 * Holds the single linked list of connected clients plus its mutex.
 * Broadcasting, per-user sends, client lookup/removal, the live user/room
 * list pushes, and small text helpers live here.
 */
#include "server.h"

Client *client_list = NULL;
pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Send data with error checking; marks the client inactive on failure. */
static bool safe_send(Client *c, const char *msg, size_t len) {
    if (!c || !c->active || !msg || len == 0) return false;
    ssize_t n = send(c->sockfd, msg, len, 0);
    if (n <= 0) {
        c->active = false;
        return false;
    }
    return true;
}

/* Caller must hold client_mutex. */
Client *net_client_find(const char *username) {
    if (!username) return NULL;
    for (Client *c = client_list; c; c = c->next) {
        if (strcmp(c->username, username) == 0) return c;
    }
    return NULL;
}

void net_send_to_user(const char *username, const char *msg) {
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

void net_broadcast_room(const char *room, const char *msg, Client *except) {
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c != except && c->active && strcmp(c->current_room, room) == 0) {
            safe_send(c, msg, strlen(msg));
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

void net_broadcast(const char *msg, Client *except) {
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c != except && c->active) {
            safe_send(c, msg, strlen(msg));
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

void net_client_remove(Client *target) {
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

/* Append `item` to a comma-separated list string, guarding the buffer size. */
void net_list_append(char *dst, size_t dst_sz, const char *sep, const char *item) {
    size_t used = strlen(dst);
    if (used > 0) {
        size_t sep_len = strlen(sep);
        if (used + sep_len + 1 < dst_sz) {
            strncat(dst, sep, dst_sz - used - 1);
            used += sep_len;
        }
    }
    strncat(dst, item, dst_sz - used - 1);
}

void net_broadcast_user_list(void) {
    char userlist[MAX_MESSAGE] = {0};
    pthread_mutex_lock(&client_mutex);
    for (Client *p = client_list; p; p = p->next) {
        if (!p->active) continue;
        net_list_append(userlist, sizeof(userlist), ",", p->username);
        net_list_append(userlist, sizeof(userlist), "", ":1");
    }
    pthread_mutex_unlock(&client_mutex);
    char out[MAX_MESSAGE + 64];
    snprintf(out, sizeof(out), "USERS|%s\n", userlist);
    net_broadcast(out, NULL);
}

void net_broadcast_room_list(void) {
    char rooms[MAX_MESSAGE] = {0};
    room_list(rooms, sizeof(rooms));
    char out[MAX_MESSAGE + 64];
    snprintf(out, sizeof(out), "ROOMS|%s\n", rooms);
    net_broadcast(out, NULL);
}

void net_send_status(Client *c) {
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
void net_finish_join(Client *c, const char *room) {
    strncpy(c->current_room, room, MAX_ROOM_NAME - 1);
    c->current_room[MAX_ROOM_NAME - 1] = '\0';
    history_replay(c->sockfd, room);
    char ok[128];
    snprintf(ok, sizeof(ok), "JOIN_OK|%s\n", room);
    send(c->sockfd, ok, strlen(ok), 0);
    char msg[256];
    snprintf(msg, sizeof(msg), "NOTIFY|%s joined the room.\n", c->username);
    net_broadcast_room(room, msg, c);
}

/* Make a safe filename: strip path separators and control characters. */
void net_sanitize_filename(char *dst, size_t dst_len, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '/' || c == '\\' || c == '|' || c == '\n' || c == '\r' || c == '\0') continue;
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
    if (j == 0) strncpy(dst, "file", dst_len - 1);
}

void net_get_timestamp(char *buf, size_t len) {
    time_t t = time(NULL);
    strftime(buf, len, "%I:%M %p", localtime(&t));
}

/* Allocate a Client for a freshly accepted socket, register it in the list,
 * and run its handler in a detached thread. */
void net_spawn_client(int fd, struct sockaddr_in addr) {
    Client *c = calloc(1, sizeof(Client));
    if (!c) { close(fd); return; }
    c->sockfd = fd;
    c->addr = addr;
    c->active = true;
    c->status = 1;
    c->login_time = time(NULL);
    c->current_room[0] = '\0';

    pthread_mutex_lock(&client_mutex);
    c->next = client_list;
    client_list = c;
    pthread_mutex_unlock(&client_mutex);

    if (pthread_create(&c->thread, NULL, handle_client, c) != 0) {
        close(fd);
        c->active = false;
    } else {
        pthread_detach(c->thread);
    }
}

/* Close and free every remaining client (shutdown path). */
void net_close_all_clients(void) {
    pthread_mutex_lock(&client_mutex);
    while (client_list) {
        Client *tmp = client_list;
        client_list = client_list->next;
        shutdown(tmp->sockfd, SHUT_RDWR);
        close(tmp->sockfd);
        free(tmp);
    }
    pthread_mutex_unlock(&client_mutex);
}
