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

int net_connect(const char *host, int port) {
    struct hostent *he = gethostbyname(host);
    if (!he) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr, he->h_length);

    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool net_send_line(int sockfd, const char *line) {
    if (sockfd < 0 || !line) return false;
    size_t len = strlen(line);
    if (len > BUFFER_SIZE) len = BUFFER_SIZE;
    char buf[BUFFER_SIZE + 4];
    memcpy(buf, line, len);
    buf[len] = '\n';
    size_t total = len + 1;
    size_t sent = 0;
    while (sent < total) {
        ssize_t n = send(sockfd, buf + sent, total - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

void net_close(int sockfd) {
    if (sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
    }
}
