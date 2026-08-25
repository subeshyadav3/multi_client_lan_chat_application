/* history.c - per-room recent-message history.
 *
 * Every room keeps its last ROOM_HISTORY_MAX lines (a small linked list
 * per room) so that a user who just joined sees some recent context via
 * history_replay(). Each room's history has its own mutex so different
 * rooms can be written in parallel without blocking each other.
 */
#include "server.h"

/* One stored message line inside a room's history list. */
typedef struct RoomMessage {
    char *text;                 /* a copy of the whole protocol line */
    struct RoomMessage *next;
} RoomMessage;

/* The history buffer for ONE room. */
typedef struct {
    char room_name[MAX_ROOM_NAME];
    RoomMessage *head;          /* oldest message */
    RoomMessage *tail;          /* newest message */
    int count;                  /* how many messages are stored */
    pthread_mutex_t lock;       /* guards this room's list */
} RoomHistory;

/* A fixed array of room histories (MAX_ROOMS of them) and how many are
 * actually in use. We keep room 0..count-1 contiguous. */
static RoomHistory room_histories[MAX_ROOMS];
static int room_history_count = 0;

/* Reset all history (called once at server startup). */
void history_init(void) {
    room_history_count = 0;
    memset(room_histories, 0, sizeof(room_histories));
}

/* Find the RoomHistory for `room`, creating (and locking-init) one if it
 * does not exist yet. Returns NULL if we have run out of room buffers. */
static RoomHistory *history_for_room(const char *room) {
    for (int i = 0; i < room_history_count; i++) {
        if (strcmp(room_histories[i].room_name, room) == 0)
            return &room_histories[i];
    }
    if (room_history_count >= MAX_ROOMS) return NULL;   /* no more buffers */
    RoomHistory *h = &room_histories[room_history_count++];
    strncpy(h->room_name, room, MAX_ROOM_NAME - 1);
    h->head = h->tail = NULL;
    h->count = 0;
    pthread_mutex_init(&h->lock, NULL);
    return h;
}

/* Remember one protocol line for the given room, trimming the oldest
 * message when we exceed ROOM_HISTORY_MAX. Called for every PUBLIC line. */
void history_add(const char *room, const char *line) {
    RoomHistory *h = history_for_room(room);
    if (!h) return;
    pthread_mutex_lock(&h->lock);
    RoomMessage *msg = calloc(1, sizeof(RoomMessage));
    if (!msg) { pthread_mutex_unlock(&h->lock); return; }
    msg->text = strdup(line);

    /* Append to the tail (newest end). */
    if (h->tail) h->tail->next = msg;
    else         h->head = msg;
    h->tail = msg;
    h->count++;

    /* If there are too many, drop from the head (oldest end). */
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

/* Send every stored line for `room` to the given socket, oldest first.
 * Used to give a joining user the recent room context. */
void history_replay(int sockfd, const char *room) {
    RoomHistory *h = history_for_room(room);
    if (!h) return;
    pthread_mutex_lock(&h->lock);
    for (RoomMessage *m = h->head; m; m = m->next) {
        send(sockfd, m->text, strlen(m->text), 0);
    }
    pthread_mutex_unlock(&h->lock);
}
