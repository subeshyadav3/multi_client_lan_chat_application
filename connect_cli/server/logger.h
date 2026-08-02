#ifndef LOGGER_H
#define LOGGER_H

/* logger.h - public interface of the tiny file logger (see logger.c).
 *
 * The server opens one log file at startup with logger_init(log_dir) and
 * closes it at shutdown with logger_close(). Between those two calls,
 * any module can record a line with log_message():
 *
 *   log_message("INFO", "User '%s' connected", name);
 *
 * level is a short tag (INFO, MSG, FILE, CTRL, ...) that appears in the
 * line next to a timestamp. The format/... arguments work like printf().
 */

void logger_init(const char *log_dir);     /* open logs/<dir>/server.log for appending */
void logger_close(void);                   /* close the log file */
void log_message(const char *level, const char *format, ...); /* write one line */

#endif /* LOGGER_H */
