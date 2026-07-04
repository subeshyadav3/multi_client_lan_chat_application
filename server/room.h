#ifndef ROOM_H
#define ROOM_H
#include <stdbool.h>
#include <pthread.h>
#include "../shared/constants.h"

typedef struct RoomNode {
    char name[MAX_ROOM_NAME];
    struct RoomNode *next;
} RoomNode;

typedef struct RoomList {
    RoomNode *head;
    int count;
} RoomList;

extern RoomList *global_rooms;
extern pthread_mutex_t room_mutex;

void room_init(void);
void room_destroy(void);
bool room_create(const char *name);
void room_remove(const char *name);
RoomNode *room_find(const char *name);
void room_list(char *out, size_t out_len);

#endif
