#ifndef CLIENT_H
#define CLIENT_H

#include <stdio.h>
#include <stdbool.h>
#include "../shared/constants.h"

#define MAX_CHAT_LINES 600
#define MAX_SIDEBAR_W 26
#define MAX_CMDHIST 100
#define MAX_PENDING_OFFERS 16

typedef enum {
    LN_NORMAL = 0, LN_NOTIFY, LN_PM, LN_ANNOUNCE,
    LN_ERROR, LN_SELF, LN_FILE, LN_STATUS, LN_TYPING
} LineKind;

typedef struct {
    LineKind kind;
    char text[MAX_MESSAGE];
} ChatLine;

typedef struct {
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    long size;
    char target[MAX_USERNAME];
} PendingOffer;

typedef struct {
    int sockfd;
    bool connected;
    bool logged_in;
    bool is_admin;
    char username[MAX_USERNAME];
    char current_room[MAX_ROOM_NAME];

    ChatLine lines[MAX_CHAT_LINES];
    int line_count;

    char users[MAX_CLIENTS][MAX_USERNAME];
    int user_count;
    char rooms[MAX_ROOMS][MAX_ROOM_NAME];
    int room_count;

    char typing[MAX_USERNAME];

    char input[MAX_MESSAGE];
    int input_len;
    int mask_input;
    int login_step;   /* 0 done, 1 ask username, 2 ask password */

    char history[MAX_CMDHIST][MAX_MESSAGE];
    int history_count;
    int history_index;

    PendingOffer offers[MAX_PENDING_OFFERS];
    int offer_count;

    char recv_filename[MAX_FILENAME];
    char recv_sender[MAX_USERNAME];
    long recv_total;
    long recv_done;
    FILE *recv_fp;
    bool receiving;

    char send_filename[MAX_FILENAME];
    char send_path[MAX_MESSAGE];
    long send_total;
    long send_done;
    char send_target[MAX_USERNAME];
    char send_token[32];
    FILE *send_fp;
    int send_state;      /* 0 none, 1 offered/awaiting grant, 2 granted/waiting recipient accept, 3 streaming */
    long pending_send_size;
} App;

/* tui.c */
void tui_enter_raw(void);
void tui_restore(void);
void tui_get_size(int *rows, int *cols);
void tui_draw(App *app);
void tui_add_line(App *app, LineKind kind, const char *fmt, ...);
void tui_add_notify(App *app, const char *fmt, ...);
void tui_set_input(App *app, const char *s);

/* client.c */
void cmd_process(App *app, const char *line);
void handle_line(App *app, const char *line);
void client_quit(App *app);

#endif
