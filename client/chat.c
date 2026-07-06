#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>

#include "chat.h"
#include "../shared/protocol.h"
#include "../shared/constants.h"

static int sockfd = -1;
static pthread_t recv_thread;
static volatile bool connected = false;
static volatile bool running = true;
static bool thread_created = false;
static on_msg_callback_t message_cb = NULL;
static on_file_progress_t file_cb = NULL;
static on_disconnect_t disconnect_cb = NULL;

static void* client_receive_loop(void* arg);
static void handle_incoming(const char* line);
static bool send_line(const char* line);

bool client_connect(const char* ip, int port) {
    struct hostent* he = gethostbyname(ip);
    if (!he) {
        fprintf(stderr, "[CLIENT] Failed to resolve host\n");
        return false;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return false;
    }

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    memcpy(&serv_addr.sin_addr, he->h_addr, he->h_length);

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sockfd);
        sockfd = -1;
        return false;
    }

    running = true;
    if (pthread_create(&recv_thread, NULL, client_receive_loop, NULL) != 0) {
        close(sockfd);
        sockfd = -1;
        return false;
    }
    thread_created = true;
    connected = true;
    return true;
}

void client_disconnect(void) {
    if (!connected && !thread_created) return;
    running = false;
    connected = false;
    if (sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
        sockfd = -1;
    }
    if (thread_created) {
        pthread_cancel(recv_thread);
        pthread_join(recv_thread, NULL);
        thread_created = false;
    }
}

bool client_send_login(const char* username) {
    if (!connected) return false;
    char line[128];
    snprintf(line, sizeof(line), "LOGIN|%s", username);
    return send_line(line);
}

bool client_send_public(const char* room, const char* text) {
    if (!connected) return false;
    char line[MAX_MESSAGE + 128];
    snprintf(line, sizeof(line), "PUBLIC|%s|%s", room, text);
    return send_line(line);
}

bool client_send_private(const char* to, const char* text) {
    if (!connected) return false;
    char line[MAX_MESSAGE + 128];
    snprintf(line, sizeof(line), "PRIVATE|%s|%s", to, text);
    return send_line(line);
}

bool client_send_typing(const char* room) {
    if (!connected) return false;
    char line[128];
    const char* target = room ? room : "general";
    snprintf(line, sizeof(line), "TYPING|%s", target);
    return send_line(line);
}

bool client_send_file_offer(const char* filename, long size, const char* target) {
    if (!connected) return false;
    char line[256];
    snprintf(line, sizeof(line), "FILE_OFFER|%s|%ld|%s", filename, size, target?target:"");
    return send_line(line);
}

bool client_send_raw(const char* data) {
    if (!connected) return false;
    return send_line(data);
}

void client_register_callbacks(on_msg_callback_t msg_cb, on_file_progress_t file_cb_in, on_disconnect_t dis_cb) {
    message_cb = msg_cb;
    file_cb = file_cb_in;
    disconnect_cb = dis_cb;
}

bool client_is_connected(void) {
    return connected;
}

static bool send_line(const char* line) {
    size_t len = strlen(line);
    if (len > BUFFER_SIZE) len = BUFFER_SIZE;
    char buf[BUFFER_SIZE + 4];
    memcpy(buf, line, len);
    buf[len] = '\n';
    size_t total = len + 1;
    size_t sent_total = 0;
    while (sent_total < total) {
        ssize_t n = send(sockfd, buf + sent_total, total - sent_total, 0);
        if (n <= 0) return false;
        sent_total += (size_t)n;
    }
    return true;
}

static void* client_receive_loop(void* arg) {
    (void)arg;
    char buffer[BUFFER_SIZE];
    char line[BUFFER_SIZE];
    size_t line_pos = 0;
    while (connected && running) {
        ssize_t n = recv(sockfd, buffer, sizeof(buffer)-1, 0);
        if (n <= 0) {
            connected = false;
            if (disconnect_cb) disconnect_cb("Connection closed by server");
            break;
        }
        buffer[n] = '\0';
        for (ssize_t i = 0; i < n; i++) {
            if (buffer[i] == '\n') {
                line[line_pos] = '\0';
                handle_incoming(line);
                line_pos = 0;
            } else {
                if (line_pos < sizeof(line)-1) {
                    line[line_pos++] = buffer[i];
                }
            }
        }
    }
    return NULL;
}

static void handle_incoming(const char* line) {
    if (message_cb) {
        message_cb((char*)line);
    }
}
