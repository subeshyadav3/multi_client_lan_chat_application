/* history.c - per-room recent-message history.
 *
 * Every room keeps the last ROOM_HISTORY_MAX lines so late joiners get
 * some context on entering. Each room has its own small mutex.
 */
#include "server.h"

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

void history_init(void) {
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

void history_add(const char *room, const char *line) {
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

void history_replay(int sockfd, const char *room) {
    RoomHistory *h = history_for_room(room);
    if (!h) return;
    pthread_mutex_lock(&h->lock);
    for (RoomMessage *m = h->head; m; m = m->next) {
        send(sockfd, m->text, strlen(m->text), 0);
    }
    pthread_mutex_unlock(&h->lock);
}
