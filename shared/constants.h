#ifndef CONSTANTS_H
#define CONSTANTS_H

/* ============================================================
 * ConnectHub shared constants
 *
 * These values are used by BOTH the client and the server so that
 * both sides always agree on buffer sizes and length limits. They
 * also stop the program from overflowing its fixed-size arrays.
 * ============================================================ */

/* ---------- Network configuration ---------- */
#define PORT 8080            /* Default TCP port the server listens on        */
#define MAX_CLIENTS 128      /* Maximum simultaneous client connections       */
#define BUFFER_SIZE 4096     /* Size of one raw network read/write buffer     */

/* ---------- Text size limits ---------- */
#define MAX_USERNAME 32      /* Maximum username length                       */
#define MAX_PASSWORD 32      /* Maximum password length                       */
#define MAX_MESSAGE 2048     /* Maximum chat-message length                   */
#define MAX_ROOM_NAME 64     /* Maximum room-name length                      */
#define MAX_ROOMS 32         /* Maximum number of rooms kept in the list      */
#define MAX_SEARCH_RESULTS 100 /* Maximum search results returned             */

/* ---------- File transfer ---------- */
#define MAX_FILE_SIZE (2 * 1024 * 1024)   /* 2 MB maximum accepted file size   */
#define MAX_FILENAME 256                  /* Maximum filename length          */
#define FILE_CHUNK_SIZE 2048              /* Bytes sent in one file-data chunk */

/* ---------- History ---------- */
#define MAX_HISTORY_MESSAGES 500 /* Past messages kept in memory per room     */

#endif /* CONSTANTS_H */
