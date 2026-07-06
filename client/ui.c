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

static GtkTextTag *tag_sent = NULL;
static GtkTextTag *tag_received = NULL;
static GtkTextTag *tag_private = NULL;
static GtkTextTag *tag_announce = NULL;
static GtkTextTag *tag_notify = NULL;
static GtkTextTag *tag_ts = NULL;
static GtkTextTag *tag_name = NULL;

static char current_user[128] = "user";
static char current_room[64] = "general";
static char server_host[128] = "127.0.0.1";
static int server_port = 8080;

static guint typing_tag = 0;

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
                snprintf(b, sizeof(b), "  %s  ", r);
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
        "window { background-color: #0d1117; }"
        "#sidebar { background-color: #161b22; border-right: 1px solid #21262d; }"
        "#sidebar label { color: #8b949e; font-weight: 600; font-size: 11px; text-transform: uppercase; letter-spacing: 0.5px; padding: 4px 12px 0; }"
        "#chatview { background-color: #0d1117; }"
        "#chatview text { background-color: transparent; color: #c9d1d9; }"
        "entry { background-color: #21262d; color: #c9d1d9; border: 1px solid #30363d; border-radius: 8px; padding: 8px 14px; caret-color: #58a6ff; }"
        "entry:focus { border-color: #58a6ff; }"
        "button { background: linear-gradient(135deg, #238636, #2ea043); color: #ffffff; border: none; border-radius: 8px; padding: 8px 18px; font-weight: 600; }"
        "button:hover { background: linear-gradient(135deg, #2ea043, #3fb950); }"
        "button:active { background: #238636; }"
        "#btn-file { background: #21262d; border: 1px solid #30363d; color: #c9d1d9; padding: 8px 14px; font-weight: 500; }"
        "#btn-file:hover { background: #30363d; }"
        "list { background-color: transparent; }"
        "list row { background-color: transparent; color: #c9d1d9; padding: 5px 12px; border-radius: 6px; }"
        "list row:selected { background-color: #1f6feb33; color: #c9d1d9; }"
        "label { color: #8b949e; }"
        "#lbl-room { background-color: #161b22; color: #c9d1d9; font-size: 14px; font-weight: 600; padding: 8px 16px; border-bottom: 1px solid #21262d; }"
        "#lbl-brand { color: #58a6ff; font-size: 15px; font-weight: 700; padding: 12px; }"
        "#lbl-typing { color: #8b949e; font-style: italic; font-size: 12px; padding: 2px 16px; }"
        ".status-err { color: #f85149; font-style: italic; font-size: 12px; }"
        ".sidebar-title { color: #8b949e; font-weight: 600; font-size: 11px; text-transform: uppercase; letter-spacing: 0.5px; padding: 12px 12px 4px; }";
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

