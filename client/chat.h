#ifndef CHAT_H
#define CHAT_H

#include <stdbool.h>

typedef void (*on_msg_callback_t)(const char* msg);
typedef void (*on_file_progress_t)(const char* filename, int percent);
typedef void (*on_disconnect_t)(const char* reason);

bool client_connect(const char* ip, int port);
void client_disconnect(void);
bool client_send_login(const char* username);
bool client_send_public(const char* room, const char* text);
bool client_send_private(const char* to, const char* text);
bool client_send_typing(const char* room);
bool client_send_file_offer(const char* filename, long size, const char* target);
bool client_send_raw(const char* data);
void client_register_callbacks(on_msg_callback_t msg_cb, on_file_progress_t file_cb, on_disconnect_t dis_cb);
bool client_is_connected(void);

#endif
