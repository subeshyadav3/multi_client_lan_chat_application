#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "ui.h"
#include "chat.h"
#include "../shared/constants.h"

/* ── state ── */
static GtkWidget *win_login = NULL;
static GtkWidget *entry_login = NULL;
static GtkWidget *entry_pass = NULL;
static GtkWidget *entry_host = NULL;
static GtkWidget *entry_port = NULL;
static GtkWidget *lbl_status = NULL;

static GtkWidget *win_main = NULL;
static GtkWidget *lst_chat = NULL;
static GtkWidget *entry_chat = NULL;
static GtkWidget *lbl_typing = NULL;
static GtkWidget *lst_users = NULL;
static GtkWidget *lst_rooms = NULL;
static GtkWidget *lst_files = NULL;
static GtkWidget *lbl_room_header = NULL;
static GtkWidget *btn_back = NULL;
static GtkWidget *btn_create_room = NULL;
static GtkWidget *box_chat_area = NULL;
static GtkScrolledWindow *sw_chat = NULL;

static char current_user[128] = "user";
static char current_room[64] = "general";
static char server_host[128] = "127.0.0.1";
static int server_port = 8080;

/* PM mode */
static bool pm_mode = false;
static char pm_target[64] = "";

/* ── message history ── */
typedef struct {
    gchar *conversation;
    gchar *type;
    gchar *sender;
    gchar *text;
    gchar *ts;
    gchar *extra;
    gchar *filepath;
} MsgEntry;

static GList *msg_history = NULL;
static guint typing_tag = 0;
static GList *received_files = NULL;

/* ── incoming file transfers ── */
typedef struct PendingFile {
    char filename[MAX_FILENAME];
    char sender[MAX_USERNAME];
    long size;
    long received;
    char tmp_path[512];
    char final_path[512];
    bool accepted;
    struct PendingFile *next;
} PendingFile;

static PendingFile *pending_files = NULL;
static pthread_mutex_t pending_files_mutex = PTHREAD_MUTEX_INITIALIZER;

static PendingFile *pending_find(const char *filename) {
    pthread_mutex_lock(&pending_files_mutex);
    for (PendingFile *pf = pending_files; pf; pf = pf->next) {
        if (strcmp(pf->filename, filename) == 0) {
            pthread_mutex_unlock(&pending_files_mutex);
            return pf;
        }
    }
    pthread_mutex_unlock(&pending_files_mutex);
    return NULL;
}

static PendingFile *pending_add(const char *sender, const char *filename, long size) {
    pthread_mutex_lock(&pending_files_mutex);
    PendingFile *pf = calloc(1, sizeof(PendingFile));
    if (pf) {
        strncpy(pf->sender, sender, MAX_USERNAME - 1);
        strncpy(pf->filename, filename, MAX_FILENAME - 1);
        pf->size = size;
        snprintf(pf->tmp_path, sizeof(pf->tmp_path), "files/%s.tmp", filename);
        snprintf(pf->final_path, sizeof(pf->final_path), "files/%s", filename);
        pf->accepted = false;
        pf->next = pending_files;
        pending_files = pf;
    }
    pthread_mutex_unlock(&pending_files_mutex);
    return pf;
}

