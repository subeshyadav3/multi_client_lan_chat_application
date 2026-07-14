#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include "ui.h"
#include "chat.h"
#include "../shared/constants.h"

/* ════════════════════════════════════════════════════════════════════════════
 *                              DESIGN TOKENS
 *
 * Color palette — calm, modern dark theme with clear hierarchy:
 *   bg-0      #0d1117  app background
 *   bg-1      #161b22  panels (sidebar, header, footer)
 *   bg-2      #1c2128  inputs, hover
 *   bg-3      #21262d  borders, dividers
 *   bg-4      #2d333b  strong hover, active rows
 *   text-0    #e6edf3  primary text
 *   text-1    #c9d1d9  secondary
 *   text-2    #8b949e  tertiary / labels
 *   text-3    #6e7681  disabled / muted
 *   accent    #58a6ff  primary brand / links
 *   accent-2  #1f6feb  accent deep
 *   success   #3fb950  online / success
 *   warn      #d29922  warning / announcements
 *   danger    #f85149  errors / destructive
 *   violet    #d2a8ff  PM accent
 *
 * Spacing scale:  4, 6, 8, 12, 16, 20, 24
 * Radius scale:   6, 8, 10, 12, 16, 20
 * ════════════════════════════════════════════════════════════════════════════ */

/* ── globals / state ─────────────────────────────────────────────────────── */
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
static GtkWidget *lbl_room_header = NULL;
static GtkWidget *lbl_room_sub = NULL;
static GtkWidget *lbl_statusbar = NULL;
static GtkWidget *lbl_profile_name = NULL;
static GtkWidget *lbl_profile_status = NULL;
static GtkWidget *img_profile_avatar = NULL;
static GtkWidget *lst_users = NULL;
static GtkWidget *lst_rooms = NULL;
static GtkWidget *lst_files = NULL;
static GtkWidget *btn_back = NULL;
static GtkWidget *btn_create_room = NULL;
static GtkWidget *btn_send = NULL;
static GtkWidget *btn_attach = NULL;
static GtkWidget *chat_panel = NULL;
static GtkWidget *sidebar = NULL;
static GtkScrolledWindow *sw_chat = NULL;

static char current_user[128] = "user";
static char current_room[64] = "general";
static char server_host[128] = "127.0.0.1";
static int  server_port = 8080;
static int  current_room_user_count = 0;
static bool connected = false;

/* PM mode */
static bool pm_mode = false;
static char pm_target[64] = "";

/* flag to skip quit during login→main transition */
static bool transitioning_to_main = false;

/* ── message history ── */
typedef struct {
    gchar *conversation;
    gchar *type;       /* public | pm | notify | announce | file | system */
    gchar *sender;
    gchar *text;
    gchar *ts;
    gchar *extra;
    gchar *filepath;
    long   filesize;
} MsgEntry;

static GList *msg_history = NULL;
static GList *received_files = NULL;
static guint  typing_tag = 0;

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

static char resolved_files_dir[1024] = "files";

static void init_files_dir(void) {
    char exe_path[1024] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            last_slash = strrchr(exe_path, '/');
            if (last_slash) {
                *last_slash = '\0';
                snprintf(resolved_files_dir, sizeof(resolved_files_dir), "%s/files", exe_path);
            }
        }
    }
    mkdir(resolved_files_dir, 0755);
    mkdir("files", 0755);
}

static PendingFile *pending_add(const char *sender, const char *filename, long size) {
    pthread_mutex_lock(&pending_files_mutex);
    PendingFile *pf = calloc(1, sizeof(PendingFile));
    if (pf) {
        strncpy(pf->sender, sender, MAX_USERNAME - 1);
        strncpy(pf->filename, filename, MAX_FILENAME - 1);
        pf->size = size;
        snprintf(pf->tmp_path,  sizeof(pf->tmp_path),  "%s/%s.tmp", resolved_files_dir, filename);
        snprintf(pf->final_path, sizeof(pf->final_path), "%s/%s", resolved_files_dir, filename);
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
        if (strcmp(e->type, "pm") != 0) return false;
        return (strcmp(e->sender, pm_target) == 0) ||
               (e->extra && strcmp(e->extra, pm_target) == 0);
    }
    return (strcmp(e->type, "public") == 0 && g_strcmp0(e->extra, current_room) == 0) ||
           strcmp(e->type, "notify") == 0 ||
           strcmp(e->type, "announce") == 0 ||
           strcmp(e->type, "file") == 0 ||
           strcmp(e->type, "system") == 0;
}

/* ── forward declarations ── */
static void on_open_file(GtkWidget *b, gpointer d);
static void on_file_row_clicked(GtkListBox *box, GtkListBoxRow *row, gpointer d);
static void on_user_clicked(GtkListBox *box, GtkListBoxRow *row, gpointer d);
static void on_room_clicked(GtkListBox *box, GtkListBoxRow *row, gpointer d);
static void on_back_to_room(GtkWidget *w, gpointer d);
static void on_create_room(GtkWidget *w, gpointer d);
static gboolean on_entry_keypress(GtkWidget *w, GdkEventKey *e, gpointer d);
static void update_statusbar(void);
static void refresh_profile_panel(void);
static void update_header_for_mode(void);
static void update_files_list(void);

/* ════════════════════════════════════════════════════════════════════════════
 *                          AVATAR & BUBBLE WIDGETS
 * ════════════════════════════════════════════════════════════════════════════ */

/* Deterministic color for a username so the same person always gets the same avatar hue. */
static const char *avatar_color_for(const char *name) {
    if (!name || !name[0]) return "#4c5862";
    static const char *palette[] = {
        "#1f6feb", "#2ea043", "#bf3989", "#bf5d3f", "#8b5cf6",
        "#0e7490", "#c2410c", "#15803d", "#7c3aed", "#be185d",
        "#0369a1", "#a16207", "#475569", "#0891b2", "#6d28d9"
    };
    unsigned h = 0;
    for (const char *p = name; *p; ++p) h = h * 31u + (unsigned char)*p;
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

/* Build a circular colored avatar with the user's initial letter. */
static GtkWidget *make_avatar(const char *name, int size_px) {
    char initial[4] = "?";
    if (name && name[0]) {
        initial[0] = (char)toupper((unsigned char)name[0]);
        initial[1] = 0;
    }
    const char *color = avatar_color_for(name);

    GtkWidget *lbl = gtk_label_new(NULL);
    int font_size = (size_px >= 64) ? 28 : (size_px >= 40 ? 18 : (size_px >= 28 ? 13 : 11));
    char *m = g_strdup_printf(
        "<span font='%d' font_weight='700' color='#ffffff'>%s</span>",
        font_size, initial);
    gtk_label_set_markup(GTK_LABEL(lbl), m);
    g_free(m);

    GtkWidget *ev = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(ev), lbl);
    gtk_widget_set_size_request(ev, size_px, size_px);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);

    char css[256];
    snprintf(css, sizeof(css),
        "GtkEventBox { background: %s; border-radius: %dpx; }"
        "GtkEventBox GtkLabel { background: transparent; }",
        color, size_px / 2);
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(ev),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_unref(p);
    return ev;
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              CHAT RENDERING
 * ════════════════════════════════════════════════════════════════════════════ */

/* Format a timestamp for compact display (HH:MM). */
static void format_time_short(const char *ts, char *out, size_t out_len) {
    if (!ts || !ts[0]) { snprintf(out, out_len, ""); return; }
    /* Input is like "02:30 PM" from server; trim seconds and convert to 24h-ish compact form. */
    int hh = 0, mm = 0;
    char ampm[8] = {0};
    if (sscanf(ts, "%d:%d %7s", &hh, &mm, ampm) == 3) {
        bool pm = (ampm[0] == 'P' || ampm[0] == 'p');
        if (pm && hh < 12) hh += 12;
        if (!pm && hh == 12) hh = 0;
        snprintf(out, out_len, "%02d:%02d", hh, mm);
    } else {
        snprintf(out, out_len, "%s", ts);
    }
}

