#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "client.h"
#include "net.h"
#include "../shared/constants.h"

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

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64encode(const unsigned char *in, size_t len, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = in[i] << 16;
        if (i + 1 < len) n |= in[i + 1] << 8;
        if (i + 2 < len) n |= in[i + 2];
        out[o++] = b64tab[(n >> 18) & 63];
        out[o++] = b64tab[(n >> 12) & 63];
        out[o++] = (i + 1 < len) ? b64tab[(n >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? b64tab[n & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64decode(const char *in, unsigned char *out) {
    size_t o = 0;
    size_t len = strlen(in);
    int buf = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64val(in[i]);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((buf >> bits) & 0xFF);
        }
    }
    return o;
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
static const char *path_basename(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* Initiate a file send to `target` (empty = current room). */
static void remove_offer(App *app, const char *sender, const char *filename);

static void request_send_file(App *app, const char *target, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        tui_add_line(app, LN_ERROR, "Cannot open file: %s", path);
        return;
    }
    if (st.st_size > MAX_FILE_SIZE) {
        tui_add_line(app, LN_ERROR, "File too large (max %d MB)", MAX_FILE_SIZE / (1024*1024));
        return;
    }
    const char *name = path_basename(path);
    strncpy(app->send_filename, name, sizeof(app->send_filename)-1);
    strncpy(app->send_path, path, sizeof(app->send_path)-1);
    strncpy(app->send_target, target, sizeof(app->send_target)-1);
    app->send_total = (long)st.st_size;
    app->send_done = 0;
    app->send_state = 1; /* requested, waiting grant */
    char line[512];
    snprintf(line, sizeof(line), "FILE_REQUEST|%s|%ld|%s", name, (long)st.st_size, target);
    net_send_line(app->sockfd, line);
    tui_add_line(app, LN_FILE, "Offering '%s' (%ld bytes)%s ...", name, (long)st.st_size,
                 target[0] ? " to " : " to room");
}

void cmd_process(App *app, const char *line) {
    char cmd[128] = {0};
    char rest[MAX_MESSAGE] = {0};
    sscanf(line, "/%127[^ ] %2047[^\n]", cmd, rest);

    if (strcmp(cmd, "help") == 0) {
        tui_add_line(app, LN_STATUS, "-- ConnectHub commands --");
        tui_add_line(app, LN_STATUS, "  /msg <user> <text>       private 1-on-1 message");
        tui_add_line(app, LN_STATUS, "  /join <room> [password]  join a room   /leave = back to #general");
        tui_add_line(app, LN_STATUS, "  /create <room>  /createroom <name> [title|desc|pw]  /deleteroom <room>");
        tui_add_line(app, LN_STATUS, "  /who [room]  /history  /rooms  /users  /clear  /typing  /help  /quit");
        tui_add_line(app, LN_STATUS, "  /sendfile [@user] <path> offer a file (default: to room)");
        tui_add_line(app, LN_STATUS, "  /accept <offer#>         accept an incoming file (see [1] prompts)");
        tui_add_line(app, LN_STATUS, "  /reject <offer#> [why]   decline an incoming file");
        tui_add_line(app, LN_STATUS, "Admin: /announce <t> /kick <u> <why> /createuser <u> <p> /deleteuser <u>");
        tui_add_line(app, LN_STATUS, "       /resetpass <u> <p> /accounts /stats");
    } else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        if (app->connected) net_send_line(app->sockfd, "LOGOUT");
        client_quit(app);
    } else if (strcmp(cmd, "logout") == 0) {
        if (app->logged_in) {
            net_send_line(app->sockfd, "LOGOUT");
            app->logout_pending = true;
            tui_add_notify(app, "Logging out...");
        } else {
            tui_add_line(app, LN_ERROR, "You are not logged in");
        }
    } else if (strcmp(cmd, "msg") == 0) {
        char user[MAX_USERNAME] = {0}; char text[MAX_MESSAGE] = {0};
        sscanf(rest, "%31s %2047[^\n]", user, text);
        if (user[0] && text[0]) {
            char out[MAX_MESSAGE + 64];
            snprintf(out, sizeof(out), "PRIVATE|%s|%s", user, text);
            net_send_line(app->sockfd, out);
            char ts[32]; time_t t = time(NULL); strftime(ts, sizeof(ts), "%I:%M %p", localtime(&t));
            tui_add_line(app, LN_PM, "[%s] -> %s: %s", ts, user, text);
        } else tui_add_line(app, LN_ERROR, "Usage: /msg <username> <text>");
    } else if (strcmp(cmd, "join") == 0) {
        char room[MAX_ROOM_NAME] = {0}; char pw[64] = {0};
        int n = sscanf(rest, "%63s %63s", room, pw);
        if (n >= 1 && room[0]) {
            char out[128];
            if (pw[0]) snprintf(out, sizeof(out), "JOIN|%s|%s", room, pw);
            else       snprintf(out, sizeof(out), "JOIN|%s", room);
            net_send_line(app->sockfd, out);
        } else tui_add_line(app, LN_ERROR, "Usage: /join <room> [password]");
    } else if (strcmp(cmd, "leave") == 0) {
        net_send_line(app->sockfd, "LEAVE|general");
    } else if (strcmp(cmd, "create") == 0) {
        char room[MAX_ROOM_NAME] = {0};
        sscanf(rest, "%63s", room);
        if (room[0]) {
            char out[128]; snprintf(out, sizeof(out), "CREATE|%s", room);
            net_send_line(app->sockfd, out);
        } else tui_add_line(app, LN_ERROR, "Usage: /create <room>");
    } else if (strcmp(cmd, "createroom") == 0) {
        char name[MAX_ROOM_NAME]={0}, title[128]={0}, desc[256]={0}, pw[64]={0};
        (void)sscanf(rest, "%63s %127[^|]|%[^|]|%63s", name, title, desc, pw);
        if (name[0]) {
            char out[700];
            snprintf(out, sizeof(out), "CREATE_ROOM|%s|%s|%s|%s", name, title, desc, pw);
            net_send_line(app->sockfd, out);
        } else tui_add_line(app, LN_ERROR, "Usage: /createroom <name> [title|desc|password]");
    } else if (strcmp(cmd, "rooms") == 0) {
        net_send_line(app->sockfd, "LIST_ROOMS");
    } else if (strcmp(cmd, "who") == 0) {
        char room[MAX_ROOM_NAME] = {0};
        sscanf(rest, "%63s", room);
        if (!room[0]) strncpy(room, app->current_room, MAX_ROOM_NAME - 1);
        char out[160]; snprintf(out, sizeof(out), "WHO|%s", room);
        net_send_line(app->sockfd, out);
    } else if (strcmp(cmd, "history") == 0) {
        char out[160]; snprintf(out, sizeof(out), "HISTORY|%s", app->current_room);
        net_send_line(app->sockfd, out);
    } else if (strcmp(cmd, "deleteroom") == 0) {
        char room[MAX_ROOM_NAME] = {0};
        sscanf(rest, "%63s", room);
        if (room[0]) {
            char out[128]; snprintf(out, sizeof(out), "DELETE_ROOM|%s", room);
            net_send_line(app->sockfd, out);
        } else tui_add_line(app, LN_ERROR, "Usage: /deleteroom <room>");
    } else if (strcmp(cmd, "users") == 0) {
        net_send_line(app->sockfd, "LIST_USERS");
    } else if (strcmp(cmd, "clear") == 0) {
        app->line_count = 0;
    } else if (strcmp(cmd, "typing") == 0) {
        net_send_line(app->sockfd, "TYPING|general");
    } else if (strcmp(cmd, "stats") == 0) {
        net_send_line(app->sockfd, "STATS");
    } else if (strcmp(cmd, "announce") == 0) {
        if (rest[0]) {
            char out[MAX_MESSAGE + 64]; snprintf(out, sizeof(out), "ANNOUNCE|%s", rest);
            net_send_line(app->sockfd, out);
        } else tui_add_line(app, LN_ERROR, "Usage: /announce <text>");
    } else if (strcmp(cmd, "kick") == 0) {
        char user[MAX_USERNAME]={0}, why[256]={0};
        sscanf(rest, "%31s %255[^\n]", user, why);
        if (user[0]) {
            char out[MAX_MESSAGE+64]; snprintf(out, sizeof(out), "KICK|%s|%s", user, why[0]?why:"no reason");
            net_send_line(app->sockfd, out);
        } else tui_add_line(app, LN_ERROR, "Usage: /kick <user> <reason>");
    } else if (strcmp(cmd, "createuser") == 0) {
        char user[MAX_USERNAME]={0}, pw[64]={0};
        sscanf(rest, "%31s %63s", user, pw);
        if (user[0] && pw[0]) {
            char out[128]; snprintf(out, sizeof(out), "CREATE_USER|%s|%s", user, pw);
            net_send_line(app->sockfd, out);
        } else tui_add_line(app, LN_ERROR, "Usage: /createuser <user> <password>");
    } else if (strcmp(cmd, "deleteuser") == 0) {
        char user[MAX_USERNAME]={0};
        sscanf(rest, "%31s", user);
        if (user[0]) {
            char out[128]; snprintf(out, sizeof(out), "DELETE_USER|%s", user);
            net_send_line(app->sockfd, out);
        } else tui_add_line(app, LN_ERROR, "Usage: /deleteuser <user>");
    } else if (strcmp(cmd, "resetpass") == 0) {
        char user[MAX_USERNAME]={0}, pw[64]={0};
        sscanf(rest, "%31s %63s", user, pw);
        if (user[0] && pw[0]) {
            char out[128]; snprintf(out, sizeof(out), "RESET_PASS|%s|%s", user, pw);
            net_send_line(app->sockfd, out);
        } else tui_add_line(app, LN_ERROR, "Usage: /resetpass <user> <password>");
    } else if (strcmp(cmd, "accounts") == 0) {
        net_send_line(app->sockfd, "LIST_ACCOUNTS");
    } else if (strcmp(cmd, "sendfile") == 0) {
        char target[MAX_USERNAME] = {0};
        char path[512] = {0};
        if (rest[0] == '@') {
            sscanf(rest, "@%31s %511[^\n]", target, path);
        } else {
            sscanf(rest, "%511[^\n]", path);
        }
        if (path[0]) request_send_file(app, target, path);
        else tui_add_line(app, LN_ERROR, "Usage: /sendfile [@user] <file path>");
    } else if (strcmp(cmd, "accept") == 0) {
        char sender[MAX_USERNAME]={0}, fn[MAX_FILENAME]={0};
        int id = 0;
        if (sscanf(rest, "%d", &id) == 1 && id >= 1 && id <= app->offer_count) {
            strncpy(sender, app->offers[id-1].sender, sizeof(sender)-1);
            strncpy(fn, app->offers[id-1].filename, sizeof(fn)-1);
        } else {
            sscanf(rest, "%31s %255s", sender, fn);
        }
        if (sender[0] && fn[0]) {
            char out[512]; snprintf(out, sizeof(out), "FILE_ACCEPT|%s|%s", sender, fn);
            net_send_line(app->sockfd, out);
            remove_offer(app, sender, fn);
        } else tui_add_line(app, LN_ERROR, "Usage: /accept <offer#>  or  /accept <sender> <filename>");
    } else if (strcmp(cmd, "reject") == 0) {
        char sender[MAX_USERNAME]={0}, fn[MAX_FILENAME]={0}, why[256]={0};
        int id = 0;
        if (sscanf(rest, "%d", &id) == 1 && id >= 1 && id <= app->offer_count) {
            strncpy(sender, app->offers[id-1].sender, sizeof(sender)-1);
            strncpy(fn, app->offers[id-1].filename, sizeof(fn)-1);
            const char *after = rest;
            while (*after && *after != ' ' && *after != '\t') after++;
            while (*after == ' ' || *after == '\t') after++;
            if (after[0]) snprintf(why, sizeof(why), "%s", after);
        } else {
            sscanf(rest, "%31s %255s %255[^\n]", sender, fn, why);
        }
        if (sender[0] && fn[0]) {
            char out[512]; snprintf(out, sizeof(out), "FILE_REJECT|%s|%s|%s", sender, fn, why[0]?why:"declined");
            net_send_line(app->sockfd, out);
            remove_offer(app, sender, fn);
        } else tui_add_line(app, LN_ERROR, "Usage: /reject <offer#> [why]  or  /reject <sender> <filename> [why]");
    } else {
        tui_add_line(app, LN_ERROR, "Unknown command '/%s'. Type /help", cmd[0]?cmd:"");
    }
}
static PendingOffer *find_offer(App *app, const char *sender, const char *filename) {
    for (int i = 0; i < app->offer_count; i++) {
        if (strcmp(app->offers[i].sender, sender) == 0 &&
            strcmp(app->offers[i].filename, filename) == 0)
            return &app->offers[i];
    }
    return NULL;
}

static void add_offer(App *app, const char *sender, const char *filename, long size, const char *target) {
    if (find_offer(app, sender, filename)) return;
    if (app->offer_count >= MAX_PENDING_OFFERS) {
        tui_add_line(app, LN_FILE, "%s offers '%s' (%ld bytes)%s (offer queue full)",
                     sender, filename, size, target[0] ? " to you" : " to room");
        return;
    }
    PendingOffer *o = &app->offers[app->offer_count];
    strncpy(o->sender, sender, sizeof(o->sender)-1);
    strncpy(o->filename, filename, sizeof(o->filename)-1);
    strncpy(o->target, target, sizeof(o->target)-1);
    o->size = size;
    int id = app->offer_count + 1;
    app->offer_count++;
    tui_add_line(app, LN_FILE, "[%d] %s offers '%s' (%ld bytes)%s. /accept %d or /reject %d",
                 id, sender, filename, size, target[0] ? " to you" : " to room", id, id);
}

static void remove_offer(App *app, const char *sender, const char *filename) {
    for (int i = 0; i < app->offer_count; i++) {
        if (strcmp(app->offers[i].sender, sender) == 0 &&
            strcmp(app->offers[i].filename, filename) == 0) {
            memmove(&app->offers[i], &app->offers[i + 1],
                    sizeof(PendingOffer) * (app->offer_count - i - 1));
            app->offer_count--;
            return;
        }
    }
}

/* Rename .tmp to a unique files/<name> (append (n) if needed). */
static void finalize_received(App *app) {
    if (!app->receiving || !app->recv_fp) return;
    fclose(app->recv_fp);
    app->recv_fp = NULL;
    char finalpath[512];
    snprintf(finalpath, sizeof(finalpath), "files/%s", app->recv_filename);
    char tmppath[512];
    snprintf(tmppath, sizeof(tmppath), "files/%s.tmp", app->recv_filename);
    FILE *exists = fopen(finalpath, "r");
    if (exists) {
        fclose(exists);
        /* append unique suffix */
        char unique[600];
        int n = 1;
        do {
            snprintf(unique, sizeof(unique), "files/%s (%d)", app->recv_filename, n++);
            exists = fopen(unique, "r");
            if (exists) { fclose(exists); continue; }
        } while (0);
        snprintf(finalpath, sizeof(finalpath), "%s", unique);
    }
    rename(tmppath, finalpath);
    tui_add_line(app, LN_FILE, "Received '%s' (%ld bytes) -> %s", app->recv_filename, app->recv_done, finalpath);
    app->receiving = false;
    app->recv_filename[0] = 0;
    app->recv_done = 0;
}

void handle_line(App *app, const char *line) {
    char p[6][256];
    parse_pipe(line, p, 6);

    if (strcmp(p[0], "LOGIN_OK") == 0) {
        app->logged_in = true;
        if (p[1][0]) strncpy(app->username, p[1], MAX_USERNAME-1);
        app->is_admin = (strcmp(app->username, "admin") == 0);
        strncpy(app->current_room, "general", MAX_ROOM_NAME-1);
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
        strncpy(text, p[3], MAX_MESSAGE-1);
        char *nl = strchr(text, '\n'); if (nl) *nl = 0;
        tui_add_line(app, LN_NORMAL, "[%s] %s: %s", p[4][0]?p[4]:"?", p[2], text);
    } else if (strcmp(p[0], "PRIVATE") == 0 && p[1][0]) {
        char text[MAX_MESSAGE];
        strncpy(text, p[3], MAX_MESSAGE-1);
        char *nl = strchr(text, '\n'); if (nl) *nl = 0;
        tui_add_line(app, LN_PM, "[%s] (PM) %s: %s", p[4][0]?p[4]:"?", p[1], text);
    } else if (strcmp(p[0], "NOTIFY") == 0) {
        char ntext[MAX_MESSAGE];
        strncpy(ntext, p[1], MAX_MESSAGE-1);
        char *nl = strchr(ntext, '\n'); if (nl) *nl = 0;
        if (ntext[0]) tui_add_line(app, LN_NOTIFY, "%s", ntext);
    } else if (strcmp(p[0], "ANNOUNCE") == 0) {
        char atext[MAX_MESSAGE];
        strncpy(atext, p[2], MAX_MESSAGE-1);
        char *nl = strchr(atext, '\n'); if (nl) *nl = 0;
        const char *who = p[1][0] ? p[1] : "server";
        const char *when = p[3][0] ? p[3] : "?";
        tui_add_line(app, LN_ANNOUNCE, "[ANNOUNCEMENT] %s @ %s: %s", who, when, atext);
    } else if (strcmp(p[0], "TYPING") == 0 && p[2][0]) {
        if (strcmp(p[2], app->username) != 0) {
            strncpy(app->typing, p[2], MAX_USERNAME-1);
            app->typing[MAX_USERNAME-1] = 0;
            app->typing_at = time(NULL);
        }
    } else if (strcmp(p[0], "USERS") == 0) {
        update_user_list(app, p[1]);
    } else if (strcmp(p[0], "ROOMS") == 0) {
        update_room_list(app, p[1]);
    } else if (strcmp(p[0], "JOIN_OK") == 0) {
        char room[MAX_ROOM_NAME];
        strncpy(room, p[1][0]?p[1]:"general", MAX_ROOM_NAME-1);
        strncpy(app->current_room, room, MAX_ROOM_NAME-1);
        app->current_room[MAX_ROOM_NAME-1] = 0;
        tui_add_notify(app, "Now in room #%s", room);
        request_lists(app);
    } else if (strcmp(p[0], "JOIN_FAIL") == 0) {
        tui_add_line(app, LN_ERROR, "Join failed: %s", p[1][0]?p[1]:"unknown");
    } else if (strcmp(p[0], "ROOM_CREATED") == 0) {
        tui_add_notify(app, "Room '%s' created!", p[1]);
        request_lists(app);
    } else if (strcmp(p[0], "STATUS") == 0) {
        tui_add_line(app, LN_STATUS, "%s", p[1]);
    } else if (strcmp(p[0], "ACCOUNT_LIST") == 0) {
        tui_add_line(app, LN_STATUS, "Accounts: %s", p[1]);
    } else if (strcmp(p[0], "KICK") == 0) {
        tui_add_line(app, LN_ERROR, "You were kicked: %s", p[1][0]?p[1]:"by administrator");
        app->connected = false;
    } else if (strcmp(p[0], "ERROR") == 0) {
        tui_add_line(app, LN_ERROR, "Error: %s", p[1][0]?p[1]:"server error");
    } else if (strcmp(p[0], "FILE_GRANTED") == 0 && p[1][0]) {
        /* FILE_GRANTED|sender|filename|token|size - we are the sender. */
        strncpy(app->send_token, p[3], sizeof(app->send_token)-1);
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
        add_offer(app, p[1], p[2], atol(p[3]), p[4]);
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
        tui_add_line(app, LN_FILE, "%s rejected '%s': %s", p[1], p[2], p[3][0]?p[3]:"declined");
        if (app->send_fp) { fclose(app->send_fp); app->send_fp = NULL; }
        app->send_state = 0;
    } else if (strcmp(p[0], "FILE_DATA") == 0 && p[1][0]) {
        const char *b64 = after_pipes(line, 3);
        if (b64) {
            char b64copy[BUFFER_SIZE];
            strncpy(b64copy, b64, sizeof(b64copy)-1);
            b64copy[sizeof(b64copy)-1] = 0;
            char *nl = strchr(b64copy, '\n'); if (nl) *nl = 0;
            if (!app->receiving || strcmp(app->recv_filename, p[2]) != 0) {
                /* start a fresh receive */
                if (app->recv_fp) { fclose(app->recv_fp); app->recv_fp = NULL; }
                strncpy(app->recv_filename, p[2], sizeof(app->recv_filename)-1);
                strncpy(app->recv_sender, p[1], sizeof(app->recv_sender)-1);
                PendingOffer *o = find_offer(app, p[1], p[2]);
                app->recv_total = o ? o->size : 0;
                app->recv_done = 0;
                char tmppath[512];
                snprintf(tmppath, sizeof(tmppath), "files/%s.tmp", p[2]);
                app->recv_fp = fopen(tmppath, "wb");
                app->receiving = true;
            }
            if (app->recv_fp) {
                unsigned char decoded[BUFFER_SIZE];
                size_t got = b64decode(b64copy, decoded);
                fwrite(decoded, 1, got, app->recv_fp);
                app->recv_done += (long)got;
                if (app->recv_total > 0 && app->receiving) {
                    int pct = (int)(app->recv_done * 100 / app->recv_total);
                    if (pct % 25 == 0)
                        tui_add_line(app, LN_FILE, "Receiving '%s': %d%%", p[2], pct);
                }
            }
        }
    } else if (strcmp(p[0], "FILE_END") == 0 && p[1][0]) {
        finalize_received(app);
    }
}

/* Send one chunk of the outbound file transfer (called from the main loop). */
static void try_send_chunk(App *app) {
    if (app->send_state != 3 || !app->send_fp) return;
    unsigned char raw[FILE_CHUNK_SIZE];
    size_t got = fread(raw, 1, FILE_CHUNK_SIZE, app->send_fp);
    if (got == 0) {
        fclose(app->send_fp);
        app->send_fp = NULL;
        char line[MAX_MESSAGE + 64];
        snprintf(line, sizeof(line), "FILE_END|%s", app->send_filename);
        net_send_line(app->sockfd, line);
        tui_add_line(app, LN_FILE, "Sent '%s' (%ld bytes).", app->send_filename, app->send_total);
        app->send_state = 0;
        return;
    }
    char b64[BUFFER_SIZE];
    b64encode(raw, got, b64);
    char out[BUFFER_SIZE + 64];
    snprintf(out, sizeof(out), "FILE_DATA|%s|%s|%s", app->send_filename, app->send_token, b64);
    net_send_line(app->sockfd, out);
    app->send_done += (long)got;
    if (app->send_total > 0) {
        int pct = (int)(app->send_done * 100 / app->send_total);
        if (pct % 10 == 0)
            tui_add_line(app, LN_FILE, "Uploading '%s': %d%%", app->send_filename, pct);
    }
}
static void push_history(App *app, const char *s) {
    if (app->history_count == 0 ||
        strcmp(app->history[app->history_count - 1], s) != 0) {
        if (app->history_count >= MAX_CMDHIST) {
            memmove(&app->history[0], &app->history[1],
                    sizeof(app->history[0]) * (MAX_CMDHIST - 1));
            app->history_count--;
        }
        strncpy(app->history[app->history_count], s, sizeof(app->history[0]) - 1);
        app->history[app->history_count][sizeof(app->history[0]) - 1] = 0;
        app->history_count++;
    }
    app->history_index = app->history_count;
}

static void send_typing(App *app) {
    if (!app || !app->connected || !app->logged_in) return;
    char out[128];
    snprintf(out, sizeof(out), "TYPING|%s", app->current_room);
    net_send_line(app->sockfd, out);
}

static void process_input(App *app) {
    if (app->input_len == 0) return;
    char inp[MAX_MESSAGE];
    strncpy(inp, app->input, sizeof(inp) - 1);
    inp[sizeof(inp) - 1] = 0;

    if (!app->logged_in) {
        if (app->login_step == 1) {
            strncpy(app->username, inp, MAX_USERNAME - 1);
            app->username[MAX_USERNAME - 1] = 0;
            app->login_step = 2;
            app->mask_input = 1;
            tui_add_notify(app, "Enter password for '%s':", app->username);
        } else {
            char out[160];
            snprintf(out, sizeof(out), "LOGIN|%s|%s", app->username, inp);
            net_send_line(app->sockfd, out);
            app->mask_input = 0;
            app->login_step = 0;
        }
        strcpy(app->input, "");
        app->input_len = 0;
        return;
    }

    push_history(app, inp);
    if (inp[0] == '/') {
        cmd_process(app, inp);
    } else {
        char out[MAX_MESSAGE + 128];
        snprintf(out, sizeof(out), "PUBLIC|%s|%s", app->current_room, inp);
        net_send_line(app->sockfd, out);
        char ts[32];
        time_t t = time(NULL);
        strftime(ts, sizeof(ts), "%I:%M %p", localtime(&t));
        strncpy(app->typing, "", sizeof(app->typing));
        tui_add_line(app, LN_SELF, "[%s] %s: %s", ts, app->username, inp);
    }
    strcpy(app->input, "");
    app->input_len = 0;
}

void client_quit(App *app) {
    tui_restore();
    net_close(app->sockfd);
    exit(0);
}
static volatile sig_atomic_t g_quit = 0;
static void on_signal(int s) { (void)s; g_quit = 1; }

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = PORT;
    const char *user = NULL, *pass = NULL;

    /* Flags (easy) with legacy positional fallback:
     *   --host/--port/--user/--pass   or   host port [user pass] */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)      host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) user = argv[++i];
        else if (strcmp(argv[i], "--pass") == 0 && i + 1 < argc) pass = argv[++i];
        else if (strcmp(argv[i], "--admin") == 0)                { user = "admin"; }
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: chatclient [--host <host>] [--port <port>] [--user <u>] [--pass <p>] [--admin]\n"
                   "  legacy: chatclient <host> <port> <username> <password>\n");
            return 0;
        }
        else if (i == 1) host = argv[i];      /* legacy positional */
        else if (i == 2) port = atoi(argv[i]);
        else if (i == 3) user = argv[i];
        else if (i == 4) pass = argv[i];
    }

    App app;
    memset(&app, 0, sizeof(app));
    strncpy(app.current_room, "general", MAX_ROOM_NAME - 1);
    app.login_step = 1;
    app.history_index = -1;
    app.input_len = 0;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    tui_enter_raw();

    char rbuf[BUFFER_SIZE];
    char rline[BUFFER_SIZE];

    bool first_connect = true;
    while (!g_quit) {
        int fd = net_connect(host, port);
        if (fd < 0) {
            tui_restore();
            printf("Failed to connect to %s:%d\n", host, port);
            return 1;
        }
        app.sockfd = fd;
        app.connected = true;
        app.logout_pending = false;

        /* Reset to the login screen after a logout/reconnect. */
        app.logged_in = false;
        app.is_admin = false;
        app.username[0] = 0;
        app.current_room[0] = 0;
        strncpy(app.current_room, "general", MAX_ROOM_NAME - 1);
        app.login_step = 1;
        app.mask_input = 0;
        app.line_count = 0;
        app.user_count = 0;
        app.room_count = 0;
        app.typing[0] = 0;
        app.offer_count = 0;
        app.receiving = false;
        strcpy(app.input, ""); app.input_len = 0;

        tui_add_notify(&app, "Connected to %s:%d. Enter your username:", host, port);
        tui_draw(&app);

        /* Optional auto-login: chatclient --user <u> --pass <p> (or positional).
         * Only on the very first connection; a later /logout must re-prompt. */
        if (user && pass && first_connect) {
            strncpy(app.username, user, MAX_USERNAME - 1);
            app.username[MAX_USERNAME - 1] = 0;
            char out[160];
            snprintf(out, sizeof(out), "LOGIN|%s|%s", user, pass);
            net_send_line(fd, out);
            app.login_step = 0;
            tui_add_notify(&app, "Logging in as %s...", user);
        }
        first_connect = false;

        size_t rpos = 0;
        while (app.connected && !g_quit) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            FD_SET(fd, &rfds);
            struct timeval tv = {0, 100000};
            int maxfd = (fd > STDIN_FILENO ? fd : STDIN_FILENO) + 1;
            int r = select(maxfd, &rfds, NULL, NULL, &tv);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (FD_ISSET(fd, &rfds)) {
                ssize_t n = recv(fd, rbuf, sizeof(rbuf) - 1, 0);
                if (n <= 0) {
                    if (app.logout_pending) break;
                    tui_add_notify(&app, "Disconnected from server.");
                    app.connected = false;
                    break;
                }
                rbuf[n] = 0;
                for (ssize_t i = 0; i < n; i++) {
                    if (rbuf[i] == '\n') {
                        rline[rpos] = 0;
                        rpos = 0;
                        handle_line(&app, rline);
                    } else if (rpos < sizeof(rline) - 1) {
                        rline[rpos++] = rbuf[i];
                    }
                }
            }

            if (FD_ISSET(STDIN_FILENO, &rfds)) {
                char c;
                if (read(STDIN_FILENO, &c, 1) != 1) continue;
                if (c == 3) { g_quit = 1; break; }        /* Ctrl-C */
                else if (c == 10 || c == 13) process_input(&app);
                else if (c == 127 || c == 8) {             /* backspace */
                    if (app.input_len > 0) app.input[--app.input_len] = 0;
                } else if (c == 27) {                      /* escape / arrows */
                    char seq[3]; seq[0] = c;
                    if (read(STDIN_FILENO, &seq[1], 1) == 1 && seq[1] == '[') {
                        if (read(STDIN_FILENO, &seq[2], 1) == 1) {
                            if (seq[2] == 'A') {           /* up */
                                if (app.history_index > 0) {
                                    app.history_index--;
                                    tui_set_input(&app, app.history[app.history_index]);
                                }
                            } else if (seq[2] == 'B') {    /* down */
                                if (app.history_index < app.history_count) {
                                    app.history_index++;
                                    if (app.history_index < app.history_count)
                                        tui_set_input(&app, app.history[app.history_index]);
                                    else { strcpy(app.input, ""); app.input_len = 0; }
                                }
                            }
                        }
                    }
            } else if (c >= 32 && c < 127) {
                if (app.input_len < (int)sizeof(app.input) - 1)
                    app.input[app.input_len++] = c;
                app.input[app.input_len] = 0;
                app.last_typed = time(NULL);
                send_typing(&app);
            }
        }

        /* Auto-clear typing indicator after 2.5s of silence. */
        if (app.typing[0] && (time(NULL) - app.typing_at) > 2) {
            app.typing[0] = 0;
        }
        try_send_chunk(&app);
        tui_draw(&app);
        }

        net_close(fd);
        app.connected = false;
        if (!app.logout_pending || g_quit) break;
    }

    tui_restore();
    return 0;
}






