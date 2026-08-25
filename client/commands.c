/* commands.c - slash commands and the input processor.
 *
 * process_input() is called by the main loop when the user presses Enter.
 * While not logged in, it drives the login wizard (username then password).
 * Once logged in, it either runs a slash command (e.g. /msg) or, for any
 * other text, sends it to the current room as a PUBLIC message.
 *
 * cmd_process() is the small dispatcher that figures out which slash
 * command was typed, then calls the matching helper below. Every helper
 * builds the exact '|'-separated line the server expects.
 */
#include "client.h"
#include "net.h"
#include "../shared/constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- command history (for the up/down arrow keys) ---- */

/* Remember this command so the user can recall it with the arrow keys. */
static void push_history(App *app, const char *s) {
    /* Don't store a duplicate of the most recent command. */
    if (app->history_count > 0 &&
        strcmp(app->history[app->history_count - 1], s) == 0) {
        app->history_index = app->history_count;
        return;
    }
    /* If the list is full, drop the oldest entry to make room. */
    if (app->history_count >= MAX_CMDHIST) {
        memmove(&app->history[0], &app->history[1],
                sizeof(app->history[0]) * (MAX_CMDHIST - 1));
        app->history_count--;
    }
    strncpy(app->history[app->history_count], s, sizeof(app->history[0]) - 1);
    app->history[app->history_count][sizeof(app->history[0]) - 1] = 0;
    app->history_count++;
    app->history_index = app->history_count;
}

/* ---- the login wizard ---- */

/* Tell the server we are typing in the current room (drives the indicator). */
void send_typing(App *app) {
    if (!app || !app->connected || !app->logged_in) return;
    char out[128];
    snprintf(out, sizeof(out), "TYPING|%s", app->current_room);
    net_send_line(app->sockfd, out);
}

/* Handle one line of the login wizard while the user is not yet logged in. */
static void handle_login_input(App *app, const char *inp) {
    if (app->login_step == 1) {
        /* Step 1: the user typed a username; ask for the password next. */
        strncpy(app->username, inp, MAX_USERNAME - 1);
        app->username[MAX_USERNAME - 1] = 0;
        app->login_step = 2;
        app->mask_input = 1;
        tui_add_notify(app, "Enter password for '%s':", app->username);
    } else {
        /* Step 2: the user typed a password; send a LOGIN line to the server. */
        char out[160];
        snprintf(out, sizeof(out), "LOGIN|%s|%s", app->username, inp);
        net_send_line(app->sockfd, out);
        app->mask_input = 0;
        app->login_step = 0;
    }
    strcpy(app->input, "");
    app->input_len = 0;
}