static void format_filesize(long bytes, char *out, size_t out_len) {
    if (bytes < 1024) snprintf(out, out_len, "%ld B", bytes);
    else if (bytes < 1024 * 1024) snprintf(out, out_len, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024 * 1024) snprintf(out, out_len, "%.1f MB", bytes / (1024.0 * 1024.0));
    else snprintf(out, out_len, "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

/* Returns true if the two messages share the same sender and are within ~5 minutes. */
static bool should_group_with(MsgEntry *prev, MsgEntry *cur) {
    if (!prev || !cur) return false;
    if (strcmp(prev->type, "public") != 0 && strcmp(prev->type, "pm") != 0) return false;
    if (strcmp(cur->type,  "public") != 0 && strcmp(cur->type,  "pm") != 0) return false;
    if (strcmp(prev->sender, cur->sender) != 0) return false;
    return true;
}

/* Build a notification/announcement row (centered pill). */
static GtkWidget *make_pill_row(const char *icon, const char *text, const char *kind) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(wrap, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(wrap, 6);
    gtk_widget_set_margin_bottom(wrap, 6);
    gtk_widget_set_margin_start(wrap, 24);
    gtk_widget_set_margin_end(wrap, 24);

    GtkWidget *ev = gtk_event_box_new();
    gtk_widget_set_name(ev, kind);

    GtkWidget *lbl = gtk_label_new(NULL);
    char *esc = g_markup_escape_text(text, -1);
    char *m = g_strdup_printf(
        "<span font='11px'>%s  <span color='#c9d1d9'>%s</span></span>",
        icon ? icon : "", esc);
    gtk_label_set_markup(GTK_LABEL(lbl), m);
    g_free(m); g_free(esc);
    gtk_widget_set_margin_start(lbl, 10);
    gtk_widget_set_margin_end(lbl, 10);
    gtk_widget_set_margin_top(lbl, 5);
    gtk_widget_set_margin_bottom(lbl, 5);

    gtk_container_add(GTK_CONTAINER(ev), lbl);
    gtk_box_pack_start(GTK_BOX(wrap), ev, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), wrap);
    return row;
}

static GtkWidget *make_file_card(MsgEntry *e) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(card, 380, -1);
    gtk_widget_set_margin_top(card, 4);
    gtk_widget_set_margin_bottom(card, 4);
    gtk_widget_set_margin_start(card, 24);
    gtk_widget_set_margin_end(card, 24);

    GtkWidget *ev = gtk_event_box_new();
    gtk_widget_set_name(ev, "file-card");

    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(inner, 14);
    gtk_widget_set_margin_end(inner, 14);
    gtk_widget_set_margin_top(inner, 12);
    gtk_widget_set_margin_bottom(inner, 12);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ico = gtk_label_new(NULL);
    char *m1 = g_strdup_printf(
        "<span font='18px'>📎</span>  <span font_weight='600' color='#e6edf3'>%s</span>",
        e->sender && e->sender[0] ? e->sender : "File");
    gtk_label_set_markup(GTK_LABEL(ico), m1);
    g_free(m1);
    gtk_box_pack_start(GTK_BOX(head), ico, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(inner), head, FALSE, FALSE, 0);

    GtkWidget *body = gtk_label_new(NULL);
    char *esc = g_markup_escape_text(e->text ? e->text : "", -1);
    char sizebuf[32] = "";
    if (e->filesize > 0) format_filesize(e->filesize, sizebuf, sizeof(sizebuf));
    char *m2 = g_strdup_printf(
        "<span color='#c9d1d9' font='12px'>%s%s%s</span>",
        esc, sizebuf[0] ? "  ·  " : "", sizebuf);
    gtk_label_set_markup(GTK_LABEL(body), m2);
    g_free(m2); g_free(esc);
    gtk_label_set_line_wrap(GTK_LABEL(body), TRUE);
    gtk_widget_set_halign(body, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(body), 0.0f);
    gtk_box_pack_start(GTK_BOX(inner), body, FALSE, FALSE, 0);

    if (e->filepath && e->filepath[0]) {
        GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(btns, GTK_ALIGN_END);
        GtkWidget *btn_open = gtk_button_new_with_label("Open");
        gtk_widget_set_name(btn_open, "btn-secondary");
        g_object_set_data_full(G_OBJECT(btn_open), "filepath", g_strdup(e->filepath), g_free);
        g_signal_connect(btn_open, "clicked", G_CALLBACK(on_open_file), NULL);
        gtk_box_pack_start(GTK_BOX(btns), btn_open, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(inner), btns, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(ev), inner);
    gtk_box_pack_start(GTK_BOX(card), ev, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(row), card);
    return row;
}

static GtkWidget *make_message_row(MsgEntry *e, bool grouped, bool is_last_in_group) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

    bool is_own = (strcmp(e->sender, current_user) == 0);
    bool is_pm  = (strcmp(e->type, "pm") == 0);

    /* Outer horizontal box: [avatar] [content] [spacer] or [spacer] [content] [avatar] */
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(outer, is_last_in_group ? 8 : 1);
    gtk_widget_set_margin_bottom(outer, 0);
    gtk_widget_set_margin_start(outer, 16);
    gtk_widget_set_margin_end(outer, 16);

    /* Avatar (hidden when grouped, except as a spacer) */
    if (!grouped) {
        GtkWidget *av = make_avatar(e->sender, 36);
        if (is_own) gtk_box_pack_end(GTK_BOX(outer), av, FALSE, FALSE, 0);
        else        gtk_box_pack_start(GTK_BOX(outer), av, FALSE, FALSE, 0);
    } else {
        GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(sp, 36, 1);
        if (is_own) gtk_box_pack_end(GTK_BOX(outer), sp, FALSE, FALSE, 0);
        else        gtk_box_pack_start(GTK_BOX(outer), sp, FALSE, FALSE, 0);
    }

    /* Content column: [header line (name + time)] [bubble] */
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    if (is_own) col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    /* Header line */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (!grouped) {
        char tshort[16];
        format_time_short(e->ts, tshort, sizeof(tshort));
        GtkWidget *name_lbl = gtk_label_new(NULL);
        char *n_esc = g_markup_escape_text(e->sender, -1);
        const char *name_color = is_pm ? "#d2a8ff" : (is_own ? "#79c0ff" : "#58a6ff");
        char *nm;
        if (is_own && is_pm)
            nm = g_strdup_printf("<span font_weight='700' color='%s'>You</span> <span color='#6e7681' font='10px'>→ %s</span>",
                name_color, e->extra && e->extra[0] ? e->extra : "?");
        else if (is_own)
            nm = g_strdup_printf("<span font_weight='700' color='%s'>You</span>", name_color);
        else if (is_pm)
            nm = g_strdup_printf("<span font_weight='700' color='%s'>🔒 %s</span>", name_color, n_esc);
        else
            nm = g_strdup_printf("<span font_weight='700' color='%s'>%s</span>", name_color, n_esc);
        gtk_label_set_markup(GTK_LABEL(name_lbl), nm);
        g_free(nm); g_free(n_esc);
        gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(header), name_lbl, FALSE, FALSE, 0);

        GtkWidget *ts_lbl = gtk_label_new(NULL);
        char *tm = g_strdup_printf("<span color='#6e7681' font='10px'>%s</span>", tshort);
        gtk_label_set_markup(GTK_LABEL(ts_lbl), tm);
        g_free(tm);
        gtk_widget_set_halign(ts_lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(header), ts_lbl, FALSE, FALSE, 0);
    }
    if (is_own) gtk_widget_set_halign(header, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(col), header, FALSE, FALSE, 0);

    /* Bubble */
    GtkWidget *ev = gtk_event_box_new();
    gtk_widget_set_name(ev, is_pm ? "bubble-pm" : (is_own ? "bubble-sent" : "bubble-recv"));
    GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *text_lbl = gtk_label_new(NULL);
    char *t_esc = g_markup_escape_text(e->text ? e->text : "", -1);
    char *tm2 = g_strdup_printf("<span color='#e6edf3' font='13px'>%s</span>", t_esc);
    gtk_label_set_markup(GTK_LABEL(text_lbl), tm2);
    g_free(tm2); g_free(t_esc);
    gtk_label_set_line_wrap(GTK_LABEL(text_lbl), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(text_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(text_lbl), 56);
    gtk_label_set_selectable(GTK_LABEL(text_lbl), TRUE);
    gtk_widget_set_halign(text_lbl, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(text_lbl), 0.0f);
    gtk_widget_set_margin_start(text_lbl, 12);
    gtk_widget_set_margin_end(text_lbl, 12);
    gtk_widget_set_margin_top(text_lbl, 8);
    gtk_widget_set_margin_bottom(text_lbl, 8);
    gtk_container_add(GTK_CONTAINER(bubble), text_lbl);
    gtk_container_add(GTK_CONTAINER(ev), bubble);
    gtk_box_pack_start(GTK_BOX(col), ev, FALSE, FALSE, 0);

    if (is_own) {
        gtk_widget_set_halign(col, GTK_ALIGN_END);
        gtk_box_pack_start(GTK_BOX(outer), col, FALSE, FALSE, 0);
    } else {
        gtk_widget_set_halign(col, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(outer), col, FALSE, FALSE, 0);
    }

    /* Spacer to push own messages right, others left */
    if (is_own) {
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(outer), spacer, TRUE, TRUE, 0);
    } else {
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start(GTK_BOX(outer), spacer, TRUE, TRUE, 0);
    }

    gtk_container_add(GTK_CONTAINER(row), outer);
    return row;
}

static void rerender_chat(void) {
    if (!lst_chat) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(lst_chat));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);

    MsgEntry *prev_visible = NULL;
    bool prev_was_separator = true;

    for (GList *l = msg_history; l; l = l->next) {
        MsgEntry *e = (MsgEntry *)l->data;
        if (!entry_matches_mode(e)) continue;

        /* Insert day separator before first visible message of each session, or
           when sender group changes meaningfully. For simplicity, separator every
           20 visible messages so the layout stays calm. */
        GtkWidget *row = NULL;

        if (strcmp(e->type, "notify") == 0) {
            row = make_pill_row("·", e->text, "pill-notify");
            prev_was_separator = true;
            prev_visible = NULL;
        } else if (strcmp(e->type, "announce") == 0) {
            row = make_pill_row("📢", e->text, "pill-announce");
            prev_was_separator = true;
            prev_visible = NULL;
        } else if (strcmp(e->type, "file") == 0) {
            row = make_file_card(e);
            prev_was_separator = true;
            prev_visible = NULL;
        } else {
            bool grouped = !prev_was_separator && should_group_with(prev_visible, e);
            /* Look ahead: is this the last message in its group? */
            bool is_last = true;
            GList *next = g_list_next(l);
            while (next) {
                MsgEntry *nx = (MsgEntry *)next->data;
                if (!entry_matches_mode(nx)) { next = g_list_next(next); continue; }
                if (strcmp(nx->type, "notify") == 0 || strcmp(nx->type, "announce") == 0 ||
                    strcmp(nx->type, "file") == 0) break;
                if (should_group_with(e, nx)) { is_last = false; break; }
                break;
            }
            row = make_message_row(e, grouped, is_last);
            prev_was_separator = false;
            prev_visible = e;
        }

        if (row) {
            gtk_list_box_prepend(GTK_LIST_BOX(lst_chat), row);
        }
    }
    gtk_widget_show_all(GTK_WIDGET(lst_chat));

    /* Scroll to bottom */
    if (sw_chat) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(sw_chat);
        gtk_adjustment_set_value(adj,
            gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
    }
}

