/* files.c - everything about file transfer on the server side.
 *
 * Three pieces of state, each guarded by its own mutex:
 *   - upload_slots[]   : at most MAX_CONCURRENT_UPLOADS in-flight uploads,
 *                        each with a random token the sender must present
 *                        (upload_mutex);
 *   - upload queue     : FIFO of FILE_REQUESTs that could not start yet
 *                        because slots or the byte budget were full
 *                        (upload_mutex);
 *   - transfer_list    : active offers, used to route FILE_DATA/FILE_END
 *                        to the right recipient (transfer_mutex).
 *
 * The FILE_* protocol handlers all live here too, so the chat handlers
 * (handlers.c) never need to know any of this.
 */
#include "server.h"

/* ── in-flight upload slots ── */
static UploadSlot upload_slots[MAX_CONCURRENT_UPLOADS];
static pthread_mutex_t upload_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── FIFO upload queue ── */
static UploadQueueEntry *upload_queue_head = NULL;
static UploadQueueEntry *upload_queue_tail = NULL;
static int upload_queue_count = 0;

/* ── active file transfer offers ── */
static FileTransfer *transfer_list = NULL;
static pthread_mutex_t transfer_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- transfer bookkeeping ---- */

static void transfer_add(const char *sender, const char *filename, long size, const char *recipient) {
    pthread_mutex_lock(&transfer_mutex);
    FileTransfer *t = calloc(1, sizeof(FileTransfer));
    if (t) {
        strncpy(t->sender, sender, MAX_USERNAME - 1);
        strncpy(t->filename, filename, MAX_FILENAME - 1);
        strncpy(t->recipient, recipient ? recipient : "", MAX_USERNAME - 1);
        t->size = size;
        t->next = transfer_list;
        transfer_list = t;
    }
    pthread_mutex_unlock(&transfer_mutex);
}

static void transfer_remove(const char *sender, const char *filename) {
    pthread_mutex_lock(&transfer_mutex);
    FileTransfer **pp = &transfer_list;
    while (*pp) {
        if (strcmp((*pp)->sender, sender) == 0 && strcmp((*pp)->filename, filename) == 0) {
            FileTransfer *tmp = *pp;
            *pp = (*pp)->next;
            free(tmp);
            break;
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&transfer_mutex);
}

/* Caller must hold transfer_mutex; returns true and fills a snapshot. */
static bool transfer_find(const char *sender, const char *filename, FileTransfer *out) {
    for (FileTransfer *t = transfer_list; t; t = t->next) {
        if (strcmp(t->sender, sender) == 0 && strcmp(t->filename, filename) == 0) {
            if (out) { memcpy(out, t, sizeof(FileTransfer)); out->next = NULL; }
            return true;
        }
    }
    return false;
}

/* Drop every offer sent by a disconnected client. */
void files_remove_transfers(const char *sender) {
    pthread_mutex_lock(&transfer_mutex);
    FileTransfer **pp = &transfer_list;
    while (*pp) {
        if (strcmp((*pp)->sender, sender) == 0) {
            FileTransfer *tmp = *pp;
            *pp = (*pp)->next;
            free(tmp);
        } else {
            pp = &((*pp)->next);
        }
    }
    pthread_mutex_unlock(&transfer_mutex);
}

/* ---- upload slot / queue helpers (all share upload_mutex) ---- */

/* Sum of sizes of currently-granted (in-flight) uploads. */
static long upload_active_bytes(void) {
    long total = 0;
    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++)
        if (upload_slots[i].active) total += upload_slots[i].size;
    return total;
}

/* Can a new transfer of `size` bytes start right now? */
static bool upload_can_accept(long size) {
    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++)
        if (!upload_slots[i].active)
            return upload_active_bytes() + size <= MAX_TOTAL_UPLOAD_BYTES;
    return false;
}

/* Generate a fresh random token for an upload slot. */
static void make_token(char *token, size_t len, Client *c) {
    unsigned h = (unsigned)(time(NULL) ^ (uintptr_t)c ^ (unsigned)rand());
    for (size_t i = 0; i + 1 < len; i++) {
        int r = (h >> (i * 2)) & 0xf;
        token[i] = "0123456789abcdef"[r % 16];
        h = h * 1103515245u + 12345u;
    }
    token[len - 1] = '\0';
}

