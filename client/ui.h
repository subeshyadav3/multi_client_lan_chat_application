#ifndef UI_H
#define UI_H

#include <stdbool.h>

void ui_init(int *argc, char ***argv);
void ui_set_username(const char *name);
void ui_show_login_window(void);
void ui_show_login_error(const char *reason);
void ui_show_main_window(void);
void ui_append_message(const char *room, const char *sender, const char *text, const char *timestamp);
void ui_append_private_message(const char *sender, const char *text, const char *timestamp);
void ui_append_announcement(const char *text, const char *timestamp);
void ui_update_user_list(const char *users_csv, int count);
void ui_update_room_list(const char *rooms_csv, int count);
void ui_add_notification(const char *text);
void ui_show_typing(const char *room, const char *username);
void ui_run(void);
void ui_cleanup(void);

#endif