static void add_msg_entry(const char *conv, const char *type, const char *sender,
                          const char *text, const char *ts, const char *extra,
                          const char *filepath, long filesize) {
    if (!conv || !type) return;
    MsgEntry *e = g_new(MsgEntry, 1);
    e->conversation = g_strdup(conv);
    e->type        = g_strdup(type);
    e->sender      = g_strdup(sender ? sender : "");
    e->text        = g_strdup(text ? text : "");
    e->ts          = g_strdup(ts ? ts : "");
    e->extra       = g_strdup(extra ? extra : "");
    e->filepath    = g_strdup(filepath ? filepath : "");
    e->filesize    = filesize;
    msg_history = g_list_append(msg_history, e);
    rerender_chat();
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              GTK HELPERS
 * ════════════════════════════════════════════════════════════════════════════ */

static void open_file_by_path(const char *fp) {
    if (!fp || !fp[0]) return;
    char *uri = g_filename_to_uri(fp, NULL, NULL);
    if (uri) {
        GError *err = NULL;
        gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, &err);
        g_free(uri);
        if (err) g_error_free(err);
    }
}

static void on_open_file(GtkWidget *b, gpointer d) {
    (void)d;
    const char *fp = (const char *)g_object_get_data(G_OBJECT(b), "filepath");
    open_file_by_path(fp);
}

static void on_file_row_clicked(GtkListBox *box, GtkListBoxRow *row, gpointer d) {
    (void)box; (void)d;
    if (!row) return;
    GtkWidget *w = gtk_bin_get_child(GTK_BIN(row));
    if (!w) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(w));
    for (GList *l = children; l; l = l->next) {
        GtkWidget *child = GTK_WIDGET(l->data);
        if (GTK_IS_BUTTON(child)) {
            const char *fp = (const char *)g_object_get_data(G_OBJECT(child), "filepath");
            if (fp) { open_file_by_path(fp); break; }
        }
    }
    g_list_free(children);
}

static gboolean typing_cb(gpointer d) {
    (void)d; typing_tag = 0;
    if (lbl_typing) gtk_label_set_text(GTK_LABEL(lbl_typing), "");
    return FALSE;
}

static gboolean on_delete_event(GtkWidget *w, GdkEvent *e, gpointer d) {
    (void)e; (void)d;
    if (transitioning_to_main) return TRUE;
    client_disconnect();
    if (w == win_main)  win_main  = NULL;
    if (w == win_login) win_login = NULL;
    if (g_main_context_is_owner(NULL) && gtk_main_level() > 0)
        gtk_main_quit();
    return TRUE;
}

static void on_window_closed(GtkWidget *w, gpointer d) {
    (void)d;
    if (transitioning_to_main) return;
    client_disconnect();
    if (w == win_main)  win_main  = NULL;
    if (w == win_login) win_login = NULL;
    if (g_main_context_is_owner(NULL) && gtk_main_level() > 0)
        gtk_main_quit();
}

/* ════════════════════════════════════════════════════════════════════════════
 *                                  LOGIN
 * ════════════════════════════════════════════════════════════════════════════ */

static void on_login(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    const char *u = gtk_entry_get_text(GTK_ENTRY(entry_login));
    if (!u || !u[0]) { gtk_label_set_text(GTK_LABEL(lbl_status), "Please enter a username"); return; }
    if (strlen(u) >= 32) { gtk_label_set_text(GTK_LABEL(lbl_status), "Username must be under 32 chars"); return; }

    const char *host = gtk_entry_get_text(GTK_ENTRY(entry_host));
    const char *pt   = gtk_entry_get_text(GTK_ENTRY(entry_port));
    if (host && host[0]) {
        strncpy(server_host, host, sizeof(server_host) - 1);
        server_host[sizeof(server_host) - 1] = 0;
    }
    int port = server_port;
    if (pt && pt[0]) {
        char *end = NULL;
        long p = strtol(pt, &end, 10);
        if (*end || p <= 0 || p > 65535) { gtk_label_set_text(GTK_LABEL(lbl_status), "Invalid port"); return; }
        port = (int)p;
    }
    server_port = port;
    strncpy(current_user, u, sizeof(current_user) - 1);
    current_user[sizeof(current_user) - 1] = 0;
    if (!client_is_connected() && !client_connect(server_host, server_port)) {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Connection failed — check host & port");
        return;
    }
    const char *pw = entry_pass ? gtk_entry_get_text(GTK_ENTRY(entry_pass)) : "";
    if (!pw || !pw[0]) { gtk_label_set_text(GTK_LABEL(lbl_status), "Please enter a password"); return; }
    gtk_label_set_text(GTK_LABEL(lbl_status), "Connecting…");
    client_send_login_pw(u, pw);
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              SEND / COMMANDS
 * ════════════════════════════════════════════════════════════════════════════ */

static void on_send(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry_chat));
    if (!t || !t[0]) return;

    if (pm_mode && pm_target[0] && t[0] != '/') {
        client_send_private(pm_target, t);
    } else if (t[0] == '/' && strncmp(t, "/msg ", 5) == 0) {
        char to[64], m[2048];
        if (sscanf(t + 5, "%63s %2047[^\n]", to, m) == 2) client_send_private(to, m);
    } else if (t[0] == '/' && strncmp(t, "/join ", 6) == 0) {
        char r[64];
        if (sscanf(t + 6, "%63s", r) == 1) {
            char l[128]; snprintf(l, sizeof(l), "JOIN|%s", r);
            client_send_raw(l);
            strncpy(current_room, r, sizeof(current_room) - 1);
            current_room[sizeof(current_room) - 1] = 0;
            update_header_for_mode();
        }
    } else if (t[0] == '/' && strncmp(t, "/create ", 8) == 0) {
        char r[64];
        if (sscanf(t + 8, "%63s", r) == 1) {
            char l[128]; snprintf(l, sizeof(l), "CREATE|%s", r);
            client_send_raw(l);
        }
    } else if (t[0] == '/' && strncmp(t, "/announce ", 10) == 0 && strcmp(current_user, "admin") == 0) {
        char m[2040];
        if (sscanf(t + 10, "%2039[^\n]", m) == 1) {
            char l[2064]; snprintf(l, sizeof(l), "ANNOUNCE|%s", m);
            client_send_raw(l);
        }
    } else if (t[0] == '/' && strncmp(t, "/kick ", 6) == 0 && strcmp(current_user, "admin") == 0) {
        char u[64] = {0}, r[256] = {0};
        if (sscanf(t + 6, "%63s %255[^\n]", u, r) >= 1) {
            char l[512]; snprintf(l, sizeof(l), "KICK|%s|%s", u, r);
            client_send_raw(l);
        }
    } else if (t[0] == '/' && strncmp(t, "/createuser ", 12) == 0 && strcmp(current_user, "admin") == 0) {
        char u[64] = {0}, p[128] = {0};
        if (sscanf(t + 12, "%63s %127s", u, p) == 2) {
            char l[256]; snprintf(l, sizeof(l), "CREATE_USER|%s|%s", u, p);
            client_send_raw(l);
        }
    } else if (t[0] == '/' && strncmp(t, "/deleteuser ", 12) == 0 && strcmp(current_user, "admin") == 0) {
        char u[64] = {0};
        if (sscanf(t + 12, "%63s", u) == 1) {
            char l[128]; snprintf(l, sizeof(l), "DELETE_USER|%s", u);
            client_send_raw(l);
        }
    } else if (t[0] == '/' && strncmp(t, "/resetpass ", 11) == 0 && strcmp(current_user, "admin") == 0) {
        char u[64] = {0}, p[128] = {0};
        if (sscanf(t + 11, "%63s %127s", u, p) == 2) {
            char l[256]; snprintf(l, sizeof(l), "RESET_PASS|%s|%s", u, p);
            client_send_raw(l);
        }
    } else if (strcmp(t, "/listaccounts") == 0 && strcmp(current_user, "admin") == 0) {
        client_send_raw("LIST_ACCOUNTS");
    } else if (strcmp(t, "/help") == 0) {
        ui_add_notification("Commands: /msg, /join, /create, /help");
        if (strcmp(current_user, "admin") == 0)
            ui_add_notification("Admin: /announce, /kick, /createuser, /deleteuser, /resetpass, /listaccounts");
    } else if (t[0] == '/') {
        ui_add_notification("Unknown command. Type /help for available commands.");
    } else {
        client_send_public(current_room, t);
    }
    gtk_entry_set_text(GTK_ENTRY(entry_chat), "");
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              FILE SEND
 * ════════════════════════════════════════════════════════════════════════════ */