/* Add the request to the FIFO queue and reply FILE_WAIT with its position. */
static void upload_enqueue(Client *c, const char *sender, const char *filename,
                           const char *recipient, long size) {
    if (upload_queue_count >= MAX_QUEUE_DEPTH) {
        char err[256];
        snprintf(err, sizeof(err), "FILE_DENIED|%s|%s|Upload queue is full, try again later\n",
                 filename, sender);
        send(c->sockfd, err, strlen(err), 0);
        log_message("FILE", "%s DENIED '%s': queue full (%d queued)", sender, filename, upload_queue_count);
        return;
    }
    UploadQueueEntry *e = calloc(1, sizeof(UploadQueueEntry));
    if (!e) {
        char err[256];
        snprintf(err, sizeof(err), "FILE_DENIED|%s|%s|Server out of memory\n", filename, sender);
        send(c->sockfd, err, strlen(err), 0);
        return;
    }
    strncpy(e->sender, sender, MAX_USERNAME - 1);
    strncpy(e->filename, filename, MAX_FILENAME - 1);
    strncpy(e->recipient, recipient ? recipient : "", MAX_USERNAME - 1);
    e->size = size;
    e->client = c;
    e->queued_at = time(NULL);

    int pos = upload_queue_count + 1;
    if (upload_queue_tail) upload_queue_tail->next = e;
    else                   upload_queue_head = e;
    upload_queue_tail = e;
    upload_queue_count++;

    char wait[256];
    snprintf(wait, sizeof(wait), "FILE_WAIT|%s|%d|%ld\n", filename, pos, size);
    send(c->sockfd, wait, strlen(wait), 0);
    log_message("FILE", "%s QUEUED '%s' (%ld bytes) at position %d (in-flight %ld/%ld)",
                sender, filename, size, pos, upload_active_bytes(), (long)MAX_TOTAL_UPLOAD_BYTES);
}

/* Grant one queued entry: fill a slot, notify the owner, forward the offer. */
static void upload_grant_one(UploadQueueEntry *e) {
    if (!e || !e->client || !e->client->active) return;

    char token[TOKEN_LEN + 1];
    make_token(token, sizeof(token), e->client);

    pthread_mutex_lock(&upload_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
        if (!upload_slots[i].active) { slot = i; break; }
    }
    if (slot >= 0) {
        upload_slots[slot].active = true;
        strncpy(upload_slots[slot].token, token, TOKEN_LEN);
        strncpy(upload_slots[slot].sender, e->sender, MAX_USERNAME - 1);
        strncpy(upload_slots[slot].filename, e->filename, MAX_FILENAME - 1);
        strncpy(upload_slots[slot].recipient, e->recipient, MAX_USERNAME - 1);
        upload_slots[slot].size = e->size;
        upload_slots[slot].started_at = time(NULL);
    }
    pthread_mutex_unlock(&upload_mutex);
    if (slot < 0) return;

    total_files++;
    transfer_add(e->sender, e->filename, e->size, e->recipient);

    char grant[512];
    snprintf(grant, sizeof(grant), "FILE_GRANTED|%s|%s|%s|%ld\n",
             e->sender, e->filename, token, e->size);
    send(e->client->sockfd, grant, strlen(grant), 0);

    char fwd[512];
    snprintf(fwd, sizeof(fwd), "FILE_OFFER|%s|%s|%ld|%s\n",
             e->sender, e->filename, e->size, e->recipient);
    if (e->recipient[0]) net_send_to_user(e->recipient, fwd);
    else net_broadcast(fwd, e->client);

    log_message("FILE", "%s GRANTED from queue token %s for '%s' (%ld bytes) slot=%d",
                e->sender, token, e->filename, e->size, slot);
}

