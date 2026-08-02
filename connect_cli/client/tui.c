/* tui.c - the Text User Interface: raw terminal mode + all screen drawing.
 *
 * We drive the terminal with ANSI escape codes:
 *   \033[H       move cursor home
 *   \033[2J       clear the screen
 *   \033[<r>;<c>H move the cursor to row r, column c
 *   \033[7m       reverse video,  \033[1m bold,  \033[2m dim,  \033[0m reset
 * tui_enter_raw()/tui_restore() turn cooked mode on/off so we can read
 * single keystrokes (arrows, Enter, Backspace, Ctrl-C).
 * tui_draw() redraws the whole screen: title bar, chat area with a sidebar
 * of users/rooms, a status bar, and an input line at the bottom.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "client.h"

/* Colours and styles as short ANSI escape strings. */
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"
#define WHITE   "\033[37m"

/* The terminal settings we saved before switching to raw mode. */
static struct termios orig_termios;
static bool raw_on = false;

/* Switch to raw mode: turn off line buffering and echo so we see each
 * keystroke immediately instead of waiting for Enter. */
void tui_enter_raw(void) {
    if (raw_on) return;
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_on = true;
}

/* Put the terminal back the way we found it. */
void tui_restore(void) {
    if (!raw_on) return;
    printf("\033[?25h\033[0m\033[H\033[2J");  /* show cursor, reset, clear */
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_on = false;
}

/* Ask the terminal how big it is; fall back to safe defaults if unsure. */
void tui_get_size(int *rows, int *cols) {
    struct winsize ws;
    int r = 0, c = 0;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        r = ws.ws_row; c = ws.ws_col;
    }
    if (r < 10) r = 24;
    if (c < 20) c = 80;
    *rows = r; *cols = c;
}

/* Pick a colour for each kind of chat line. */
static const char *color_for(LineKind k) {
    switch (k) {
        case LN_SELF:     return GREEN;
        case LN_NORMAL:   return WHITE;
        case LN_PM:       return MAGENTA;
        case LN_ANNOUNCE: return YELLOW BOLD;
        case LN_NOTIFY:   return CYAN;
        case LN_ERROR:    return RED BOLD;
        case LN_FILE:     return BLUE;
        case LN_STATUS:   return CYAN;
        case LN_TYPING:   return DIM;
        default:          return RESET;
    }
}

/* Add one formatted line to the on-screen chat history. */
void tui_add_line(App *app, LineKind kind, const char *fmt, ...) {
    if (!app) return;
    /* If the buffer is full, drop its oldest line to make room. */
    if (app->line_count >= MAX_CHAT_LINES) {
        memmove(&app->lines[0], &app->lines[1],
                sizeof(ChatLine) * (MAX_CHAT_LINES - 1));
        app->line_count--;
    }
    ChatLine *cl = &app->lines[app->line_count++];
    cl->kind = kind;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cl->text, sizeof(cl->text), fmt, ap);
    va_end(ap);
}

