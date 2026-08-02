/* client.c - entry point of the ConnectHub CLI client.
 *
 * Deliberately short, to make the architecture easy to explain:
 *   - main() opens the socket, then runs a single select() loop that
 *     multiplexes two file descriptors: the server socket (incoming
 *     lines) and stdin (keystrokes);
 *   - the 0.1s select() timeout also gives file uploads a chance to
 *     stream chunks and lets the "typing..." indicator expire;
 *   - all real logic lives in the other client/ modules (see client.h).
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

static volatile sig_atomic_t g_quit = 0;

static void on_signal(int s) { (void)s; g_quit = 1; }

void client_quit(App *app) {
    tui_restore();
    net_close(app->sockfd);
    exit(0);
}

/* Read one complete server line and hand it to protocol.c.
 * Returns false when the session loop must exit. */
static bool read_from_server(App *app, int fd, char *rbuf, char *rline, size_t *rpos) {
    ssize_t n = recv(fd, rbuf, BUFFER_SIZE - 1, 0);
    if (n <= 0) {
        if (app->logout_pending) return false;
        tui_add_notify(app, "Disconnected from server.");
        app->connected = false;
        return false;
    }
    rbuf[n] = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (rbuf[i] == '\n') {
            rline[*rpos] = 0;
            *rpos = 0;
            handle_line(app, rline);
        } else if (*rpos < BUFFER_SIZE - 1) {
            rline[(*rpos)++] = rbuf[i];
        }
    }
    return true;
}

/* Read one keystroke from the terminal. */
static void read_from_stdin(App *app) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return;
    if (c == 3) { g_quit = 1; return; }              /* Ctrl-C */
    else if (c == 10 || c == 13) process_input(app);
    else if (c == 127 || c == 8) {                   /* backspace */
        if (app->input_len > 0) app->input[--app->input_len] = 0;
    } else if (c == 27) {                            /* escape / arrows */
        char seq[3]; seq[0] = c;
        if (read(STDIN_FILENO, &seq[1], 1) == 1 && seq[1] == '[') {
            if (read(STDIN_FILENO, &seq[2], 1) == 1) {
                if (seq[2] == 'A') {                 /* up */
                    if (app->history_index > 0) {
                        app->history_index--;
                        tui_set_input(app, app->history[app->history_index]);
                    }
                } else if (seq[2] == 'B') {          /* down */
                    if (app->history_index < app->history_count) {
                        app->history_index++;
                        if (app->history_index < app->history_count)
                            tui_set_input(app, app->history[app->history_index]);
                        else { strcpy(app->input, ""); app->input_len = 0; }
                    }
                }
            }
        }
    } else if (c >= 32 && c < 127) {
        if (app->input_len < (int)sizeof(app->input) - 1)
            app->input[app->input_len++] = c;
        app->input[app->input_len] = 0;
        app->last_typed = time(NULL);
        send_typing(app);
    }
}

/* The connect + read loop. Returns when the user quits or logs out. */
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

    /* Reset to the login screen after a logout/reconnect. */
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
    tui_draw(app);

    /* Optional auto-login: chatclient --user <u> --pass <p> (or positional).
     * Only on the very first connection; a later /logout must re-prompt. */
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

    char rbuf[BUFFER_SIZE];
    char rline[BUFFER_SIZE];
    size_t rpos = 0;

    while (app->connected && !g_quit) {
        /* select(): wait for input on the socket, stdin, or a 0.1s timer. */
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
            if (!read_from_server(app, fd, rbuf, rline, &rpos)) break;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            read_from_stdin(app);
            if (g_quit) break;
        }

        /* Auto-clear the typing indicator after 2.5s of silence. */
        if (app->typing[0] && (time(NULL) - app->typing_at) > 2) {
            app->typing[0] = 0;
        }
        files_try_send_chunk(app);
        tui_draw(app);
    }

    net_close(fd);
    app->connected = false;
}

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

    bool first_connect = true;
    while (!g_quit) {
        run_session(&app, host, port, user, pass, &first_connect);
        if (!app.logout_pending || g_quit) break;
    }

    tui_restore();
    return 0;
}
