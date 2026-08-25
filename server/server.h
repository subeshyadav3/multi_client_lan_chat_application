#ifndef SERVER_H
#define SERVER_H

/* server.h - the shared types, globals and public functions of the server.
 *
 * One header that every server module includes, so each file can see the
 * shared data structures (Client, UserAccount, Cmd, ...) and call each
 * other's exported functions. The module split is:
 *
 *   server.c     - main(), accept loop, shutdown
 *   connection.c - one thread per client that reads commands
 *   handlers.c   - one small function per non-file chat command
 *   files.c      - file transfer (slots, FIFO queue, tokens, FILE_* cmds)
 *   net.c        - client list + delivering bytes
 *   users.c      - user accounts (SHA-256 passwords)
 *   history.c    - per-room recent message history
 *   room.c       - rooms (optionally password protected)
 *   room_access.c- per-user access to protected rooms
 *   logger.c     - file logging
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>

#include "../shared/protocol.h"
#include "../shared/constants.h"
#include "../shared/sha256.h"
#include "logger.h"
#include "room.h"

/* ================================================================ */
/* File-upload permission token system                              */
/* ================================================================ */
/* Limits that control how many uploads / bytes the server allows at
 * once, how deep the waiting FIFO queue may grow, and timeouts. */
#define MAX_CONCURRENT_UPLOADS 2      /* how many uploads can run at once */
#define MAX_TOTAL_UPLOAD_BYTES (4 * 1024 * 1024)   /* total in-flight bytes cap (4 MB) */
#define MAX_QUEUE_DEPTH 16            /* max entries waiting in the queue */
#define UPLOAD_TIMEOUT_SEC 30         /* how long a granted slot stays valid */
#define QUEUE_TIMEOUT_SEC 120         /* how long a queue entry may wait */
#define TOKEN_LEN 16                  /* length of an upload permission token */
#define ROOM_HISTORY_MAX 50           /* lines of history kept per room */

/* ================================================================ */
/* Core data structures                                             */
/* ================================================================ */

/* One connected client (verified once the user logs in). */
typedef struct Client {
    int sockfd;                  /* the network socket for this client */
    struct sockaddr_in addr;     /* where the client is connecting from */
    char username[MAX_USERNAME]; /* the logged-in name ("" before login) */
    int status;                  /* generic status flag */
    time_t login_time;           /* when the client connected */
    pthread_t thread;            /* the receive thread for this client */
    bool active;                 /* false while being torn down */
    bool is_admin;               /* true once admin login is verified */
    char current_room[MAX_ROOM_NAME]; /* the room this client is in */
    struct Client *next;         /* next client in the linked list */
} Client;

/* A registered user account (the password is a SHA-256 hex string). */
typedef struct UserAccount {
    char username[MAX_USERNAME];
    char password[128];          /* SHA-256 hex digest of the password */
    bool active;
    struct UserAccount *next;
} UserAccount;

/* An in-flight upload slot granted to a sender. */
typedef struct {
    char token[TOKEN_LEN + 1];   /* random token the sender must present */
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char recipient[MAX_USERNAME];
    long size;
    time_t started_at;           /* used to detect stale slots */
    bool active;
} UploadSlot;

/* A queued file upload waiting for a free slot (FIFO). */
typedef struct UploadQueueEntry {
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char recipient[MAX_USERNAME];
    long size;
    struct Client *client;       /* who is waiting (to tell them FILE_WAIT) */
    time_t queued_at;            /* used to detect a timed-out wait */
    struct UploadQueueEntry *next;
} UploadQueueEntry;

/* An active file transfer offer. */
typedef struct FileTransfer {
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char recipient[MAX_USERNAME]; /* empty string = broadcast to room */
    long size;
    struct FileTransfer *next;
} FileTransfer;

/* A parsed command line: CMD|a1|a2|a3|a4.
 * `raw` points at the full, untouched line (used by FILE_DATA). */
typedef struct {
    char cmd[64];
    char a1[256];
    char a2[MAX_MESSAGE];
    char a3[256];
    char a4[256];
    int parts;                   /* how many fields were filled in */
    const char *raw;             /* the original, unparsed line */
} Cmd;

/* ================================================================ */
/* Shared runtime state (defined in the .c files)                   */
/* ================================================================ */
extern volatile sig_atomic_t server_running; /* false when we should quit */
extern int server_sock;                      /* the listening socket */
extern int total_messages, total_privmsgs, total_files; /* live counters */
extern char admin_user[64];                  /* admin login name */
extern char admin_pass_hash[65];             /* admin password hash */
extern pthread_mutex_t user_mutex;           /* guards user_list */
extern UserAccount *user_list;
extern pthread_mutex_t client_mutex;         /* guards client_list */
extern Client *client_list;

/* ================================================================ */
/* Public functions (each group lives in the module named in comments) */
/* ================================================================ */

/* net.c - client list, sockets, broadcasts */
void net_spawn_client(int fd, struct sockaddr_in addr);
void net_close_all_clients(void);
void net_send_to_user(const char *username, const char *msg);
void net_broadcast(const char *msg, Client *except);
void net_broadcast_room(const char *room, const char *msg, Client *except);
void net_broadcast_user_list(void);
void net_broadcast_room_list(void);
void net_send_status(Client *c);
void net_finish_join(Client *c, const char *room);
void net_sanitize_filename(char *dst, size_t dst_len, const char *src);
void net_get_timestamp(char *buf, size_t len);
void net_list_append(char *dst, size_t dst_sz, const char *sep, const char *item);
Client *net_client_find(const char *username);   /* caller must hold client_mutex */
void net_client_remove(Client *target);

/* users.c - user account storage */
void users_load(void);   /* caller must hold user_mutex */
void users_save(void);   /* caller must hold user_mutex */
bool user_create(const char *username, const char *password);
bool user_reset_pass(const char *username, const char *password);
bool user_validate(const char *username, const char *password);
int account_remove(const char *username);

/* history.c - per-room recent-message history */
void history_init(void);
void history_add(const char *room, const char *line);
void history_replay(int sockfd, const char *room);

/* files.c - upload slots, FIFO queue, transfers, FILE_* handlers */
void files_expire_stale(void);
void files_process_queue(void);
void files_queue_remove_client(Client *c);
void files_remove_slots(const char *sender);
void files_remove_transfers(const char *sender);
void files_release(const char *sender, const char *filename);
void files_handler_request(Client *c, Cmd *m);
void files_handler_offer(Client *c, Cmd *m);
void files_handler_data(Client *c, Cmd *m);
void files_handler_end(Client *c, Cmd *m);
void files_handler_accept(Client *c, Cmd *m);
void files_handler_reject(Client *c, Cmd *m);

/* handlers.c - one small function per chat command */
void dispatch_command(Client *c, Cmd *m);

/* connection.c - per-client receive loop */
void *handle_client(void *arg);

#endif /* SERVER_H */
