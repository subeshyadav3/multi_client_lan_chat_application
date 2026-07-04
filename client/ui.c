#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "ui.h"
#include "chat.h"

static GtkWidget *win_login = NULL;
static GtkWidget *entry_login = NULL;
static GtkWidget *entry_host = NULL;
static GtkWidget *entry_port = NULL;
static GtkWidget *lbl_status = NULL;

static GtkWidget *win_main = NULL;
static GtkWidget *tv_chat = NULL;
static GtkWidget *entry_chat = NULL;
static GtkTextBuffer *buf_chat = NULL;
static GtkTextMark *end_mark = NULL;
static GtkWidget *lbl_typing = NULL;
static GtkWidget *lst_users = NULL;
static GtkWidget *lst_rooms = NULL;
static GtkWidget *lbl_room = NULL;

static char current_user[128] = "user";
static char current_room[64] = "general";
static char server_host[128] = "127.0.0.1";
static int server_port = 8080;

static char *esc_markup(const char *t) {
    if (!t) return g_strdup("");
    GString *g = g_string_new(NULL);
    for (; *t; t++) {
        if (*t == '&') g_string_append(g, "&amp;");
        else if (*t == '<') g_string_append(g, "&lt;");
        else if (*t == '>') g_string_append(g, "&gt;");
        else g_string_append_c(g, *t);
    }
    return g_string_free(g, FALSE);
}

static void scroll_chat_to_bottom(void) {
    if (!buf_chat || !tv_chat) return;
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf_chat, &iter);
    if (!end_mark) {
        end_mark = gtk_text_buffer_create_mark(buf_chat, "end", &iter, FALSE);
    } else {
        gtk_text_buffer_move_mark(buf_chat, end_mark, &iter);
    }
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(tv_chat), end_mark, 0.0, FALSE, 0.0, 1.0);
}

static guint typing_tag = 0;
static gboolean typing_cb(gpointer d) {
    (void)d;
    typing_tag = 0;
    if (lbl_typing) gtk_label_set_text(GTK_LABEL(lbl_typing), "");
    return FALSE;
}

static void on_window_closed(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    client_disconnect();
    gtk_main_quit();
}

static void on_login(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    const char *u = gtk_entry_get_text(GTK_ENTRY(entry_login));
    if (!u || !u[0]) {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Enter username");
        return;
    }
    if (strlen(u) >= 32) {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Username too long");
        return;
    }

    const char *host = gtk_entry_get_text(GTK_ENTRY(entry_host));
    const char *port_text = gtk_entry_get_text(GTK_ENTRY(entry_port));
    if (host && host[0]) {
        strncpy(server_host, host, sizeof(server_host) - 1);
        server_host[sizeof(server_host) - 1] = '\0';
    }
    int port = server_port;
    if (port_text && port_text[0]) {
        char *end = NULL;
        long p = strtol(port_text, &end, 10);
        if (*end != '\0' || p <= 0 || p > 65535) {
            gtk_label_set_text(GTK_LABEL(lbl_status), "Invalid port");
            return;
        }
        port = (int)p;
    }
    server_port = port;

    strncpy(current_user, u, sizeof(current_user) - 1);
    current_user[sizeof(current_user) - 1] = '\0';

    if (!client_is_connected() && !client_connect(server_host, server_port)) {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Connection failed");
        return;
    }
    client_send_login(u);
}

