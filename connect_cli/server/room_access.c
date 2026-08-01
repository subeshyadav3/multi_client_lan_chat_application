#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "room.h"

/* ── Protected-room access registry ──
 * Tracks which username has successfully entered a protected room, so the
 * server does not require the password again on every re-join. This is purely
 * in-memory (no persistence between server restarts). */
typedef struct AccessEntry {
    char username[MAX_USERNAME];
    char room[MAX_ROOM_NAME];
    struct AccessEntry *next;
} AccessEntry;

static AccessEntry *access_list = NULL;
static pthread_mutex_t access_mutex = PTHREAD_MUTEX_INITIALIZER;

void room_access_clear(void) {
    pthread_mutex_lock(&access_mutex);
    AccessEntry *a = access_list;
    while (a) { AccessEntry *t = a; a = a->next; free(t); }
    access_list = NULL;
    pthread_mutex_unlock(&access_mutex);
}

void room_access_load(void) {
    /* No persistent storage; a fresh server starts without per-user grants. */
    room_access_clear();
}

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

void room_grant_access(const char *username, const char *room) {
    if (!username || !room || room_has_access(username, room)) return;
    AccessEntry *a = calloc(1, sizeof(AccessEntry));
    if (!a) return;
    strncpy(a->username, username, MAX_USERNAME - 1);
    strncpy(a->room, room, MAX_ROOM_NAME - 1);
    pthread_mutex_lock(&access_mutex);
    a->next = access_list;
    access_list = a;
    pthread_mutex_unlock(&access_mutex);
}