static void pending_remove(const char *filename) {
    pthread_mutex_lock(&pending_files_mutex);
    PendingFile **pp = &pending_files;
    while (*pp) {
        if (strcmp((*pp)->filename, filename) == 0) {
            PendingFile *tmp = *pp;
            *pp = (*pp)->next;
            free(tmp);
            break;
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&pending_files_mutex);
}

static void make_unique_path(char *dst, size_t dst_len, const char *base) {
    if (access(base, F_OK) != 0) {
        strncpy(dst, base, dst_len - 1);
        dst[dst_len - 1] = '\0';
        return;
    }
    const char *dot = strrchr(base, '.');
    char stem[256], ext[64];
    if (dot) {
        size_t stem_len = dot - base;
        if (stem_len >= sizeof(stem)) stem_len = sizeof(stem) - 1;
        strncpy(stem, base, stem_len);
        stem[stem_len] = '\0';
        strncpy(ext, dot, sizeof(ext) - 1);
        ext[sizeof(ext) - 1] = '\0';
    } else {
        strncpy(stem, base, sizeof(stem) - 1);
        stem[sizeof(stem) - 1] = '\0';
        ext[0] = '\0';
    }
    for (int i = 1; i < 1000; i++) {
        snprintf(dst, dst_len, "%s (%d)%s", stem, i, ext);
        if (access(dst, F_OK) != 0) return;
    }
    snprintf(dst, dst_len, "%s", base);
}


/* ── forward declarations ── */
static void on_open_file(GtkWidget *b, gpointer d);

/* ── helpers ── */
static gchar *make_conv_id(const char *type, const char *room, const char *pm_with) {
    if (strcmp(type, "pm") == 0)
        return g_strdup_printf("pm:%s", pm_with);
    if (strcmp(type, "public") == 0)
        return g_strdup_printf("room:%s", room);
    return g_strdup("global");
}

static bool entry_matches_mode(MsgEntry *e) {
    if (pm_mode && pm_target[0]) {
        return (strcmp(e->type, "pm") == 0 &&
                (strcmp(e->sender, pm_target) == 0 ||
                 (strcmp(e->sender, current_user) == 0 && e->extra && strcmp(e->extra, pm_target) == 0)));
    }
    return (strcmp(e->type, "public") == 0 && g_strcmp0(e->extra, current_room) == 0) ||
           strcmp(e->type, "notify") == 0 ||
           strcmp(e->type, "announce") == 0 ||
           strcmp(e->type, "file") == 0;
}

static void rerender_chat(void) {
    if (!lst_chat) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(lst_chat));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    for (GList *l = msg_history; l; l = l->next) {
        MsgEntry *e = (MsgEntry *)l->data;
        if (!entry_matches_mode(e)) continue;
        GtkWidget *bubble = NULL;

        if (strcmp(e->type, "notify") == 0) {
            bubble = gtk_label_new(NULL);
            char *esc = g_markup_escape_text(e->text, -1);
            char *m = g_strdup_printf("<span color='#8b949e' font_style='italic'>  %s  </span>", esc);
            gtk_label_set_markup(GTK_LABEL(bubble), m);
            gtk_widget_set_margin_start(bubble, 2);
            gtk_widget_set_margin_end(bubble, 2);
            gtk_widget_set_margin_top(bubble, 2);
            gtk_widget_set_margin_bottom(bubble, 2);
            g_free(m); g_free(esc);
        } else if (strcmp(e->type, "announce") == 0) {
            bubble = gtk_label_new(NULL);
            char *esc = g_markup_escape_text(e->text, -1);
            char *m = g_strdup_printf("<span color='#d29922' font_weight='600'>📢</span>  <span color='#c9d1d9'>%s</span>", esc);
            gtk_label_set_markup(GTK_LABEL(bubble), m);
            gtk_widget_set_margin_start(bubble, 4);
            gtk_widget_set_margin_end(bubble, 4);
            gtk_widget_set_margin_top(bubble, 4);
            gtk_widget_set_margin_bottom(bubble, 4);
            g_free(m); g_free(esc);
        } else if (strcmp(e->type, "file") == 0) {
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_start(box, 12);
            gtk_widget_set_margin_end(box, 12);
            gtk_widget_set_margin_top(box, 4);
            gtk_widget_set_margin_bottom(box, 4);

            GtkWidget *icon = gtk_label_new("📎");
            gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

            GtkWidget *info = gtk_label_new(NULL);
            char *esc = g_markup_escape_text(e->text, -1);
            char *m = g_strdup_printf("<span color='#c9d1d9'>%s</span>", esc);
            gtk_label_set_markup(GTK_LABEL(info), m);
            gtk_box_pack_start(GTK_BOX(box), info, TRUE, TRUE, 0);

            if (e->filepath) {
                GtkWidget *btn_open = gtk_button_new_with_label("Open");
                g_object_set_data_full(G_OBJECT(btn_open), "filepath", g_strdup(e->filepath), g_free);
                g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_file), NULL);
                gtk_box_pack_start(GTK_BOX(box), btn_open, FALSE, FALSE, 0);
            }

            bubble = box;
        } else {
            bool is_own = (strcmp(e->sender, current_user) == 0);
            bool is_pm = (strcmp(e->type, "pm") == 0);

            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_set_margin_start(box, 8);
            gtk_widget_set_margin_end(box, 8);
            gtk_widget_set_margin_top(box, 3);
            gtk_widget_set_margin_bottom(box, 3);

            if (is_own) gtk_widget_set_halign(box, GTK_ALIGN_END);

            /* avatar circle */
            GtkWidget *avatar = gtk_label_new(NULL);
            char initial[2] = {toupper(e->sender[0]), 0};
            char *am = g_strdup_printf("<span font='10' font_weight='700' color='#ffffff'>%s</span>", initial);
            gtk_label_set_markup(GTK_LABEL(avatar), am);
            g_free(am);
            gtk_widget_set_size_request(avatar, 32, 32);
            gtk_widget_set_valign(avatar, GTK_ALIGN_END);
            gtk_widget_set_margin_bottom(avatar, 2);
            char css[128];
            snprintf(css, sizeof(css), "#avatar { background: %s; border-radius: 16px; padding: 4px; }",
                is_pm ? "#7c3aed" : (is_own ? "#1f6feb" : "#30363d"));
            gtk_widget_set_name(avatar, "avatar");

            GtkWidget *bubble_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
            GtkWidget *header_lbl = gtk_label_new(NULL);
            char *s_esc = g_markup_escape_text(is_own ? "You" : e->sender, -1);
            char *hdr;
            if (is_pm)
                hdr = g_strdup_printf("<span color='%s' font='11px' font_weight='600'>🔒 %s</span>",
                    is_own ? "#3fb950" : "#d2a8ff", s_esc);
            else
                hdr = g_strdup_printf("<span color='%s' font='11px' font_weight='600'>%s</span>",
                    is_own ? "#3fb950" : "#58a6ff", s_esc);
            gtk_label_set_markup(GTK_LABEL(header_lbl), hdr);
            gtk_widget_set_halign(header_lbl, GTK_ALIGN_START);
            g_free(hdr); g_free(s_esc);

            GtkWidget *text_lbl = gtk_label_new(NULL);
            char *t_esc = g_markup_escape_text(e->text, -1);
            char *ts_esc = g_markup_escape_text(e->ts ? e->ts : "", -1);
            char *txt = g_strdup_printf("<span color='#e6edf3' font='13px'>%s</span>  <span color='#484f58' font='10px'>%s</span>",
                t_esc, ts_esc);
            gtk_label_set_markup(GTK_LABEL(text_lbl), txt);
            gtk_label_set_line_wrap(GTK_LABEL(text_lbl), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(text_lbl), 60);
            gtk_widget_set_halign(text_lbl, GTK_ALIGN_START);
            g_free(txt); g_free(t_esc); g_free(ts_esc);

            gtk_box_pack_start(GTK_BOX(bubble_box), header_lbl, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(bubble_box), text_lbl, FALSE, FALSE, 0);

            gtk_box_pack_start(GTK_BOX(box), avatar, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(box), bubble_box, FALSE, FALSE, 0);

            /* bubble background */
            GtkWidget *ev = gtk_event_box_new();
            gtk_container_add(GTK_CONTAINER(ev), box);
            gtk_widget_set_name(ev, is_pm ? "bubble-pm" : (is_own ? "bubble-sent" : "bubble-recv"));
            bubble = ev;
        }

        if (bubble) {
            gtk_list_box_prepend(GTK_LIST_BOX(lst_chat), bubble);
        }
    }
    gtk_widget_show_all(GTK_WIDGET(lst_chat));

    /* scroll to bottom */
    if (sw_chat) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(sw_chat);
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
    }
}

static void add_msg_entry(const char *conv, const char *type, const char *sender,
                          const char *text, const char *ts, const char *extra, const char *filepath) {
    MsgEntry *e = g_new(MsgEntry, 1);
    e->conversation = g_strdup(conv);
    e->type = g_strdup(type);
    e->sender = g_strdup(sender ? sender : "");
    e->text = g_strdup(text ? text : "");
    e->ts = g_strdup(ts ? ts : "");
    e->extra = g_strdup(extra ? extra : "");
    e->filepath = g_strdup(filepath ? filepath : "");
    msg_history = g_list_append(msg_history, e);
    rerender_chat();
}

