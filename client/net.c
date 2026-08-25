/* net.c - tiny wrappers around the OS socket API.
 *
 * The whole program talks to the server as a stream of newline-terminated
 * "lines". This file gives us three easy helpers:
 *   - net_connect()   open a TCP connection to a host + port
 *   - net_send_line() write one whole line (with its trailing '\n')
 *   - net_close()     tidy up and close the socket
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "net.h"
#include "../shared/constants.h"

/* Resolve the host name, open a TCP socket, and connect to it.
 * Returns the socket file descriptor, or -1 if anything went wrong. */
int net_connect(const char *host, int port) {
    /* Look up the address of the given host name. */
    struct hostent *he = gethostbyname(host);
    if (!he) return -1;

    /* Ask the OS for a TCP socket. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* Fill in the address we want to connect to. */
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);

    /* Attempt the actual connection. */
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Write one newline-terminated line to the socket.
 * send() may not write everything in one go, so we loop until it is all out.
 * Returns true when the whole line was sent, and false on error. */
bool net_send_line(int sockfd, const char *line) {
    if (sockfd < 0 || !line) return false;
    size_t len = strlen(line);
    if (len > BUFFER_SIZE) len = BUFFER_SIZE;   /* don't overrun the buffer */

    /* Copy the line and add the required '\n' terminator. */
    char buf[BUFFER_SIZE + 4];
    memcpy(buf, line, len);
    buf[len] = '\n';

    size_t total = len + 1;
    size_t sent = 0;
    while (sent < total) {
        ssize_t n = send(sockfd, buf + sent, total - sent, 0);
        if (n <= 0) return false;               /* error or closed connection */
        sent += (size_t)n;
    }
    return true;
}

/* Politely shut down and close the socket. */
void net_close(int sockfd) {
    if (sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
    }
}
