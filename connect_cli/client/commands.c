/* commands.c - slash commands and the input processor.
 *
 * cmd_process() handles "/command rest" lines. process_input() is the
 * hook called from the main loop on Enter: it runs the login wizard, and
 * once logged in either dispatches a slash command or sends PUBLIC text.
 */
#include "client.h"
#include "net.h"
#include "../shared/constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

void send_typing(App *app) {
    if (!app || !app->connected || !app->logged_in) return;
    char out[128];
    snprintf(out, sizeof(out), "TYPING|%s", app->current_room);
    net_send_line(app->sockfd, out);
}

void process_input(App *app) {
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
        if (path[0]) files_request_send_file(app, target, path);
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
            files_remove_offer(app, sender, fn);
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
            files_remove_offer(app, sender, fn);
        } else tui_add_line(app, LN_ERROR, "Usage: /reject <offer#> [why]  or  /reject <sender> <filename> [why]");
    } else {
        tui_add_line(app, LN_ERROR, "Unknown command '/%s'. Type /help", cmd[0]?cmd:"");
    }
}
