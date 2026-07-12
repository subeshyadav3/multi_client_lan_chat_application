#ifndef ROOM_H
#define ROOM_H
#include <stdbool.h>
#include <pthread.h>
#include "../shared/constants.h"

typedef struct RoomNode {
    char name[MAX_ROOM_NAME];
    char title[MAX_ROOM_NAME * 2];
    char description[512];
    char password[64];
    char creator[MAX_USERNAME];
    bool is_protected;
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
bool room_create_extended(const char *name, const char *title, const char *desc, const char *password, const char *creator);
bool room_delete(const char *name, const char *requester);
bool room_update_field(const char *name, const char *field, const char *value);
bool room_check_password(const char *name, const char *password);
bool room_is_protected(const char *name);
RoomNode *room_find(const char *name);
void room_list(char *out, size_t out_len);

#endif