void ui_show_login_window(void) {
    if (win_login) return;
    win_login = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win_login), "ConnectHub");
    gtk_window_set_default_size(GTK_WINDOW(win_login), 380, 400);
    gtk_window_set_resizable(GTK_WINDOW(win_login), FALSE);
    g_signal_connect(win_login, "destroy", G_CALLBACK(on_window_closed), NULL);

    GtkWidget *overlay = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win_login), overlay);

    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(center, 32);
    gtk_widget_set_margin_end(center, 32);
    gtk_widget_set_margin_top(center, 32);
    gtk_widget_set_margin_bottom(center, 32);
    gtk_box_set_spacing(GTK_BOX(center), 16);
    gtk_container_add(GTK_CONTAINER(overlay), center);

    GtkWidget *brand = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(brand), "<span font='24' font_weight='700' color='#58a6ff'>ConnectHub</span>");
    gtk_box_pack_start(GTK_BOX(center), brand, FALSE, FALSE, 0);

    GtkWidget *sub = gtk_label_new("Connect with friends");
    gtk_widget_set_opacity(sub, 0.7);
    gtk_box_pack_start(GTK_BOX(center), sub, FALSE, FALSE, 0);

    entry_login = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_login), "Username");
    gtk_widget_set_margin_top(entry_login, 8);
    gtk_box_pack_start(GTK_BOX(center), entry_login, FALSE, FALSE, 0);

    entry_host = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_host), server_host);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_host), "Host (e.g. 127.0.0.1)");
    gtk_box_pack_start(GTK_BOX(center), entry_host, FALSE, FALSE, 0);

    entry_port = gtk_entry_new();
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", server_port);
    gtk_entry_set_text(GTK_ENTRY(entry_port), port_buf);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_port), "Port");
    gtk_box_pack_start(GTK_BOX(center), entry_port, FALSE, FALSE, 0);

    lbl_status = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_status), "status-err");
    gtk_box_pack_start(GTK_BOX(center), lbl_status, FALSE, FALSE, 0);

    GtkWidget *b = gtk_button_new_with_label("Connect");
    gtk_widget_set_margin_top(b, 8);
    gtk_widget_set_size_request(b, -1, 40);
    g_signal_connect(b, "clicked", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_login, "activate", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_host, "activate", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_port, "activate", G_CALLBACK(on_login), NULL);
    gtk_box_pack_start(GTK_BOX(center), b, FALSE, FALSE, 0);

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
    snprintf(title, sizeof(title), "ConnectHub — %s", current_user);
    gtk_window_set_title(GTK_WINDOW(win_main), title);
    gtk_window_set_default_size(GTK_WINDOW(win_main), 960, 680);
    g_signal_connect(win_main, "destroy", G_CALLBACK(on_window_closed), NULL);

    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(win_main), hpaned);

    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(side, "sidebar");
    gtk_widget_set_size_request(side, 220, -1);
    gtk_paned_pack1(GTK_PANED(hpaned), side, FALSE, FALSE);

    GtkWidget *lbl_brand = gtk_label_new(NULL);
    gtk_widget_set_name(lbl_brand, "lbl-brand");
    gtk_label_set_markup(GTK_LABEL(lbl_brand), "ConnectHub");
    gtk_box_pack_start(GTK_BOX(side), lbl_brand, FALSE, FALSE, 0);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_opacity(sep, 0.1);
    gtk_box_pack_start(GTK_BOX(side), sep, FALSE, FALSE, 0);

    GtkWidget *lu = gtk_label_new("Online");
    gtk_style_context_add_class(gtk_widget_get_style_context(lu), "sidebar-title");
    gtk_box_pack_start(GTK_BOX(side), lu, FALSE, FALSE, 0);

    lst_users = gtk_list_box_new();
    gtk_box_pack_start(GTK_BOX(side), lst_users, TRUE, TRUE, 0);

    GtkWidget *lr = gtk_label_new("Rooms");
    gtk_style_context_add_class(gtk_widget_get_style_context(lr), "sidebar-title");
    gtk_box_pack_start(GTK_BOX(side), lr, FALSE, FALSE, 0);

    lst_rooms = gtk_list_box_new();
    gtk_box_pack_start(GTK_BOX(side), lst_rooms, TRUE, TRUE, 0);

    if (strcmp(current_user, "admin") == 0) {
        GtkWidget *admin_lbl = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(admin_lbl), "<span color='#d2a8ff' size='x-small'>/announce or /kick</span>");
        gtk_widget_set_margin_start(admin_lbl, 6);
        gtk_widget_set_margin_end(admin_lbl, 6);
        gtk_widget_set_margin_top(admin_lbl, 6);
        gtk_widget_set_margin_bottom(admin_lbl, 6);
        gtk_box_pack_start(GTK_BOX(side), admin_lbl, FALSE, FALSE, 0);
    }

    GtkWidget *chat_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_pack2(GTK_PANED(hpaned), chat_panel, TRUE, FALSE);

    lbl_room = gtk_label_new("");
    gtk_widget_set_name(lbl_room, "lbl-room");
    char rb[128];
    snprintf(rb, sizeof(rb), "  %s  ", current_room);
    gtk_label_set_text(GTK_LABEL(lbl_room), rb);
    gtk_box_pack_start(GTK_BOX(chat_panel), lbl_room, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_vexpand(sw, TRUE);
    tv_chat = gtk_text_view_new();
    gtk_widget_set_name(tv_chat, "chatview");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv_chat), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv_chat), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv_chat), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv_chat), 16);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv_chat), 16);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv_chat), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(tv_chat), 8);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), tv_chat);
    gtk_box_pack_start(GTK_BOX(chat_panel), sw, TRUE, TRUE, 0);

    buf_chat = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv_chat));

    tag_sent = gtk_text_buffer_create_tag(buf_chat, "sent", NULL);
    tag_received = gtk_text_buffer_create_tag(buf_chat, "received", NULL);
    tag_private = gtk_text_buffer_create_tag(buf_chat, "private", NULL);
    tag_announce = gtk_text_buffer_create_tag(buf_chat, "announce", NULL);
    tag_notify = gtk_text_buffer_create_tag(buf_chat, "notify", NULL);
    tag_ts = gtk_text_buffer_create_tag(buf_chat, "ts", NULL);
    tag_name = gtk_text_buffer_create_tag(buf_chat, "name", NULL);

    lbl_typing = gtk_label_new("");
    gtk_widget_set_name(lbl_typing, "lbl-typing");
    gtk_widget_set_halign(lbl_typing, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(chat_panel), lbl_typing, FALSE, FALSE, 0);

    GtkWidget *input_area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(input_area, 12);
    gtk_widget_set_margin_end(input_area, 12);
    gtk_widget_set_margin_top(input_area, 8);
    gtk_widget_set_margin_bottom(input_area, 12);
    gtk_box_pack_start(GTK_BOX(chat_panel), input_area, FALSE, FALSE, 0);

    GtkWidget *btn_file = gtk_button_new_with_label("+");
    gtk_widget_set_name(btn_file, "btn-file");
    gtk_widget_set_size_request(btn_file, 40, 40);
    g_signal_connect(btn_file, "clicked", G_CALLBACK(on_file), NULL);
    gtk_box_pack_start(GTK_BOX(input_area), btn_file, FALSE, FALSE, 0);

    entry_chat = gtk_entry_new();
    gtk_widget_set_hexpand(entry_chat, TRUE);
    gtk_widget_set_size_request(entry_chat, -1, 40);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), "Type a message... /msg user text");
    g_signal_connect(entry_chat, "activate", G_CALLBACK(on_send), NULL);
    gtk_box_pack_start(GTK_BOX(input_area), entry_chat, TRUE, TRUE, 0);

    GtkWidget *btn_send = gtk_button_new_with_label("Send");
    gtk_widget_set_size_request(btn_send, 80, 40);
    g_signal_connect(btn_send, "clicked", G_CALLBACK(on_send), NULL);
    gtk_box_pack_start(GTK_BOX(input_area), btn_send, FALSE, FALSE, 0);

    gtk_widget_show_all(win_main);
    client_send_raw("LIST_USERS");
    client_send_raw("LIST_ROOMS");
}

