#ifndef CLIENT_H
#define CLIENT_H

#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include "../shared/constants.h"

/* How much history / UI we remember. */
#define MAX_CHAT_LINES 600
#define MAX_SIDEBAR_W 26
#define MAX_CMDHIST 100
#define MAX_PENDING_OFFERS 16

/* The "kind" of a chat line controls its colour in the terminal. */
typedef enum {
    LN_NORMAL = 0, LN_NOTIFY, LN_PM, LN_ANNOUNCE,
    LN_ERROR, LN_SELF, LN_FILE, LN_STATUS, LN_TYPING
} LineKind;

/* One row of chat: its kind (hence colour) plus the text. */
typedef struct {
    LineKind kind;
    char text[MAX_MESSAGE];
} ChatLine;

/* An incoming file offer the user has not answered yet. */
typedef struct {
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    long size;
    char target[MAX_USERNAME];
} PendingOffer;

/* The whole client state in one struct, so it is easy to pass around. */
typedef struct {
    /* Connection and login. */
    int sockfd;
    bool connected;
    bool logged_in;
    bool logout_pending;
    bool is_admin;
    char username[MAX_USERNAME];
    char current_room[MAX_ROOM_NAME];

    /* Chat lines shown on screen. */
    ChatLine lines[MAX_CHAT_LINES];
    int line_count;

    /* Sidebar lists: who is online and which rooms exist. */
    char users[MAX_CLIENTS][MAX_USERNAME];
    int user_count;
    char rooms[MAX_ROOMS][MAX_ROOM_NAME];
    int room_count;

    /* "X is typing..." indicator. */
    char typing[MAX_USERNAME];
    time_t typing_at;
    time_t last_typed;

    /* Current input line. */
    char input[MAX_MESSAGE];
    int input_len;
    int mask_input;   /* 1 while entering a password -> show *** */
    int login_step;   /* 0 done, 1 ask username, 2 ask password */

    /* Up/down arrow command history. */
    char history[MAX_CMDHIST][MAX_MESSAGE];
    int history_count;
    int history_index;

    /* Pending incoming file offers. */
    PendingOffer offers[MAX_PENDING_OFFERS];
    int offer_count;

    /* Incoming file receive state. */
    char recv_filename[MAX_FILENAME];
    char recv_sender[MAX_USERNAME];
    long recv_total;
    long recv_done;
    FILE *recv_fp;
    bool receiving;

    /* Outgoing file send state. */
    char send_filename[MAX_FILENAME];
    char send_path[MAX_MESSAGE];
    long send_total;
    long send_done;
    char send_target[MAX_USERNAME];
    char send_token[32];
    FILE *send_fp;
    int send_state;   /* 0 none, 1 offered/awaiting grant, 2 granted/waiting accept, 3 streaming */
    long pending_send_size;
} App;

/* tui.c - terminal drawing. */
void tui_enter_raw(void);
void tui_restore(void);
void tui_get_size(int *rows, int *cols);
void tui_draw(App *app);
void tui_add_line(App *app, LineKind kind, const char *fmt, ...);
void tui_add_notify(App *app, const char *fmt, ...);
void tui_set_input(App *app, const char *s);

/* protocol.c - turns incoming server lines into screen events. */
void handle_line(App *app, const char *line);

/* commands.c - slash commands + the input processor. */
void cmd_process(App *app, const char *line);
void process_input(App *app);
void send_typing(App *app);

/* files.c - outbound/inbound file transfer. */
void files_request_send_file(App *app, const char *target, const char *path);
void files_add_offer(App *app, const char *sender, const char *filename, long size, const char *target);
void files_remove_offer(App *app, const char *sender, const char *filename);
void files_receive_chunk(App *app, const char *sender, const char *filename, const char *b64);
void files_finalize_received(App *app);
void files_try_send_chunk(App *app);

/* client.c. */
void client_quit(App *app);

#endif
