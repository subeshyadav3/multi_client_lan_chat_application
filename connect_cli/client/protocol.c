/* protocol.c - turns incoming server lines into screen events.
 *
 * handle_line() is called once per received line. It splits the line on
 * '|' and updates the screen/app state for each known message type.
 * File chunks are handed straight to files.c.
 */
#include "client.h"
#include "net.h"
#include "../shared/constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Split a protocol line on '|' into up to `max` fields. */
static void parse_pipe(const char *msg, char out[][256], int max) {
    char buf[BUFFER_SIZE];
    strncpy(buf, msg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *save = NULL;
    char *tok = strtok_r(buf, "|", &save);
    int i = 0;
    while (tok && i < max) {
        strncpy(out[i], tok, 255);
        out[i][255] = 0;
        i++;
        tok = strtok_r(NULL, "|", &save);
    }
    for (int j = i; j < max; j++) out[j][0] = 0;
}

/* After a given field count of '|', return pointer to the rest (for base64). */
static const char *after_pipes(const char *m, int pipes_needed) {
    int pipes = 0;
    for (const char *cp = m; *cp; cp++) {
        if (*cp == '|') {
            pipes++;
            if (pipes == pipes_needed) return cp + 1;
        }
    }
    return NULL;
}

static void update_user_list(App *app, const char *data) {
    app->user_count = 0;
    char copy[MAX_MESSAGE];
    strncpy(copy, data, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok && app->user_count < MAX_CLIENTS;
         tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (colon) *colon = 0;
        if (!tok[0]) continue;
        strncpy(app->users[app->user_count], tok, MAX_USERNAME - 1);
        app->users[app->user_count][MAX_USERNAME - 1] = 0;
        app->user_count++;
    }
}

static void update_room_list(App *app, const char *data) {
    app->room_count = 0;
    char copy[MAX_MESSAGE];
    strncpy(copy, data, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok && app->room_count < MAX_ROOMS;
         tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (colon) *colon = 0;
        if (!tok[0]) continue;
        strncpy(app->rooms[app->room_count], tok, MAX_ROOM_NAME - 1);
        app->rooms[app->room_count][MAX_ROOM_NAME - 1] = 0;
        app->room_count++;
    }
}

static void request_lists(App *app) {
    net_send_line(app->sockfd, "LIST_USERS");
    net_send_line(app->sockfd, "LIST_ROOMS");
}

void handle_line(App *app, const char *line) {
    char p[6][256];
    parse_pipe(line, p, 6);

    if (strcmp(p[0], "LOGIN_OK") == 0) {
        app->logged_in = true;
        if (p[1][0]) strncpy(app->username, p[1], MAX_USERNAME - 1);
        app->is_admin = (strcmp(app->username, "admin") == 0);
        strncpy(app->current_room, "general", MAX_ROOM_NAME - 1);
        app->login_step = 0;
        app->mask_input = 0;
        strcpy(app->input, ""); app->input_len = 0;
        tui_add_notify(app, "Logged in as %s%s.", app->username, app->is_admin ? " (admin)" : "");
        request_lists(app);
    } else if (strcmp(p[0], "LOGIN_FAIL") == 0) {
        tui_add_line(app, LN_ERROR, "Login failed: %s", p[1][0] ? p[1] : "unknown error");
        app->login_step = 1;
        app->mask_input = 0;
        strcpy(app->input, ""); app->input_len = 0;
    } else if (strcmp(p[0], "PUBLIC") == 0 && p[1][0] && p[2][0]) {
        char text[MAX_MESSAGE];
        strncpy(text, p[3], MAX_MESSAGE - 1);
        char *nl = strchr(text, '\n'); if (nl) *nl = 0;
        tui_add_line(app, LN_NORMAL, "[%s] %s: %s", p[4][0] ? p[4] : "?", p[2], text);
    } else if (strcmp(p[0], "PRIVATE") == 0 && p[1][0]) {
        char text[MAX_MESSAGE];
        strncpy(text, p[3], MAX_MESSAGE - 1);
        char *nl = strchr(text, '\n'); if (nl) *nl = 0;
        tui_add_line(app, LN_PM, "[%s] (PM) %s: %s", p[4][0] ? p[4] : "?", p[1], text);
    } else if (strcmp(p[0], "NOTIFY") == 0) {
        char ntext[MAX_MESSAGE];
        strncpy(ntext, p[1], MAX_MESSAGE - 1);
        char *nl = strchr(ntext, '\n'); if (nl) *nl = 0;
        if (ntext[0]) tui_add_line(app, LN_NOTIFY, "%s", ntext);
    } else if (strcmp(p[0], "ANNOUNCE") == 0) {
        char atext[MAX_MESSAGE];
        strncpy(atext, p[2], MAX_MESSAGE - 1);
        char *nl = strchr(atext, '\n'); if (nl) *nl = 0;
        const char *who = p[1][0] ? p[1] : "server";
        const char *when = p[3][0] ? p[3] : "?";
        tui_add_line(app, LN_ANNOUNCE, "[ANNOUNCEMENT] %s @ %s: %s", who, when, atext);
    } else if (strcmp(p[0], "TYPING") == 0 && p[2][0]) {
        if (strcmp(p[2], app->username) != 0) {
            strncpy(app->typing, p[2], MAX_USERNAME - 1);
            app->typing[MAX_USERNAME - 1] = 0;
            app->typing_at = time(NULL);
        }
    } else if (strcmp(p[0], "USERS") == 0) {
        update_user_list(app, p[1]);
    } else if (strcmp(p[0], "ROOMS") == 0) {
        update_room_list(app, p[1]);
    } else if (strcmp(p[0], "JOIN_OK") == 0) {
        char room[MAX_ROOM_NAME];
        strncpy(room, p[1][0] ? p[1] : "general", MAX_ROOM_NAME - 1);
        strncpy(app->current_room, room, MAX_ROOM_NAME - 1);
        app->current_room[MAX_ROOM_NAME - 1] = 0;
        tui_add_notify(app, "Now in room #%s", room);
        request_lists(app);
    } else if (strcmp(p[0], "JOIN_FAIL") == 0) {
        tui_add_line(app, LN_ERROR, "Join failed: %s", p[1][0] ? p[1] : "unknown");
    } else if (strcmp(p[0], "ROOM_CREATED") == 0) {
        tui_add_notify(app, "Room '%s' created!", p[1]);
        request_lists(app);
    } else if (strcmp(p[0], "STATUS") == 0) {
        tui_add_line(app, LN_STATUS, "%s", p[1]);
    } else if (strcmp(p[0], "ACCOUNT_LIST") == 0) {
        tui_add_line(app, LN_STATUS, "Accounts: %s", p[1]);
    } else if (strcmp(p[0], "KICK") == 0) {
        tui_add_line(app, LN_ERROR, "You were kicked: %s", p[1][0] ? p[1] : "by administrator");
        app->connected = false;
    } else if (strcmp(p[0], "ERROR") == 0) {
        tui_add_line(app, LN_ERROR, "Error: %s", p[1][0] ? p[1] : "server error");
    } else if (strcmp(p[0], "FILE_GRANTED") == 0 && p[1][0]) {
        /* FILE_GRANTED|sender|filename|token|size - we are the sender. */
        strncpy(app->send_token, p[3], sizeof(app->send_token) - 1);
        if (p[4][0]) app->send_total = atol(p[4]);
        app->send_done = 0;
        app->send_state = 2; /* granted, waiting for recipient accept */
        tui_add_line(app, LN_FILE, "Granted to send '%s'. Waiting for the recipient to accept...", p[2]);
    } else if (strcmp(p[0], "FILE_DENIED") == 0) {
        tui_add_line(app, LN_ERROR, "File send denied: %s", p[3][0] ? p[3] : "by server");
        if (app->send_fp) { fclose(app->send_fp); app->send_fp = NULL; }
        app->send_state = 0;
    } else if (strcmp(p[0], "FILE_WAIT") == 0 && p[1][0]) {
        tui_add_line(app, LN_FILE, "Queue busy: '%s' queued at position %s. Waiting for a free slot...",
                     p[1], p[2][0] ? p[2] : "?");
        app->send_state = 1;
    } else if (strcmp(p[0], "FILE_OFFER") == 0 && p[1][0]) {
        files_add_offer(app, p[1], p[2], atol(p[3]), p[4]);
    } else if (strcmp(p[0], "FILE_ACCEPT") == 0 && p[1][0]) {
        /* FILE_ACCEPT|recipient|filename - recipient accepted our offer. */
        tui_add_line(app, LN_FILE, "%s accepted '%s'. Sending...", p[1], p[2]);
        if (app->send_fp) fclose(app->send_fp);
        app->send_fp = fopen(app->send_path, "rb");
        if (!app->send_fp) {
            tui_add_line(app, LN_ERROR, "Cannot reopen '%s' for sending", app->send_path);
            app->send_state = 0;
        } else {
            app->send_done = 0;
            app->send_state = 3; /* streaming */
        }
    } else if (strcmp(p[0], "FILE_REJECT") == 0 && p[1][0]) {
        tui_add_line(app, LN_FILE, "%s rejected '%s': %s", p[1], p[2], p[3][0] ? p[3] : "declined");
        if (app->send_fp) { fclose(app->send_fp); app->send_fp = NULL; }
        app->send_state = 0;
    } else if (strcmp(p[0], "FILE_DATA") == 0 && p[1][0]) {
        const char *b64 = after_pipes(line, 3);
        if (b64) files_receive_chunk(app, p[1], p[2], b64);
    } else if (strcmp(p[0], "FILE_END") == 0 && p[1][0]) {
        files_finalize_received(app);
    }
}