static void send_file_to(const char *target) {
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Send File", GTK_WINDOW(win_main),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",  GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);

    GtkFileFilter *filter_all = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_all, "All Files");
    gtk_file_filter_add_pattern(filter_all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter_all);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (!fn) { gtk_widget_destroy(dlg); return; }

        gchar *data = NULL;
        gsize len = 0;
        GError *err = NULL;
        if (!g_file_get_contents(fn, &data, &len, &err)) {
            ui_add_notification("Failed to read file");
            if (err) g_error_free(err);
            g_free(fn);
            gtk_widget_destroy(dlg);
            return;
        }

        gchar *base = g_path_get_basename(fn);
        const char *send_target = (target && target[0]) ? target : "";
        client_send_file_offer(base, (long)len, send_target);

        int mxc = 2048;
        for (gsize off = 0; off < len; off += mxc) {
            gsize chk = (off + (gsize)mxc < len) ? (gsize)mxc : (len - off);
            gchar *b64 = g_base64_encode((guchar*)data + off, chk);
            char line[4096 + 512];
            snprintf(line, sizeof(line), "FILE_DATA|%s|%s", base, b64);
            client_send_raw(line);
            g_free(b64);
        }

        char endline[512];
        snprintf(endline, sizeof(endline), "FILE_END|%s", base);
        client_send_raw(endline);

        char info[512];
        snprintf(info, sizeof(info), "Sent file '%s' (%ld bytes)", base, (long)len);
        ui_add_notification(info);

        g_free(base); g_free(data); g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

static void on_file(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    if (pm_mode && pm_target[0]) send_file_to(pm_target);
    else                          send_file_to("");
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              PM MODE / PROFILE
 * ════════════════════════════════════════════════════════════════════════════ */

static void open_pm(const char *name) {
    pm_mode = true;
    strncpy(pm_target, name, sizeof(pm_target) - 1);
    pm_target[sizeof(pm_target) - 1] = 0;
    update_header_for_mode();
    char ph[128];
    snprintf(ph, sizeof(ph), "Message %s…", name);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), ph);
    rerender_chat();
}

static void show_profile(const char *name) {
    if (!name || !name[0] || strcmp(name, current_user) == 0) return;

    GtkWidget *dlg = gtk_dialog_new_with_buttons(NULL,
        GTK_WINDOW(win_main), GTK_DIALOG_MODAL,
        "_Send Message", GTK_RESPONSE_ACCEPT,
        "Send _File",    100,
        "_Close",        GTK_RESPONSE_REJECT, NULL);
    gtk_window_set_title(GTK_WINDOW(dlg), "User Profile");
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *c = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(c), 28);
    gtk_box_set_spacing(GTK_BOX(c), 12);

    GtkWidget *av = make_avatar(name, 80);
    gtk_widget_set_halign(av, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(c), av, FALSE, FALSE, 0);

    char *nm = g_strdup_printf("<span font='20px' font_weight='700' color='#e6edf3'>%s</span>", name);
    GtkWidget *nl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(nl), nm);
    g_free(nm);
    gtk_widget_set_halign(nl, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(c), nl, FALSE, FALSE, 0);

    GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(status_row, GTK_ALIGN_CENTER);
    GtkWidget *dot = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(dot), "<span color='#3fb950' font='14px'>●</span>");
    gtk_box_pack_start(GTK_BOX(status_row), dot, FALSE, FALSE, 0);
    GtkWidget *ol = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(ol), "<span color='#8b949e' font='12px'>Online</span>");
    gtk_box_pack_start(GTK_BOX(status_row), ol, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), status_row, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_ACCEPT) open_pm(name);
    else if (resp == 100)              send_file_to(name);
    gtk_widget_destroy(dlg);
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              SIDEBAR CLICKS
 * ════════════════════════════════════════════════════════════════════════════ */

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
    strncpy(current_room, name, sizeof(current_room) - 1);
    current_room[sizeof(current_room) - 1] = 0;
    if (password && password[0]) {
        char cmd[256]; snprintf(cmd, sizeof(cmd), "JOIN|%s|%s", name, password);
        client_send_raw(cmd);
    } else {
        char l[128]; snprintf(l, sizeof(l), "JOIN|%s", name);
        client_send_raw(l);
    }
    update_header_for_mode();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), "Type a message…");
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
        gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
        GtkWidget *ca = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
        gtk_container_set_border_width(GTK_CONTAINER(ca), 20);
        gtk_box_set_spacing(GTK_BOX(ca), 8);
        char *m = g_strdup_printf(
            "<span font='14px' font_weight='600' color='#e6edf3'>Enter password</span>\n"
            "<span color='#8b949e' font='11px'>Room '%s' is protected.</span>", name);
        GtkWidget *ll = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(ll), m);
        g_free(m);
        gtk_widget_set_halign(ll, GTK_ALIGN_START);
        gtk_label_set_xalign(GTK_LABEL(ll), 0.0f);
        gtk_box_pack_start(GTK_BOX(ca), ll, FALSE, FALSE, 0);
        GtkWidget *pw_entry = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(pw_entry), FALSE);
        gtk_entry_set_placeholder_text(GTK_ENTRY(pw_entry), "Password");
        gtk_box_pack_start(GTK_BOX(ca), pw_entry, FALSE, FALSE, 0);
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
    update_header_for_mode();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), "Type a message…");
    rerender_chat();
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              CREATE ROOM
 * ════════════════════════════════════════════════════════════════════════════ */

static void on_create_room(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Create Room", GTK_WINDOW(win_main),
        GTK_DIALOG_MODAL, "_Create", GTK_RESPONSE_ACCEPT, "_Cancel", GTK_RESPONSE_REJECT, NULL);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);

    GtkWidget *c = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_set_spacing(GTK_BOX(c), 6);
    gtk_container_set_border_width(GTK_CONTAINER(c), 20);

    GtkWidget *header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header),
        "<span font='15px' font_weight='700' color='#e6edf3'>Create a new room</span>");
    gtk_widget_set_halign(header, GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(header, 8);
    gtk_box_pack_start(GTK_BOX(c), header, FALSE, FALSE, 0);

    GtkWidget *e_name  = gtk_entry_new();
    GtkWidget *e_title = gtk_entry_new();
    GtkWidget *e_desc  = gtk_entry_new();
    GtkWidget *e_pass  = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(e_name),  "room-name (e.g. gaming)");
    gtk_entry_set_placeholder_text(GTK_ENTRY(e_title), "Display title");
    gtk_entry_set_placeholder_text(GTK_ENTRY(e_desc),  "Description (optional)");
    gtk_entry_set_placeholder_text(GTK_ENTRY(e_pass),  "Password (optional)");
    gtk_entry_set_visibility(GTK_ENTRY(e_pass), FALSE);

    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *l1 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l1),
        "<span color='#8b949e' font='11px' font_weight='600'>ROOM NAME</span>");
    gtk_widget_set_halign(l1, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row1), l1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row1), e_name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), row1, FALSE, FALSE, 0);

    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *l2 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l2),
        "<span color='#8b949e' font='11px' font_weight='600'>TITLE</span>");
    gtk_widget_set_halign(l2, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row2), l2, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row2), e_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), row2, FALSE, FALSE, 0);

    GtkWidget *row3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *l3 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l3),
        "<span color='#8b949e' font='11px' font_weight='600'>DESCRIPTION</span>");
    gtk_widget_set_halign(l3, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row3), l3, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row3), e_desc, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), row3, FALSE, FALSE, 0);

    GtkWidget *row4 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *l4 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l4),
        "<span color='#8b949e' font='11px' font_weight='600'>PASSWORD</span>");
    gtk_widget_set_halign(l4, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(row4), l4, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row4), e_pass, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), row4, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const char *rn = gtk_entry_get_text(GTK_ENTRY(e_name));
        const char *rt = gtk_entry_get_text(GTK_ENTRY(e_title));
        const char *rd = gtk_entry_get_text(GTK_ENTRY(e_desc));
        const char *rp = gtk_entry_get_text(GTK_ENTRY(e_pass));
        if (rn && rn[0]) {
            char cmd[1024];
            snprintf(cmd, sizeof(cmd), "CREATE_ROOM|%s|%s|%s|%s",
                rn, rt ? rt : "", rd ? rd : "", rp ? rp : "");
            client_send_raw(cmd);
            ui_add_notification("Room creation requested…");
        }
    }
    gtk_widget_destroy(dlg);
}

/* ════════════════════════════════════════════════════════════════════════════
 *                            HEADER & STATUS BAR
 * ════════════════════════════════════════════════════════════════════════════ */

static void update_header_for_mode(void) {
    if (!lbl_room_header) return;
    if (pm_mode && pm_target[0]) {
        char *m = g_strdup_printf(
            "<span font='15px' font_weight='700' color='#e6edf3'>@%s</span>", pm_target);
        gtk_label_set_markup(GTK_LABEL(lbl_room_header), m);
        g_free(m);
        if (lbl_room_sub) gtk_label_set_markup(GTK_LABEL(lbl_room_sub),
            "<span color='#8b949e' font='11px'>Direct message</span>");
        gtk_widget_set_visible(btn_back, TRUE);
    } else {
        char *m = g_strdup_printf(
            "<span font='15px' font_weight='700' color='#e6edf3'># %s</span>", current_room);
        gtk_label_set_markup(GTK_LABEL(lbl_room_header), m);
        g_free(m);
        if (lbl_room_sub) {
            char *sm = g_strdup_printf(
                "<span color='#8b949e' font='11px'>%d member%s online</span>",
                current_room_user_count, current_room_user_count == 1 ? "" : "s");
            gtk_label_set_markup(GTK_LABEL(lbl_room_sub), sm);
            g_free(sm);
        }
        gtk_widget_set_visible(btn_back, FALSE);
    }
}

static void update_statusbar(void) {
    if (!lbl_statusbar) return;
    char *m = g_strdup_printf(
        "<span color='#6e7681' font='10px'>%s%s · %s:%d · %s</span>",
        connected ? "<span color='#3fb950'>●</span> " : "<span color='#f85149'>●</span> ",
        connected ? "Connected" : "Disconnected",
        server_host, server_port, current_user);
    gtk_label_set_markup(GTK_LABEL(lbl_statusbar), m);
    g_free(m);
}

