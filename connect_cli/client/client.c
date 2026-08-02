/* client.c - the entry point of the ConnectHub CLI client.
 *
 * This file is deliberately short. It does three simple things:
 *   1. main() reads the command-line flags (host, port, optional login).
 *   2. run_session() opens the socket and runs a single select() loop.
 *   3. The select() loop multiplexes two file descriptors:
 *        - the server socket  -> incoming lines are read and handled
 *        - the terminal stdin -> keystrokes drive the input box
 *   A 0.1 s timer in select() also lets file uploads stream chunks and lets
 *   the "typing..." indicator expire, while we keep redrawing the screen.
 *   All of the real work lives in the other modules (see client.h).
 */
#include "client.h"
#include "net.h"
#include "../shared/constants.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>

/* Set to 1 when the user presses Ctrl-C, so the loops know to stop. */
static volatile sig_atomic_t g_quit = 0;

/* When we get SIGINT/SIGTERM we just set g_quit; the loop sees it. */
static void on_signal(int s) { (void)s; g_quit = 1; }

/* Quit cleanly: restore the terminal, close the socket, and exit. */
void client_quit(App *app) {
    tui_restore();
    net_close(app->sockfd);
    exit(0);
}

/* ---- small helpers used by the main loop ---- */

/* Read whatever arrived from the server and split it into complete lines.
 * Returns false when we should stop looping (socket closed or logged out). */
static bool read_from_server(App *app, int fd, char *rbuf, char *rline, size_t *rpos) {
    ssize_t n = recv(fd, rbuf, BUFFER_SIZE - 1, 0);
    if (n <= 0) {
        /* The socket closed. If we asked to log out, that is expected. */
        if (app->logout_pending) return false;
        tui_add_notify(app, "Disconnected from server.");
        app->connected = false;
        return false;
    }
    rbuf[n] = 0;
    /* Build complete lines out of the raw bytes, one byte at a time. */
    for (ssize_t i = 0; i < n; i++) {
        if (rbuf[i] == '\n') {
            rline[*rpos] = 0;   /* terminate the current line */
            *rpos = 0;          /* and start a fresh one */
            handle_line(app, rline);
        } else if (*rpos < BUFFER_SIZE - 1) {
            rline[(*rpos)++] = rbuf[i];   /* accumulate the next byte */
        }
    }
    return true;
}

/* Walk the typed-command history with the up/down arrow keys.
 * direction is 'A' (up) or 'B' (down). */
static void move_through_history(App *app, int direction) {
    if (direction == 'A') {                 /* up arrow: older command */
        if (app->history_index > 0) {
            app->history_index--;
            tui_set_input(app, app->history[app->history_index]);
        }
    } else {                                /* down arrow: newer command */
        if (app->history_index < app->history_count) {
            app->history_index++;
            if (app->history_index < app->history_count)
                tui_set_input(app, app->history[app->history_index]);
            else { strcpy(app->input, ""); app->input_len = 0; }
        }
    }
}

/* Handle an escape sequence; we only care about the arrow keys. */
static void handle_escape(App *app) {
    char seq[3]; seq[0] = 27;              /* the escape byte we already read */
    if (read(STDIN_FILENO, &seq[1], 1) == 1 && seq[1] == '[') {
        if (read(STDIN_FILENO, &seq[2], 1) == 1) {
            if (seq[2] == 'A' || seq[2] == 'B')
                move_through_history(app, seq[2]);
        }
    }
}

/* Add a plain printable character to the input box. */
static void handle_printable_char(App *app, char c) {
    if (app->input_len < (int)sizeof(app->input) - 1)
        app->input[app->input_len++] = c;
    app->input[app->input_len] = 0;
    app->last_typed = time(NULL);
    send_typing(app);   /* let the room know we are typing */
}

/* Read one keystroke from the terminal and act on it. */
static void read_from_stdin(App *app) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return;   /* nothing to read */

    if (c == 3) { g_quit = 1; return; }            /* Ctrl-C stops the app */
    else if (c == 10 || c == 13) process_input(app);        /* Enter submits */
    else if (c == 127 || c == 8) {                 /* Backspace deletes */
        if (app->input_len > 0) app->input[--app->input_len] = 0;
    } else if (c == 27) {                          /* Escape starts a sequence */
        handle_escape(app);
    } else if (c >= 32 && c < 127) {               /* Ordinary character */
        handle_printable_char(app, c);
    }
}

