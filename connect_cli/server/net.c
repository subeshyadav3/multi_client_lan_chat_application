/* net.c - the connected-client list and everything that delivers bytes.
 *
 * This module owns the single linked list of connected clients
 * (client_list) together with the mutex that protects it (client_mutex).
 * It is the "plumbing" that the command handlers use to reach the network:
 *
 *    safe_send()          - write bytes to one socket (flags inactive on error)
 *    net_client_find()    - look up a client node by username
 *    net_send_to_user()   - deliver a line to one specific user
 *    net_broadcast*()     - deliver a line to many users
 *    net_client_remove()  - unlink a client and free its memory
 *    net_spawn_client()   - wrap a fresh socket and start its receive thread
 *
 * Small text helpers used to build protocol lines also live here
 * (net_list_append, net_get_timestamp, net_sanitize_filename).
 */
#include "server.h"

/* The list of everyone connected, plus the lock that guards it. */
Client *client_list = NULL;
pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Low-level byte delivery                                             */
/* ------------------------------------------------------------------ */

/* Write `len` bytes to one client's socket.
 * Returns true on success. If the socket errors (peer closed / reset),
 * we mark the client inactive so its receive loop stops and the client
 * gets cleaned up. This is the ONLY function that actually calls send()
 * for the shared list paths.
 */
static bool safe_send(Client *c, const char *msg, size_t len) {
    if (!c || !c->active || !msg || len == 0) return false;
    ssize_t n = send(c->sockfd, msg, len, 0);
    if (n <= 0) {
        c->active = false;   /* the peer is gone; stop using this socket */
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                             */
/* ------------------------------------------------------------------ */

/* Find the linked-list node for `username`, or NULL if not connected.
 * IMPORTANT: the caller must already be holding client_mutex.
 */
Client *net_client_find(const char *username) {
    if (!username) return NULL;
    for (Client *c = client_list; c; c = c->next) {
        if (strcmp(c->username, username) == 0) return c;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Delivering to one user / to everyone                               */
/* ------------------------------------------------------------------ */

/* Send `msg` to the (single) connected client with the given username.
 * We lock the list so another thread can't free the client while we
 * are writing to it.
 */
void net_send_to_user(const char *username, const char *msg) {
    if (!username || !msg) return;
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c->active && strcmp(c->username, username) == 0) {
            safe_send(c, msg, strlen(msg));
            break;   /* only one client can hold a username */
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

/* Send `msg` to every active client currently inside the room `room`,
 * skipping `except` (usually the sender themself). Used for PUBLIC chat. */
void net_broadcast_room(const char *room, const char *msg, Client *except) {
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c != except && c->active && strcmp(c->current_room, room) == 0) {
            safe_send(c, msg, strlen(msg));
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

/* Send `msg` to every active client, skipping `except`. Used for
 * the user/room lists, announcements and global notifications. */
void net_broadcast(const char *msg, Client *except) {
    pthread_mutex_lock(&client_mutex);
    for (Client *c = client_list; c; c = c->next) {
        if (c != except && c->active) {
            safe_send(c, msg, strlen(msg));
        }
    }
    pthread_mutex_unlock(&client_mutex);
}

/* ------------------------------------------------------------------ */
/* Removing a client                                                  */
/* ------------------------------------------------------------------ */

/* Unlink `target` from client_list, close its socket and free it.
 * Locking is needed because other threads may be walking the list. */
void net_client_remove(Client *target) {
    pthread_mutex_lock(&client_mutex);
    Client **pp = &client_list;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;   /* skip over target in the list */
            break;
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&client_mutex);
    if (target->sockfd >= 0) close(target->sockfd);
    free(target);                 /* caller no longer uses this pointer */
}

/* ------------------------------------------------------------------ */
/* Text helpers used to build protocol lines                          */
/* ------------------------------------------------------------------ */

/* Append `item` to the comma-separated string `dst`, inserting `sep`
 * (",") between items. Never writes past the end of the buffer
 * (dst_sz bytes). This is how we build USERS / ROOMS / WHO lists.
 */
void net_list_append(char *dst, size_t dst_sz, const char *sep, const char *item) {
    size_t used = strlen(dst);
    if (used > 0) {                          /* not the first item: add separator */
        size_t sep_len = strlen(sep);
        if (used + sep_len + 1 < dst_sz) {
            strncat(dst, sep, dst_sz - used - 1);
            used += sep_len;
        }
    }
    strncat(dst, item, dst_sz - used - 1);   /* then the item itself */
}

/* Build and broadcast the current USERS|<names> line to everyone.
 * Each name is followed by :1 (a placeholder the client ignores). */
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

/* Build and broadcast the current ROOMS|<names> line to everyone. */
void net_broadcast_room_list(void) {
    char rooms[MAX_MESSAGE] = {0};
    room_list(rooms, sizeof(rooms));   /* ask room.c for the list string */
    char out[MAX_MESSAGE + 64];
    snprintf(out, sizeof(out), "ROOMS|%s\n", rooms);
    net_broadcast(out, NULL);
}

/* Send the admin-only STATUS line (counts of users/messages, etc.). */
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

/* Complete a room join: remember the room, replay recent history to the
 * joining client, send JOIN_OK back, and tell others in the room. */
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

/* Make a safe filename from `src`: strip path separators and control
 * characters so a client can't use ../../ or a newline in a filename.
 * If nothing survives we fall back to the literal name "file". */
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

/* Fill `buf` with the current time formatted like "08:42 PM"
 * (12-hour clock), used in PUBLIC / PRIVATE / ANNOUNCE lines. */
void net_get_timestamp(char *buf, size_t len) {
    time_t t = time(NULL);
    strftime(buf, len, "%I:%M %p", localtime(&t));
}

/* ------------------------------------------------------------------ */
/* Accepting a connection                                              */
/* ------------------------------------------------------------------ */

/* Wrap a freshly accepted socket in a Client node, add it to the global
 * list, and start its own thread (handle_client from connection.c) that
 * will read commands from that socket. The thread is detached so we do
 * not have to join it later.
 */
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
    c->next = client_list;   /* insert at the head of the list */
    client_list = c;
    pthread_mutex_unlock(&client_mutex);

    if (pthread_create(&c->thread, NULL, handle_client, c) != 0) {
        close(fd);
        c->active = false;
    } else {
        pthread_detach(c->thread);
    }
}

/* Close and free every remaining client. This is only called during
 * server shutdown, when no more work will arrive. */
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