/* ── GTK helpers ── */
static void on_open_file(GtkWidget *b, gpointer d) {
    (void)d;
    const char *fp = (const char *)g_object_get_data(G_OBJECT(b), "filepath");
    if (fp) {
        char *uri = g_filename_to_uri(fp, NULL, NULL);
        if (uri) {
            GError *err = NULL;
            gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, &err);
            g_free(uri);
            if (err) g_error_free(err);
        }
    }
}

static gboolean typing_cb(gpointer d) {
    (void)d; typing_tag = 0;
    if (lbl_typing) gtk_label_set_text(GTK_LABEL(lbl_typing), "");
    return FALSE;
}

static void on_window_closed(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    client_disconnect();
    if (g_main_context_is_owner(NULL) && gtk_main_level() > 0)
        gtk_main_quit();
}

/* ── login ── */
static void on_login(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    const char *u = gtk_entry_get_text(GTK_ENTRY(entry_login));
    if (!u || !u[0]) { gtk_label_set_text(GTK_LABEL(lbl_status), "Enter username"); return; }
    if (strlen(u) >= 32) { gtk_label_set_text(GTK_LABEL(lbl_status), "Username too long"); return; }

    const char *host = gtk_entry_get_text(GTK_ENTRY(entry_host));
    const char *pt = gtk_entry_get_text(GTK_ENTRY(entry_port));
    if (host && host[0]) { strncpy(server_host, host, sizeof(server_host)-1); server_host[sizeof(server_host)-1]=0; }
    int port = server_port;
    if (pt && pt[0]) {
        char *end = NULL; long p = strtol(pt, &end, 10);
        if (*end || p<=0 || p>65535) { gtk_label_set_text(GTK_LABEL(lbl_status), "Invalid port"); return; }
        port = (int)p;
    }
    server_port = port;
    strncpy(current_user, u, sizeof(current_user)-1); current_user[sizeof(current_user)-1]=0;
    if (!client_is_connected() && !client_connect(server_host, server_port))
        { gtk_label_set_text(GTK_LABEL(lbl_status), "Connection failed"); return; }
    const char *pw = entry_pass ? gtk_entry_get_text(GTK_ENTRY(entry_pass)) : "";
    if (!pw || !pw[0]) { gtk_label_set_text(GTK_LABEL(lbl_status), "Enter password"); return; }
    client_send_login_pw(u, pw);
}

/* ── send / commands ── */
static void on_send(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry_chat));
    if (!t || !t[0]) return;

    if (pm_mode && pm_target[0] && t[0] != '/') {
        client_send_private(pm_target, t);
    } else if (t[0] == '/' && strncmp(t, "/msg ", 5) == 0) {
        char to[64], m[2048];
        if (sscanf(t+5, "%63s %2047[^\n]", to, m) == 2) client_send_private(to, m);
    } else if (t[0] == '/' && strncmp(t, "/join ", 6) == 0) {
        char r[64];
        if (sscanf(t+6, "%63s", r) == 1) { char l[128]; snprintf(l,sizeof(l),"JOIN|%s",r); client_send_raw(l);
            strncpy(current_room,r,sizeof(current_room)-1); current_room[sizeof(current_room)-1]=0;
            char b[128]; snprintf(b,sizeof(b),"  #%s  ",r); gtk_label_set_text(GTK_LABEL(lbl_room_header),b); }
    } else if (t[0] == '/' && strncmp(t, "/create ", 8) == 0) {
        char r[64];
        if (sscanf(t+8, "%63s", r) == 1) { char l[128]; snprintf(l,sizeof(l),"CREATE|%s",r); client_send_raw(l); }
    } else if (t[0] == '/' && strncmp(t, "/announce ", 10) == 0 && strcmp(current_user,"admin")==0) {
        char m[2040];
        if (sscanf(t+10,"%2039[^\n]",m)==1) { char l[2064]; snprintf(l,sizeof(l),"ANNOUNCE|%s",m); client_send_raw(l); }
    } else if (t[0] == '/' && strncmp(t, "/kick ", 6) == 0 && strcmp(current_user,"admin")==0) {
        char u[64]={0},r[256]={0};
        if (sscanf(t+6,"%63s %255[^\n]",u,r)>=1) { char l[512]; snprintf(l,sizeof(l),"KICK|%s|%s",u,r); client_send_raw(l); }
    } else if (t[0] == '/' && strncmp(t, "/createuser ", 12) == 0 && strcmp(current_user,"admin")==0) {
        char u[64]={0},p[128]={0};
        if (sscanf(t+12,"%63s %127s",u,p)==2) { char l[256]; snprintf(l,sizeof(l),"CREATE_USER|%s|%s",u,p); client_send_raw(l); }
    } else if (t[0] == '/' && strncmp(t, "/deleteuser ", 12) == 0 && strcmp(current_user,"admin")==0) {
        char u[64]={0};
        if (sscanf(t+12,"%63s",u)==1) { char l[128]; snprintf(l,sizeof(l),"DELETE_USER|%s",u); client_send_raw(l); }
    } else if (t[0] == '/' && strncmp(t, "/resetpass ", 11) == 0 && strcmp(current_user,"admin")==0) {
        char u[64]={0},p[128]={0};
        if (sscanf(t+11,"%63s %127s",u,p)==2) { char l[256]; snprintf(l,sizeof(l),"RESET_PASS|%s|%s",u,p); client_send_raw(l); }
    } else if (strcmp(t, "/listaccounts") == 0 && strcmp(current_user,"admin")==0) {
        client_send_raw("LIST_ACCOUNTS");
    } else {
        client_send_public(current_room, t);
    }
    gtk_entry_set_text(GTK_ENTRY(entry_chat), "");
}

