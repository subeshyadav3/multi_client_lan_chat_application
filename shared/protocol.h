#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include "constants.h"

/* =========================================================================
 * THE WIRE PROTOCOL
 *
 * Every message exchanged over the socket is a plain-text line with its
 * fields joined by the '|' character, and is ended with a newline. For
 * example:   PUBLIC|general|alice|hello everyone|02:30 PM
 *
 * This header defines the *kind* of each message (MessageType), the
 * Message struct that holds its fields in memory, and the helper
 * functions used to build and read those lines.
 * ========================================================================= */

/* Every possible kind of message. The value is just a tag; the actual
 * text sent is produced by the format_* helpers in protocol.c. */
typedef enum {
    MSG_LOGIN, MSG_LOGIN_OK, MSG_LOGIN_FAIL,          /* login / authentication  */
    MSG_PUBLIC, MSG_PRIVATE,                          /* chat messages           */
    MSG_JOIN_ROOM, MSG_LEAVE_ROOM, MSG_CREATE_ROOM,   /* room management         */
    MSG_LIST_USERS, MSG_USERS, MSG_TYPING, MSG_STATUS,/* users and presence      */
    MSG_FILE_OFFER, MSG_FILE_ACCEPT, MSG_FILE_REJECT, MSG_FILE_DATA, MSG_FILE_END, /* file transfer */
    MSG_FILE_PROGRESS,
    MSG_ANNOUNCE, MSG_NOTIFY, MSG_KICK,               /* notices and admin       */
    MSG_JOIN_OK, MSG_JOIN_FAIL, MSG_ROOM_CREATED,
    MSG_SEARCH, MSG_SEARCH_RESULT,
    MSG_LOGOUT, MSG_PING, MSG_PONG, MSG_ERROR
} MessageType;

/* A single logical message. Each string field holds one '|'-separated
 * part of a wire line so it is easy to move between text and memory. */
typedef struct {
    MessageType type;          /* which kind of message this is        */
    char sender[MAX_USERNAME]; /* who sent it                          */
    char recipient[MAX_USERNAME]; /* who it is for (private messages)  */
    char room[MAX_ROOM_NAME];  /* which room it belongs to             */
    char body[MAX_MESSAGE];    /* the actual text / payload            */
    char timestamp[32];        /* human-readable time, e.g. "02:30 PM" */
} Message;

/* ---------- Helpers that BUILD a wire line ----------
 * Each format_* function writes a complete protocol line into an internal
 * static buffer and returns a pointer to it. Callers can then send it
 * straight over the socket. */
char* format_public_msg(Message *msg);            /* PUBLIC|room|sender|body|time      */
char* format_private_msg(Message *msg);           /* PRIVATE|sender|recipient|body|time */
char* format_login(const char *username);         /* LOGIN|username                    */
char* format_login_ok(const char *username);      /* LOGIN_OK|username                 */
char* format_login_fail(const char *reason);      /* LOGIN_FAIL|reason                 */
char* format_announce(const char *text, const char *timestamp); /* ANNOUNCE|text|time */
char* format_notify(const char *text);            /* NOTIFY|text                       */
char* format_kick(const char *reason);            /* KICK|reason                       */
char* format_user_list(const char **users, int count); /* comma-joined user list       */
char* format_error(const char *reason);           /* ERROR|reason                      */

/* ---------- Helper that READS a wire line ----------
 * Parse a line back into a Message struct. Returns true on success. */
bool parse_message(const char *data, Message *msg);

#endif /* PROTOCOL_H */
