#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "logger.h"

static FILE *log_fp = NULL;
static char log_path[128] = {0};

void logger_init(const char *log_dir) {
    char path[128];
    snprintf(path, sizeof(path), "%s/server.log", log_dir);
    strncpy(log_path, path, sizeof(log_path)-1);
    log_path[sizeof(log_path)-1] = '\0';
    log_fp = fopen(log_path, "a");
    if (log_fp) {
        setbuf(log_fp, NULL);
    }
}

void logger_close(void) {
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

void log_message(const char *level, const char *format, ...) {
    if (!log_fp) return;
    va_list args;
    time_t now = time(NULL);
    char timestr[64];
    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_start(args, format);
    fprintf(log_fp, "[%s] [%s] ", timestr, level);
    vfprintf(log_fp, format, args);
    fprintf(log_fp, "\n");
    va_end(args);
}