/* The main input hook: called by the main loop on Enter. */
void process_input(App *app) {
    if (app->input_len == 0) return;
    /* Make a safe copy of what the user typed. */
    char inp[MAX_MESSAGE];
    strncpy(inp, app->input, sizeof(inp) - 1);
    inp[sizeof(inp) - 1] = 0;

    if (!app->logged_in) {
        handle_login_input(app, inp);
        return;
    }

    push_history(app, inp);
    if (inp[0] == '/') {
        cmd_process(app, inp);
    } else {
        /* Plain text: send it to the current room and echo it on our screen. */
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

/* ---- individual slash-command helpers ---- */

static void cmd_help(App *app) {
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
}

static void cmd_quit(App *app) {
    if (app->connected) net_send_line(app->sockfd, "LOGOUT");
    client_quit(app);
}

static void cmd_logout(App *app) {
    if (app->logged_in) {
        net_send_line(app->sockfd, "LOGOUT");
        app->logout_pending = true;
        tui_add_notify(app, "Logging out...");
    } else {
        tui_add_line(app, LN_ERROR, "You are not logged in");
    }
}

static void cmd_msg(App *app, const char *rest) {
    char user[MAX_USERNAME] = {0}; char text[MAX_MESSAGE] = {0};
    sscanf(rest, "%31s %2047[^\n]", user, text);
    if (user[0] && text[0]) {
        char out[MAX_MESSAGE + 64];
        snprintf(out, sizeof(out), "PRIVATE|%s|%s", user, text);
        net_send_line(app->sockfd, out);
        char ts[32]; time_t t = time(NULL); strftime(ts, sizeof(ts), "%I:%M %p", localtime(&t));
        tui_add_line(app, LN_PM, "[%s] -> %s: %s", ts, user, text);
    } else tui_add_line(app, LN_ERROR, "Usage: /msg <username> <text>");
}

static void cmd_join(App *app, const char *rest) {
    char room[MAX_ROOM_NAME] = {0}; char pw[64] = {0};
    int n = sscanf(rest, "%63s %63s", room, pw);
    if (n >= 1 && room[0]) {
        char out[128];
        if (pw[0]) snprintf(out, sizeof(out), "JOIN|%s|%s", room, pw);
        else       snprintf(out, sizeof(out), "JOIN|%s", room);
        net_send_line(app->sockfd, out);
    } else tui_add_line(app, LN_ERROR, "Usage: /join <room> [password]");
}

static void parse_room_args(const char *rest, char *name, char *title, char *desc, char *pw) {
    name[0] = title[0] = desc[0] = pw[0] = '\0';
    if (!rest || !rest[0]) return;

    /* If pipe-delimited format was used: e.g. dev Title|Desc|password */
    if (strchr(rest, '|')) {
        char copy[512];
        strncpy(copy, rest, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *first_pipe = strchr(copy, '|');
        char *first_space = strchr(copy, ' ');
        if (first_space && first_space < first_pipe) {
            *first_space = '\0';
            strncpy(name, copy, MAX_ROOM_NAME - 1);
            char *p = first_space + 1;
            while (*p == ' ') p++;
            char *p1 = strchr(p, '|');
            if (p1) {
                *p1 = '\0';
                strncpy(title, p, 127);
                char *p2 = strchr(p1 + 1, '|');
                if (p2) {
                    *p2 = '\0';
                    strncpy(desc, p1 + 1, 255);
                    strncpy(pw, p2 + 1, 63);
                } else {
                    strncpy(desc, p1 + 1, 255);
                }
            } else {
                strncpy(title, p, 127);
            }
        } else {
            char *p1 = strchr(copy, '|');
            *p1 = '\0';
            strncpy(name, copy, MAX_ROOM_NAME - 1);
            char *p2 = strchr(p1 + 1, '|');
            if (p2) {
                *p2 = '\0';
                strncpy(title, p1 + 1, 127);
                char *p3 = strchr(p2 + 1, '|');
                if (p3) {
                    *p3 = '\0';
                    strncpy(desc, p2 + 1, 255);
                    strncpy(pw, p3 + 1, 63);
                } else {
                    strncpy(desc, p2 + 1, 255);
                }
            } else {
                strncpy(title, p1 + 1, 127);
            }
        }
        if (!title[0]) strncpy(title, name, 127);
        return;
    }

    /* Tokenize by quotes or spaces (e.g. dev "Dev Team" "Secret Room" dev123) */
    char tokens[4][256];
    memset(tokens, 0, sizeof(tokens));
    int count = 0;
    const char *p = rest;
    while (*p && count < 4) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            int len = 0;
            while (*p && *p != '"' && len < 255) {
                tokens[count][len++] = *p++;
            }
            tokens[count][len] = '\0';
            if (*p == '"') p++;
        } else {
            int len = 0;
            while (*p && *p != ' ' && *p != '\t' && len < 255) {
                tokens[count][len++] = *p++;
            }
            tokens[count][len] = '\0';
        }
        count++;
    }

    if (count >= 1) strncpy(name, tokens[0], MAX_ROOM_NAME - 1);
    if (count == 1) {
        strncpy(title, tokens[0], 127);
    } else if (count == 2) {
        /* /create dev dev123 or /createroom dev dev123 -> tokens[0]=name, tokens[1]=password */
        strncpy(title, tokens[0], 127);
        strncpy(pw, tokens[1], 63);
    } else if (count == 3) {
        /* /createroom dev "Dev Team" dev123 -> name, title, password */
        strncpy(title, tokens[1], 127);
        strncpy(pw, tokens[2], 63);
    } else if (count >= 4) {
        /* /createroom dev "Dev Team" "Desc" dev123 -> name, title, desc, password */
        strncpy(title, tokens[1], 127);
        strncpy(desc, tokens[2], 255);
        strncpy(pw, tokens[3], 63);
    }
}

static void cmd_create(App *app, const char *rest) {
    char name[MAX_ROOM_NAME]={0}, title[128]={0}, desc[256]={0}, pw[64]={0};
    parse_room_args(rest, name, title, desc, pw);
    if (name[0]) {
        char out[700];
        snprintf(out, sizeof(out), "CREATE_ROOM|%s|%s|%s|%s", name, title, desc, pw);
        net_send_line(app->sockfd, out);
    } else {
        tui_add_line(app, LN_ERROR, "Usage: /create <room> [password]");
    }
}

static void cmd_createroom(App *app, const char *rest) {
    char name[MAX_ROOM_NAME]={0}, title[128]={0}, desc[256]={0}, pw[64]={0};
    parse_room_args(rest, name, title, desc, pw);
    if (name[0]) {
        char out[700];
        snprintf(out, sizeof(out), "CREATE_ROOM|%s|%s|%s|%s", name, title, desc, pw);
        net_send_line(app->sockfd, out);
    } else {
        tui_add_line(app, LN_ERROR, "Usage: /createroom <name> [password] or /createroom <name> [title] [desc] [password]");
    }
}

static void cmd_rooms(App *app) {
    char list[MAX_MESSAGE] = {0};
    for (int i = 0; i < app->room_count; i++) {
        if (i > 0) strncat(list, ", ", sizeof(list) - strlen(list) - 1);
        strncat(list, "#", sizeof(list) - strlen(list) - 1);
        strncat(list, app->rooms[i], sizeof(list) - strlen(list) - 1);
    }
    tui_add_line(app, LN_STATUS, "Rooms online (%d): %s", app->room_count, list[0] ? list : "#general");
    net_send_line(app->sockfd, "LIST_ROOMS");
}

static void cmd_users(App *app) {
    char list[MAX_MESSAGE] = {0};
    for (int i = 0; i < app->user_count; i++) {
        if (i > 0) strncat(list, ", ", sizeof(list) - strlen(list) - 1);
        strncat(list, app->users[i], sizeof(list) - strlen(list) - 1);
    }
    tui_add_line(app, LN_STATUS, "Users online (%d): %s", app->user_count, list[0] ? list : "none");
    net_send_line(app->sockfd, "LIST_USERS");
}

static void cmd_who(App *app, const char *rest) {
    char room[MAX_ROOM_NAME] = {0};
    sscanf(rest, "%63s", room);
    if (!room[0]) strncpy(room, app->current_room, MAX_ROOM_NAME - 1);
    char out[160]; snprintf(out, sizeof(out), "WHO|%s", room);
    net_send_line(app->sockfd, out);
}

static void cmd_history(App *app) {
    char out[160]; snprintf(out, sizeof(out), "HISTORY|%s", app->current_room);
    net_send_line(app->sockfd, out);
}

static void cmd_deleteroom(App *app, const char *rest) {
    char room[MAX_ROOM_NAME] = {0};
    sscanf(rest, "%63s", room);
    if (room[0]) {
        char out[128]; snprintf(out, sizeof(out), "DELETE_ROOM|%s", room);
        net_send_line(app->sockfd, out);
    } else tui_add_line(app, LN_ERROR, "Usage: /deleteroom <room>");
}

static void cmd_announce(App *app, const char *rest) {
    if (rest[0]) {
        char out[MAX_MESSAGE + 64]; snprintf(out, sizeof(out), "ANNOUNCE|%s", rest);
        net_send_line(app->sockfd, out);
    } else tui_add_line(app, LN_ERROR, "Usage: /announce <text>");
}

static void cmd_kick(App *app, const char *rest) {
    char user[MAX_USERNAME]={0}, why[256]={0};
    sscanf(rest, "%31s %255[^\n]", user, why);
    if (user[0]) {
        char out[MAX_MESSAGE+64]; snprintf(out, sizeof(out), "KICK|%s|%s", user, why[0]?why:"no reason");
        net_send_line(app->sockfd, out);
    } else tui_add_line(app, LN_ERROR, "Usage: /kick <user> <reason>");
}

static void cmd_createuser(App *app, const char *rest) {
    char user[MAX_USERNAME]={0}, pw[64]={0};
    sscanf(rest, "%31s %63s", user, pw);
    if (user[0] && pw[0]) {
        char out[128]; snprintf(out, sizeof(out), "CREATE_USER|%s|%s", user, pw);
        net_send_line(app->sockfd, out);
    } else tui_add_line(app, LN_ERROR, "Usage: /createuser <user> <password>");
}

static void cmd_deleteuser(App *app, const char *rest) {
    char user[MAX_USERNAME]={0};
    sscanf(rest, "%31s", user);
    if (user[0]) {
        char out[128]; snprintf(out, sizeof(out), "DELETE_USER|%s", user);
        net_send_line(app->sockfd, out);
    } else tui_add_line(app, LN_ERROR, "Usage: /deleteuser <user>");
}

static void cmd_resetpass(App *app, const char *rest) {
    char user[MAX_USERNAME]={0}, pw[64]={0};
    sscanf(rest, "%31s %63s", user, pw);
    if (user[0] && pw[0]) {
        char out[128]; snprintf(out, sizeof(out), "RESET_PASS|%s|%s", user, pw);
        net_send_line(app->sockfd, out);
    } else tui_add_line(app, LN_ERROR, "Usage: /resetpass <user> <password>");
}

static void cmd_sendfile(App *app, const char *rest) {
    char target[MAX_USERNAME] = {0};
    char path[512] = {0};
    if (rest[0] == '@') {
        sscanf(rest, "@%31s %511[^\n]", target, path);
    } else {
        sscanf(rest, "%511[^\n]", path);
    }
    if (path[0]) files_request_send_file(app, target, path);
    else tui_add_line(app, LN_ERROR, "Usage: /sendfile [@user] <file path>");
}

static void cmd_accept(App *app, const char *rest) {
    char sender[MAX_USERNAME]={0}, fn[MAX_FILENAME]={0};
    int id = 0;
    if (sscanf(rest, "%d", &id) == 1 && id >= 1 && id <= app->offer_count) {
        /* The user referred to an offer by number: look it up. */
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
}

static void cmd_reject(App *app, const char *rest) {
    char sender[MAX_USERNAME]={0}, fn[MAX_FILENAME]={0}, why[256]={0};
    int id = 0;
    if (sscanf(rest, "%d", &id) == 1 && id >= 1 && id <= app->offer_count) {
        strncpy(sender, app->offers[id-1].sender, sizeof(sender)-1);
        strncpy(fn, app->offers[id-1].filename, sizeof(fn)-1);
        /* The rest after the offer number is the "why" reason. */
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
}

/* Dispatcher: match the command word to its helper. */
void cmd_process(App *app, const char *line) {
    char cmd[128] = {0};
    char rest[MAX_MESSAGE] = {0};
    sscanf(line, "/%127[^ ] %2047[^\n]", cmd, rest);

    if (strcmp(cmd, "help") == 0)            cmd_help(app);
    else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) cmd_quit(app);
    else if (strcmp(cmd, "logout") == 0)     cmd_logout(app);
    else if (strcmp(cmd, "msg") == 0)        cmd_msg(app, rest);
    else if (strcmp(cmd, "join") == 0)       cmd_join(app, rest);
    else if (strcmp(cmd, "leave") == 0)      net_send_line(app->sockfd, "LEAVE|general");
    else if (strcmp(cmd, "create") == 0)     cmd_create(app, rest);
    else if (strcmp(cmd, "createroom") == 0) cmd_createroom(app, rest);
    else if (strcmp(cmd, "rooms") == 0)      cmd_rooms(app);
    else if (strcmp(cmd, "who") == 0)        cmd_who(app, rest);
    else if (strcmp(cmd, "history") == 0)    cmd_history(app);
    else if (strcmp(cmd, "deleteroom") == 0) cmd_deleteroom(app, rest);
    else if (strcmp(cmd, "users") == 0)      cmd_users(app);
    else if (strcmp(cmd, "clear") == 0)      app->line_count = 0;
    else if (strcmp(cmd, "typing") == 0)     net_send_line(app->sockfd, "TYPING|general");
    else if (strcmp(cmd, "stats") == 0)      net_send_line(app->sockfd, "STATS");
    else if (strcmp(cmd, "announce") == 0)   cmd_announce(app, rest);
    else if (strcmp(cmd, "kick") == 0)       cmd_kick(app, rest);
    else if (strcmp(cmd, "createuser") == 0) cmd_createuser(app, rest);
    else if (strcmp(cmd, "deleteuser") == 0) cmd_deleteuser(app, rest);
    else if (strcmp(cmd, "resetpass") == 0)  cmd_resetpass(app, rest);
    else if (strcmp(cmd, "accounts") == 0)   net_send_line(app->sockfd, "LIST_ACCOUNTS");
    else if (strcmp(cmd, "sendfile") == 0)   cmd_sendfile(app, rest);
    else if (strcmp(cmd, "accept") == 0)     cmd_accept(app, rest);
    else if (strcmp(cmd, "reject") == 0)     cmd_reject(app, rest);
    else tui_add_line(app, LN_ERROR, "Unknown command '/%s'. Type /help", cmd[0]?cmd:"");
}