/* Add a notification-style line (formatted like tui_add_line). */
void tui_add_notify(App *app, const char *fmt, ...) {
    static char buf[MAX_MESSAGE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tui_add_line(app, LN_NOTIFY, buf);
}

/* Set the input-box text (used by the arrow-key command history). */
void tui_set_input(App *app, const char *s) {
    if (!s) return;
    strncpy(app->input, s, sizeof(app->input) - 1);
    app->input_len = (int)strlen(app->input);
}

/* Print `s` padded/truncated to exactly `width` columns from the cursor. */
static void print_cell(const char *s, int width) {
    int i = 0;
    while (s[i] && i < width) { putchar(s[i]); i++; }
    while (i < width) { putchar(' '); i++; }
}

/* ---- drawing helpers used by tui_draw ---- */

/* Draw the title bar at the top of the screen. */
static void draw_title(int cols, App *app) {
    char title[256];
    if (app->logged_in)
        snprintf(title, sizeof(title), " ConnectHub CLI  |  user: %s%s  |  room: #%s ",
                 app->username, app->is_admin ? " (admin)" : "", app->current_room);
    else
        snprintf(title, sizeof(title), " ConnectHub CLI  |  not connected ");
    printf("\033[1;1H\033[7m");      /* home position, then reverse video */
    print_cell(title, cols);
    printf(RESET);
}

/* Draw a horizontal dashed separator line across the whole screen. */
static void draw_separator(int row, int cols) {
    printf("\033[%d;1H\033[2m", row);   /* move to the row, dim it */
    for (int i = 0; i < cols; i++) putchar('-');
    printf(RESET);
}

/* A centred login screen: box frame, title, prompt and an input box. */
static void draw_login(App *app, int rows, int cols) {
    int box_w = 46;
    if (box_w > cols - 4) box_w = cols - 4;
    int box_h = 7;
    int top = (rows - box_h) / 2;
    int left = (cols - box_w) / 2;

    printf("\033[?25l\033[2J\033[H");
    printf("\033[7m"); print_cell(" ConnectHub CLI  |  login ", cols); printf(RESET);

    /* Top edge of the box. */
    printf("\033[%d;%dH\033[1m+", top, left);
    for (int i = 0; i < box_w - 2; i++) putchar('-');
    printf("+\033[0m");
    /* Left and right edges. */
    for (int r = top + 1; r < top + box_h - 1; r++) {
        printf("\033[%d;%dH\033[1m|\033[0m", r, left);
        printf("\033[%d;%dH\033[1m|\033[0m", r, left + box_w - 1);
    }
    /* Bottom edge of the box. */
    printf("\033[%d;%dH\033[1m+", top + box_h - 1, left);
    for (int i = 0; i < box_w - 2; i++) putchar('-');
    printf("+\033[0m");

    /* App title inside the box. */
    int cx = left + box_w / 2;
    printf("\033[%d;%dH\033[1;36mConnectHub\033[0m", top + 1, cx - 5);
    printf("\033[%d;%dH\033[2mLAN chat over sockets\033[0m", top + 2, cx - 9);

    /* The prompt changes between the username and password steps. */
    const char *prompt = (app->login_step == 2)
        ? "Enter password:"
        : "Enter username:";
    int pl = (int)strlen(prompt);
    printf("\033[%d;%dH\033[33m%s\033[0m", top + 4, cx - pl / 2, prompt);

    /* The input box (masked with * while entering a password). */
    int in_w = box_w - 6;
    if (in_w < 10) in_w = 10;
    int il = cx - in_w / 2;
    printf("\033[%d;%dH\033[7m", top + 5, il);
    if (app->mask_input) {
        for (int i = 0; i < in_w; i++) putchar(i < app->input_len ? '*' : ' ');
    } else {
        print_cell(app->input, in_w);
    }
    printf("\033[0m");

    /* Put the cursor just after whatever the user has typed. */
    int cur = il + app->input_len;
    if (cur > il + in_w - 1) cur = il + in_w - 1;
    printf("\033[%d;%dH\033[?25h", top + 5, cur + 1);
    fflush(stdout);
}

/* Build the sidebar text (users + rooms) into `side`, one string per row.
 * Returns how many rows were filled. */
static int build_sidebar(App *app, char side[][48], char *side_map[], int chat_height) {
    int side_n = 0;
    snprintf(side[side_n], sizeof(side[0]), "%s Users (%d) %s", "\033[7m", app->user_count, "\033[0m");
    side_map[side_n] = side[side_n]; side_n++;
    /* One line for each online user. */
    for (int i = 0; i < app->user_count && side_n < chat_height; i++) {
        snprintf(side[side_n], sizeof(side[0]), "  %s%s", app->users[i], RESET);
        side_map[side_n] = side[side_n]; side_n++;
    }
    /* A blank line, then the room list (current room marked with *). */
    if (side_n < chat_height) { snprintf(side[side_n], sizeof(side[0]), " "); side_map[side_n]=side[side_n]; side_n++; }
    snprintf(side[side_n], sizeof(side[0]), "%s Rooms (%d) %s", "\033[7m", app->room_count, "\033[0m");
    side_map[side_n] = side[side_n]; side_n++;
    for (int i = 0; i < app->room_count && side_n < chat_height; i++) {
        char mark[4] = "";
        if (strcmp(app->rooms[i], app->current_room) == 0) strcpy(mark, "*");
        snprintf(side[side_n], sizeof(side[0]), "  %s%s%s", mark, app->rooms[i], RESET);
        side_map[side_n] = side[side_n]; side_n++;
    }
    return side_n;
}

/* Draw the chat rows in the middle of the screen, plus the sidebar divider. */
static void draw_chat_rows(App *app, int chat_top, int chat_height, int chat_w,
                           int sidebar_w, char *side_map[], int side_n) {
    for (int k = 0; k < chat_height; k++) {
        int row = chat_top + k;
        int src_idx = app->line_count - chat_height + k;  /* show the newest lines */
        printf("\033[%d;1H", row);
        if (src_idx >= 0) {
            ChatLine *cl = &app->lines[src_idx];
            printf("%s", color_for(cl->kind));
            print_cell(cl->text, chat_w);
            printf(RESET);
        } else {
            print_cell("", chat_w);
        }
        /* Print the vertical divider and the matching sidebar cell. */
        if (sidebar_w > 0) {
            printf("\033[%d;%dH", row, chat_w + 1);
            putchar('|');
            printf("\033[%d;%dH", row, chat_w + 2);
            if (k < side_n) printf("%s", side_map[k]);
        }
    }
}

/* Draw the status bar just above the input line. */
static void draw_status_bar(App *app, int rows, int cols) {
    printf("\033[%d;1H\033[2m", rows - 1);
    char status[256];
    if (app->connected) {
        snprintf(status, sizeof(status), " Connected  |  room: #%s  |  online: %d  |  typing: %s ",
                 app->current_room, app->user_count,
                 app->typing[0] ? app->typing : "-");
    } else {
        snprintf(status, sizeof(status), " Disconnected ");
    }
    print_cell(status, cols);
    printf(RESET);
}

/* Draw the "> prompt" line and leave the cursor after the typed text. */
static void draw_input_line(App *app, int rows, int cols) {
    printf("\033[%d;1H> ", rows);
    int inw = cols - 2;
    if (inw < 0) inw = 0;
    if (app->mask_input) {
        /* Hide the password behind asterisks. */
        for (int i = 0; i < inw; i++) putchar(i < app->input_len ? '*' : ' ');
    } else {
        print_cell(app->input, inw);
    }
    int cx = 2 + app->input_len;   /* cursor goes right after the typed text */
    if (cx > cols) cx = cols;
    printf("\033[%d;%dH\033[?25h", rows, cx + 1);
    fflush(stdout);
}

/* Redraw the whole screen from the app state. */
void tui_draw(App *app) {
    int rows, cols;
    tui_get_size(&rows, &cols);
    if (!app->logged_in) {
        draw_login(app, rows, cols);
        return;
    }

    /* How wide is the right-hand sidebar (users + rooms)? */
    int sidebar_w = (cols > 76) ? 22 : (cols > 50 ? 14 : 0);
    int chat_w = cols - sidebar_w - (sidebar_w ? 1 : 0);

    printf("\033[?25l\033[2J\033[H");
    draw_title(cols, app);
    draw_separator(2, cols);

    int chat_top = 3;              /* first chat row */
    int chat_bot = rows - 3;       /* last chat row */
    if (chat_bot < chat_top) chat_bot = chat_top;
    int chat_height = chat_bot - chat_top + 1;

    /* Fill the sidebar only when the screen is wide enough to show one. */
    char side[512][48];
    char *side_map[512];
    int side_n = 0;
    if (sidebar_w > 0)
        side_n = build_sidebar(app, side, side_map, chat_height);

    draw_chat_rows(app, chat_top, chat_height, chat_w, sidebar_w, side_map, side_n);
    draw_separator(rows - 2, cols);
    draw_status_bar(app, rows, cols);
    draw_input_line(app, rows, cols);
}