static void insert_with_tags(GtkTextBuffer *buf, const char *sender, const char *text, const char *ts, GtkTextTag *main_tag) {
    (void)main_tag;
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf, &iter);

    char ts_display[32] = "";
    if (ts && ts[0]) {
        snprintf(ts_display, sizeof(ts_display), "  %s", ts);
    }

    bool is_own = (sender && current_user[0] && strcmp(sender, current_user) == 0);
    const char *display_sender = is_own ? "You" : sender;

    char *name_markup = g_markup_printf_escaped("<span color='%s' font_weight='600'>%s</span>",
        is_own ? "#3fb950" : "#58a6ff", display_sender);
    char *text_escaped = g_markup_escape_text(text, -1);
    char *ts_escaped = g_markup_escape_text(ts_display, -1);

    char *full = g_strdup_printf("%s  %s  <span color='#484f58' size='small'>%s</span>",
        name_markup, text_escaped, ts_escaped);

    if (is_own) {
        gtk_text_buffer_insert_with_tags_by_name(buf, &iter, "     ", -1, "ts", NULL);
        gtk_text_buffer_get_end_iter(buf, &iter);
    }
    gtk_text_buffer_insert_markup(buf, &iter, full, -1);

    g_free(name_markup);
    g_free(text_escaped);
    g_free(ts_escaped);
    g_free(full);

    gtk_text_buffer_get_end_iter(buf, &iter);
    gtk_text_buffer_insert(buf, &iter, "\n", 1);
    scroll_chat_to_bottom();
}

void ui_append_message(const char *room, const char *sender, const char *text, const char *ts) {
    (void)room;
    if (!buf_chat) return;
    bool is_own = (sender && current_user[0] && strcmp(sender, current_user) == 0);
    insert_with_tags(buf_chat, sender, text, ts, is_own ? tag_sent : tag_received);
}

void ui_append_private_message(const char *sender, const char *text, const char *ts) {
    if (!buf_chat) return;
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf_chat, &iter);

    char ts_display[32] = "";
    if (ts && ts[0]) {
        snprintf(ts_display, sizeof(ts_display), "  %s", ts);
    }

    bool is_own = (sender && current_user[0] && strcmp(sender, current_user) == 0);
    const char *label = is_own ? "You → " : "";
    const char *pm_label = is_own ? "" : "PM → ";

    char *full;
    if (is_own) {
        full = g_strdup_printf("<span color='#d2a8ff' font_weight='600'>%s%s</span><span color='#e0e0f0'>  %s</span>  <span color='#484f58' size='small'>%s</span>",
            label, sender, text, ts_display);
    } else {
        full = g_strdup_printf("<span color='#d2a8ff' font_weight='600'>%s%s</span><span color='#e0e0f0'>  %s</span>  <span color='#484f58' size='small'>%s</span>",
            pm_label, sender, text, ts_display);
    }

    gtk_text_buffer_insert_markup(buf_chat, &iter, full, -1);
    gtk_text_buffer_insert(buf_chat, &iter, "\n", 1);
    g_free(full);
    scroll_chat_to_bottom();
}