/* ── file send ── */
static void send_file_to(const char *target) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Send File", GTK_WINDOW(win_main),
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",GTK_RESPONSE_CANCEL,"_Open",GTK_RESPONSE_ACCEPT,NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gchar *data; gsize len;
        if (g_file_get_contents(fn, &data, &len, NULL)) {
            gchar *base = g_path_get_basename(fn);
            client_send_file_offer(base, (long)len, target);
            int mxc = 2048;
            for (gsize off=0; off<len; off+=mxc) {
                gsize chk = (off + (gsize)mxc < len) ? (gsize)mxc : (len - off);
                gchar *b64 = g_base64_encode((guchar*)data+off, chk);
                char line[4096+256]; snprintf(line,sizeof(line),"FILE_DATA|%s|%s",base,b64);
                client_send_raw(line); g_free(b64);
            }
            char line[256]; snprintf(line,sizeof(line),"FILE_END|%s",base);
            client_send_raw(line); g_free(base); g_free(data);
        } g_free(fn);
    } gtk_widget_destroy(dlg);
}

static void on_file(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    send_file_to(pm_mode ? pm_target : NULL);
}

/* ── profile dialog ── */
static void open_pm(const char *name) {
    pm_mode = true;
    strncpy(pm_target, name, sizeof(pm_target)-1); pm_target[sizeof(pm_target)-1]=0;
    char b[128]; snprintf(b,sizeof(b),"  ✉ %s  ", name);
    gtk_label_set_text(GTK_LABEL(lbl_room_header), b);
    gtk_widget_set_visible(btn_back, TRUE);
    char ph[128]; snprintf(ph,sizeof(ph),"Message %s...", name);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), ph);
    rerender_chat();
}

static void show_profile(const char *name) {
    if (!name || !name[0] || strcmp(name, current_user) == 0) return;

    GtkWidget *dlg = gtk_dialog_new_with_buttons("Profile",
        GTK_WINDOW(win_main), GTK_DIALOG_MODAL,
        "Send Message", GTK_RESPONSE_ACCEPT,
        "Send File", 100,
        "_Close", GTK_RESPONSE_REJECT, NULL);
    GtkWidget *c = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(c), 24);
    gtk_box_set_spacing(GTK_BOX(c), 12);

    char initial[2] = {toupper(name[0]), 0};
    GtkWidget *avatar = gtk_label_new(NULL);
    char *am = g_strdup_printf("<span font='36px' font_weight='700' color='#ffffff'>%s</span>", initial);
    gtk_label_set_markup(GTK_LABEL(avatar), am);
    g_free(am);
    gtk_widget_set_size_request(avatar, 80, 80);
    gtk_widget_set_halign(avatar, GTK_ALIGN_CENTER);
    GtkWidget *avatar_ev = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(avatar_ev), avatar);
    gtk_widget_set_name(avatar_ev, "avatar-profile");
    gtk_box_pack_start(GTK_BOX(c), avatar_ev, FALSE, FALSE, 0);

    char *nm = g_strdup_printf("<span font='18px' font_weight='600' color='#c9d1d9'>%s</span>", name);
    GtkWidget *nl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(nl), nm);
    g_free(nm);
    gtk_widget_set_halign(nl, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(c), nl, FALSE, FALSE, 0);

    GtkWidget *ol = gtk_label_new("🟢 Online");
    gtk_widget_set_halign(ol, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(c), ol, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_ACCEPT)
        open_pm(name);
    else if (resp == 100)
        send_file_to(name);
    gtk_widget_destroy(dlg);
}

/* ── sidebar clicks ── */
static void on_user_clicked(GtkListBox *box, GtkListBoxRow *row, gpointer d) {
    (void)box; (void)d;
    if (!row) return;
    GtkWidget *w = gtk_bin_get_child(GTK_BIN(row));
    if (!w) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(w));
    GtkWidget *lbl = NULL;
    for (GList *l = children; l; l = l->next) {
        const char *n = gtk_widget_get_name(GTK_WIDGET(l->data));
        if (n && strcmp(n, "uname") == 0) { lbl = GTK_WIDGET(l->data); break; }
    }
    g_list_free(children);
    if (!lbl) return;
    const char *name = gtk_label_get_text(GTK_LABEL(lbl));
    show_profile(name);
}

static void join_room(const char *name, const char *password) {
    pm_mode = false; pm_target[0] = 0;
    strncpy(current_room, name, sizeof(current_room)-1); current_room[sizeof(current_room)-1]=0;
    if (password && password[0]) {
        char cmd[256]; snprintf(cmd,sizeof(cmd),"JOIN|%s|%s",name,password);
        client_send_raw(cmd);
    } else {
        char l[128]; snprintf(l,sizeof(l),"JOIN|%s",name); client_send_raw(l);
    }
    char b[128]; snprintf(b,sizeof(b),"  #%s  ",name); gtk_label_set_text(GTK_LABEL(lbl_room_header),b);
    gtk_widget_set_visible(btn_back, FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), "Type a message...");
    rerender_chat();
}

static void on_room_clicked(GtkListBox *box, GtkListBoxRow *row, gpointer d) {
    (void)box; (void)d;
    if (!row) return;
    GtkWidget *w = gtk_bin_get_child(GTK_BIN(row));
    if (!w) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(w));
    GtkWidget *lbl = NULL;
    for (GList *l = children; l; l = l->next) {
        const char *wn = gtk_widget_get_name(GTK_WIDGET(l->data));
        if (wn && strcmp(wn, "rname") == 0) { lbl = GTK_WIDGET(l->data); break; }
    }
    g_list_free(children);
    if (!lbl) return;
    const char *name = gtk_label_get_text(GTK_LABEL(lbl));
    if (!name || !name[0]) return;

    bool is_protected = g_object_get_data(G_OBJECT(row), "protected") != NULL;

    if (is_protected && strcmp(current_user, "admin") != 0) {
        GtkWidget *dlg = gtk_dialog_new_with_buttons("Room Password",
            GTK_WINDOW(win_main), GTK_DIALOG_MODAL,
            "_Join", GTK_RESPONSE_ACCEPT, "_Cancel", GTK_RESPONSE_REJECT, NULL);
        GtkWidget *ca = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
        gtk_container_set_border_width(GTK_CONTAINER(ca), 16);
        gtk_box_set_spacing(GTK_BOX(ca), 8);
        gtk_box_pack_start(GTK_BOX(ca), gtk_label_new("This room requires a password:"), FALSE,FALSE,0);
        GtkWidget *pw_entry = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(pw_entry), FALSE);
        gtk_entry_set_placeholder_text(GTK_ENTRY(pw_entry), "Password");
        gtk_box_pack_start(GTK_BOX(ca), pw_entry, FALSE,FALSE,0);
        gtk_widget_show_all(dlg);

        if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            const char *pw = gtk_entry_get_text(GTK_ENTRY(pw_entry));
            join_room(name, pw ? pw : "");
        }
        gtk_widget_destroy(dlg);
    } else {
        join_room(name, NULL);
    }
}

