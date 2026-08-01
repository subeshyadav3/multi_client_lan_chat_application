#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "client.h"

/* ANSI helpers */
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

static struct termios orig_termios;
static bool raw_on = false;

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

void tui_restore(void) {
    if (!raw_on) return;
    /* Show cursor, reset color, clear screen, restore termios. */
    printf("\033[?25h\033[0m\033[H\033[2J");
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_on = false;
}

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

void tui_add_line(App *app, LineKind kind, const char *fmt, ...) {
    if (!app) return;
    if (app->line_count >= MAX_CHAT_LINES) {
        /* Drop the oldest line (shift). */
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

void tui_add_notify(App *app, const char *fmt, ...) {
    static char buf[MAX_MESSAGE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tui_add_line(app, LN_NOTIFY, buf);
}

void tui_set_input(App *app, const char *s) {
    if (!s) return;
    strncpy(app->input, s, sizeof(app->input) - 1);
    app->input_len = (int)strlen(app->input);
}

/* Print text truncated/padded to `width` starting at current cursor. */
static void print_cell(const char *s, int width) {
    int i = 0;
    while (s[i] && i < width) { putchar(s[i]); i++; }
    while (i < width) { putchar(' '); i++; }
}
static void draw_title(int cols, App *app) {
    char title[256];
    if (app->logged_in)
        snprintf(title, sizeof(title), " ConnectHub CLI  |  user: %s%s  |  room: #%s ",
                 app->username, app->is_admin ? " (admin)" : "", app->current_room);
    else
        snprintf(title, sizeof(title), " ConnectHub CLI  |  not connected ");
    printf("\033[1;1H\033[7m");
    print_cell(title, cols);
    printf(RESET);
}

static void draw_separator(int row, int cols) {
    printf("\033[%d;1H\033[2m", row);
    for (int i = 0; i < cols; i++) putchar('-');
    printf(RESET);
}

/* Centered login screen: no sidebar, prompt + input box in the middle. */
static void draw_login(App *app, int rows, int cols) {
    int box_w = 46;
    if (box_w > cols - 4) box_w = cols - 4;
    int box_h = 7;
    int top = (rows - box_h) / 2;
    int left = (cols - box_w) / 2;

    printf("\033[?25l\033[2J\033[H");
    printf("\033[7m"); print_cell(" ConnectHub CLI  |  login ", cols); printf(RESET);

    /* Box frame. */
    printf("\033[%d;%dH\033[1m+", top, left);
    for (int i = 0; i < box_w - 2; i++) putchar('-');
    printf("+\033[0m");
    for (int r = top + 1; r < top + box_h - 1; r++) {
        printf("\033[%d;%dH\033[1m|\033[0m", r, left);
        printf("\033[%d;%dH\033[1m|\033[0m", r, left + box_w - 1);
    }
    printf("\033[%d;%dH\033[1m+", top + box_h - 1, left);
    for (int i = 0; i < box_w - 2; i++) putchar('-');
    printf("+\033[0m");

    /* App title. */
    int cx = left + box_w / 2;
    printf("\033[%d;%dH\033[1;36mConnectHub\033[0m", top + 1, cx - 5);
    printf("\033[%d;%dH\033[2mLAN chat over sockets\033[0m", top + 2, cx - 9);

    /* Prompt. */
    const char *prompt = (app->login_step == 2)
        ? "Enter password:"
        : "Enter username:";
    int pl = (int)strlen(prompt);
    printf("\033[%d;%dH\033[33m%s\033[0m", top + 4, cx - pl / 2, prompt);

    /* Input box. */
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

    /* Cursor. */
    int cur = il + app->input_len;
    if (cur > il + in_w - 1) cur = il + in_w - 1;
    printf("\033[%d;%dH\033[?25h", top + 5, cur + 1);
    fflush(stdout);
}

void tui_draw(App *app) {
    int rows, cols;
    tui_get_size(&rows, &cols);
    if (!app->logged_in) {
        draw_login(app, rows, cols);
        return;
    }

    int sidebar_w = (cols > 76) ? 22 : (cols > 50 ? 14 : 0);
    int chat_w = cols - sidebar_w - (sidebar_w ? 1 : 0);

    printf("\033[?25l\033[2J\033[H");
    draw_title(cols, app);
    draw_separator(2, cols);

    int chat_top = 3;              /* 1-based first chat row */
    int chat_bot = rows - 3;       /* 1-based last chat row (inclusive) */
    if (chat_bot < chat_top) chat_bot = chat_top;
    int chat_height = chat_bot - chat_top + 1;

    /* Build sidebar content as an array of up to chat_height rows. */
    char side[512][48];
    char *side_map[512];
    int side_n = 0;
    if (sidebar_w > 0) {
        side_n = 0;
        snprintf(side[side_n], sizeof(side[0]), "%s Users (%d) %s", "\033[7m", app->user_count, "\033[0m");
        side_map[side_n] = side[side_n]; side_n++;
        for (int i = 0; i < app->user_count && side_n < chat_height; i++) {
            snprintf(side[side_n], sizeof(side[0]), "  %s%s", app->users[i], RESET);
            side_map[side_n] = side[side_n]; side_n++;
        }
        if (side_n < chat_height) { snprintf(side[side_n], sizeof(side[0]), " "); side_map[side_n]=side[side_n]; side_n++; }
        snprintf(side[side_n], sizeof(side[0]), "%s Rooms (%d) %s", "\033[7m", app->room_count, "\033[0m");
        side_map[side_n] = side[side_n]; side_n++;
        for (int i = 0; i < app->room_count && side_n < chat_height; i++) {
            char mark[4] = "";
            if (strcmp(app->rooms[i], app->current_room) == 0) strcpy(mark, "*");
            snprintf(side[side_n], sizeof(side[0]), "  %s%s%s", mark, app->rooms[i], RESET);
            side_map[side_n] = side[side_n]; side_n++;
        }
    }

    /* Chat rows. */
    for (int k = 0; k < chat_height; k++) {
        int row = chat_top + k;
        int src_idx = app->line_count - chat_height + k;
        printf("\033[%d;1H", row);
        if (src_idx >= 0) {
            ChatLine *cl = &app->lines[src_idx];
            printf("%s", color_for(cl->kind));
            print_cell(cl->text, chat_w);
            printf(RESET);
        } else {
            print_cell("", chat_w);
        }
        if (sidebar_w > 0) {
            printf("\033[%d;%dH", row, chat_w + 1);
            putchar('|');
            printf("\033[%d;%dH", row, chat_w + 2);
            if (k < side_n) {
                /* Sidebar cells may contain their own ANSI colors; pad to width
                   based on approximate visible length is handled loosely. */
                printf("%s", side_map[k]);
            }
        }
    }

    draw_separator(rows - 2, cols);

    /* Status bar. */
    printf("\033[%d;1H\033[2m", rows - 1);
    char status[256];
    if (app->connected) {
        snprintf(status, sizeof(status), " Connected  |  room: #%s  |  online: %d  |  typing: %s", 
                 app->current_room, app->user_count,
                 app->typing[0] ? app->typing : "-");
    } else {
        snprintf(status, sizeof(status), " Disconnected ");
    }
    print_cell(status, cols);
    printf(RESET);

    /* Input line. */
    printf("\033[%d;1H> ", rows);
    int inw = cols - 2;
    if (inw < 0) inw = 0;
    if (app->mask_input) {
        for (int i = 0; i < inw; i++) putchar(i < app->input_len ? '*' : ' ');
    } else {
        print_cell(app->input, inw);
    }
    /* Cursor placed just after the typed text. */
    int cx = 2 + app->input_len;
    if (cx > cols) cx = cols;
    printf("\033[%d;%dH\033[?25h", rows, cx + 1);
    fflush(stdout);
}

