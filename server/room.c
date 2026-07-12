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
        strncpy(n->title, "General Chat", sizeof(n->title) - 1);
        strncpy(n->description, "Default chat room", sizeof(n->description) - 1);
        n->password[0] = '\0';
        n->is_protected = false;
        strncpy(n->creator, "system", MAX_USERNAME - 1);
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
    return room_create_extended(name, name, "", "", "");
}

bool room_create_extended(const char *name, const char *title, const char *desc, const char *password, const char *creator) {
    if (!global_rooms || !name || !name[0]) return false;
    pthread_mutex_lock(&room_mutex);
    if (room_find(name)) { pthread_mutex_unlock(&room_mutex); return false; }
    RoomNode *n = calloc(1, sizeof(RoomNode));
    if (n) {
        strncpy(n->name, name, MAX_ROOM_NAME - 1);
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
        n->next = global_rooms->head;
        global_rooms->head = n;
        global_rooms->count++;
    }
    pthread_mutex_unlock(&room_mutex);
    return n != NULL;
}

bool room_delete(const char *name, const char *requester) {
    if (!global_rooms || !name || !requester) return false;
    pthread_mutex_lock(&room_mutex);
    RoomNode **pp = &global_rooms->head;
    bool found = false;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            if (strcmp((*pp)->creator, requester) == 0 || strcmp(requester, "admin") == 0) {
                RoomNode *tmp = *pp;
                *pp = (*pp)->next;
                free(tmp);
                global_rooms->count--;
                found = true;
            }
            break;
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&room_mutex);
    return found;
}

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
    }
    pthread_mutex_unlock(&room_mutex);
    return true;
}

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

bool room_is_protected(const char *name) {
    if (!global_rooms || !name) return false;
    pthread_mutex_lock(&room_mutex);
    RoomNode *n = room_find(name);
    bool p = n ? n->is_protected : false;
    pthread_mutex_unlock(&room_mutex);
    return p;
}

RoomNode *room_find(const char *name) {
    if (!global_rooms || !name) return NULL;
    for (RoomNode *n = global_rooms->head; n; n = n->next) {
        if (strcmp(n->name, name) == 0) return n;
    }
    return NULL;
}

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