/* Start as many queued transfers as the active limits allow. */
void files_process_queue(void) {
    UploadQueueEntry *head = NULL;
    pthread_mutex_lock(&upload_mutex);
    while (upload_queue_head) {
        UploadQueueEntry *e = upload_queue_head;
        if (!upload_can_accept(e->size) || !e->client || !e->client->active) break;
        upload_queue_head = e->next;
        if (!upload_queue_head) upload_queue_tail = NULL;
        upload_queue_count--;
        e->next = head;
        head = e;
    }
    pthread_mutex_unlock(&upload_mutex);
    while (head) {
        UploadQueueEntry *e = head;
        head = e->next;
        upload_grant_one(e);
        free(e);
    }
}

/* Drop queue entries whose wait timed out (reply FILE_DENIED to the owner). */
static void upload_expire_queue(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&upload_mutex);
    UploadQueueEntry **pp = &upload_queue_head;
    while (*pp) {
        if ((now - (*pp)->queued_at) > QUEUE_TIMEOUT_SEC) {
            UploadQueueEntry *tmp = *pp;
            *pp = tmp->next;
            if (tmp->client && tmp->client->active) {
                char err[256];
                snprintf(err, sizeof(err),
                         "FILE_DENIED|%s|%s|Upload wait timed out\n",
                         tmp->filename, tmp->sender);
                send(tmp->client->sockfd, err, strlen(err), 0);
            }
            free(tmp);
            upload_queue_count--;
        } else {
            pp = &((*pp)->next);
        }
    }
    if (!upload_queue_head) upload_queue_tail = NULL;
    else {
        UploadQueueEntry *t = upload_queue_head;
        while (t->next) t = t->next;
        upload_queue_tail = t;
    }
    pthread_mutex_unlock(&upload_mutex);
}

/* Deactivate stale slots and timed-out queue entries, then promote the queue. */
void files_expire_stale(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&upload_mutex);
    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
        if (upload_slots[i].active &&
            (now - upload_slots[i].started_at) > UPLOAD_TIMEOUT_SEC) {
            upload_slots[i].active = false;
        }
    }
    pthread_mutex_unlock(&upload_mutex);
    upload_expire_queue();
    files_process_queue();
}

/* Drop every queued entry belonging to a disconnected client. */
void files_queue_remove_client(Client *c) {
    pthread_mutex_lock(&upload_mutex);
    UploadQueueEntry **pp = &upload_queue_head;
    while (*pp) {
        if ((*pp)->client == c) {
            UploadQueueEntry *tmp = *pp;
            *pp = tmp->next;
            free(tmp);
            upload_queue_count--;
        } else {
            pp = &((*pp)->next);
        }
    }
    if (!upload_queue_head) upload_queue_tail = NULL;
    else {
        UploadQueueEntry *t = upload_queue_head;
        while (t->next) t = t->next;
        upload_queue_tail = t;
    }
    pthread_mutex_unlock(&upload_mutex);
}

/* Release every slot held by a disconnected sender. */
void files_remove_slots(const char *sender) {
    pthread_mutex_lock(&upload_mutex);
    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
        if (upload_slots[i].active && strcmp(upload_slots[i].sender, sender) == 0)
            upload_slots[i].active = false;
    }
    pthread_mutex_unlock(&upload_mutex);
}

/* Free the slot + offer for a finished/rejected transfer and promote queue. */
void files_release(const char *sender, const char *filename) {
    transfer_remove(sender, filename);
    pthread_mutex_lock(&upload_mutex);
    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
        if (upload_slots[i].active &&
            strcmp(upload_slots[i].sender, sender) == 0 &&
            strcmp(upload_slots[i].filename, filename) == 0) {
            upload_slots[i].active = false;
            break;
        }
    }
    pthread_mutex_unlock(&upload_mutex);
    files_process_queue();
}

/* ---- FILE_* protocol handlers ---- */

/* FILE_REQUEST|filename|size|target   (target empty = broadcast to room).
 * If the limits allow, grant a slot now; otherwise enqueue the request and
 * reply FILE_WAIT with its queue position. */