static void on_back_to_room(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    pm_mode = false; pm_target[0] = 0;
    gtk_widget_set_visible(btn_back, FALSE);
    char b[128]; snprintf(b,sizeof(b),"  #%s  ",current_room); gtk_label_set_text(GTK_LABEL(lbl_room_header),b);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), "Type a message...");
    rerender_chat();
}

/* ── create room dialog ── */
static void on_create_room(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Create Room", GTK_WINDOW(win_main),
        GTK_DIALOG_MODAL, "_Create",GTK_RESPONSE_ACCEPT,"_Cancel",GTK_RESPONSE_REJECT,NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    GtkWidget *c = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_set_spacing(GTK_BOX(c), 8);
    gtk_container_set_border_width(GTK_CONTAINER(c), 16);

    GtkWidget *e_name = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(e_name), "Room name (e.g. gaming)");
    GtkWidget *e_title = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(e_title), "Display title");
    GtkWidget *e_desc = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(e_desc), "Description");
    GtkWidget *e_pass = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(e_pass), "Password (optional)");
    gtk_entry_set_visibility(GTK_ENTRY(e_pass), FALSE);

    gtk_box_pack_start(GTK_BOX(c), gtk_label_new("Room Name"), FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(c), e_name, FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(c), gtk_label_new("Title"), FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(c), e_title, FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(c), gtk_label_new("Description"), FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(c), e_desc, FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(c), gtk_label_new("Password"), FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(c), e_pass, FALSE,FALSE,0);
    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *rn = gtk_entry_get_text(GTK_ENTRY(e_name));
        const char *rt = gtk_entry_get_text(GTK_ENTRY(e_title));
        const char *rd = gtk_entry_get_text(GTK_ENTRY(e_desc));
        const char *rp = gtk_entry_get_text(GTK_ENTRY(e_pass));
        if (rn && rn[0]) {
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "CREATE_ROOM|%s|%s|%s|%s", rn, rt ? rt : "", rd ? rd : "", rp ? rp : "");
            client_send_raw(cmd);
            ui_add_notification("Room creation requested...");
        }
    }
    gtk_widget_destroy(dlg);
}

/* ── init ── */
void ui_init(int *argc, char ***argv) {
    gtk_init(argc, argv);
    GtkCssProvider *p = gtk_css_provider_new();
    const char *css =
        "window { background-color: #0d1117; }"
        "#sidebar { background-color: #161b22; border-right: 1px solid #21262d; }"
        "#chatview { background-color: #0d1117; }"
        "#chatview text { background-color: transparent; color: #c9d1d9; }"
        "entry { background-color: #21262d; color: #c9d1d9; border: 1px solid #30363d; border-radius: 8px; padding: 8px 14px; caret-color: #58a6ff; }"
        "entry:focus { border-color: #58a6ff; }"
        "button { background: linear-gradient(135deg, #238636, #2ea043); color: #fff; border: none; border-radius: 8px; padding: 8px 18px; font-weight: 600; }"
        "button:hover { background: linear-gradient(135deg, #2ea043, #3fb950); }"
        "button:active { background: #238636; }"
        "#btn-file, #btn-back { background: #21262d; border: 1px solid #30363d; color: #c9d1d9; padding: 8px 14px; font-weight: 500; }"
        "#btn-file:hover, #btn-back:hover { background: #30363d; }"
        "#btn-create-room { background: #1f6feb; border: none; color: #fff; padding: 6px; font-size: 16px; font-weight: 700; }"
        "#btn-create-room:hover { background: #388bfd; }"
        "#bubble-sent { background: rgba(31,111,235,0.2); border-radius: 12px; margin: 2px 60px 2px 8px; }"
        "#bubble-recv { background: rgba(48,54,61,0.6); border-radius: 12px; margin: 2px 8px 2px 60px; }"
        "#bubble-pm { background: rgba(124,58,237,0.15); border-radius: 12px; margin: 2px 8px 2px 60px; }"
        "#sidebar label { color: #8b949e; font-weight: 600; font-size: 11px; letter-spacing: 0.5px; padding: 4px 12px 0; }"
        "list { background-color: transparent; }"
        "list row { background-color: transparent; color: #c9d1d9; padding: 4px 8px; border-radius: 6px; }"
        "list row:selected { background-color: rgba(31,111,235,0.2); }"
        "label { color: #8b949e; }"
        "#lbl-room { background-color: #161b22; color: #c9d1d9; font-size: 14px; font-weight: 600; padding: 8px 12px; border-bottom: 1px solid #21262d; }"
        "#lbl-brand { color: #58a6ff; font-size: 15px; font-weight: 700; padding: 12px; }"
        "#lbl-typing { color: #8b949e; font-style: italic; font-size: 12px; padding: 2px 16px; }"
        ".status-err { color: #f85149; font-style: italic; font-size: 12px; }"
        ".sidebar-title { color: #8b949e; font-weight: 600; font-size: 11px; letter-spacing: 0.5px; padding: 12px 12px 4px; }"
        "#avatar-profile { background: #1f6feb; border-radius: 40px; padding: 8px; }";
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ── login window ── */
void ui_show_login_window(void) {
    if (win_login) return;
    win_login = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win_login), "ConnectHub");
    gtk_window_set_default_size(GTK_WINDOW(win_login), 380, 420);
    gtk_window_set_resizable(GTK_WINDOW(win_login), FALSE);
    g_signal_connect(win_login, "destroy", G_CALLBACK(on_window_closed), NULL);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win_login), v);
    GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(c, GTK_ALIGN_CENTER); gtk_widget_set_halign(c, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(c, 32); gtk_widget_set_margin_end(c, 32);
    gtk_widget_set_margin_top(c, 32); gtk_widget_set_margin_bottom(c, 32);
    gtk_box_set_spacing(GTK_BOX(c), 16);
    gtk_container_add(GTK_CONTAINER(v), c);

    GtkWidget *brand = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(brand), "<span font='24' font_weight='700' color='#58a6ff'>ConnectHub</span>");
    gtk_box_pack_start(GTK_BOX(c), brand, FALSE, FALSE, 0);
    GtkWidget *sub = gtk_label_new("Connect with friends"); gtk_widget_set_opacity(sub, 0.7);
    gtk_box_pack_start(GTK_BOX(c), sub, FALSE, FALSE, 0);

    entry_login = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry_login), "Username");
    gtk_widget_set_margin_top(entry_login, 8);
    gtk_box_pack_start(GTK_BOX(c), entry_login, FALSE, FALSE, 0);
    entry_pass = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry_pass), "Password");
    gtk_entry_set_visibility(GTK_ENTRY(entry_pass), FALSE);
    gtk_box_pack_start(GTK_BOX(c), entry_pass, FALSE, FALSE, 0);
    entry_host = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(entry_host), server_host);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_host), "Host (e.g. 127.0.0.1)");
    gtk_box_pack_start(GTK_BOX(c), entry_host, FALSE, FALSE, 0);
    entry_port = gtk_entry_new(); char pbuf[16]; snprintf(pbuf,sizeof(pbuf),"%d",server_port);
    gtk_entry_set_text(GTK_ENTRY(entry_port), pbuf);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_port), "Port");
    gtk_box_pack_start(GTK_BOX(c), entry_port, FALSE, FALSE, 0);

    lbl_status = gtk_label_new("");
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_status), "status-err");
    gtk_box_pack_start(GTK_BOX(c), lbl_status, FALSE, FALSE, 0);

    GtkWidget *b = gtk_button_new_with_label("Connect");
    gtk_widget_set_margin_top(b, 8); gtk_widget_set_size_request(b, -1, 40);
    g_signal_connect(b,"clicked",G_CALLBACK(on_login),NULL);
    g_signal_connect(entry_login,"activate",G_CALLBACK(on_login),NULL);
    g_signal_connect(entry_host,"activate",G_CALLBACK(on_login),NULL);
    g_signal_connect(entry_port,"activate",G_CALLBACK(on_login),NULL);
    gtk_box_pack_start(GTK_BOX(c), b, FALSE, FALSE, 0);
    gtk_widget_show_all(win_login);
}

