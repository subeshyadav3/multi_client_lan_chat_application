#ifndef SERVER_H
#define SERVER_H

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

/* ── File upload permission token system ── */
#define MAX_CONCURRENT_UPLOADS 2
#define MAX_TOTAL_UPLOAD_BYTES (1024 * 1024)
#define MAX_QUEUE_DEPTH 16
#define UPLOAD_TIMEOUT_SEC 30
#define QUEUE_TIMEOUT_SEC 120
#define TOKEN_LEN 16
#define ROOM_HISTORY_MAX 50

/* One connected client. */
typedef struct Client {
    int sockfd;
    struct sockaddr_in addr;
    char username[MAX_USERNAME];
    int status;
    time_t login_time;
    pthread_t thread;
    bool active;
    bool is_admin;
    char current_room[MAX_ROOM_NAME];
    struct Client *next;
} Client;

/* A user account (password stored as a SHA-256 hex string). */
typedef struct UserAccount {
    char username[MAX_USERNAME];
    char password[128];
    bool active;
    struct UserAccount *next;
} UserAccount;

/* An in-flight upload slot granted to a sender. */
typedef struct {
    char token[TOKEN_LEN + 1];
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char recipient[MAX_USERNAME];
    long size;
    time_t started_at;
    bool active;
} UploadSlot;

/* A queued file upload waiting for a free slot (FIFO). */
typedef struct UploadQueueEntry {
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char recipient[MAX_USERNAME];
    long size;
    struct Client *client;
    time_t queued_at;
    struct UploadQueueEntry *next;
} UploadQueueEntry;

/* An active file transfer offer. */
typedef struct FileTransfer {
    char sender[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char recipient[MAX_USERNAME]; /* empty = broadcast */
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
    int parts;
    const char *raw;
} Cmd;

/* Shared runtime state. */
extern volatile sig_atomic_t server_running;
extern int server_sock;
extern int total_messages, total_privmsgs, total_files;
extern char admin_user[64];
extern char admin_pass_hash[65];
extern pthread_mutex_t user_mutex;
extern UserAccount *user_list;
extern pthread_mutex_t client_mutex;   /* guards client_list */
extern Client *client_list;

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

#endif