void files_handler_request(Client *c, Cmd *m) {
    long fsize = atol(m->a2);
    char target[MAX_USERNAME] = {0};
    strncpy(target, m->a3, sizeof(target) - 1);
    char safe_name[MAX_FILENAME];
    net_sanitize_filename(safe_name, sizeof(safe_name), m->a1);

    if (fsize <= 0 || fsize > MAX_FILE_SIZE) {
        char err[256];
        snprintf(err, sizeof(err), "FILE_DENIED|%s|%s|File too large or invalid size\n", safe_name, c->username);
        send(c->sockfd, err, strlen(err), 0);
        return;
    }

    pthread_mutex_lock(&upload_mutex);
    if (!upload_can_accept(fsize)) {
        upload_enqueue(c, c->username, safe_name, target, fsize);
        pthread_mutex_unlock(&upload_mutex);
        return;
    }
    int slot = -1;
    for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
        if (!upload_slots[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        upload_enqueue(c, c->username, safe_name, target, fsize);
        pthread_mutex_unlock(&upload_mutex);
        return;
    }

    char token[TOKEN_LEN + 1];
    make_token(token, sizeof(token), c);

    upload_slots[slot].active = true;
    strncpy(upload_slots[slot].token, token, TOKEN_LEN);
    strncpy(upload_slots[slot].sender, c->username, MAX_USERNAME - 1);
    strncpy(upload_slots[slot].filename, safe_name, MAX_FILENAME - 1);
    strncpy(upload_slots[slot].recipient, target, MAX_USERNAME - 1);
    upload_slots[slot].size = fsize;
    upload_slots[slot].started_at = time(NULL);
    pthread_mutex_unlock(&upload_mutex);

    total_files++;
    transfer_add(c->username, safe_name, fsize, target);

    char grant[512];
    snprintf(grant, sizeof(grant), "FILE_GRANTED|%s|%s|%s|%ld\n",
             c->username, safe_name, token, fsize);
    send(c->sockfd, grant, strlen(grant), 0);

    char fwd[512];
    snprintf(fwd, sizeof(fwd), "FILE_OFFER|%s|%s|%ld|%s\n",
             c->username, safe_name, fsize, target);
    if (target[0]) net_send_to_user(target, fwd);
    else net_broadcast(fwd, c);

    log_message("FILE", "%s GRANTED token %s for '%s' (%ld bytes) slot=%d",
                c->username, token, safe_name, fsize, slot);
}

/* FILE_OFFER|filename|size|recipient - legacy/peer-visible offer. */
void files_handler_offer(Client *c, Cmd *m) {
    total_files++;
    char safe_name[MAX_FILENAME];
    net_sanitize_filename(safe_name, sizeof(safe_name), m->a1);
    transfer_add(c->username, safe_name, atol(m->a2), m->a3);
    char fwd[512];
    snprintf(fwd, sizeof(fwd), "FILE_OFFER|%s|%s|%s|%s\n", c->username, safe_name, m->a2, m->a3);
    if (strlen(m->a3) > 0) net_send_to_user(m->a3, fwd);
    else net_broadcast(fwd, c);
    log_message("FILE", "%s offered file '%s' (%s bytes) to %s", c->username, safe_name, m->a2, m->a3[0] ? m->a3 : "all");
}

/* FILE_DATA|filename|token|base64 - forward a chunk to the recipient.
 * Parsed from the raw line (base64 may be larger than a normal arg). */
void files_handler_data(Client *c, Cmd *m) {
    const char *line = m->raw;
    if (!line) return;
    const char *p1 = strchr(line, '|');
    if (!p1) return;
    const char *p2 = strchr(p1 + 1, '|');
    if (!p2) return;

    char fname[MAX_FILENAME];
    size_t fn_len = (size_t)(p2 - p1 - 1);
    if (fn_len >= MAX_FILENAME) fn_len = MAX_FILENAME - 1;
    memcpy(fname, p1 + 1, fn_len);
    fname[fn_len] = '\0';

    FileTransfer tf;
    bool found = false;
    pthread_mutex_lock(&transfer_mutex);
    found = transfer_find(c->username, fname, &tf);
    pthread_mutex_unlock(&transfer_mutex);
    if (!found) return;

    /* Validate the upload token embedded after the filename. */
    const char *b64 = p2 + 1;
    char token_match[TOKEN_LEN + 1] = {0};
    const char *base64_data = b64;
    bool valid_token = false;
    const char *token_delim = strchr(b64, '|');
    if (token_delim) {
        size_t tok_len = token_delim - b64;
        if (tok_len > TOKEN_LEN) tok_len = TOKEN_LEN;
        strncpy(token_match, b64, tok_len);
        token_match[tok_len] = '\0';
        base64_data = token_delim + 1;
        pthread_mutex_lock(&upload_mutex);
        for (int i = 0; i < MAX_CONCURRENT_UPLOADS; i++) {
            if (upload_slots[i].active &&
                strcmp(upload_slots[i].sender, c->username) == 0 &&
                strcmp(upload_slots[i].filename, fname) == 0 &&
                strcmp(upload_slots[i].token, token_match) == 0) {
                valid_token = true;
                upload_slots[i].started_at = time(NULL);
                break;
            }
        }
        pthread_mutex_unlock(&upload_mutex);
    } else {
        valid_token = true; /* backward-compatible */
    }
    if (!valid_token) return;

    char fwd[BUFFER_SIZE + 64];
    snprintf(fwd, sizeof(fwd), "FILE_DATA|%s|%s|%s\n", c->username, fname, base64_data);
    if (tf.recipient[0]) net_send_to_user(tf.recipient, fwd);
    else net_broadcast(fwd, c);
}

/* FILE_END|filename - the sender finished; notify and release the slot. */
void files_handler_end(Client *c, Cmd *m) {
    FileTransfer tf;
    bool found = false;
    pthread_mutex_lock(&transfer_mutex);
    found = transfer_find(c->username, m->a1, &tf);
    pthread_mutex_unlock(&transfer_mutex);
    if (!found) return;

    char msg[256];
    snprintf(msg, sizeof(msg), "FILE_END|%s|%s\n", c->username, m->a1);
    if (tf.recipient[0]) net_send_to_user(tf.recipient, msg);
    else net_broadcast(msg, c);
    files_release(c->username, m->a1);
    log_message("FILE", "File '%s' from %s completed", m->a1, c->username);
}

/* FILE_ACCEPT|sender|filename - accept an offer back to its owner. */
void files_handler_accept(Client *c, Cmd *m) {
    FileTransfer tf;
    bool found = false;
    pthread_mutex_lock(&transfer_mutex);
    found = transfer_find(m->a1, m->a2, &tf);
    pthread_mutex_unlock(&transfer_mutex);
    /* Accepting works for a targeted recipient OR any user when
       the file was offered to the room (broadcast). */
    if (found && (tf.recipient[0] == 0 || strcmp(tf.recipient, c->username) == 0)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "FILE_ACCEPT|%s|%s\n", c->username, m->a2);
        net_send_to_user(m->a1, msg);
        log_message("FILE", "%s accepted file '%s' from %s", c->username, m->a2, m->a1);
    } else {
        char err[256];
        snprintf(err, sizeof(err), "ERROR|No active file offer from '%s' named '%s'\n", m->a1, m->a2);
        send(c->sockfd, err, strlen(err), 0);
    }
}

/* FILE_REJECT|sender|filename|reason - decline an offer back to its owner. */
void files_handler_reject(Client *c, Cmd *m) {
    FileTransfer tf;
    bool found = false;
    pthread_mutex_lock(&transfer_mutex);
    found = transfer_find(m->a1, m->a2, &tf);
    pthread_mutex_unlock(&transfer_mutex);
    if (found && (tf.recipient[0] == 0 || strcmp(tf.recipient, c->username) == 0)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "FILE_REJECT|%s|%s|%s\n", c->username, m->a2, m->a3);
        net_send_to_user(m->a1, msg);
        files_release(m->a1, m->a2);
        log_message("FILE", "%s rejected file '%s' from %s: %s", c->username, m->a2, m->a1, m->a3);
    } else {
        char err[256];
        snprintf(err, sizeof(err), "ERROR|No active file offer from '%s' named '%s'\n", m->a1, m->a2);
        send(c->sockfd, err, strlen(err), 0);
    }
}