static void refresh_profile_panel(void) {
    if (!lbl_profile_name || !img_profile_avatar) return;
    char *m = g_strdup_printf(
        "<span font='14px' font_weight='700' color='#e6edf3'>%s</span>", current_user);
    gtk_label_set_markup(GTK_LABEL(lbl_profile_name), m);
    g_free(m);
    if (lbl_profile_status) {
        gtk_label_set_markup(GTK_LABEL(lbl_profile_status),
            connected
                ? "<span color='#3fb950' font='11px'>● online</span>"
                : "<span color='#f85149' font='11px'>● offline</span>");
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *                                  INIT
 * ════════════════════════════════════════════════════════════════════════════ */

static const char *THEME_CSS =
/* ── Base ────────────────────────────────────────────────────────────────── */
"window { background-color: #0d1117; color: #e6edf3; }"
"window, dialog { font-family: -gtk-interface-font, sans-serif; }"

/* ── Sidebar ─────────────────────────────────────────────────────────────── */
"#sidebar { background-color: #161b22; border-right: 1px solid #21262d; }"
"#sidebar-header { background-color: #161b22; border-bottom: 1px solid #21262d; padding: 14px 16px; }"
"#brand-row { padding: 0; }"
"#brand-name { font-size: 16px; font-weight: 700; color: #58a6ff; letter-spacing: 0.2px; }"
"#brand-sub  { font-size: 10px; color: #6e7681; font-weight: 600; letter-spacing: 1px; }"

"#sidebar-section-header {"
"  background-color: #161b22;"
"  color: #6e7681;"
"  font-size: 10px;"
"  font-weight: 700;"
"  letter-spacing: 1px;"
"  padding: 14px 16px 6px;"
"}"

"#profile-card { background-color: #0d1117; padding: 14px 16px; border-top: 1px solid #21262d; }"
"#profile-card #profile-name { color: #e6edf3; font-weight: 700; font-size: 13px; }"
"#profile-card #profile-status { color: #8b949e; font-size: 11px; }"

/* ── Sidebar lists ───────────────────────────────────────────────────────── */
"list#sidebar-list { background-color: transparent; }"
"list#sidebar-list row { background-color: transparent; padding: 6px 12px; border-radius: 6px; }"
"list#sidebar-list row:hover { background-color: #1c2128; }"
"list#sidebar-list row:selected { background-color: #1f6feb; }"
"list#sidebar-list row:selected #uname, list#sidebar-list row:selected #rname { color: #ffffff; font-weight: 600; }"
"#uname { color: #c9d1d9; font-size: 13px; }"
"#rname { color: #c9d1d9; font-size: 13px; }"

/* ── Room rows w/ active highlight ───────────────────────────────────────── */
"list#sidebar-list row.active-room { background-color: rgba(31,111,235,0.18); }"
"list#sidebar-list row.active-room #rname { color: #ffffff; font-weight: 600; }"

/* ── Buttons ─────────────────────────────────────────────────────────────── */
"button {"
"  background-color: #21262d;"
"  color: #c9d1d9;"
"  border: 1px solid #30363d;"
"  border-radius: 6px;"
"  padding: 7px 14px;"
"  font-weight: 500;"
"  font-size: 12px;"
"}"
"button:hover { background-color: #30363d; border-color: #484f58; }"
"button:active { background-color: #1c2128; }"

"#btn-primary, #btn-send {"
"  background-color: #238636;"
"  color: #ffffff;"
"  border: 1px solid rgba(240,246,252,0.1);"
"  font-weight: 600;"
"}"
"#btn-primary:hover, #btn-send:hover { background-color: #2ea043; }"
"#btn-primary:active, #btn-send:active { background-color: #1a7f37; }"

"#btn-danger { background-color: #21262d; color: #f85149; border-color: rgba(248,81,73,0.2); }"
"#btn-danger:hover { background-color: rgba(248,81,73,0.13); border-color: #f85149; }"

"#btn-secondary {"
"  background-color: #1c2128;"
"  color: #c9d1d9;"
"  border: 1px solid #30363d;"
"  font-size: 11px;"
"  padding: 5px 10px;"
"}"

"#btn-back {"
"  background-color: transparent;"
"  color: #58a6ff;"
"  border: none;"
"  font-weight: 500;"
"  padding: 4px 8px;"
"  font-size: 12px;"
"}"
"#btn-back:hover { background-color: #1c2128; }"

"#btn-icon {"
"  background-color: transparent;"
"  color: #8b949e;"
"  border: none;"
"  border-radius: 6px;"
"  padding: 6px 8px;"
"  font-size: 14px;"
"  min-width: 28px;"
"  min-height: 28px;"
"}"
"#btn-icon:hover { background-color: #21262d; color: #e6edf3; }"

"#btn-attach {"
"  background-color: transparent;"
"  color: #8b949e;"
"  border: none;"
"  border-radius: 8px;"
"  padding: 0 10px;"
"  font-size: 18px;"
"  min-width: 40px;"
"  min-height: 40px;"
"}"
"#btn-attach:hover { background-color: #21262d; color: #58a6ff; }"

"#btn-create-room {"
"  background-color: transparent;"
"  color: #58a6ff;"
"  border: 1px dashed #30363d;"
"  border-radius: 8px;"
"  padding: 8px 12px;"
"  font-size: 12px;"
"  font-weight: 600;"
"  margin: 4px 12px 12px;"
"}"
"#btn-create-room:hover { background-color: #1c2128; border-color: #58a6ff; }"

/* ── Inputs ──────────────────────────────────────────────────────────────── */
"entry {"
"  background-color: #0d1117;"
"  color: #e6edf3;"
"  border: 1px solid #30363d;"
"  border-radius: 8px;"
"  padding: 10px 14px;"
"  caret-color: #58a6ff;"
"  font-size: 13px;"
"  min-height: 18px;"
"}"
"entry:focus { border-color: #58a6ff; box-shadow: 0 0 0 3px rgba(88,166,255,0.18); }"
"entry:disabled { background-color: #161b22; color: #6e7681; }"

"#input-chat {"
"  background-color: #161b22;"
"  border: 1px solid #30363d;"
"  border-radius: 10px;"
"  padding: 11px 14px;"
"  font-size: 13px;"
"}"
"#input-chat:focus { border-color: #58a6ff; box-shadow: 0 0 0 3px rgba(88,166,255,0.15); }"

/* ── Login window ────────────────────────────────────────────────────────── */
"#login-window { background-color: #0d1117; }"
"#login-card { background-color: #161b22; border: 1px solid #21262d; border-radius: 14px; padding: 32px; }"
"#login-brand { color: #58a6ff; font-size: 28px; font-weight: 700; letter-spacing: -0.3px; }"
"#login-tagline { color: #8b949e; font-size: 13px; margin-bottom: 8px; }"
"#login-label { color: #8b949e; font-size: 11px; font-weight: 700; letter-spacing: 0.6px; padding-bottom: 4px; }"
"#login-status { color: #f85149; font-size: 12px; font-style: italic; min-height: 16px; }"
"#login-footer { color: #6e7681; font-size: 11px; }"

/* ── Chat header / status bar ────────────────────────────────────────────── */
"#chat-header { background-color: #161b22; border-bottom: 1px solid #21262d; padding: 12px 20px; }"
"#chat-header-title { color: #e6edf3; font-size: 15px; font-weight: 700; }"
"#chat-header-sub { color: #8b949e; font-size: 11px; }"

"#chat-viewport { background-color: #0d1117; }"

"#input-area { background-color: #161b22; border-top: 1px solid #21262d; padding: 12px 16px; }"
"#typing-row { background-color: #161b22; padding: 0 20px 4px; }"
"#typing-row label { color: #58a6ff; font-style: italic; font-size: 11px; }"

"#status-bar { background-color: #161b22; border-top: 1px solid #21262d; padding: 4px 16px; }"
"#status-bar label { color: #6e7681; font-size: 10px; }"

/* ── Message bubbles ─────────────────────────────────────────────────────── */
"#bubble-sent, #bubble-recv, #bubble-pm {"
"  border-radius: 12px;"
"  padding: 0;"
"  background-clip: padding-box;"
"}"
"#bubble-sent { background-color: #1f6feb; }"
"#bubble-recv { background-color: #1c2128; border: 1px solid #30363d; }"
"#bubble-pm   { background-color: #2d1b4e; border: 1px solid #4c1d95; }"

"#pill-notify {"
"  background-color: #161b22;"
"  border: 1px solid #21262d;"
"  border-radius: 999px;"
"  padding: 0;"
"}"
"#pill-announce {"
"  background-color: rgba(210,153,34,0.08);"
"  border: 1px solid rgba(210,153,34,0.3);"
"  border-radius: 999px;"
"  padding: 0;"
"}"

"#file-card {"
"  background-color: #161b22;"
"  border: 1px solid #30363d;"
"  border-radius: 10px;"
"  padding: 0;"
"}"
"#file-card:hover { border-color: #58a6ff; }"

/* ── Dialogs ─────────────────────────────────────────────────────────────── */
"dialog { background-color: #161b22; }"
"dialog .dialog-action-area button { padding: 8px 16px; }"
"dialog label { color: #c9d1d9; }"
"dialog entry { background-color: #0d1117; }"

/* ── Scrollbars ──────────────────────────────────────────────────────────── */
"scrollbar { background-color: transparent; }"
"scrollbar.vertical { background-color: transparent; }"
"scrollbar.horizontal { background-color: transparent; }"
"scrollbar slider { background-color: #21262d; border-radius: 5px; min-width: 6px; min-height: 6px; }"
"scrollbar slider:hover { background-color: #30363d; }"
"scrollbar slider:active { background-color: #484f58; }"
;

void ui_init(int *argc, char ***argv) {
    init_files_dir();
    gtk_init(argc, argv);

    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, THEME_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              LOGIN WINDOW
 * ════════════════════════════════════════════════════════════════════════════ */

void ui_show_login_window(void) {
    if (win_login) return;
    win_login = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win_login), "ConnectHub — Sign In");
    gtk_window_set_default_size(GTK_WINDOW(win_login), 440, 620);
    gtk_window_set_resizable(GTK_WINDOW(win_login), FALSE);
    gtk_window_set_position(GTK_WINDOW(win_login), GTK_WIN_POS_CENTER);
    gtk_widget_set_name(win_login, "login-window");
    g_signal_connect(win_login, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(win_login, "destroy", G_CALLBACK(on_window_closed), NULL);

    /* Outer wrapper — vertically center the card. */
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(outer, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(outer, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(win_login), outer);

    /* Login card. */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_name(card, "login-card");
    gtk_widget_set_size_request(card, 360, -1);
    gtk_box_set_spacing(GTK_BOX(card), 14);
    gtk_container_set_border_width(GTK_CONTAINER(card), 32);
    gtk_box_pack_start(GTK_BOX(outer), card, FALSE, FALSE, 0);

    /* Logo / brand */
    GtkWidget *brand_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    brand_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(brand_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(brand_box, 4);

    /* Logo mark — gradient-style circle with C */
    GtkWidget *logo_lbl = gtk_label_new(NULL);
    char *logo_m = g_strdup_printf(
        "<span font='20px' font_weight='800' color='#ffffff'>C</span>");
    gtk_label_set_markup(GTK_LABEL(logo_lbl), logo_m);
    g_free(logo_m);
    GtkWidget *logo_ev = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(logo_ev), logo_lbl);
    gtk_widget_set_size_request(logo_ev, 44, 44);
    char logo_css[256];
    snprintf(logo_css, sizeof(logo_css),
        "GtkEventBox { background-color: #1f6feb;"
        " border-radius: 22px; }"
        "GtkEventBox GtkLabel { background: transparent; }");
    GtkCssProvider *lp = gtk_css_provider_new();
    gtk_css_provider_load_from_data(lp, logo_css, -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(logo_ev),
        GTK_STYLE_PROVIDER(lp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_unref(lp);
    gtk_box_pack_start(GTK_BOX(brand_box), logo_ev, FALSE, FALSE, 0);

    GtkWidget *brand_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *brand_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(brand_lbl),
        "<span font='22px' font_weight='800' color='#e6edf3' letter_spacing='-200'>ConnectHub</span>");
    gtk_widget_set_halign(brand_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(brand_text), brand_lbl, FALSE, FALSE, 0);
    GtkWidget *sub = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sub),
        "<span color='#6e7681' font='11px' font_weight='600' letter_spacing='800'>LAN CHAT · FILE SHARING</span>");
    gtk_widget_set_halign(sub, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(brand_text), sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brand_box), brand_text, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), brand_box, FALSE, FALSE, 0);

    /* Tagline */
    GtkWidget *tagline = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(tagline),
        "<span color='#8b949e' font='13px'>Sign in to join the chat</span>");
    gtk_widget_set_halign(tagline, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(tagline, 8);
    gtk_box_pack_start(GTK_BOX(card), tagline, FALSE, FALSE, 0);

    /* Username field with label */
    GtkWidget *user_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *user_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(user_lbl), "USERNAME");
    gtk_widget_set_name(user_lbl, "login-label");
    gtk_widget_set_halign(user_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(user_row), user_lbl, FALSE, FALSE, 0);
    entry_login = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_login), "your username");
    gtk_widget_set_size_request(entry_login, -1, 40);
    gtk_box_pack_start(GTK_BOX(user_row), entry_login, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), user_row, FALSE, FALSE, 0);

    /* Password field with label */
    GtkWidget *pass_row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *pass_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(pass_lbl), "PASSWORD");
    gtk_widget_set_name(pass_lbl, "login-label");
    gtk_widget_set_halign(pass_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(pass_row), pass_lbl, FALSE, FALSE, 0);
    entry_pass = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry_pass), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_pass), "••••••••");
    gtk_widget_set_size_request(entry_pass, -1, 40);
    gtk_box_pack_start(GTK_BOX(pass_row), entry_pass, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), pass_row, FALSE, FALSE, 0);

    /* Server details — labeled section */
    GtkWidget *server_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *server_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(server_lbl), "SERVER");
    gtk_widget_set_name(server_lbl, "login-label");
    gtk_widget_set_halign(server_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(server_section), server_lbl, FALSE, FALSE, 0);

    GtkWidget *server_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    entry_host = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_host), server_host);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_host), "Host");
    gtk_widget_set_hexpand(entry_host, TRUE);
    gtk_widget_set_size_request(entry_host, -1, 40);

    entry_port = gtk_entry_new();
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%d", server_port);
    gtk_entry_set_text(GTK_ENTRY(entry_port), pbuf);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_port), "Port");
    gtk_widget_set_size_request(entry_port, 90, 40);

    gtk_box_pack_start(GTK_BOX(server_row), entry_host, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(server_row), entry_port, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(server_section), server_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), server_section, FALSE, FALSE, 0);

    /* Status line */
    lbl_status = gtk_label_new("");
    gtk_widget_set_name(lbl_status, "login-status");
    gtk_widget_set_halign(lbl_status, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(lbl_status, 4);
    gtk_box_pack_start(GTK_BOX(card), lbl_status, FALSE, FALSE, 0);

    /* Primary button */
    GtkWidget *btn = gtk_button_new_with_label("Sign In");
    gtk_widget_set_name(btn, "btn-primary");
    gtk_widget_set_size_request(btn, -1, 44);
    gtk_widget_set_margin_top(btn, 6);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_login, "activate", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_pass, "activate", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_host, "activate", G_CALLBACK(on_login), NULL);
    g_signal_connect(entry_port, "activate", G_CALLBACK(on_login), NULL);
    gtk_box_pack_start(GTK_BOX(card), btn, FALSE, FALSE, 0);

    /* Footer */
    GtkWidget *footer = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(footer),
        "<span color='#6e7681' font='11px'>Plain C · POSIX sockets · GTK3</span>");
    gtk_widget_set_halign(footer, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(footer, 6);
    gtk_box_pack_start(GTK_BOX(card), footer, FALSE, FALSE, 0);

    gtk_widget_show_all(win_login);
    /* Ensure window is visible, not minimised, and on top. */
    gtk_window_deiconify(GTK_WINDOW(win_login));
    gtk_window_present(GTK_WINDOW(win_login));
    gtk_widget_grab_focus(entry_login);
    if (gdk_display_get_default()) gdk_display_flush(gdk_display_get_default());
}