static void on_send(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry_chat));
    if (!t || !t[0]) return;
    if (t[0] == '/' && strncmp(t, "/msg ", 5) == 0) {
        char to[64], m[2048];
        if (sscanf(t + 5, "%63s %2047[^\n]", to, m) == 2) {
            client_send_private(to, m);
        }
    } else if (t[0] == '/' && strncmp(t, "/join ", 6) == 0) {
        char r[64];
        if (sscanf(t + 6, "%63s", r) == 1) {
            char l[128];
            snprintf(l, sizeof(l), "JOIN|%s", r);
            client_send_raw(l);
            strncpy(current_room, r, sizeof(current_room) - 1);
            current_room[sizeof(current_room) - 1] = '\0';
            if (lbl_room) {
                char b[128];
                snprintf(b, sizeof(b), "Room: %s", r);
                gtk_label_set_text(GTK_LABEL(lbl_room), b);
            }
        }
    } else if (t[0] == '/' && strncmp(t, "/create ", 8) == 0) {
        char r[64];
        if (sscanf(t + 8, "%63s", r) == 1) {
            char l[128];
            snprintf(l, sizeof(l), "CREATE|%s", r);
            client_send_raw(l);
        }
    } else if (t[0] == '/' && strncmp(t, "/announce ", 10) == 0 && strcmp(current_user, "admin") == 0) {
        char m[2040];
        if (sscanf(t + 10, "%2039[^\n]", m) == 1) {
            char l[2064];
            snprintf(l, sizeof(l), "ANNOUNCE|%s", m);
            client_send_raw(l);
        }
    } else if (t[0] == '/' && strncmp(t, "/kick ", 6) == 0 && strcmp(current_user, "admin") == 0) {
        char u[64] = {0}, r[256] = {0};
        if (sscanf(t + 6, "%63s %255[^\n]", u, r) >= 1) {
            char l[512];
            snprintf(l, sizeof(l), "KICK|%s|%s", u, r);
            client_send_raw(l);
        }
    } else {
        client_send_public(current_room, t);
    }
    gtk_entry_set_text(GTK_ENTRY(entry_chat), "");
}

static void on_file(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Send File", GTK_WINDOW(win_main),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gchar *data;
        gsize len;
        if (g_file_get_contents(fn, &data, &len, NULL)) {
            gchar *base = g_path_get_basename(fn);
            client_send_file_offer(base, (long)len, NULL);
            const int max_chunk = 2048;
            for (gsize off = 0; off < len; off += max_chunk) {
                gsize chunk = (off + max_chunk < len) ? max_chunk : (len - off);
                gchar *b64 = g_base64_encode((guchar *)data + off, chunk);
                char line[4096 + 256];
                snprintf(line, sizeof(line), "FILE_DATA|%s|%s", base, b64);
                client_send_raw(line);
                g_free(b64);
            }
            char line[256];
            snprintf(line, sizeof(line), "FILE_END|%s", base);
            client_send_raw(line);
            g_free(base);
            g_free(data);
        }
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

void ui_init(int *argc, char ***argv) {
    gtk_init(argc, argv);
    GtkCssProvider *p = gtk_css_provider_new();
    const char *css =
        "window { background-color: #1e1e1e; }"
        "#sidebar { background-color: #252526; border-right: 1px solid #333333; }"
        "#chatview { background-color: #1e1e1e; color: #e0e0e0; }"
        "#chatview text { background-color: #1e1e1e; color: #e0e0e0; }"
        "entry { background-color: #3c3c3c; color: #ffffff; border: 1px solid #555555; }"
        "entry:focus { border-color: #0e639c; }"
        "button { background-color: #0e639c; color: #ffffff; border: none; padding: 6px 12px; }"
        "button:hover { background-color: #1177bb; }"
        "label { color: #cccccc; }"
        "list { background-color: #252526; }"
        "list row { color: #cccccc; padding: 4px 8px; }"
        "list row:selected { background-color: #0e639c; color: #ffffff; }"
        ".status-label { color: #ff8a65; font-style: italic; }";
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

void ui_show_login_window(void) {
    if (win_login) return;
    win_login = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win_login), "ConnectHub - Login");
    gtk_window_set_default_size(GTK_WINDOW(win_login), 320, 260);
    gtk_window_set_resizable(GTK_WINDOW(win_login), FALSE);
    g_signal_connect(win_login, "destroy", G_CALLBACK(on_window_closed), NULL);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(v), 24);
    gtk_container_add(GTK_CONTAINER(win_login), v);

    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), "<big><b>ConnectHub</b></big>");
    gtk_box_pack_start(GTK_BOX(v), l, FALSE, FALSE, 0);

    entry_login = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_login), "Username");
    gtk_box_pack_start(GTK_BOX(v), entry_login, FALSE, FALSE, 0);

    entry_host = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_host), server_host);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_host), "Host (e.g. 127.0.0.1)");
    gtk_box_pack_start(GTK_BOX(v), entry_host, FALSE, FALSE, 0);

    entry_port = gtk_entry_new();
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", server_port);
    gtk_entry_set_text(GTK_ENTRY(entry_port), port_buf);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_port), "Port");
    gtk_box_pack_start(GTK_BOX(v), entry_port, FALSE, FALSE, 0);

    lbl_status = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_status), "status-label");
    gtk_box_pack_start(GTK_BOX(v), lbl_status, FALSE, FALSE, 0);

    GtkWidget *b = gtk_button_new_with_label("Connect");
    g_signal_connect(b, "clicked", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_login, "activate", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_host, "activate", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_port, "activate", G_CALLBACK(on_login), NULL);
    gtk_box_pack_start(GTK_BOX(v), b, FALSE, FALSE, 0);

    gtk_widget_show_all(win_login);
}

