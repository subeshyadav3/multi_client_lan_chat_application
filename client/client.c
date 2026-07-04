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

static void on_message_received(const char *msg) {
    if (!msg) return;
    char p[6][256];
    parse_pipe(msg, p, 6);

    if (strcmp(p[0], "PUBLIC") == 0 && p[1][0] && p[2][0] && p[3][0])
        ui_append_message(p[1], p[2], p[3], p[4]);
    else if (strcmp(p[0], "PRIVATE") == 0 && p[1][0] && p[2][0])
        ui_append_private_message(p[1], p[3], p[4]);
    else if (strcmp(p[0], "ANNOUNCE") == 0 && p[1][0])
        ui_append_announcement(p[2], p[3]);
    else if (strcmp(p[0], "NOTIFY") == 0)
        ui_add_notification(p[1]);
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
    else if (strcmp(p[0], "KICK") == 0) {
        ui_add_notification("You were kicked by the administrator.");
        client_disconnect();
    } else if (strcmp(p[0], "FILE_OFFER") == 0 && p[1][0]) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "File offer: %s from %s (%s bytes)", p[2], p[1], p[3]);
        ui_add_notification(buf);
    } else if (strcmp(p[0], "FILE_DATA") == 0 && p[1][0] && p[2][0]) {
        gsize len;
        guchar *decoded = g_base64_decode(p[2], &len);
        if (decoded) {
            char path[512];
            snprintf(path, sizeof(path), "files/%s.tmp", p[1]);
            FILE *fp = fopen(path, "ab");
            if (fp) {
                fwrite(decoded, 1, len, fp);
                fclose(fp);
            }
            g_free(decoded);
        }
    } else if (strcmp(p[0], "FILE_END") == 0 && p[1][0]) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "files/%s.tmp", p[1]);
        snprintf(dst, sizeof(dst), "files/%s", p[1]);
        rename(src, dst);
        char buf[512];
        snprintf(buf, sizeof(buf), "File received: %s", p[1]);
        ui_add_notification(buf);
    }
}

static void on_disconnect(const char *reason) {
    (void)reason;
    ui_add_notification("Disconnected from server.");
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
