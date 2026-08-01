#include <string.h>
#include <stdio.h>
#include "../shared/protocol.h"

char* format_public_msg(Message *msg) {
    static char buf[MAX_MESSAGE + 256];
    snprintf(buf, sizeof(buf), "PUBLIC|%s|%s|%s|%s", msg->room, msg->sender, msg->body, msg->timestamp);
    return buf;
}

char* format_private_msg(Message *msg) {
    static char buf[MAX_MESSAGE + 256];
    snprintf(buf, sizeof(buf), "PRIVATE|%s|%s|%s|%s", msg->sender, msg->recipient, msg->body, msg->timestamp);
    return buf;
}

char* format_login(const char *username) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "LOGIN|%s", username);
    return buf;
}

char* format_login_ok(const char *username) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "LOGIN_OK|%s", username);
    return buf;
}

char* format_login_fail(const char *reason) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "LOGIN_FAIL|%s", reason);
    return buf;
}

char* format_announce(const char *text, const char *timestamp) {
    static char buf[MAX_MESSAGE + 256];
    snprintf(buf, sizeof(buf), "ANNOUNCE|%s|%s", text, timestamp);
    return buf;
}

char* format_notify(const char *text) {
    static char buf[MAX_MESSAGE + 256];
    snprintf(buf, sizeof(buf), "NOTIFY|%s", text);
    return buf;
}

char* format_kick(const char *reason) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "KICK|%s", reason);
    return buf;
}

char* format_user_list(const char **users, int count) {
    static char buf[MAX_MESSAGE];
    buf[0] = '\0';
    for (int i = 0; i < count; i++) {
        strncat(buf, users[i], sizeof(buf) - strlen(buf) - 1);
        if (i < count - 1) strncat(buf, ",", sizeof(buf) - strlen(buf) - 1);
    }
    return buf;
}

char* format_error(const char *reason) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "ERROR|%s", reason);
    return buf;
}

bool parse_message(const char *data, Message *msg) {
    if (!data || !msg) return false;
    int type;
    char buf[5][MAX_MESSAGE];
    int n = sscanf(data, "%d|%[^|]|%[^|]|%[^|]|%[^|]", &type, buf[0], buf[1], buf[2], buf[3]);
    if (n < 1) return false;
    msg->type = type;
    if (n > 1) { strncpy(msg->sender, buf[0], MAX_USERNAME-1); msg->sender[MAX_USERNAME-1]='\0'; }
    if (n > 2) { strncpy(msg->recipient, buf[1], MAX_USERNAME-1); msg->recipient[MAX_USERNAME-1]='\0'; }
    if (n > 3) { strncpy(msg->body, buf[2], MAX_MESSAGE-1); msg->body[MAX_MESSAGE-1]='\0'; }
    if (n > 4) { strncpy(msg->timestamp, buf[3], 31); msg->timestamp[31]='\0'; }
    return true;
}