void ui_show_login_error(const char *r) {
    if (lbl_status) gtk_label_set_text(GTK_LABEL(lbl_status), r ? r : "Login failed");
}

void ui_set_username(const char *name) {
    if (name) { strncpy(current_user,name,sizeof(current_user)-1); current_user[sizeof(current_user)-1]=0; }
}

/* ── main window ── */
void ui_show_main_window(void) {
    if (win_main) { gtk_widget_show_all(win_main); if (win_login) gtk_widget_hide(win_login); return; }
    if (win_login) gtk_widget_hide(win_login);

    win_main = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[256]; snprintf(title,sizeof(title),"ConnectHub — %s",current_user);
    gtk_window_set_title(GTK_WINDOW(win_main), title);
    gtk_window_set_default_size(GTK_WINDOW(win_main), 960, 680);
    g_signal_connect(win_main, "destroy", G_CALLBACK(on_window_closed), NULL);

    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(win_main), hpaned);

    /* ── sidebar ── */
    GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(side, "sidebar");
    gtk_widget_set_size_request(side, 230, -1);
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
    g_signal_connect(lst_users, "row-activated", G_CALLBACK(on_user_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(side), lst_users, TRUE, TRUE, 0);

    GtkWidget *lr = gtk_label_new("Rooms");
    gtk_style_context_add_class(gtk_widget_get_style_context(lr), "sidebar-title");
    gtk_box_pack_start(GTK_BOX(side), lr, FALSE, FALSE, 0);

    lst_rooms = gtk_list_box_new();
    g_signal_connect(lst_rooms, "row-activated", G_CALLBACK(on_room_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(side), lst_rooms, TRUE, TRUE, 0);

    btn_create_room = gtk_button_new_with_label("+");
    gtk_widget_set_name(btn_create_room, "btn-create-room");
    gtk_widget_set_size_request(btn_create_room, -1, 32);
    g_signal_connect(btn_create_room, "clicked", G_CALLBACK(on_create_room), NULL);
    gtk_box_pack_start(GTK_BOX(side), btn_create_room, FALSE, FALSE, 0);

    GtkWidget *lf = gtk_label_new("Files");
    gtk_style_context_add_class(gtk_widget_get_style_context(lf), "sidebar-title");
    gtk_box_pack_start(GTK_BOX(side), lf, FALSE, FALSE, 0);

    lst_files = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lst_files), GTK_SELECTION_NONE);
    gtk_box_pack_start(GTK_BOX(side), lst_files, TRUE, TRUE, 0);

    if (strcmp(current_user, "admin") == 0) {
        GtkWidget *al = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(al), "<span color='#d2a8ff' size='x-small'>/announce or /kick</span>");
        gtk_widget_set_margin_start(al,6); gtk_widget_set_margin_end(al,6);
        gtk_widget_set_margin_top(al,4); gtk_widget_set_margin_bottom(al,4);
        gtk_box_pack_start(GTK_BOX(side), al, FALSE, FALSE, 0);
    }

    /* ── chat panel ── */
    GtkWidget *chat_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_pack2(GTK_PANED(hpaned), chat_panel, TRUE, FALSE);

    /* header */
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(header_box, "lbl-room");
    gtk_box_pack_start(GTK_BOX(chat_panel), header_box, FALSE, FALSE, 0);

    btn_back = gtk_button_new_with_label("← Back");
    gtk_widget_set_name(btn_back, "btn-back");
    gtk_widget_set_visible(btn_back, FALSE);
    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_to_room), NULL);
    gtk_box_pack_start(GTK_BOX(header_box), btn_back, FALSE, FALSE, 0);

    lbl_room_header = gtk_label_new(NULL);
    char rb[128]; snprintf(rb,sizeof(rb),"  #%s  ",current_room);
    gtk_label_set_text(GTK_LABEL(lbl_room_header), rb);
    gtk_box_pack_start(GTK_BOX(header_box), lbl_room_header, FALSE, FALSE, 0);

    /* chat list */
    box_chat_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(box_chat_area, TRUE);

    sw_chat = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    gtk_scrolled_window_set_policy(sw_chat, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(GTK_WIDGET(sw_chat), TRUE);

    lst_chat = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lst_chat), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(sw_chat), lst_chat);
    gtk_box_pack_start(GTK_BOX(box_chat_area), GTK_WIDGET(sw_chat), TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(chat_panel), box_chat_area, TRUE, TRUE, 0);

    /* typing indicator */
    lbl_typing = gtk_label_new("");
    gtk_widget_set_name(lbl_typing, "lbl-typing");
    gtk_widget_set_halign(lbl_typing, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(chat_panel), lbl_typing, FALSE, FALSE, 0);

    /* input area */
    GtkWidget *input_area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(input_area, 12);
    gtk_widget_set_margin_end(input_area, 12);
    gtk_widget_set_margin_top(input_area, 8);
    gtk_widget_set_margin_bottom(input_area, 12);
    gtk_box_pack_start(GTK_BOX(chat_panel), input_area, FALSE, FALSE, 0);

    GtkWidget *btn_file = gtk_button_new_with_label("📎");
    gtk_widget_set_name(btn_file, "btn-file");
    gtk_widget_set_size_request(btn_file, 40, 40);
    g_signal_connect(btn_file, "clicked", G_CALLBACK(on_file), NULL);
    gtk_box_pack_start(GTK_BOX(input_area), btn_file, FALSE, FALSE, 0);

    entry_chat = gtk_entry_new();
    gtk_widget_set_hexpand(entry_chat, TRUE);
    gtk_widget_set_size_request(entry_chat, -1, 40);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), "Type a message...");
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

