#ifndef ROOM_H
#define ROOM_H

/* room.h - public interface of the room system (see room.c and
 * room_access.c). A room is a named chat place that may optionally be
 * protected by a password and have a title / description / creator. */

#include <stdbool.h>
#include <pthread.h>
#include "../shared/constants.h"

/* One chat room. name is the unique key; title/description are nicer
 * texts a human sees; password (if is_protected) gates entry. */
typedef struct RoomNode {
    char name[MAX_ROOM_NAME];
    char title[MAX_ROOM_NAME * 2];
    char description[512];
    char password[64];
    char creator[MAX_USERNAME];
    bool is_protected;          /* true if a password is set */
    struct RoomNode *next;      /* next room in the list */
} RoomNode;

/* A doubly-usable container: just the head of the room linked list
 * plus a count of how many rooms there are. */
typedef struct RoomList {
    RoomNode *head;
    int count;
} RoomList;

/* The global list of rooms and the mutex that guards it. */
extern RoomList *global_rooms;
extern pthread_mutex_t room_mutex;

void room_init(void);    /* create the list with the default "general" room */
void room_destroy(void); /* free every room (shutdown) */
bool room_create(const char *name); /* shorthand: create a plain room named after itself */
bool room_create_extended(const char *name, const char *title, const char *desc,
                          const char *password, const char *creator);
bool room_delete(const char *name, const char *requester); /* only owner/admin */
bool room_update_field(const char *name, const char *field, const char *value);
bool room_check_password(const char *name, const char *password);
bool room_is_protected(const char *name);
bool room_exists(const char *name);
RoomNode *room_find(const char *name);   /* bare lookup, caller holds room_mutex */
void room_list(char *out, size_t out_len); /* build "name,name:p,..." string */

/* Per-user access tracking for protected rooms (see room_access.c).
 * Once a user has entered a protected room with the right password, we
 * remember that so we don't ask again on every re-join. */
void room_access_load(void);            /* prepare (empty) access registry */
void room_access_clear(void);           /* drop every access grant (shutdown) */
bool room_has_access(const char *username, const char *room);
void room_grant_access(const char *username, const char *room);
void room_access_remove_room(const char *room);

#endif /* ROOM_H */
