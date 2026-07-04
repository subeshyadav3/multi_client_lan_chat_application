#ifndef LOGGER_H
#define LOGGER_H

void logger_init(const char *log_dir);
void logger_close(void);
void log_message(const char *level, const char *format, ...);

#endif
