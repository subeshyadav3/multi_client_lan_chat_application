#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chat.h"
#include "ui.h"
#include "../shared/constants.h"

static void parse_pipe(const char *msg, char out[][256], int max) {
    char buf[2048];
    strncpy(buf, msg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(buf, "|", &saveptr);
    int i = 0;
    while (tok && i < max) {
        strncpy(out[i], tok, 255);
        out[i][255] = 0;
        i++;
        tok = strtok_r(NULL, "|", &saveptr);
    }
    for (int j = i; j < max; j++) out[j][0] = 0;
}

typedef struct {
    /* File chunks can be almost BUFFER_SIZE bytes after base64 encoding. */
    char msg[BUFFER_SIZE];
} DispatchMsg;

static gboolean process_on_main_thread(gpointer data) {
    DispatchMsg *d = (DispatchMsg *)data;
    char p[6][256];
    parse_pipe(d->msg, p, 6);

    if (strcmp(p[0], "PUBLIC") == 0 && p[1][0] && p[2][0] && p[3][0])
        ui_append_message(p[1], p[2], p[3], p[4][0] ? p[4] : NULL);
    else if (strcmp(p[0], "PRIVATE") == 0 && p[1][0] && p[3][0])
        ui_append_private_message(p[1], p[2], p[3], p[4][0] ? p[4] : NULL);
    else if (strcmp(p[0], "ANNOUNCE") == 0 && p[2][0])
        ui_append_announcement(p[2], p[3][0] ? p[3] : NULL);
    else if (strcmp(p[0], "NOTIFY") == 0) {
        ui_add_notification(p[1]);
        /* Defensive: whenever a user joins or leaves the chat, ask the server
         * for a fresh USERS list so the sidebar stays in sync even if the
         * pushed broadcast USERS is missed or processed late. */
        if (p[1][0]) {
            const char *joined = strstr(p[1], "joined the chat");
            const char *left   = strstr(p[1], "left the chat");
            if (joined || left) client_send_raw("LIST_USERS");
        }
    }
    else if (strcmp(p[0], "TYPING") == 0 && p[1][0])
        ui_show_typing(p[1], p[2]);
    else if (strcmp(p[0], "LOGIN_OK") == 0)
        ui_show_main_window();
    else if (strcmp(p[0], "LOGIN_FAIL") == 0)
        ui_show_login_error(p[1]);
    else if (strcmp(p[0], "USERS") == 0)
        ui_update_user_list(p[1], 0);
    else if (strcmp(p[0], "ROOMS") == 0)
        ui_update_room_list(p[1], 0);
    else if (strcmp(p[0], "JOIN_OK") == 0) {
        ui_joined_room(p[1][0] ? p[1] : "general");
        ui_add_notification(p[1][0] ? p[1] : "Joined room");
    }
    else if (strcmp(p[0], "JOIN_FAIL") == 0)
        ui_add_notification(p[1][0] ? p[1] : "Failed to join room");
    else if (strcmp(p[0], "ROOM_CREATED") == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Room '%s' created!", p[1][0] ? p[1] : "?");
        ui_add_notification(buf);
        client_send_raw("LIST_ROOMS");
    } else if (strcmp(p[0], "KICK") == 0) {
        ui_add_notification("You were kicked by the administrator.");
        client_disconnect();
    } else if (strcmp(p[0], "ERROR") == 0) {
        ui_add_notification(p[1][0] ? p[1] : "Server error");
    } else if (strcmp(p[0], "FILE_OFFER") == 0 && p[1][0] && p[2][0]) {
        /* FILE_OFFER|sender|filename|size|target — strip trailing newlines */
        if (p[4][0]) { char *nl = strchr(p[4], '\n'); if (nl) *nl = '\0'; }
        ui_show_file_offer(p[1], p[2], p[3], p[4]);
    } else if (strcmp(p[0], "FILE_DATA") == 0) {
        /* FILE_DATA|sender|filename|base64 — extract base64 after 3rd pipe */
        const char *m = d->msg;
        int pipes = 0;
        const char *base64 = NULL;
        for (const char *cp = m; *cp; cp++) {
            if (*cp == '|') {
                pipes++;
                if (pipes == 3) { base64 = cp + 1; break; }
            }
        }
        /* Strip trailing newline if present */
        if (base64) {
            char *nl = strchr(base64, '\n');
            if (nl) *nl = '\0';
        }
        if (base64 && *base64 && p[2][0]) {
            ui_append_file_chunk(p[2], base64);
        }
    } else if (strcmp(p[0], "FILE_END") == 0) {
        /* FILE_END|sender|filename — strip trailing newlines from filename */
        if (p[2][0]) {
            char *nl = strchr(p[2], '\n');
            if (nl) *nl = '\0';
            ui_finish_file(p[2]);
        }
    } else if (strcmp(p[0], "FILE_REJECT") == 0 && p[1][0] && p[2][0]) {
        /* FILE_REJECT|recipient|filename|reason */
        ui_on_file_rejected(p[2], p[1], p[3]);
    } else if (strcmp(p[0], "FILE_ACCEPT") == 0 && p[1][0] && p[2][0]) {
        /* FILE_ACCEPT|recipient|filename */
        ui_send_accepted_file(p[1], p[2]);
    }

    g_free(d);
    return FALSE;
}
static void on_message_received(const char *msg) {
    if (!msg) return;
    DispatchMsg *d = g_new(DispatchMsg, 1);
    strncpy(d->msg, msg, sizeof(d->msg) - 1);
    d->msg[sizeof(d->msg) - 1] = '\0';
    g_idle_add(process_on_main_thread, d);
}

static gboolean disconnect_on_main_thread(gpointer data) {
    (void)data;
    ui_add_notification("Disconnected from server.");
    return FALSE;
}

static void on_disconnect(const char *reason) {
    (void)reason;
    g_idle_add(disconnect_on_main_thread, NULL);
}

int main(int argc, char *argv[]) {
    ui_init(&argc, &argv);
    client_register_callbacks(on_message_received, NULL, on_disconnect);
    ui_show_login_window();
    ui_run();
    client_disconnect();
    ui_cleanup();
    return 0;
}
