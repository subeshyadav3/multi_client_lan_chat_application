/* server.c - the entry point of the ConnectHub chat server.
 *
 * This file stays deliberately short:
 *   1. read the port (and admin credentials) from the command line / files;
 *   2. start every subsystem (logger, rooms, users, history, access);
 *   3. open the listening socket;
 *   4. loop accepting connections until we are told to stop.
 *
 * CONCURRENCY MODEL:
 *   - main() uses one select() on the listening socket with a 1-second
 *     timeout. The timeout lets us also run the periodic file-upload
 *     housekeeping (frees stale slots / promotes the queue) every second.
 *   - each accepted connection is handed to its own detached thread
 *     (handle_client in connection.c), which then does blocking recv()/send().
 */
#include "server.h"

/* Shared runtime state (the externs in server.h point at these). */
volatile sig_atomic_t server_running = 1;  /* flipped to 0 on shutdown */
int server_sock = -1;                      /* the listening socket */
int total_messages = 0, total_privmsgs = 0, total_files = 0; /* counters */
char admin_user[64] = "admin";             /* default admin login */
char admin_pass_hash[65] = "";             /* admin password digest */

/* Plain-text admin password read from config/admin.cred; we hash it
 * into admin_pass_hash but keep it only long enough to do that. */
static char admin_pass[128] = "";

/* Stop everything: mark not-running, close sockets, and tear down the
 * subsystems we started. Called once at the end of main(). */
static void server_shutdown(void) {
    server_running = 0;
    if (server_sock >= 0) close(server_sock);
    net_close_all_clients();   /* stop and free every client thread's socket */
    logger_close();            /* close the log file */
    room_destroy();            /* free the room list */
    room_access_clear();       /* free the access registry */
}

/* SIGINT / SIGTERM handler: just ask the accept loop to stop. Closing
 * server_sock also makes a blocked select()/accept() wake up. */
static void signal_handler(int sig) {
    (void)sig;
    server_running = 0;
    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }
}

/* Read admin user:hashedPassword from config/admin.cred. The stored value
 * may be plain text; we hash it into admin_pass_hash for later compare. */
static void load_admin_credentials(void) {
    FILE *af = fopen("config/admin.cred", "r");
    if (!af) return;
    char line[256];
    if (fgets(line, sizeof(line), af)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;   /* drop newline */
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = 0;   /* split into name : password */
            strncpy(admin_user, line, sizeof(admin_user) - 1);
            strncpy(admin_pass, colon + 1, sizeof(admin_pass) - 1);
        }
    }
    fclose(af);
    sha256_hex(admin_pass, admin_pass_hash);   /* store the digest */
}

/* Open the listening socket, bind it to the port and start listening. */
static int open_listening_socket(int port) {
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;    /* accept from any interface */
    addr.sin_port = htons(port);

    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_sock);
        return -1;
    }
    if (listen(server_sock, MAX_CLIENTS) < 0) { perror("listen"); return -1; }
    return 0;
}

/* Accept every new connection until told to stop. select() with a 1-second
 * timeout lets us also run housekeeping when nothing is arriving. */
static void accept_loop(void) {
    while (server_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_sock, &rfds);
        struct timeval tv = {1, 0};
        int sel = select(server_sock + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;   /* interrupted by a signal */
            break;
        }
        if (sel == 0) {   /* timeout: no new connections -> housekeeping */
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
        /* Hand the socket to a fresh thread; connection.c takes it over. */
        net_spawn_client(cfd, caddr);
    }
}

int main(int argc, char **argv) {
    int port = PORT;
    if (argc > 1) port = atoi(argv[1]);
    if (port <= 0 || port > 65535) port = PORT;   /* sanity check */

    /* Load admin credentials, then start every subsystem. */
    load_admin_credentials();
    logger_init("logs");
    room_init();
    pthread_mutex_lock(&user_mutex);
    users_load();
    pthread_mutex_unlock(&user_mutex);
    room_access_load();
    history_init();
    srand((unsigned)time(NULL));   /* seed RNG (used for upload tokens) */

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (open_listening_socket(port) < 0) return 1;

    printf("[SERVER] Listening on port %d\n", port);
    log_message("INFO", "Server started on port %d", port);

    accept_loop();

    printf("[SERVER] Shutting down...\n");
    server_shutdown();
    return 0;
}
