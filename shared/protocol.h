#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include "constants.h"

typedef enum {
    MSG_LOGIN, MSG_LOGIN_OK, MSG_LOGIN_FAIL,
    MSG_PUBLIC, MSG_PRIVATE,
    MSG_JOIN_ROOM, MSG_LEAVE_ROOM, MSG_CREATE_ROOM,
    MSG_LIST_USERS, MSG_USERS, MSG_TYPING, MSG_STATUS,
    MSG_FILE_OFFER, MSG_FILE_ACCEPT, MSG_FILE_REJECT, MSG_FILE_DATA, MSG_FILE_END,
    MSG_ANNOUNCE, MSG_NOTIFY, MSG_KICK,
    MSG_SEARCH, MSG_SEARCH_RESULT,
    MSG_LOGOUT, MSG_PING, MSG_PONG, MSG_ERROR
} MessageType;

typedef struct {
    MessageType type;
    char sender[MAX_USERNAME];
    char recipient[MAX_USERNAME];
    char room[MAX_ROOM_NAME];
    char body[MAX_MESSAGE];
    char timestamp[32];
} Message;

char* format_public_msg(Message *msg);
char* format_private_msg(Message *msg);
char* format_login(const char *username);
char* format_login_ok(const char *username);
char* format_login_fail(const char *reason);
char* format_announce(const char *text, const char *timestamp);
char* format_notify(const char *text);
char* format_kick(const char *reason);
char* format_user_list(const char **users, int count);
char* format_error(const char *reason);
bool parse_message(const char *data, Message *msg);

#endif