void ui_show_login_error(const char *r) {
    if (lbl_status) {
        char *m = g_strdup_printf("⚠  %s", r ? r : "Login failed");
        gtk_label_set_markup(GTK_LABEL(lbl_status), m);
        g_free(m);
    }
}

void ui_set_username(const char *name) {
    if (name) {
        strncpy(current_user, name, sizeof(current_user) - 1);
        current_user[sizeof(current_user) - 1] = 0;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              MAIN WINDOW
 * ════════════════════════════════════════════════════════════════════════════ */

void ui_show_main_window(void) {
    if (win_main) {
        gtk_widget_show_all(win_main);
        if (win_login) { gtk_widget_destroy(win_login); win_login = NULL; }
        return;
    }
    if (win_login) {
        transitioning_to_main = true;
        gtk_widget_destroy(win_login);
        win_login = NULL;
        transitioning_to_main = false;
    }

    connected = true;
    win_main = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[256];
    snprintf(title, sizeof(title), "ConnectHub — %s", current_user);
    gtk_window_set_title(GTK_WINDOW(win_main), title);
    gtk_window_set_default_size(GTK_WINDOW(win_main), 1100, 720);
    gtk_window_set_position(GTK_WINDOW(win_main), GTK_WIN_POS_CENTER);
    g_signal_connect(win_main, "delete-event", G_CALLBACK(on_delete_event), NULL);
    g_signal_connect(win_main, "destroy", G_CALLBACK(on_window_closed), NULL);

    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(win_main), hpaned);

    /* ─────────── SIDEBAR ─────────── */
    sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 260, -1);
    gtk_paned_pack1(GTK_PANED(hpaned), sidebar, FALSE, FALSE);

    /* Sidebar header (brand + sub) */
    GtkWidget *sb_header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(sb_header, "sidebar-header");
    GtkWidget *brand_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_name(brand_row, "brand-row");
    GtkWidget *brand_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(brand_lbl),
        "<span font='16px' font_weight='700' color='#58a6ff'>ConnectHub</span>");
    gtk_widget_set_halign(brand_lbl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(brand_row), brand_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sb_header), brand_row, FALSE, FALSE, 0);
    GtkWidget *brand_sub = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(brand_sub),
        "<span font='10px' font_weight='700' color='#6e7681' letter_spacing='800'>LAN CHAT</span>");
    gtk_widget_set_halign(brand_sub, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sb_header), brand_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), sb_header, FALSE, FALSE, 0);

    /* Profile card at top of sidebar */
    GtkWidget *profile_card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_name(profile_card, "profile-card");
    img_profile_avatar = make_avatar(current_user, 36);
    gtk_box_pack_start(GTK_BOX(profile_card), img_profile_avatar, FALSE, FALSE, 0);
    GtkWidget *profile_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    lbl_profile_name = gtk_label_new(NULL);
    gtk_widget_set_name(lbl_profile_name, "profile-name");
    gtk_widget_set_halign(lbl_profile_name, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(profile_text), lbl_profile_name, FALSE, FALSE, 0);
    lbl_profile_status = gtk_label_new(NULL);
    gtk_widget_set_name(lbl_profile_status, "profile-status");
    gtk_widget_set_halign(lbl_profile_status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(profile_text), lbl_profile_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(profile_card), profile_text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), profile_card, FALSE, FALSE, 0);

    /* Sidebar scrollable content */
    GtkWidget *sb_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sb_scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *sb_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(sb_scroll), sb_list_box);
    gtk_box_pack_start(GTK_BOX(sidebar), sb_scroll, TRUE, TRUE, 0);

    /* Direct Messages section */
    GtkWidget *dm_hdr = gtk_label_new("DIRECT MESSAGES");
    gtk_widget_set_name(dm_hdr, "sidebar-section-header");
    gtk_widget_set_halign(dm_hdr, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(sb_list_box), dm_hdr, FALSE, FALSE, 0);

    lst_users = gtk_list_box_new();
    gtk_widget_set_name(lst_users, "sidebar-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lst_users), GTK_SELECTION_SINGLE);
    g_signal_connect(lst_users, "row-activated", G_CALLBACK(on_user_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(sb_list_box), lst_users, FALSE, FALSE, 0);

    /* Channels section */
    GtkWidget *ch_hdr = gtk_label_new("CHANNELS");
    gtk_widget_set_name(ch_hdr, "sidebar-section-header");
    gtk_widget_set_halign(ch_hdr, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ch_hdr, 4);
    gtk_box_pack_start(GTK_BOX(sb_list_box), ch_hdr, FALSE, FALSE, 0);

    lst_rooms = gtk_list_box_new();
    gtk_widget_set_name(lst_rooms, "sidebar-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lst_rooms), GTK_SELECTION_SINGLE);
    g_signal_connect(lst_rooms, "row-activated", G_CALLBACK(on_room_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(sb_list_box), lst_rooms, FALSE, FALSE, 0);

    /* Create room button (under channels) */
    btn_create_room = gtk_button_new_with_label("+  Create channel");
    gtk_widget_set_name(btn_create_room, "btn-create-room");
    g_signal_connect(btn_create_room, "clicked", G_CALLBACK(on_create_room), NULL);
    gtk_box_pack_start(GTK_BOX(sb_list_box), btn_create_room, FALSE, FALSE, 0);

    /* Files section */
    GtkWidget *files_hdr = gtk_label_new("FILES");
    gtk_widget_set_name(files_hdr, "sidebar-section-header");
    gtk_widget_set_halign(files_hdr, GTK_ALIGN_START);
    gtk_widget_set_margin_top(files_hdr, 8);
    gtk_box_pack_start(GTK_BOX(sb_list_box), files_hdr, FALSE, FALSE, 0);

    lst_files = gtk_list_box_new();
    gtk_widget_set_name(lst_files, "sidebar-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lst_files), GTK_SELECTION_SINGLE);
    g_signal_connect(lst_files, "row-activated", G_CALLBACK(on_file_row_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(sb_list_box), lst_files, FALSE, FALSE, 0);

    /* Admin hint */
    if (strcmp(current_user, "admin") == 0) {
        GtkWidget *al = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(al),
            "<span color='#d2a8ff' font='10px' letter_spacing='400'>"
            "ADMIN · /announce · /kick · /createuser"
            "</span>");
        gtk_widget_set_halign(al, GTK_ALIGN_START);
        gtk_widget_set_margin_start(al, 16);
        gtk_widget_set_margin_end(al, 16);
        gtk_widget_set_margin_top(al, 8);
        gtk_widget_set_margin_bottom(al, 8);
        gtk_box_pack_start(GTK_BOX(sb_list_box), al, FALSE, FALSE, 0);
    }

    /* ─────────── CHAT PANEL ─────────── */
    chat_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_pack2(GTK_PANED(hpaned), chat_panel, TRUE, FALSE);

    /* Header */
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_name(header_box, "chat-header");
    btn_back = gtk_button_new_with_label("←  Channels");
    gtk_widget_set_name(btn_back, "btn-back");
    gtk_widget_set_visible(btn_back, FALSE);
    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_back_to_room), NULL);
    gtk_box_pack_start(GTK_BOX(header_box), btn_back, FALSE, FALSE, 0);

    GtkWidget *title_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    lbl_room_header = gtk_label_new(NULL);
    gtk_widget_set_name(lbl_room_header, "chat-header-title");
    gtk_widget_set_halign(lbl_room_header, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(title_col), lbl_room_header, FALSE, FALSE, 0);
    lbl_room_sub = gtk_label_new(NULL);
    gtk_widget_set_name(lbl_room_sub, "chat-header-sub");
    gtk_widget_set_halign(lbl_room_sub, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(title_col), lbl_room_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_box), title_col, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(chat_panel), header_box, FALSE, FALSE, 0);

    /* Chat viewport */
    sw_chat = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    gtk_widget_set_name(GTK_WIDGET(sw_chat), "chat-viewport");
    gtk_scrolled_window_set_policy(sw_chat, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(GTK_WIDGET(sw_chat), TRUE);
    lst_chat = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lst_chat), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(sw_chat), lst_chat);
    gtk_box_pack_start(GTK_BOX(chat_panel), GTK_WIDGET(sw_chat), TRUE, TRUE, 0);

    /* Typing indicator row */
    GtkWidget *typing_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(typing_row, "typing-row");
    lbl_typing = gtk_label_new("");
    gtk_widget_set_halign(lbl_typing, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(typing_row), lbl_typing, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(chat_panel), typing_row, FALSE, FALSE, 0);

    /* Input area */
    GtkWidget *input_area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_name(input_area, "input-area");
    gtk_widget_set_margin_start(input_area, 16);
    gtk_widget_set_margin_end(input_area, 16);
    gtk_widget_set_margin_top(input_area, 8);
    gtk_widget_set_margin_bottom(input_area, 12);

    btn_attach = gtk_button_new_with_label("📎");
    gtk_widget_set_name(btn_attach, "btn-attach");
    gtk_widget_set_size_request(btn_attach, 40, 44);
    gtk_widget_set_tooltip_text(btn_attach, "Send file");
    g_signal_connect(btn_attach, "clicked", G_CALLBACK(on_file), NULL);
    gtk_box_pack_start(GTK_BOX(input_area), btn_attach, FALSE, FALSE, 0);

    entry_chat = gtk_entry_new();
    gtk_widget_set_name(entry_chat, "input-chat");
    gtk_widget_set_hexpand(entry_chat, TRUE);
    gtk_widget_set_size_request(entry_chat, -1, 44);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_chat), "Type a message…");
    g_signal_connect(entry_chat, "activate", G_CALLBACK(on_send), NULL);
    g_signal_connect(entry_chat, "key-press-event", G_CALLBACK(on_entry_keypress), NULL);
    gtk_box_pack_start(GTK_BOX(input_area), entry_chat, TRUE, TRUE, 0);

    btn_send = gtk_button_new_with_label("Send");
    gtk_widget_set_name(btn_send, "btn-send");
    gtk_widget_set_size_request(btn_send, 80, 44);
    g_signal_connect(btn_send, "clicked", G_CALLBACK(on_send), NULL);
    gtk_box_pack_start(GTK_BOX(input_area), btn_send, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(chat_panel), input_area, FALSE, FALSE, 0);

    /* Status bar */
    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_name(status_bar, "status-bar");
    lbl_statusbar = gtk_label_new("");
    gtk_widget_set_halign(lbl_statusbar, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(status_bar), lbl_statusbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(chat_panel), status_bar, FALSE, FALSE, 0);

    refresh_profile_panel();
    update_header_for_mode();
    update_statusbar();

    gtk_widget_show_all(win_main);
    if (gdk_display_get_default()) gdk_display_flush(gdk_display_get_default());
    client_send_raw("LIST_USERS");
    client_send_raw("LIST_ROOMS");
}

