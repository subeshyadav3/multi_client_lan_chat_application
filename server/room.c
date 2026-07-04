#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include "room.h"

RoomList *global_rooms = NULL;
pthread_mutex_t room_mutex = PTHREAD_MUTEX_INITIALIZER;

void room_init(void) {
    global_rooms = calloc(1, sizeof(RoomList));
    if (global_rooms) {
        RoomNode *n = calloc(1, sizeof(RoomNode));
        strncpy(n->name, "general", MAX_ROOM_NAME - 1);
        global_rooms->head = n;
        global_rooms->count = 1;
    }
}

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

bool room_create(const char *name) {
    if (!global_rooms || !name) return false;
    pthread_mutex_lock(&room_mutex);
    RoomNode *n = calloc(1, sizeof(RoomNode));
    if (n) {
        strncpy(n->name, name, MAX_ROOM_NAME - 1);
        n->next = global_rooms->head;
        global_rooms->head = n;
        global_rooms->count++;
    }
    pthread_mutex_unlock(&room_mutex);
    return true;
}

void room_remove(const char *name) {
    if (!global_rooms || !name) return;
    pthread_mutex_lock(&room_mutex);
    RoomNode **pp = &global_rooms->head;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            RoomNode *tmp = *pp;
            *pp = (*pp)->next;
            free(tmp);
            global_rooms->count--;
            break;
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&room_mutex);
}

RoomNode *room_find(const char *name) {
    if (!global_rooms || !name) return NULL;
    pthread_mutex_lock(&room_mutex);
    RoomNode *res = NULL;
    for (RoomNode *n = global_rooms->head; n; n = n->next) {
        if (strcmp(n->name, name) == 0) {
            res = n;
            break;
        }
    }
    pthread_mutex_unlock(&room_mutex);
    return res;
}

void room_list(char *out, size_t out_len) {
    if (!global_rooms || !out || out_len == 0) return;
    pthread_mutex_lock(&room_mutex);
    out[0] = 0;
    for (RoomNode *n = global_rooms->head; n; n = n->next) {
        strncat(out, n->name, out_len - strlen(out) - 1);
        if (n->next) strncat(out, ",", out_len - strlen(out) - 1);
    }
    pthread_mutex_unlock(&room_mutex);
}