/* ---- the session loop ---- */

/* Clear the app state so we start at a fresh login screen.
 * This runs whenever we (re)connect, including after a /logout. */
static void reset_app_state(App *app, const char *host, int port) {
    app->logged_in = false;
    app->is_admin = false;
    app->username[0] = 0;
    app->current_room[0] = 0;
    strncpy(app->current_room, "general", MAX_ROOM_NAME - 1);
    app->login_step = 1;
    app->mask_input = 0;
    app->line_count = 0;
    app->user_count = 0;
    app->room_count = 0;
    app->typing[0] = 0;
    app->offer_count = 0;
    app->receiving = false;
    strcpy(app->input, ""); app->input_len = 0;
    tui_add_notify(app, "Connected to %s:%d. Enter your username:", host, port);
}

/* If --user/--pass were given on the very first run, log in automatically. */
static void auto_login_if_requested(App *app, int fd, const char *user,
                                    const char *pass, bool *first_connect) {
    if (user && pass && *first_connect) {
        strncpy(app->username, user, MAX_USERNAME - 1);
        app->username[MAX_USERNAME - 1] = 0;
        char out[160];
        snprintf(out, sizeof(out), "LOGIN|%s|%s", user, pass);
        net_send_line(fd, out);
        app->login_step = 0;
        tui_add_notify(app, "Logging in as %s...", user);
    }
    *first_connect = false;
}

/* Open the socket, reset state, and run the select() read loop. */
static void run_session(App *app, const char *host, int port,
                        const char *user, const char *pass, bool *first_connect) {
    int fd = net_connect(host, port);
    if (fd < 0) {
        tui_restore();
        printf("Failed to connect to %s:%d\n", host, port);
        exit(1);
    }
    app->sockfd = fd;
    app->connected = true;
    app->logout_pending = false;

    reset_app_state(app, host, port);
    tui_draw(app);

    auto_login_if_requested(app, fd, user, pass, first_connect);

    char rbuf[BUFFER_SIZE];
    char rline[BUFFER_SIZE];
    size_t rpos = 0;

    while (app->connected && !g_quit) {
        /* select(): wait for input on the socket, the keyboard, or a timer. */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {0, 100000};   /* wait at most 0.1 s */
        int maxfd = (fd > STDIN_FILENO ? fd : STDIN_FILENO) + 1;
        int r = select(maxfd, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;  /* interrupted by a signal: redo */
            break;
        }

        if (FD_ISSET(fd, &rfds)) {
            if (!read_from_server(app, fd, rbuf, rline, &rpos)) break;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            read_from_stdin(app);
            if (g_quit) break;
        }

        /* Auto-clear the "typing..." indicator after a while of silence. */
        if (app->typing[0] && (time(NULL) - app->typing_at) > 2) {
            app->typing[0] = 0;
        }
        files_try_send_chunk(app);   /* stream the next file chunk if any */
        tui_draw(app);
    }

    net_close(fd);
    app->connected = false;
}

/* ---- command-line parsing ---- */

/* Read the flags so we know where to connect and whether to auto-login. */
static void parse_args(int argc, char **argv, const char **host, int *port,
                       const char **user, const char **pass) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)      *host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) *port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) *user = argv[++i];
        else if (strcmp(argv[i], "--pass") == 0 && i + 1 < argc) *pass = argv[++i];
        else if (strcmp(argv[i], "--admin") == 0)                { *user = "admin"; }
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: chatclient [--host <host>] [--port <port>] [--user <u>] [--pass <p>] [--admin]\n"
                   "  legacy: chatclient <host> <port> <username> <password>\n");
            exit(0);
        }
        else if (i == 1) *host = argv[i];      /* legacy positional */
        else if (i == 2) *port = atoi(argv[i]);
        else if (i == 3) *user = argv[i];
        else if (i == 4) *pass = argv[i];
    }
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = PORT;
    const char *user = NULL, *pass = NULL;
    parse_args(argc, argv, &host, &port, &user, &pass);

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

    bool first_connect = true;
    while (!g_quit) {
        run_session(&app, host, port, user, pass, &first_connect);
        if (!app.logout_pending || g_quit) break;
    }

    tui_restore();
    return 0;
}
