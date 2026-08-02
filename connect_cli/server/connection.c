/* connection.c - the per-client receive thread.
 *
 * For every accepted socket, net_spawn_client() (in net.c) starts one
 * thread that runs handle_client(). That loop:
 *   1. reads raw bytes from the socket,
 *   2. cuts them into whole lines (ending in '\n'),
 *   3. parses each line into a Cmd (CMD|a1|a2|a3|a4),
 *   4. hands it to dispatch_command() in handlers.c,
 *   5. when the peer disconnects, releases everything that client owned.
 */
#include "server.h"

/* Split `line` on '|' into the Cmd fields, keeping empty fields intact.
 * `raw` is left pointing at the original, untouched line (some handlers,
 * e.g. FILE_DATA, need the full text because a field can be very large). */
static void parse_command(const char *line, Cmd *m) {
    memset(m, 0, sizeof(*m));
    m->raw = line;
    const char *p = line;
    while (m->parts < 5 && *p) {
        /* Find the next '|' (or the end of the string). The text from p
         * up to there is the next field. */
        const char *pipe = strchr(p, '|');
        size_t len = pipe ? (size_t)(pipe - p) : strlen(p);

        /* Pick the right destination buffer for this field index. */
        char *dst;
        size_t cap;
        switch (m->parts) {
            case 0: dst = m->cmd; cap = sizeof(m->cmd); break;
            case 1: dst = m->a1;  cap = sizeof(m->a1);  break;
            case 2: dst = m->a2;  cap = sizeof(m->a2);  break;
            case 3: dst = m->a3;  cap = sizeof(m->a3);  break;
            default: dst = m->a4; cap = sizeof(m->a4);  break;
        }
        if (len >= cap) len = cap - 1;   /* don't overflow the buffer */
        memcpy(dst, p, len);
        dst[len] = '\0';
        m->parts++;
        if (!pipe) break;   /* no more '|': we are done */
        p = pipe + 1;
    }
}

/* Called when a client's socket ends. We free every file/slot/transfer
 * owned by this client, remove it from the list, and tell the others.
 * We snapshot the name/room first because the Client is about to be freed. */
static void disconnect_cleanup(Client *c, const char *uname, const char *last_room) {
    files_queue_remove_client(c);   /* drop its waiting uploads */
    files_remove_slots(uname);      /* release its upload slots */
    files_remove_transfers(uname);  /* drop its offers */
    net_client_remove(c);           /* unlink + free the Client */

    /* If it had logged in, refresh the user list and tell its last room. */
    if (uname[0]) {
        net_broadcast_user_list();
        if (last_room[0] && strcmp(last_room, "general") != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "NOTIFY|%s disconnected.\n", uname);
            net_broadcast_room(last_room, msg, NULL);
        }
        log_message("INFO", "User '%s' disconnected", uname);
    }
}

/* The main loop for one client. Runs on its own thread. */
void *handle_client(void *arg) {
    Client *c = (Client *)arg;
    char buf[BUFFER_SIZE];     /* bytes just read from the socket */
    char line[BUFFER_SIZE];    /* the current line being assembled */
    size_t line_len = 0;       /* how much of `line` is filled */

    while (c->active && server_running) {
        ssize_t n = recv(c->sockfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;     /* peer closed or error */
        buf[n] = '\0';

        /* Walk the received bytes, building one line at a time. */
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';   /* done with this line */
                line_len = 0;
                Cmd m;
                parse_command(line, &m);
                dispatch_command(c, &m);
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = buf[i];   /* keep building the line */
            }
        }
    }

    /* Snapshots before the Client is freed, then clean up. */
    char uname[MAX_USERNAME] = {0};
    char last_room[MAX_ROOM_NAME] = {0};
    strncpy(uname, c->username, MAX_USERNAME - 1);
    strncpy(last_room, c->current_room, MAX_ROOM_NAME - 1);

    disconnect_cleanup(c, uname, last_room);
    return NULL;
}