/* ════════════════════════════════════════════════════════════════════════════
 *                              UI API
 * ════════════════════════════════════════════════════════════════════════════ */

void ui_append_message(const char *room, const char *sender, const char *text, const char *ts) {
    gchar *cid = make_conv_id("public", room, NULL);
    add_msg_entry(cid, "public", sender, text, ts, room, NULL, 0);
    g_free(cid);
    if (g_strcmp0(room, current_room) == 0) {
        current_room_user_count++;
        update_header_for_mode();
    }
}

void ui_append_private_message(const char *sender, const char *recipient, const char *text, const char *ts) {
    const char *other = NULL;
    if (sender && strcmp(sender, current_user) == 0 && recipient && recipient[0])
        other = recipient;
    else if (sender && sender[0])
        other = sender;
    if (!other || !other[0]) other = "unknown";

    gchar *cid = make_conv_id("pm", NULL, other);
    add_msg_entry(cid, "pm", sender, text, ts, other, NULL, 0);
    g_free(cid);
}

void ui_append_announcement(const char *text, const char *ts) {
    add_msg_entry(g_strdup("global"), "announce", "Server", text, ts, "", NULL, 0);
}

void ui_add_notification(const char *text) {
    add_msg_entry(g_strdup("global"), "notify", "", text, "", "", NULL, 0);
}

static guint typing_send_tag = 0;
static gboolean typing_send_cb(gpointer d) {
    (void)d;
    typing_send_tag = 0;
    if (!pm_mode) client_send_typing(current_room);
    return FALSE;
}

static gboolean on_entry_keypress(GtkWidget *w, GdkEventKey *e, gpointer d) {
    (void)w; (void)e; (void)d;
    if (!typing_send_tag)
        typing_send_tag = g_timeout_add_seconds(1, typing_send_cb, NULL);
    return FALSE;
}