/* ── public message ── */
void ui_append_message(const char *room, const char *sender, const char *text, const char *ts) {
    gchar *cid = make_conv_id("public", room, NULL);
    add_msg_entry(cid, "public", sender, text, ts, room, NULL);
    g_free(cid);
}

/* ── private message ── */
void ui_append_private_message(const char *sender, const char *text, const char *ts) {
    bool is_own = (sender && current_user[0] && strcmp(sender, current_user) == 0);
    const char *other = is_own ? pm_target : sender;
    if (!is_own && strcmp(sender, current_user) != 0) other = sender;
    if (!other || !other[0]) other = sender;
    gchar *cid = make_conv_id("pm", NULL, other);
    add_msg_entry(cid, "pm", sender, text, ts, other, NULL);
    g_free(cid);
}

/* ── announcement ── */
void ui_append_announcement(const char *text, const char *ts) {
    add_msg_entry(g_strdup("global"), "announce", "Server", text, ts, "", NULL);
}

/* ── notification ── */
void ui_add_notification(const char *text) {
    add_msg_entry(g_strdup("global"), "notify", "", text, "", "", NULL);
}

/* ── typing ── */
void ui_show_typing(const char *room, const char *user) {
    (void)room;
    if (!lbl_typing || !user) return;
    if (typing_tag) { g_source_remove(typing_tag); typing_tag = 0; }
    char buf[256]; snprintf(buf,sizeof(buf),"  %s is typing...", user);
    gtk_label_set_text(GTK_LABEL(lbl_typing), buf);
    typing_tag = g_timeout_add_seconds(3, typing_cb, NULL);
}

/* ── file received ── */

void ui_show_file_offer(const char *sender, const char *filename, const char *size, const char *target) {
    if (!win_main) return;
    if (!target || !target[0] || strcmp(target, current_user) == 0 || strcmp(target, "all") == 0) {
        /* Only show dialog if this client is the intended recipient or broadcast */
    } else {
        return;
    }

    /* Ignore duplicate offer for same file */
    if (pending_find(filename)) return;

    long fsize = atol(size ? size : "0");
    pending_add(sender, filename, fsize);

    GtkWidget *dlg = gtk_dialog_new_with_buttons("Incoming File",
        GTK_WINDOW(win_main), GTK_DIALOG_MODAL,
        "Accept", GTK_RESPONSE_ACCEPT,
        "Reject", GTK_RESPONSE_REJECT, NULL);
    GtkWidget *c = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(c), 20);
    gtk_box_set_spacing(GTK_BOX(c), 10);

    char title[512];
    snprintf(title, sizeof(title), "<span font='14' font_weight='600' color='#c9d1d9'>%s wants to send you a file</span>", sender);
    GtkWidget *lt = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lt), title);
    gtk_box_pack_start(GTK_BOX(c), lt, FALSE, FALSE, 0);

    char info[512];
    snprintf(info, sizeof(info), "<span color='#8b949e'>%s (%s bytes)</span>", filename, size ? size : "unknown");
    GtkWidget *li = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(li), info);
    gtk_box_pack_start(GTK_BOX(c), li, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    PendingFile *pf = pending_find(filename);
    if (!pf) return;

    if (resp == GTK_RESPONSE_ACCEPT) {
        pf->accepted = true;
        /* Clear any stale tmp file */
        remove(pf->tmp_path);
        client_send_file_accept(sender, filename);
        char buf[512];
        snprintf(buf, sizeof(buf), "Accepting file '%s' from %s", filename, sender);
        ui_add_notification(buf);
    } else {
        client_send_file_reject(sender, filename, "declined by user");
        pending_remove(filename);
        char buf[512];
        snprintf(buf, sizeof(buf), "Declined file '%s' from %s", filename, sender);
        ui_add_notification(buf);
    }
}

void ui_append_file_chunk(const char *filename, const char *base64) {
    PendingFile *pf = pending_find(filename);
    if (!pf || !pf->accepted) return;
    gsize len;
    guchar *decoded = g_base64_decode(base64, &len);
    if (!decoded) return;
    FILE *fp = fopen(pf->tmp_path, "ab");
    if (fp) {
        fwrite(decoded, 1, len, fp);
        fclose(fp);
        pf->received += (long)len;
        if (pf->size > 0) {
            int pct = (int)((pf->received * 100) / pf->size);
            char buf[128];
            snprintf(buf, sizeof(buf), "Receiving '%s': %d%%", filename, pct);
            ui_add_notification(buf);
        }
    }
    g_free(decoded);
}

void ui_finish_file(const char *filename) {
    PendingFile *pf = pending_find(filename);
    if (!pf) return;
    if (!pf->accepted) { pending_remove(filename); return; }

    make_unique_path(pf->final_path, sizeof(pf->final_path), pf->final_path);
    rename(pf->tmp_path, pf->final_path);

    char *fullpath = realpath(pf->final_path, NULL);
    ui_on_file_received(filename, fullpath ? fullpath : pf->final_path);
    if (fullpath) free(fullpath);
    pending_remove(filename);
}

