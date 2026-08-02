/* connection.c - the per-client receive thread.
 *
 * Each accepted socket runs here: it reads newline-delimited protocol
 * lines, splits each one into a Cmd (CMD|a1|a2|a3|a4), hands it to
 * dispatch_command() (handlers.c), and cleans up on disconnect.
 */
#include "server.h"

/* Split `line` on '|' into the Cmd fields, preserving empty fields.
 * `raw` is left pointing at the original, untouched line. */
static void parse_command(const char *line, Cmd *m) {
    memset(m, 0, sizeof(*m));
    m->raw = line;
    const char *p = line;
    while (m->parts < 5 && *p) {
        const char *pipe = strchr(p, '|');
        size_t len = pipe ? (size_t)(pipe - p) : strlen(p);

        char *dst;
        size_t cap;
        switch (m->parts) {
            case 0: dst = m->cmd; cap = sizeof(m->cmd); break;
            case 1: dst = m->a1;  cap = sizeof(m->a1);  break;
            case 2: dst = m->a2;  cap = sizeof(m->a2);  break;
            case 3: dst = m->a3;  cap = sizeof(m->a3);  break;
            default: dst = m->a4; cap = sizeof(m->a4);  break;
        }
        if (len >= cap) len = cap - 1;
        memcpy(dst, p, len);
        dst[len] = '\0';
        m->parts++;
        if (!pipe) break;
        p = pipe + 1;
    }
}

void *handle_client(void *arg) {
    Client *c = (Client *)arg;
    char buf[BUFFER_SIZE];
    char line[BUFFER_SIZE];
    size_t line_len = 0;

    while (c->active && server_running) {
        ssize_t n = recv(c->sockfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[line_len] = '\0';
                line_len = 0;

                Cmd m;
                parse_command(line, &m);
                dispatch_command(c, &m);
            } else if (line_len < sizeof(line) - 1) {
                line[line_len++] = buf[i];
            }
        }
    }

    /* Client disconnected: release everything it owned. */
    char uname[MAX_USERNAME] = {0};
    char last_room[MAX_ROOM_NAME] = {0};
    strncpy(uname, c->username, MAX_USERNAME - 1);
    strncpy(last_room, c->current_room, MAX_ROOM_NAME - 1);

    files_queue_remove_client(c);
    files_remove_slots(uname);
    files_process_queue();
    files_remove_transfers(uname);
    net_client_remove(c);

    if (uname[0]) {
        net_broadcast_user_list();
        if (last_room[0] && strcmp(last_room, "general") != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "NOTIFY|%s disconnected.\n", uname);
            net_broadcast_room(last_room, msg, NULL);
        }
        log_message("INFO", "User '%s' disconnected", uname);
    }
    return NULL;
}
