/* room.c - creating / deleting / looking-up chat rooms.
 *
 * Rooms live in one linked list (global_rooms->head) guarded by room_mutex.
 * There is always a default room named "general" created at startup.
 * Some rooms are password-protected; access to those is tracked separately
 * in room_access.c.
 *
 * THREAD NOTE: room_mutex guards the shared list. The public functions
 * take the lock themselves, and room_find() is provided for callers that
 * already hold the lock (it does NOT lock itself).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include "room.h"

/* The global room list and its mutex. */
RoomList *global_rooms = NULL;
pthread_mutex_t room_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Build the initial list containing the always-available "general" room. */
void room_init(void) {
    global_rooms = calloc(1, sizeof(RoomList));
    if (global_rooms) {
        RoomNode *n = calloc(1, sizeof(RoomNode));
        strncpy(n->name, "general", MAX_ROOM_NAME - 1);
        strncpy(n->title, "General Chat", sizeof(n->title) - 1);
        strncpy(n->description, "Default chat room", sizeof(n->description) - 1);
        n->password[0] = '\0';
        n->is_protected = false;
        strncpy(n->creator, "system", MAX_USERNAME - 1);
        global_rooms->head = n;
        global_rooms->count = 1;
    }
}

/* Free every room node and the list container (server shutdown). */
void room_destroy(void) {
    if (!global_rooms) return;
    RoomNode *curr = global_rooms->head;
    while (curr) {
        RoomNode *tmp = curr;
        curr = curr->next;
        free(tmp);
    }
    free(global_rooms);
    global_rooms = NULL;
}

/* Bare list lookup. Does NOT lock - the caller must already hold
 * room_mutex. Returns the matching node or NULL. */
RoomNode *room_find(const char *name) {
    if (!global_rooms || !name) return NULL;
    for (RoomNode *n = global_rooms->head; n; n = n->next) {
        if (strcmp(n->name, name) == 0) return n;
    }
    return NULL;
}

/* Shorthand: create a plain room with no password, using the name as
 * its own title. Equivalent to room_create_extended(name,name,"","",""). */
bool room_create(const char *name) {
    return room_create_extended(name, name, "", "", "");
}

/* Create a room with full details (may be password protected).
 * Returns false if the name is empty or a room with that name exists. */
bool room_create_extended(const char *name, const char *title, const char *desc,
                          const char *password, const char *creator) {
    if (!global_rooms || !name || !name[0]) return false;
    pthread_mutex_lock(&room_mutex);
    if (room_find(name)) { pthread_mutex_unlock(&room_mutex); return false; }
    RoomNode *n = calloc(1, sizeof(RoomNode));
    if (n) {
        strncpy(n->name, name, MAX_ROOM_NAME - 1);
        /* If no title given, fall back to the room name. */
        strncpy(n->title, title && title[0] ? title : name, sizeof(n->title) - 1);
        strncpy(n->description, desc ? desc : "", sizeof(n->description) - 1);
        strncpy(n->creator, creator ? creator : "", MAX_USERNAME - 1);
        if (password && password[0]) {
            strncpy(n->password, password, sizeof(n->password) - 1);
            n->is_protected = true;
        } else {
            n->password[0] = '\0';
            n->is_protected = false;
        }
        n->next = global_rooms->head;   /* insert at head */
        global_rooms->head = n;
        global_rooms->count++;
    }
    pthread_mutex_unlock(&room_mutex);
    return n != NULL;
}

/* Delete a room, but only if the requester is its creator or is admin.
 * Returns true if a room was actually removed. */
bool room_delete(const char *name, const char *requester) {
    if (!global_rooms || !name || !requester) return false;
    pthread_mutex_lock(&room_mutex);
    RoomNode **pp = &global_rooms->head;
    bool found = false;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            if (strcmp((*pp)->creator, requester) == 0 || strcmp(requester, "admin") == 0) {
                RoomNode *tmp = *pp;
                *pp = (*pp)->next;   /* unlink */
                free(tmp);
                global_rooms->count--;
                found = true;
            }
            break;   /* found the room; stop looking */
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&room_mutex);
    if (found) {
        room_access_remove_room(name);
    }
    return found;
}

/* Change one field of a room: title, description, or password. When the
 * password is set to an empty string the room becomes unprotected.
 * Returns false if the room is missing or the field name is unknown. */
bool room_update_field(const char *name, const char *field, const char *value) {
    if (!global_rooms || !name || !field || !value) return false;
    pthread_mutex_lock(&room_mutex);
    RoomNode *n = room_find(name);
    if (!n) { pthread_mutex_unlock(&room_mutex); return false; }
    if (strcmp(field, "title") == 0)
        strncpy(n->title, value, sizeof(n->title) - 1);
    else if (strcmp(field, "description") == 0)
        strncpy(n->description, value, sizeof(n->description) - 1);
    else if (strcmp(field, "password") == 0) {
        if (value[0]) {
            strncpy(n->password, value, sizeof(n->password) - 1);
            n->is_protected = true;
        } else {
            n->password[0] = '\0';
            n->is_protected = false;
        }
    } else {
        pthread_mutex_unlock(&room_mutex);
        return false;   /* unknown field name */
    }
    pthread_mutex_unlock(&room_mutex);
    return true;
}

/* Is the given password correct for room `name`?
 * Non-protected rooms always accept (returns true); a missing room, or
 * a wrong password on a protected room, returns false. */
bool room_check_password(const char *name, const char *password) {
    if (!global_rooms || !name) return true;
    pthread_mutex_lock(&room_mutex);
    RoomNode *n = room_find(name);
    if (!n) { pthread_mutex_unlock(&room_mutex); return false; }
    if (!n->is_protected) { pthread_mutex_unlock(&room_mutex); return true; }
    bool ok = (password && strcmp(n->password, password) == 0);
    pthread_mutex_unlock(&room_mutex);
    return ok;
}

/* Does the room require a password to enter? */
bool room_is_protected(const char *name) {
    if (!global_rooms || !name) return false;
    pthread_mutex_lock(&room_mutex);
    RoomNode *n = room_find(name);
    bool p = n ? n->is_protected : false;
    pthread_mutex_unlock(&room_mutex);
    return p;
}

/* Does a room with this name exist? */
bool room_exists(const char *name) {
    if (!global_rooms || !name || !name[0]) return false;
    pthread_mutex_lock(&room_mutex);
    bool exists = room_find(name) != NULL;
    pthread_mutex_unlock(&room_mutex);
    return exists;
}

/* Build a string of all room names, one comma-separated; protected rooms
 * are marked with a trailing ":p". Used to build the ROOMS| line. */
void room_list(char *out, size_t out_len) {
    if (!global_rooms || !out || out_len == 0) return;
    pthread_mutex_lock(&room_mutex);
    out[0] = 0;
    for (RoomNode *n = global_rooms->head; n; n = n->next) {
        strncat(out, n->name, out_len - strlen(out) - 1);
        if (n->is_protected) {
            strncat(out, ":p", out_len - strlen(out) - 1);
        }
        if (n->next) strncat(out, ",", out_len - strlen(out) - 1);
    }
    pthread_mutex_unlock(&room_mutex);
}