void ui_on_file_rejected(const char *filename, const char *recipient, const char *reason) {
    (void)recipient;
    char buf[512];
    snprintf(buf, sizeof(buf), "File '%s' was rejected: %s", filename, reason && reason[0] ? reason : "no reason");
    ui_add_notification(buf);
}

static void update_files_list(void) {
    if (!lst_files) return;
    GList *c = gtk_container_get_children(GTK_CONTAINER(lst_files));
    for (GList *l = c; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(c);

    for (GList *l = received_files; l; l = l->next) {
        MsgEntry *e = (MsgEntry *)l->data;
        if (!e->filepath || !e->filepath[0]) continue;

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row, 4); gtk_widget_set_margin_end(row, 4);
        gtk_widget_set_margin_top(row, 2); gtk_widget_set_margin_bottom(row, 2);

        GtkWidget *icon = gtk_label_new("📎");
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

        GtkWidget *name = gtk_label_new(e->text ? e->text : e->filepath);
        gtk_widget_set_name(name, "fname");
        gtk_widget_set_halign(name, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);

        GtkWidget *btn_open = gtk_button_new_with_label("Open");
        gtk_widget_set_name(btn_open, "btn-file");
        g_object_set_data_full(G_OBJECT(btn_open), "filepath", g_strdup(e->filepath), g_free);
        g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_file), NULL);
        gtk_box_pack_start(GTK_BOX(row), btn_open, FALSE, FALSE, 0);

        GtkWidget *lr = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(lr), row);
        gtk_list_box_prepend(GTK_LIST_BOX(lst_files), lr);
    }
    gtk_widget_show_all(lst_files);
}

void ui_on_file_received(const char *filename, const char *fullpath) {
    char txt[1024];
    if (fullpath && fullpath[0])
        snprintf(txt, sizeof(txt), "File saved: %s", fullpath);
    else
        snprintf(txt, sizeof(txt), "File received: %s", filename ? filename : "unknown");
    add_msg_entry(g_strdup("global"), "file", "", txt, "", filename ? filename : "", fullpath);

    if (fullpath && fullpath[0]) {
        MsgEntry *e = calloc(1, sizeof(MsgEntry));
        e->conversation = g_strdup("global");
        e->type = g_strdup("file");
        e->sender = g_strdup("");
        e->text = g_strdup(filename ? filename : "file");
        e->ts = g_strdup("");
        e->extra = g_strdup(filename ? filename : "");
        e->filepath = g_strdup(fullpath);
        received_files = g_list_append(received_files, e);
        update_files_list();
    }
}

/* ── list updates ── */
static void update_user_list(GtkWidget *list, const char *csv) {
    GList *c = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = c; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(c);
    if (!csv || !csv[0]) return;
    char *copy = g_strdup(csv); char *sp = NULL;
    for (char *tok = strtok_r(copy, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
        char *s = strchr(tok, ':'); if (s) *s = 0;
        if (!tok[0]) continue;
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row, 4); gtk_widget_set_margin_end(row, 4);
        gtk_widget_set_margin_top(row, 2); gtk_widget_set_margin_bottom(row, 2);

        GtkWidget *dot = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(dot), "<span color='#3fb950' size='x-small'>●</span>");
        gtk_box_pack_start(GTK_BOX(row), dot, FALSE, FALSE, 0);

        GtkWidget *lbl = gtk_label_new(tok);
        gtk_widget_set_name(lbl, "uname");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

        GtkWidget *lr = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(lr), row);
        gtk_list_box_prepend(GTK_LIST_BOX(list), lr);
    }
    g_free(copy);
    gtk_widget_show_all(list);
}

void ui_update_user_list(const char *csv, int count) {
    (void)count;
    if (!lst_users) return;
    update_user_list(lst_users, csv);
}

static void update_room_list(GtkWidget *list, const char *csv) {
    GList *c = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = c; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(c);
    if (!csv || !csv[0]) return;
    char *copy = g_strdup(csv); char *sp = NULL;
    for (char *tok = strtok_r(copy, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
        if (!tok[0]) continue;
        bool is_protected = false;
        char rname_buf[128];
        strncpy(rname_buf, tok, sizeof(rname_buf)-1); rname_buf[sizeof(rname_buf)-1]=0;
        char *p = strstr(rname_buf, ":p");
        if (p) { *p = 0; is_protected = true; }
        if (!rname_buf[0]) continue;

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(row, 4); gtk_widget_set_margin_end(row, 4);
        gtk_widget_set_margin_top(row, 2); gtk_widget_set_margin_bottom(row, 2);

        GtkWidget *hash = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hash), "<span color='#484f58' font_weight='700'>#</span>");
        gtk_box_pack_start(GTK_BOX(row), hash, FALSE, FALSE, 0);

        GtkWidget *lbl = gtk_label_new(rname_buf);
        gtk_widget_set_name(lbl, "rname");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

        if (is_protected) {
            GtkWidget *lock = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(lock), "<span color='#d29922' size='x-small'>🔒</span>");
            gtk_box_pack_start(GTK_BOX(row), lock, FALSE, FALSE, 0);
        }

        GtkWidget *lr = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(lr), row);
        gtk_list_box_prepend(GTK_LIST_BOX(list), lr);

        if (is_protected) {
            g_object_set_data(G_OBJECT(lr), "protected", GINT_TO_POINTER(1));
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

void ui_run(void) { gtk_main(); }
void ui_cleanup(void) {
    if (win_login && GTK_IS_WIDGET(win_login)) gtk_widget_destroy(win_login);
    if (win_main && GTK_IS_WIDGET(win_main)) gtk_widget_destroy(win_main);
    for (GList *l = msg_history; l; l = l->next) {
        MsgEntry *e = (MsgEntry *)l->data;
        if (e) { g_free(e->conversation); g_free(e->type); g_free(e->sender);
                 g_free(e->text); g_free(e->ts); g_free(e->extra); g_free(e->filepath); g_free(e); }
    }
    g_list_free(msg_history); msg_history = NULL;
}
