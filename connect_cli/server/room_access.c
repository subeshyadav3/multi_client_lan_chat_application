/* room_access.c - who is allowed into each protected room.
 *
 * The rule is: the first time a user joins a password-protected room with
 * the correct password, we record (username, room) here. After that the
 * server lets them re-join that room without asking for the password.
 * This registry is only in-memory (no file), so a server restart starts
 * everyone from scratch - which is fine and simple.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "room.h"

/* One granted entry: user X is approved for room Y. */
typedef struct AccessEntry {
    char username[MAX_USERNAME];
    char room[MAX_ROOM_NAME];
    struct AccessEntry *next;
} AccessEntry;

/* The registry list and the mutex that guards it. */
static AccessEntry *access_list = NULL;
static pthread_mutex_t access_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Free every entry (used at shutdown). */
void room_access_clear(void) {
    pthread_mutex_lock(&access_mutex);
    AccessEntry *a = access_list;
    while (a) { AccessEntry *t = a; a = a->next; free(t); }
    access_list = NULL;
    pthread_mutex_unlock(&access_mutex);
}

/* Prepare the registry at startup. There is no persistent storage, so
 * we simply start empty. */
void room_access_load(void) {
    room_access_clear();
}

/* Does user `username` already have access to room `room`? */
bool room_has_access(const char *username, const char *room) {
    if (!username || !room) return false;
    pthread_mutex_lock(&access_mutex);
    for (AccessEntry *a = access_list; a; a = a->next) {
        if (strcmp(a->username, username) == 0 && strcmp(a->room, room) == 0) {
            pthread_mutex_unlock(&access_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&access_mutex);
    return false;
}

/* Record that user `username` may enter room `room` (unless already
 * granted, in which case we do nothing to avoid duplicates). */
void room_grant_access(const char *username, const char *room) {
    if (!username || !room || room_has_access(username, room)) return;
    AccessEntry *a = calloc(1, sizeof(AccessEntry));
    if (!a) return;
    strncpy(a->username, username, MAX_USERNAME - 1);
    strncpy(a->room, room, MAX_ROOM_NAME - 1);
    pthread_mutex_lock(&access_mutex);
    a->next = access_list;   /* insert at head */
    access_list = a;
    pthread_mutex_unlock(&access_mutex);
}