void ui_append_announcement(const char *text, const char *ts) {
    if (!buf_chat) return;
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf_chat, &iter);

    char ts_display[32] = "";
    if (ts && ts[0]) {
        snprintf(ts_display, sizeof(ts_display), "  %s", ts);
    }

    char *esc = g_markup_escape_text(text ? text : "", -1);
    char *full = g_strdup_printf("<span color='#d29922' font_weight='600'>📢</span>  <span color='#c9d1d9'>%s</span><span color='#484f58' size='small'>%s</span>",
        esc, ts_display);
    gtk_text_buffer_insert_markup(buf_chat, &iter, full, -1);
    gtk_text_buffer_insert(buf_chat, &iter, "\n", 1);
    g_free(esc);
    g_free(full);
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
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_set_margin_start(row, 4);
            gtk_widget_set_margin_end(row, 4);
            gtk_widget_set_margin_top(row, 2);
            gtk_widget_set_margin_bottom(row, 2);

            GtkWidget *dot = gtk_label_new("●");
            gtk_label_set_markup(GTK_LABEL(dot), "<span color='#3fb950' size='x-small'>●</span>");
            gtk_box_pack_start(GTK_BOX(row), dot, FALSE, FALSE, 0);

            GtkWidget *lbl = gtk_label_new(tok);
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

            GtkWidget *list_row = gtk_list_box_row_new();
            gtk_container_add(GTK_CONTAINER(list_row), row);
            gtk_list_box_prepend(GTK_LIST_BOX(list), list_row);
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

static void update_room_list(GtkWidget *list, const char *csv) {
    GList *c = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = c; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(c);
    if (!csv || !csv[0]) return;
    char *copy = g_strdup(csv);
    char *sp = NULL;
    for (char *tok = strtok_r(copy, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
        if (tok[0]) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_set_margin_start(row, 4);
            gtk_widget_set_margin_end(row, 4);
            gtk_widget_set_margin_top(row, 2);
            gtk_widget_set_margin_bottom(row, 2);

            GtkWidget *hash = gtk_label_new("#");
            gtk_label_set_markup(GTK_LABEL(hash), "<span color='#484f58' font_weight='700'>#</span>");
            gtk_box_pack_start(GTK_BOX(row), hash, FALSE, FALSE, 0);

            GtkWidget *lbl = gtk_label_new(tok);
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

            GtkWidget *list_row = gtk_list_box_row_new();
            gtk_container_add(GTK_CONTAINER(list_row), row);
            gtk_list_box_prepend(GTK_LIST_BOX(list), list_row);
        }
    }
    g_free(copy);
    gtk_widget_show_all(list);
}

void ui_update_room_list(const char *csv, int count) {
    (void)count;
    if (!lst_rooms) return;
    update_room_list(lst_rooms, csv);
}

void ui_add_notification(const char *text) {
    if (!buf_chat || !text) return;
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(buf_chat, &iter);

    char *esc = g_markup_escape_text(text, -1);
    char *m = g_strdup_printf("<span color='#8b949e' font_style='italic'>  %s  </span>\n", esc);
    g_free(esc);
    gtk_text_buffer_insert_markup(buf_chat, &iter, m, -1);
    g_free(m);
    scroll_chat_to_bottom();
}

void ui_show_typing(const char *room, const char *user) {
    (void)room;
    if (!lbl_typing || !user) return;
    if (typing_tag) { g_source_remove(typing_tag); typing_tag = 0; }
    char buf[256];
    snprintf(buf, sizeof(buf), "  %s is typing...", user);
    gtk_label_set_text(GTK_LABEL(lbl_typing), buf);
    typing_tag = g_timeout_add_seconds(3, typing_cb, NULL);
}

void ui_run(void) { gtk_main(); }

void ui_cleanup(void) {
    if (win_login) gtk_widget_destroy(win_login);
    if (win_main) gtk_widget_destroy(win_main);
}
