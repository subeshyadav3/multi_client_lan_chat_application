/* files.c - file transfer on the client side.
 *
 * Sending and receiving files with base64 coding. Two separate machines:
 *   - Outbound: after the user runs /sendfile we ask the server for a grant,
 *     then stream chunks from the main loop (files_try_send_chunk).
 *   - Inbound: incoming offers are kept in app->offers; when the remote side
 *     accepts, we write FILE_DATA chunks to files/<name>.tmp and at FILE_END
 *     rename it to files/<name>.
 * We bring our own tiny base64 coder so there are no extra dependencies.
 */
#include "client.h"
#include "net.h"
#include "../shared/constants.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* Standard base64 alphabet: each 6-bit group maps to one of these chars. */
static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode raw bytes into base64 text. `out` must be big enough (4 per 3 bytes). */
static size_t b64encode(const unsigned char *in, size_t len, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        /* Pack up to 3 bytes into one 24-bit value. */
        unsigned int n = in[i] << 16;
        if (i + 1 < len) n |= in[i + 1] << 8;
        if (i + 2 < len) n |= in[i + 2];
        /* Emit four characters; missing input bytes become '=' padding. */
        out[o++] = b64tab[(n >> 18) & 63];
        out[o++] = b64tab[(n >> 12) & 63];
        out[o++] = (i + 1 < len) ? b64tab[(n >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? b64tab[n & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* Turn a base64 character back into its 6-bit value, or -1 if invalid. */
static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode base64 text back into raw bytes; returns how many bytes we wrote. */
static size_t b64decode(const char *in, unsigned char *out) {
    size_t o = 0;
    size_t len = strlen(in);
    int buf = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64val(in[i]);
        if (v < 0) continue;          /* skip padding / non-base64 chars */
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {              /* we have a full byte: write it out */
            bits -= 8;
            out[o++] = (unsigned char)((buf >> bits) & 0xFF);
        }
    }
    return o;
}

/* Return just the file name part of a path (everything after the last '/'). */
static const char *path_basename(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* ---- pending incoming offers ---- */

/* Find an offer by sender + filename, or NULL if there is no match yet. */
static PendingOffer *find_offer(App *app, const char *sender, const char *filename) {
    for (int i = 0; i < app->offer_count; i++) {
        if (strcmp(app->offers[i].sender, sender) == 0 &&
            strcmp(app->offers[i].filename, filename) == 0)
            return &app->offers[i];
    }
    return NULL;
}

/* Remember a new incoming offer and tell the user they can /accept or /reject. */
void files_add_offer(App *app, const char *sender, const char *filename, long size, const char *target) {
    if (find_offer(app, sender, filename)) return;   /* we already know this one */
    if (app->offer_count >= MAX_PENDING_OFFERS) {
        tui_add_line(app, LN_FILE, "%s offers '%s' (%ld bytes)%s (offer queue full)",
                     sender, filename, size, target[0] ? " to you" : " to room");
        return;
    }
    PendingOffer *o = &app->offers[app->offer_count];
    strncpy(o->sender, sender, sizeof(o->sender) - 1);
    strncpy(o->filename, filename, sizeof(o->filename) - 1);
    strncpy(o->target, target, sizeof(o->target) - 1);
    o->size = size;
    int id = app->offer_count + 1;   /* offers are numbered 1..N for the user */
    app->offer_count++;
    tui_add_line(app, LN_FILE, "[%d] %s offers '%s' (%ld bytes)%s. /accept %d or /reject %d",
                 id, sender, filename, size, target[0] ? " to you" : " to room", id, id);
}

/* Remove a resolved offer (after /accept or /reject) from the pending list. */
void files_remove_offer(App *app, const char *sender, const char *filename) {
    for (int i = 0; i < app->offer_count; i++) {
        if (strcmp(app->offers[i].sender, sender) == 0 &&
            strcmp(app->offers[i].filename, filename) == 0) {
            /* Shift the remaining offers down over the removed one. */
            memmove(&app->offers[i], &app->offers[i + 1],
                    sizeof(PendingOffer) * (app->offer_count - i - 1));
            app->offer_count--;
            return;
        }
    }
}

/* ---- outbound: ask the server for permission to send a file ---- */

void files_request_send_file(App *app, const char *target, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        tui_add_line(app, LN_ERROR, "Cannot open file: %s", path);
        return;
    }
    if (st.st_size > MAX_FILE_SIZE) {
        tui_add_line(app, LN_ERROR, "File too large (max %d MB)", MAX_FILE_SIZE / (1024 * 1024));
        return;
    }
    /* Remember the details so we can stream it once the grant comes back. */
    const char *name = path_basename(path);
    strncpy(app->send_filename, name, sizeof(app->send_filename) - 1);
    strncpy(app->send_path, path, sizeof(app->send_path) - 1);
    strncpy(app->send_target, target, sizeof(app->send_target) - 1);
    app->send_total = (long)st.st_size;
    app->send_done = 0;
    app->send_state = 1;   /* requested; waiting for the server to grant */

    char line[512];
    snprintf(line, sizeof(line), "FILE_REQUEST|%s|%ld|%s", name, (long)st.st_size, target);
    net_send_line(app->sockfd, line);
    tui_add_line(app, LN_FILE, "Offering '%s' (%ld bytes)%s ...", name, (long)st.st_size,
                 target[0] ? " to " : " to room");
}

/* ---- inbound: the receive machine ---- */

/* Decide the final path in files/. If the plain name is already taken, fall
 * back to "name (1)". We keep the loop shape deliberate and simple. */
static void choose_final_path(App *app, char *finalpath, size_t cap) {
    snprintf(finalpath, cap, "files/%s", app->recv_filename);
    FILE *exists = fopen(finalpath, "r");
    if (!exists) return;
    fclose(exists);
    /* The plain name is taken, so try the suffixed "name (1)" variant. */
    char unique[600];
    int n = 1;
    do {
        snprintf(unique, sizeof(unique), "files/%s (%d)", app->recv_filename, n++);
        exists = fopen(unique, "r");
        if (exists) { fclose(exists); continue; }
    } while (0);
    snprintf(finalpath, cap, "%s", unique);
}

/* Close the receive file and rename the .tmp to its final files/<name>. */
void files_finalize_received(App *app) {
    if (!app->receiving || !app->recv_fp) return;
    fclose(app->recv_fp);
    app->recv_fp = NULL;
    char finalpath[512];
    char tmppath[512];
    snprintf(tmppath, sizeof(tmppath), "files/%s.tmp", app->recv_filename);
    choose_final_path(app, finalpath, sizeof(finalpath));
    rename(tmppath, finalpath);
    tui_add_line(app, LN_FILE, "Received '%s' (%ld bytes) -> %s", app->recv_filename, app->recv_done, finalpath);
    app->receiving = false;
    app->recv_filename[0] = 0;
    app->recv_done = 0;
}

/* Append one FILE_DATA chunk to the receive buffer. */
void files_receive_chunk(App *app, const char *sender, const char *filename, const char *b64) {
    if (!b64) return;
    char b64copy[BUFFER_SIZE];
    strncpy(b64copy, b64, sizeof(b64copy) - 1);
    b64copy[sizeof(b64copy) - 1] = 0;
    char *nl = strchr(b64copy, '\n'); if (nl) *nl = 0;

    /* If this is the first chunk of a new file, open a fresh .tmp and start. */
    if (!app->receiving || strcmp(app->recv_filename, filename) != 0) {
        if (app->recv_fp) { fclose(app->recv_fp); app->recv_fp = NULL; }
        strncpy(app->recv_filename, filename, sizeof(app->recv_filename) - 1);
        strncpy(app->recv_sender, sender, sizeof(app->recv_sender) - 1);
        PendingOffer *o = find_offer(app, sender, filename);
        app->recv_total = o ? o->size : 0;
        app->recv_done = 0;
        char tmppath[512];
        snprintf(tmppath, sizeof(tmppath), "files/%s.tmp", filename);
        app->recv_fp = fopen(tmppath, "wb");
        app->receiving = true;
    }

    if (app->recv_fp) {
        /* Decode the base64 back to raw bytes and write them to the file. */
        unsigned char decoded[BUFFER_SIZE];
        size_t got = b64decode(b64copy, decoded);
        fwrite(decoded, 1, got, app->recv_fp);
        app->recv_done += (long)got;
        if (app->recv_total > 0 && app->receiving) {
            int pct = (int)(app->recv_done * 100 / app->recv_total);
            if (pct % 25 == 0)
                tui_add_line(app, LN_FILE, "Receiving '%s': %d%%", filename, pct);
        }
    }
}

/* ---- outbound: stream the next chunk (called from the main loop) ---- */

void files_try_send_chunk(App *app) {
    if (app->send_state != 3 || !app->send_fp) return;   /* not ready to stream */
    unsigned char raw[FILE_CHUNK_SIZE];
    size_t got = fread(raw, 1, FILE_CHUNK_SIZE, app->send_fp);
    if (got == 0) {
        /* Reached the end of the file: close it and tell the server. */
        fclose(app->send_fp);
        app->send_fp = NULL;
        char line[MAX_MESSAGE + 64];
        snprintf(line, sizeof(line), "FILE_END|%s", app->send_filename);
        net_send_line(app->sockfd, line);
        tui_add_line(app, LN_FILE, "Sent '%s' (%ld bytes).", app->send_filename, app->send_total);
        app->send_state = 0;
        return;
    }
    /* Encode the chunk and send it as a FILE_DATA line. */
    char b64[BUFFER_SIZE];
    b64encode(raw, got, b64);
    char out[BUFFER_SIZE + 64];
    snprintf(out, sizeof(out), "FILE_DATA|%s|%s|%s", app->send_filename, app->send_token, b64);
    net_send_line(app->sockfd, out);
    app->send_done += (long)got;
    if (app->send_total > 0) {
        int pct = (int)(app->send_done * 100 / app->send_total);
        if (pct % 10 == 0)
            tui_add_line(app, LN_FILE, "Uploading '%s': %d%%", app->send_filename, pct);
    }
}