void ui_show_login_error(const char *r) {
    if (lbl_status) gtk_label_set_text(GTK_LABEL(lbl_status), r ? r : "Login failed");
}

void ui_set_username(const char *name) {
    if (name) {
        strncpy(current_user, name, sizeof(current_user) - 1);
        current_user[sizeof(current_user) - 1] = '\0';
    }
}

void ui_show_main_window(void) {
    if (win_main) {
        gtk_widget_show_all(win_main);
        if (win_login) gtk_widget_hide(win_login);
        return;
    }
    if (win_login) gtk_widget_hide(win_login);

    win_main = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[256];
    snprintf(title, sizeof(title), "ConnectHub - %s", current_user);
    gtk_window_set_title(GTK_WINDOW(win_main), title);
    gtk_window_set_default_size(GTK_WINDOW(win_main), 950, 700);
    g_signal_connect(win_main, "destroy", G_CALLBACK(on_window_closed), NULL);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(win_main), paned);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_name(side, "sidebar");
    gtk_widget_set_size_request(side, 200, -1);
    gtk_paned_pack1(GTK_PANED(paned), side, FALSE, FALSE);

    GtkWidget *lbl_brand = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_brand), "<big><b>ConnectHub</b></big>");
    gtk_box_pack_start(GTK_BOX(side), lbl_brand, FALSE, FALSE, 5);

    gtk_box_pack_start(GTK_BOX(side), gtk_label_new("Online Users"), FALSE, FALSE, 2);
    lst_users = gtk_list_box_new();
    gtk_box_pack_start(GTK_BOX(side), lst_users, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(side), gtk_label_new("Rooms"), FALSE, FALSE, 2);
    lst_rooms = gtk_list_box_new();
    gtk_box_pack_start(GTK_BOX(side), lst_rooms, TRUE, TRUE, 0);

    if (strcmp(current_user, "admin") == 0) {
        gtk_box_pack_start(GTK_BOX(side), gtk_label_new("Admin: /announce or /kick"), FALSE, FALSE, 2);
    }

    GtkWidget *chat = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_paned_pack2(GTK_PANED(paned), chat, TRUE, FALSE);

    lbl_room = gtk_label_new("Room: general");
    gtk_box_pack_start(GTK_BOX(chat), lbl_room, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(sw, TRUE);
    tv_chat = gtk_text_view_new();
    gtk_widget_set_name(tv_chat, "chatview");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv_chat), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv_chat), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv_chat), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), tv_chat);
    gtk_box_pack_start(GTK_BOX(chat), sw, TRUE, TRUE, 0);

    buf_chat = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv_chat));

    lbl_typing = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(chat), lbl_typing, FALSE, FALSE, 0);

    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(chat), h, FALSE, FALSE, 0);

    entry_chat = gtk_entry_new();
    gtk_widget_set_hexpand(entry_chat, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), "Type a message... (try /msg user text)");
    g_signal_connect(entry_chat, "activate", G_CALLBACK(on_send), NULL);
    gtk_box_pack_start(GTK_BOX(h), entry_chat, TRUE, TRUE, 0);

    GtkWidget *btn_send = gtk_button_new_with_label("Send");
    g_signal_connect(btn_send, "clicked", G_CALLBACK(on_send), NULL);
    gtk_box_pack_start(GTK_BOX(h), btn_send, FALSE, FALSE, 0);

    GtkWidget *btn_file = gtk_button_new_with_label("File");
    g_signal_connect(btn_file, "clicked", G_CALLBACK(on_file), NULL);
    gtk_box_pack_start(GTK_BOX(h), btn_file, FALSE, FALSE, 0);

    gtk_widget_show_all(win_main);
    client_send_raw("LIST_USERS");
    client_send_raw("LIST_ROOMS");
}