void ui_show_typing(const char *room, const char *user) {
    if (!lbl_typing || !user) return;
    if (!pm_mode) {
        if (room && room[0] && strcmp(room, current_room) != 0) return;
    } else {
        if (user && strcmp(user, pm_target) != 0 && strcmp(user, current_user) != 0) return;
    }
    if (typing_tag) { g_source_remove(typing_tag); typing_tag = 0; }
    char buf[256];
    snprintf(buf, sizeof(buf), "%s is typing…", user);
    gtk_label_set_text(GTK_LABEL(lbl_typing), buf);
    typing_tag = g_timeout_add_seconds(3, typing_cb, NULL);
}

/* ── file ── */
void ui_show_file_offer(const char *sender, const char *filename, const char *size, const char *target) {
    if (!win_main) return;
    if (!target || !target[0] || strcmp(target, current_user) == 0 || strcmp(target, "all") == 0) {
        /* intended recipient */
    } else {
        return;
    }
    if (pending_find(filename)) return;

    long fsize = atol(size ? size : "0");
    pending_add(sender, filename, fsize);

    GtkWidget *dlg = gtk_dialog_new_with_buttons("Incoming File",
        GTK_WINDOW(win_main), GTK_DIALOG_MODAL,
        "_Accept", GTK_RESPONSE_ACCEPT,
        "_Reject", GTK_RESPONSE_REJECT, NULL);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *c = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(c), 24);
    gtk_box_set_spacing(GTK_BOX(c), 14);

    /* Sender avatar + name */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(head, GTK_ALIGN_CENTER);
    GtkWidget *av = make_avatar(sender, 44);
    gtk_box_pack_start(GTK_BOX(head), av, FALSE, FALSE, 0);
    GtkWidget *head_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    char *sm = g_strdup_printf(
        "<span font='14px' font_weight='700' color='#e6edf3'>%s sent a file</span>", sender);
    GtkWidget *sl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sl), sm);
    g_free(sm);
    gtk_widget_set_halign(sl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(head_text), sl, FALSE, FALSE, 0);
    char szbuf[32] = "";
    if (fsize > 0) format_filesize(fsize, szbuf, sizeof(szbuf));
    char *fm = g_strdup_printf(
        "<span color='#8b949e' font='11px'>%s%s%s</span>",
        filename, szbuf[0] ? " · " : "", szbuf);
    GtkWidget *fl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(fl), fm);
    g_free(fm);
    gtk_widget_set_halign(fl, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(head_text), fl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head_text), head_text, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(c), head, FALSE, FALSE, 0);

    gtk_widget_show_all(dlg);
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    PendingFile *pf = pending_find(filename);
    if (!pf) return;

    if (resp == GTK_RESPONSE_ACCEPT) {
        pf->accepted = true;
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
            gtk_label_set_text(GTK_LABEL(lbl_typing), buf);
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
    /* Build notification message with sender info (if known) */
    char txt[1024];
    if (fullpath && fullpath[0])
        snprintf(txt, sizeof(txt), "%s sent a file: %s", pf->sender, filename);
    else
        snprintf(txt, sizeof(txt), "File received: %s", filename ? filename : "unknown");
    add_msg_entry(g_strdup("global"), "file", pf->sender, txt, "", filename ? filename : "",
        fullpath ? fullpath : pf->final_path, pf->size);

    if (fullpath) {
        MsgEntry *e = calloc(1, sizeof(MsgEntry));
        e->conversation = g_strdup("global");
        e->type        = g_strdup("file");
        e->sender      = g_strdup(pf->sender);
        e->text        = g_strdup(filename ? filename : "file");
        e->ts          = g_strdup("");
        e->extra       = g_strdup(filename ? filename : "");
        e->filepath    = g_strdup(fullpath);
        e->filesize    = pf->size;
        received_files = g_list_append(received_files, e);
        free(fullpath);
        update_files_list();
    }
    /* Clear typing indicator if we used it for progress */
    if (lbl_typing) gtk_label_set_text(GTK_LABEL(lbl_typing), "");
    pending_remove(filename);
}

void ui_on_file_rejected(const char *filename, const char *recipient, const char *reason) {
    (void)recipient;
    char buf[512];
    snprintf(buf, sizeof(buf), "File '%s' was rejected: %s",
        filename, reason && reason[0] ? reason : "no reason");
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

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);
        gtk_widget_set_margin_top(row, 2);
        gtk_widget_set_margin_bottom(row, 2);

        GtkWidget *icon = gtk_label_new("📎");
        gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);

        GtkWidget *name_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *name = gtk_label_new(NULL);
        char *nm = g_strdup_printf(
            "<span color='#e6edf3' font='12px'>%s</span>",
            e->text ? e->text : e->filepath);
        gtk_label_set_markup(GTK_LABEL(name), nm);
        g_free(nm);
        gtk_widget_set_name(name, "fname");
        gtk_widget_set_halign(name, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(name_col), name, FALSE, FALSE, 0);

        if (e->filesize > 0) {
            char sz[32];
            format_filesize(e->filesize, sz, sizeof(sz));
            GtkWidget *sizelbl = gtk_label_new(NULL);
            char *sm = g_strdup_printf("<span color='#6e7681' font='10px'>%s</span>", sz);
            gtk_label_set_markup(GTK_LABEL(sizelbl), sm);
            g_free(sm);
            gtk_widget_set_halign(sizelbl, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(name_col), sizelbl, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(row), name_col, TRUE, TRUE, 0);

        GtkWidget *btn_open = gtk_button_new_with_label("Open");
        gtk_widget_set_name(btn_open, "btn-secondary");
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
    /* Backwards-compat: the live path now writes via ui_finish_file. */
    (void)filename; (void)fullpath;
}

/* ── list updates ── */
static void update_user_list_widget(GtkWidget *list, const char *csv) {
    GList *c = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = c; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(c);
    if (!csv || !csv[0]) return;
    char *copy = g_strdup(csv);
    char *sp = NULL;
    for (char *tok = strtok_r(copy, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
        char *s = strchr(tok, ':');
        if (s) *s = 0;
        if (!tok[0]) continue;
        if (strcmp(tok, current_user) == 0) continue; /* skip self */

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);
        gtk_widget_set_margin_top(row, 2);
        gtk_widget_set_margin_bottom(row, 2);
        gtk_widget_set_valign(row, GTK_ALIGN_CENTER);

        /* Small avatar */
        GtkWidget *av = make_avatar(tok, 24);
        gtk_box_pack_start(GTK_BOX(row), av, FALSE, FALSE, 0);

        GtkWidget *lbl = gtk_label_new(tok);
        gtk_widget_set_name(lbl, "uname");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

        /* Status dot */
        GtkWidget *dot = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(dot), "<span color='#3fb950' font='10px'>●</span>");
        gtk_box_pack_start(GTK_BOX(row), dot, FALSE, FALSE, 0);

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
    update_user_list_widget(lst_users, csv);
}

static void update_room_list_widget(GtkWidget *list, const char *csv) {
    GList *c = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = c; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(c);
    if (!csv || !csv[0]) return;

    char *copy = g_strdup(csv);
    char *sp = NULL;
    for (char *tok = strtok_r(copy, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp)) {
        if (!tok[0]) continue;
        bool is_protected = false;
        char rname_buf[128];
        strncpy(rname_buf, tok, sizeof(rname_buf) - 1);
        rname_buf[sizeof(rname_buf) - 1] = 0;
        char *p = strstr(rname_buf, ":p");
        if (p) { *p = 0; is_protected = true; }
        if (!rname_buf[0]) continue;

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);
        gtk_widget_set_margin_top(row, 2);
        gtk_widget_set_margin_bottom(row, 2);
        gtk_widget_set_valign(row, GTK_ALIGN_CENTER);

        /* # icon */
        GtkWidget *hash = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hash),
            "<span color='#6e7681' font_weight='700' font='14px'>#</span>");
        gtk_box_pack_start(GTK_BOX(row), hash, FALSE, FALSE, 0);

        GtkWidget *lbl = gtk_label_new(rname_buf);
        gtk_widget_set_name(lbl, "rname");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);

        if (is_protected) {
            GtkWidget *lock = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(lock),
                "<span color='#d29922' font='11px'>🔒</span>");
            gtk_widget_set_tooltip_text(lock, "Password protected");
            gtk_box_pack_start(GTK_BOX(row), lock, FALSE, FALSE, 0);
        }

        GtkWidget *lr = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(lr), row);
        gtk_list_box_prepend(GTK_LIST_BOX(list), lr);
        if (is_protected) g_object_set_data(G_OBJECT(lr), "protected", GINT_TO_POINTER(1));
    }
    g_free(copy);
    gtk_widget_show_all(list);
}

void ui_update_room_list(const char *csv, int count) {
    (void)count;
    if (!lst_rooms) return;
    update_room_list_widget(lst_rooms, csv);
}

void ui_run(void) { gtk_main(); }

void ui_cleanup(void) {
    if (win_login && GTK_IS_WIDGET(win_login)) gtk_widget_destroy(win_login);
    if (win_main  && GTK_IS_WIDGET(win_main))  gtk_widget_destroy(win_main);
    for (GList *l = msg_history; l; l = l->next) {
        MsgEntry *e = (MsgEntry *)l->data;
        if (e) {
            g_free(e->conversation); g_free(e->type); g_free(e->sender);
            g_free(e->text); g_free(e->ts); g_free(e->extra); g_free(e->filepath); g_free(e);
        }
    }
    g_list_free(msg_history); msg_history = NULL;
}
