/* server.c - entry point of the ConnectHub chat server.
 *
 * This file stays deliberately short: it parses arguments, opens the
 * listening socket, and runs the main accept loop. Everything else lives
 * in the other server/ modules (see server.h).
 *
 * Concurrency model:
 *   - main() uses a single select() on the *listening* socket (1s timeout
 *     so the upload-queue housekeeping can run every second);
 *   - each accepted connection is handled by its own detached thread
 *     (connection.c), which uses blocking recv()/send().
 */
#include "server.h"

/* Shared runtime state (see server.h for the externs). */
volatile sig_atomic_t server_running = 1;
int server_sock = -1;
int total_messages = 0, total_privmsgs = 0, total_files = 0;
char admin_user[64] = "admin";
char admin_pass_hash[65] = "";

static char admin_pass[128] = "";

static void server_shutdown(void) {
    server_running = 0;
    if (server_sock >= 0) close(server_sock);
    net_close_all_clients();
    logger_close();
    room_destroy();
    room_access_clear();
}

static void signal_handler(int sig) {
    (void)sig;
    server_running = 0;
    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }
}

int main(int argc, char **argv) {
    int port = PORT;
    if (argc > 1) port = atoi(argv[1]);
    if (port <= 0 || port > 65535) port = PORT;

    /* Load admin credentials. */
    FILE *af = fopen("config/admin.cred", "r");
    if (af) {
        char line[256];
        if (fgets(line, sizeof(line), af)) {
            char *nl = strchr(line, '\n'); if (nl) *nl = 0;
            char *colon = strchr(line, ':');
            if (colon) {
                *colon = 0;
                strncpy(admin_user, line, sizeof(admin_user) - 1);
                strncpy(admin_pass, colon + 1, sizeof(admin_pass) - 1);
            }
        }
        fclose(af);
        sha256_hex(admin_pass, admin_pass_hash);
    }

    logger_init("logs");
    room_init();
    pthread_mutex_lock(&user_mutex);
    users_load();
    pthread_mutex_unlock(&user_mutex);
    room_access_load();
    history_init();
    srand((unsigned)time(NULL));

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }
    if (listen(server_sock, MAX_CLIENTS) < 0) { perror("listen"); return 1; }

    printf("[SERVER] Listening on port %d\n", port);
    log_message("INFO", "Server started on port %d", port);

    while (server_running) {
        /* select() with a 1-second timeout: lets us notice new connections
         * AND run the periodic upload-slot/queue housekeeping for free. */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_sock, &rfds);
        struct timeval tv = {1, 0};
        int sel = select(server_sock + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) {              /* nothing new: housekeeping tick */
            files_expire_stale();
            continue;
        }

        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(server_sock, (struct sockaddr*)&caddr, &clen);
        if (cfd < 0) {
            if (!server_running) break;
            continue;
        }

        /* Hand the socket to a fresh thread; connection.c takes it from here. */
        net_spawn_client(cfd, caddr);
    }

    printf("[SERVER] Shutting down...\n");
    server_shutdown();
    return 0;
}
