#include <string.h>
#include <stdio.h>
#include "../shared/protocol.h"

/* =====================================================================
 * Protocol line builders (the wire protocol)
 *
 * Each format_* function turns its inputs into ONE complete protocol
 * line -- the exact text that travels over the socket. The line is
 * written into an internal static buffer and a pointer to it is
 * returned, so a caller can send it straight away.
 *
 * IMPORTANT: the order of the '|'-separated fields below IS the wire
 * protocol. It must never be changed, or the client and server will
 * stop understanding each other.
 * ===================================================================== */

/* PUBLIC|room|sender|body|timestamp  -- a message broadcast in a room */
char* format_public_msg(Message *msg) {
    static char buf[MAX_MESSAGE + 256];
    snprintf(buf, sizeof(buf), "PUBLIC|%s|%s|%s|%s",
             msg->room, msg->sender, msg->body, msg->timestamp);
    return buf;
}

/* PRIVATE|sender|recipient|body|timestamp  -- a 1-on-1 whisper */
char* format_private_msg(Message *msg) {
    static char buf[MAX_MESSAGE + 256];
    snprintf(buf, sizeof(buf), "PRIVATE|%s|%s|%s|%s",
             msg->sender, msg->recipient, msg->body, msg->timestamp);
    return buf;
}

/* LOGIN|username -- the client asks to log in */
char* format_login(const char *username) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "LOGIN|%s", username);
    return buf;
}

/* LOGIN_OK|username -- the server accepts the login */
char* format_login_ok(const char *username) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "LOGIN_OK|%s", username);
    return buf;
}

/* LOGIN_FAIL|reason -- the server rejects the login */
char* format_login_fail(const char *reason) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "LOGIN_FAIL|%s", reason);
    return buf;
}

/* ANNOUNCE|text|timestamp -- an admin broadcast to everyone */
char* format_announce(const char *text, const char *timestamp) {
    static char buf[MAX_MESSAGE + 256];
    snprintf(buf, sizeof(buf), "ANNOUNCE|%s|%s", text, timestamp);
    return buf;
}

/* NOTIFY|text -- a short system/status note for the user */
char* format_notify(const char *text) {
    static char buf[MAX_MESSAGE + 256];
    snprintf(buf, sizeof(buf), "NOTIFY|%s", text);
    return buf;
}

/* KICK|reason -- the server tells this client it was kicked */
char* format_kick(const char *reason) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "KICK|%s", reason);
    return buf;
}

/* Build a comma-separated list of usernames, e.g. "alice,bob,carol" */
char* format_user_list(const char **users, int count) {
    static char buf[MAX_MESSAGE];
    buf[0] = '\0';
    for (int i = 0; i < count; i++) {
        /* Room for the username plus the separating comma (and the null). */
        strncat(buf, users[i], sizeof(buf) - strlen(buf) - 1);
        if (i < count - 1)
            strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
    }
    return buf;
}

/* ERROR|reason -- a generic error message */
char* format_error(const char *reason) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "ERROR|%s", reason);
    return buf;
}

/* ---------------------------------------------------------------------
 * NOTE: the live client and server build and parse lines INLINE so they
 * can tolerate empty fields and newlines. parse_message() below is a
 * small reference helper used for structured inspection and simple
 * tests: it reads a wire line back into a Message struct.
 * --------------------------------------------------------------------- */
bool parse_message(const char *data, Message *msg) {
    if (!data || !msg) return false;

    int type;
    char buf[5][MAX_MESSAGE];   /* one slot per text field (up to 4 fields) */

    /* Read the numeric type, then up to four '|'-separated text fields. */
    int n = sscanf(data, "%d|%[^|]|%[^|]|%[^|]|%[^|]",
                   &type, buf[0], buf[1], buf[2], buf[3]);
    if (n < 1) return false;    /* at least the type must be present */

    msg->type = type;
    /* Copy each present field into its struct member, always keeping
     * room for the final '\0' terminator. */
    if (n > 1) { strncpy(msg->sender, buf[0], MAX_USERNAME-1);   msg->sender[MAX_USERNAME-1]='\0'; }
    if (n > 2) { strncpy(msg->recipient, buf[1], MAX_USERNAME-1); msg->recipient[MAX_USERNAME-1]='\0'; }
    if (n > 3) { strncpy(msg->body, buf[2], MAX_MESSAGE-1);      msg->body[MAX_MESSAGE-1]='\0'; }
    if (n > 4) { strncpy(msg->timestamp, buf[3], 31);             msg->timestamp[31]='\0'; }
    return true;
}
