/* logger.c - a tiny file logger used across the server.
 *
 * Every module calls log_message() to write one timestamped line to
 * logs/server.log. Writing to the file is done through a single FILE*
 * (log_fp) that stays open for the whole life of the server.
 *
 * WHY this is simple: we never lock the file - the server writes short
 * lines and the kernel appends are atomic enough for our purposes. If we
 * ever see garbled lines we could add a mutex here later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "logger.h"

/* The open log file, and a copy of its path (kept for clarity/debugging). */
static FILE *log_fp = NULL;
static char log_path[128] = {0};

/* Open the log file for appending (create it if missing).
 * Unbuffered, so every line hits disk immediately even on a crash. */
void logger_init(const char *log_dir) {
    char path[128];
    snprintf(path, sizeof(path), "%s/server.log", log_dir);
    strncpy(log_path, path, sizeof(log_path) - 1);
    log_path[sizeof(log_path) - 1] = '\0';
    log_fp = fopen(log_path, "a");   /* "a" = append */
    if (log_fp) setbuf(log_fp, NULL); /* no buffering */
}

/* Close the log file. Called once during server shutdown. */
void logger_close(void) {
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

/* Write one log line:  [timestamp] [level] message
 * level is a short tag like INFO, MSG, FILE, CTRL. Accepts printf-style
 * arguments just like printf(). If the log is not open we do nothing. */
void log_message(const char *level, const char *format, ...) {
    if (!log_fp) return;

    /* Build the timestamp once, then print the whole line. */
    time_t now = time(NULL);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", localtime(&now));

    va_list args;
    va_start(args, format);
    fprintf(log_fp, "[%s] [%s] ", timestr, level);
    vfprintf(log_fp, format, args);   /* print the user-supplied text */
    fprintf(log_fp, "\n");
    va_end(args);
}