void ui_append_message(const char *room, const char *sender, const char *text, const char *ts) {
    (void)room;
    if (!buf_chat) return;
    char *esc_s = esc_markup(sender);
    char *esc_t = esc_markup(text);
    char *m = g_strdup_printf(
        "<b><span color=\"#90caf9\">%s</span></b>: "
        "<span color=\"#e0e0e0\">%s</span> "
        "<span color=\"#888888\" size=\"small\">%s</span>\n",
        esc_s, esc_t, ts ? ts : "");
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf_chat, &iter);
    gtk_text_buffer_insert_markup(buf_chat, &iter, m, -1);
    g_free(m); g_free(esc_s); g_free(esc_t);
    scroll_chat_to_bottom();
}

void ui_append_private_message(const char *sender, const char *text, const char *ts) {
    if (!buf_chat) return;
    char *esc_s = esc_markup(sender);
    char *esc_t = esc_markup(text);
    char *m = g_strdup_printf(
        "<b><span color=\"#ce93d8\">[PM] %s</span></b>: "
        "<span color=\"#e0e0e0\">%s</span> "
        "<span color=\"#888888\" size=\"small\">%s</span>\n",
        esc_s, esc_t, ts ? ts : "");
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf_chat, &iter);
    gtk_text_buffer_insert_markup(buf_chat, &iter, m, -1);
    g_free(m); g_free(esc_s); g_free(esc_t);
    scroll_chat_to_bottom();
}

void ui_append_announcement(const char *text, const char *ts) {
    if (!buf_chat) return;
    char *esc = esc_markup(text);
    char *m = g_strdup_printf(
        "<b><span color=\"#ffd54f\">[Announcement] %s</span></b> "
        "<span color=\"#888888\" size=\"small\">%s</span>\n",
        esc, ts ? ts : "");
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf_chat, &iter);
    gtk_text_buffer_insert_markup(buf_chat, &iter, m, -1);
    g_free(m); g_free(esc);
    scroll_chat_to_bottom();
}

static void update_list(GtkWidget *list, const char *csv) {
    GList *c = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = c; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(c);
    if (!csv || !csv[0]) return;
    char *copy = g_strdup(csv);
    char *sp = NULL;
    for (char *tok = strtok_r(copy, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
        char *s = strchr(tok, ':');
        if (s) *s = 0;
        if (tok[0]) {
            GtkWidget *lbl = gtk_label_new(tok);
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_list_box_prepend(GTK_LIST_BOX(list), lbl);
        }
    }
    g_free(copy);
    gtk_widget_show_all(list);
}

void ui_update_user_list(const char *csv, int count) {
    (void)count;
    if (!lst_users) return;
    update_list(lst_users, csv);
}

void ui_update_room_list(const char *csv, int count) {
    (void)count;
    if (!lst_rooms) return;
    update_list(lst_rooms, csv);
}

void ui_add_notification(const char *text) {
    if (!buf_chat) return;
    char *esc = esc_markup(text);
    char *m = g_strdup_printf("<span color=\"#ff8a65\">* %s *</span>\n", esc);
    g_free(esc);
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf_chat, &iter);
    gtk_text_buffer_insert_markup(buf_chat, &iter, m, -1);
    g_free(m);
    scroll_chat_to_bottom();
}

void ui_show_typing(const char *room, const char *user) {
    (void)room;
    if (!lbl_typing || !user) return;
    if (typing_tag) { g_source_remove(typing_tag); typing_tag = 0; }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s is typing...", user);
    gtk_label_set_text(GTK_LABEL(lbl_typing), buf);
    typing_tag = g_timeout_add_seconds(3, typing_cb, NULL);
}

void ui_run(void) { gtk_main(); }

void ui_cleanup(void) {
    if (win_login) gtk_widget_destroy(win_login);
    if (win_main) gtk_widget_destroy(win_main);
}
