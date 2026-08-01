#ifndef NET_H
#define NET_H

#include <stdbool.h>

/* Resolve host, open a TCP socket, and connect. Returns the socket fd, or -1. */
int net_connect(const char *host, int port);

/* Write a single newline-terminated line to the socket (full, looped send). */
bool net_send_line(int sockfd, const char *line);

void net_close(int sockfd);

#endif
