/* handlers.c - one small function per chat command.
 *
 * dispatch_command() is the only entry point: it looks at the parsed
 * command name and calls the matching tiny handler. FILE_* commands are
 * forwarded to files.c; everything else is handled here.
 */
#include "server.h"

/* Raw, behavior-preserving socket write (no active-flag side effects). */
static void send_raw(Client *c, const char *msg) {
    send(c->sockfd, msg, strlen(msg), 0);
}

/* ---- LOGIN ---- */

static void h_login(Client *c, Cmd *m) {
    bool accepted = false;
    bool is_admin_user = (strcmp(m->a1, admin_user) == 0);
    pthread_mutex_lock(&client_mutex);
    bool duplicate = (net_client_find(m->a1) != NULL);
    pthread_mutex_unlock(&client_mutex);

    if (duplicate) {
        send_raw(c, "LOGIN_FAIL|User already logged in\n");
    } else if (is_admin_user) {
        char ah[65];
        sha256_hex(m->a2, ah);
        if (strcmp(ah, admin_pass_hash) == 0) {
            pthread_mutex_lock(&client_mutex);
            if (net_client_find(m->a1) == NULL) {
                strncpy(c->username, m->a1, MAX_USERNAME - 1);
                c->username[MAX_USERNAME - 1] = '\0';
                c->is_admin = true;
                accepted = true;
            }
            pthread_mutex_unlock(&client_mutex);
        } else {
            send_raw(c, "LOGIN_FAIL|Invalid admin password\n");
        }
    } else {
        pthread_mutex_lock(&user_mutex);
        bool valid = user_validate(m->a1, m->a2);
        pthread_mutex_unlock(&user_mutex);
        if (valid) {
            pthread_mutex_lock(&client_mutex);
            if (net_client_find(m->a1) == NULL) {
                strncpy(c->username, m->a1, MAX_USERNAME - 1);
                c->username[MAX_USERNAME - 1] = '\0';
                accepted = true;
            }
            pthread_mutex_unlock(&client_mutex);
        } else {
            send_raw(c, "LOGIN_FAIL|Invalid username or password\n");
        }
    }

    if (accepted) {
        c->active = true;
        strncpy(c->current_room, "general", MAX_ROOM_NAME - 1);
        char ok[128];
        snprintf(ok, sizeof(ok), "LOGIN_OK|%s\n", m->a1);
        send_raw(c, ok);
        history_replay(c->sockfd, "general");
        net_broadcast_user_list();
        net_broadcast_room_list();
        log_message("INFO", "User '%s' logged in from %s", m->a1, inet_ntoa(c->addr.sin_addr));
    }
}

/* ---- messaging ---- */

static void h_public(Client *c, Cmd *m) {
    char msg[MAX_MESSAGE + 512];
    char ts[32];
    net_get_timestamp(ts, sizeof(ts));
    snprintf(msg, sizeof(msg), "PUBLIC|%s|%s|%s|%s\n", c->current_room, c->username, m->a2, ts);
    history_add(c->current_room, msg);
    net_broadcast_room(c->current_room, msg, NULL);
    total_messages++;
    log_message("MSG", "[%s] %s: %s", c->current_room, c->username, m->a2);
}

static void h_private(Client *c, Cmd *m) {
    char msg[MAX_MESSAGE + 512];
    char ts[32];
    net_get_timestamp(ts, sizeof(ts));
    snprintf(msg, sizeof(msg), "PRIVATE|%s|%s|%s|%s\n", c->username, m->a1, m->a2, ts);
    net_send_to_user(m->a1, msg);
    net_send_to_user(c->username, msg);
    total_privmsgs++;
    log_message("PRIV", "%s -> %s: %s", c->username, m->a1, m->a2);
}

static void h_typing(Client *c, Cmd *m) {
    (void)m;
    char msg[128];
    snprintf(msg, sizeof(msg), "TYPING|%s|%s\n", c->current_room, c->username);
    net_broadcast_room(c->current_room, msg, c);
}

/* ---- rooms ---- */

static void h_join(Client *c, Cmd *m) {
    const char *room_name = m->a1;
    const char *password = (m->parts >= 3) ? m->a2 : "";
    if (!room_exists(room_name)) {
        char err[256];
        snprintf(err, sizeof(err), "JOIN_FAIL|Room '%s' does not exist\n", room_name);
        send_raw(c, err);
    } else if (room_is_protected(room_name) && !c->is_admin) {
        if (!room_has_access(c->username, room_name)) {
            if (!password[0] || !room_check_password(room_name, password)) {
                char err[256];
                snprintf(err, sizeof(err), "JOIN_FAIL|Room '%s' requires password\n", room_name);
                send_raw(c, err);
            } else {
                room_grant_access(c->username, room_name);
                net_finish_join(c, room_name);
            }
        } else {
            net_finish_join(c, room_name);
        }
    } else {
        net_finish_join(c, room_name);
    }
}

static void h_leave(Client *c, Cmd *m) {
    char prev_room[MAX_ROOM_NAME];
    strncpy(prev_room, c->current_room, sizeof(prev_room) - 1);
    strncpy(c->current_room, "general", MAX_ROOM_NAME - 1);
    c->current_room[MAX_ROOM_NAME - 1] = '\0';
    char msg[256];
    snprintf(msg, sizeof(msg), "NOTIFY|%s left room %s.\n", c->username, prev_room);
    net_broadcast_room(prev_room, msg, NULL);
    (void)m;
}

static void h_create(Client *c, Cmd *m) {
    if (room_create(m->a1)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "NOTIFY|Room '%s' created by %s.\n", m->a1, c->username);
        net_broadcast(msg, NULL);
        net_broadcast_room_list();
    }
}

static void h_create_room(Client *c, Cmd *m) {
    const char *rn = m->a1;
    const char *rt = (m->parts >= 3) ? m->a2 : "";
    const char *rd = (m->parts >= 4) ? m->a3 : "";
    const char *rp = (m->parts >= 5) ? m->a4 : "";
    if (room_create_extended(rn, rt, rd, rp, c->username)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "NOTIFY|Room '%s' created by %s.\n", rn, c->username);
        net_broadcast(msg, NULL);
        net_broadcast_room_list();
        char ok[128];
        snprintf(ok, sizeof(ok), "ROOM_CREATED|%s\n", rn);
        send_raw(c, ok);
    } else {
        char err[256];
        snprintf(err, sizeof(err), "ERROR|Room '%s' already exists or invalid\n", rn);
        send_raw(c, err);
    }
}

static void h_delete_room(Client *c, Cmd *m) {
    if (room_delete(m->a1, c->username)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "NOTIFY|Room '%s' deleted by %s.\n", m->a1, c->username);
        net_broadcast(msg, NULL);
        pthread_mutex_lock(&client_mutex);
        for (Client *p = client_list; p; p = p->next) {
            if (p->active && strcmp(p->current_room, m->a1) == 0) {
                strncpy(p->current_room, "general", MAX_ROOM_NAME - 1);
            }
        }
        pthread_mutex_unlock(&client_mutex);
        net_broadcast_room_list();
    } else {
        char err[256];
        snprintf(err, sizeof(err), "ERROR|Cannot delete room '%s' (not found or not authorized)\n", m->a1);
        send_raw(c, err);
    }
}

static void h_update_room(Client *c, Cmd *m) {
    if (room_update_field(m->a1, m->a2, m->a3)) {
        char ok[128];
        snprintf(ok, sizeof(ok), "NOTIFY|Room '%s' updated.\n", m->a1);
        net_broadcast(ok, NULL);
    } else {
        send_raw(c, "ERROR|Could not update room\n");
    }
}

/* ---- listings ---- */

static void h_list_users(Client *c, Cmd *m) {
    (void)m;
    char userlist[MAX_MESSAGE] = {0};
    pthread_mutex_lock(&client_mutex);
    for (Client *p = client_list; p; p = p->next) {
        if (p->active) {
            net_list_append(userlist, sizeof(userlist), ",", p->username);
            net_list_append(userlist, sizeof(userlist), "", ":1");
        }
    }
    pthread_mutex_unlock(&client_mutex);
    char out[MAX_MESSAGE + 64];
    snprintf(out, sizeof(out), "USERS|%s\n", userlist);
    send_raw(c, out);
}

static void h_list_rooms(Client *c, Cmd *m) {
    (void)m;
    char roomstr[MAX_MESSAGE] = {0};
    room_list(roomstr, sizeof(roomstr));
    char out[MAX_MESSAGE + 64];
    snprintf(out, sizeof(out), "ROOMS|%s\n", roomstr);
    send_raw(c, out);
}

static void h_who(Client *c, Cmd *m) {
    const char *room = m->a1;
    if (!room_exists(room)) {
        char err[256];
        snprintf(err, sizeof(err), "ERROR|Room '%s' does not exist\n", room);
        send_raw(c, err);
    } else {
        char who[MAX_MESSAGE] = {0};
        pthread_mutex_lock(&client_mutex);
        for (Client *p = client_list; p; p = p->next) {
            if (p->active && strcmp(p->current_room, room) == 0) {
                net_list_append(who, sizeof(who), ", ", p->username);
            }
        }
        pthread_mutex_unlock(&client_mutex);
        char out[MAX_MESSAGE + 128];
        if (who[0])
            snprintf(out, sizeof(out), "STATUS|Users in #%s: %s\n", room, who);
        else
            snprintf(out, sizeof(out), "STATUS|No one else is in #%s\n", room);
        send_raw(c, out);
    }
}

static void h_history(Client *c, Cmd *m) {
    (void)m;
    /* Replay this client's current-room history (no cross-room leak). */
    history_replay(c->sockfd, c->current_room);
}

/* ---- admin ---- */

static void h_stats(Client *c, Cmd *m) {
    (void)m;
    if (c->is_admin) {
        net_send_status(c);
    } else {
        send_raw(c, "ERROR|Only admin can request stats\n");
    }
}

static void h_announce(Client *c, Cmd *m) {
    if (c->is_admin) {
        char msg[MAX_MESSAGE + 256];
        char ts[32];
        net_get_timestamp(ts, sizeof(ts));
        snprintf(msg, sizeof(msg), "ANNOUNCE|%s|%s|%s\n", c->username, m->a1, ts);
        net_broadcast(msg, NULL);
        log_message("CTRL", "Announcement by admin: %s", m->a1);
    } else {
        send_raw(c, "ERROR|Only admin can announce\n");
    }
}

static void h_kick(Client *c, Cmd *m) {
    if (c->is_admin) {
        pthread_mutex_lock(&client_mutex);
        Client *target = net_client_find(m->a1);
        if (target && target->active) {
            char msg[256];
            snprintf(msg, sizeof(msg), "KICK|%s\n", m->a2);
            send(target->sockfd, msg, strlen(msg), 0);
            target->active = false;
            shutdown(target->sockfd, SHUT_RDWR);
        }
        pthread_mutex_unlock(&client_mutex);
        char notify[256];
        snprintf(notify, sizeof(notify), "NOTIFY|%s was kicked: %s\n", m->a1, m->a2);
        net_broadcast(notify, NULL);
        net_broadcast_user_list();
        log_message("CTRL", "Admin kicked %s: %s", m->a1, m->a2);
    } else {
        send_raw(c, "ERROR|Only admin can kick\n");
    }
}

static void h_create_user(Client *c, Cmd *m) {
    if (c->is_admin) {
        pthread_mutex_lock(&user_mutex);
        bool created = user_create(m->a1, m->a2);
        if (created) users_save();
        pthread_mutex_unlock(&user_mutex);
        if (created) {
            char ok[256];
            snprintf(ok, sizeof(ok), "ANNOUNCE|User '%s' created.\n", m->a1);
            send_raw(c, ok);
            log_message("INFO", "Admin created user '%s'", m->a1);
        } else {
            char err[256];
            snprintf(err, sizeof(err), "ERROR|Cannot create user '%s' (exists/invalid)\n", m->a1);
            send_raw(c, err);
        }
    } else {
        send_raw(c, "ERROR|Only admin can create users\n");
    }
}

static void h_delete_user(Client *c, Cmd *m) {
    if (c->is_admin) {
        pthread_mutex_lock(&client_mutex);
        Client *target = net_client_find(m->a1);
        if (target && target->active) {
            char msg[256];
            snprintf(msg, sizeof(msg), "KICK|Your account has been deleted.\n");
            send(target->sockfd, msg, strlen(msg), 0);
            target->active = false;
            shutdown(target->sockfd, SHUT_RDWR);
        }
        pthread_mutex_unlock(&client_mutex);
        pthread_mutex_lock(&user_mutex);
        bool removed = account_remove(m->a1);
        if (removed) users_save();
        pthread_mutex_unlock(&user_mutex);
        if (removed) {
            char ok[256];
            snprintf(ok, sizeof(ok), "ANNOUNCE|User '%s' deleted.\n", m->a1);
            net_broadcast(ok, NULL);
            net_broadcast_user_list();
            log_message("INFO", "Admin deleted user '%s'", m->a1);
        } else {
            char err[256];
            snprintf(err, sizeof(err), "ERROR|Cannot delete user '%s' (not found)\n", m->a1);
            send_raw(c, err);
        }
    } else {
        send_raw(c, "ERROR|Only admin can delete users\n");
    }
}

static void h_reset_pass(Client *c, Cmd *m) {
    if (c->is_admin) {
        pthread_mutex_lock(&user_mutex);
        bool ok = user_reset_pass(m->a1, m->a2);
        if (ok) users_save();
        pthread_mutex_unlock(&user_mutex);
        if (ok) {
            char msg[256];
            snprintf(msg, sizeof(msg), "ANNOUNCE|Password reset for '%s'.\n", m->a1);
            send_raw(c, msg);
            log_message("INFO", "Admin reset password for '%s'", m->a1);
        } else {
            char err[256];
            snprintf(err, sizeof(err), "ERROR|Cannot reset password for '%s' (not found)\n", m->a1);
            send_raw(c, err);
        }
    } else {
        send_raw(c, "ERROR|Only admin can reset passwords\n");
    }
}

static void h_list_accounts(Client *c, Cmd *m) {
    (void)m;
    if (c->is_admin) {
        char list[4096] = {0};
        pthread_mutex_lock(&user_mutex);
        for (UserAccount *u = user_list; u; u = u->next) {
            net_list_append(list, sizeof(list), ",", u->username);
        }
        pthread_mutex_unlock(&user_mutex);
        char out[4096 + 64];
        snprintf(out, sizeof(out), "ACCOUNT_LIST|%s\n", list);
        send_raw(c, out);
    } else {
        send_raw(c, "ERROR|Only admin can list accounts\n");
    }
}

static void h_logout(Client *c, Cmd *m) {
    (void)m;
    c->active = false;
}

/* ---- dispatch ---- */

void dispatch_command(Client *c, Cmd *m) {
    if (strcmp(m->cmd, "LOGIN") == 0 && m->parts >= 3)            h_login(c, m);
    else if (strcmp(m->cmd, "PUBLIC") == 0 && m->parts >= 2)      h_public(c, m);
    else if (strcmp(m->cmd, "PRIVATE") == 0 && m->parts >= 3)     h_private(c, m);
    else if (strcmp(m->cmd, "TYPING") == 0)                       h_typing(c, m);
    else if (strcmp(m->cmd, "JOIN") == 0 && m->parts >= 2)        h_join(c, m);
    else if (strcmp(m->cmd, "LEAVE") == 0 && m->parts >= 2)       h_leave(c, m);
    else if (strcmp(m->cmd, "CREATE") == 0 && m->parts >= 2)      h_create(c, m);
    else if (strcmp(m->cmd, "CREATE_ROOM") == 0 && m->parts >= 2) h_create_room(c, m);
    else if (strcmp(m->cmd, "DELETE_ROOM") == 0 && m->parts >= 2) h_delete_room(c, m);
    else if (strcmp(m->cmd, "UPDATE_ROOM") == 0 && m->parts >= 4) h_update_room(c, m);
    else if (strcmp(m->cmd, "LIST_USERS") == 0)                   h_list_users(c, m);
    else if (strcmp(m->cmd, "LIST_ROOMS") == 0)                   h_list_rooms(c, m);
    else if (strcmp(m->cmd, "WHO") == 0 && m->parts >= 2)         h_who(c, m);
    else if (strcmp(m->cmd, "HISTORY") == 0)                      h_history(c, m);
    else if (strcmp(m->cmd, "STATS") == 0)                        h_stats(c, m);
    else if (strcmp(m->cmd, "ANNOUNCE") == 0 && m->parts >= 2)    h_announce(c, m);
    else if (strcmp(m->cmd, "KICK") == 0 && m->parts >= 3)        h_kick(c, m);
    else if (strcmp(m->cmd, "CREATE_USER") == 0 && m->parts >= 3) h_create_user(c, m);
    else if (strcmp(m->cmd, "DELETE_USER") == 0 && m->parts >= 2) h_delete_user(c, m);
    else if (strcmp(m->cmd, "RESET_PASS") == 0 && m->parts >= 3)  h_reset_pass(c, m);
    else if (strcmp(m->cmd, "LIST_ACCOUNTS") == 0)                h_list_accounts(c, m);
    else if (strcmp(m->cmd, "FILE_REQUEST") == 0 && m->parts >= 3) files_handler_request(c, m);
    else if (strcmp(m->cmd, "FILE_OFFER") == 0 && m->parts >= 3)  files_handler_offer(c, m);
    else if (strcmp(m->cmd, "FILE_DATA") == 0)                    files_handler_data(c, m);
    else if (strcmp(m->cmd, "FILE_END") == 0 && m->parts >= 2)    files_handler_end(c, m);
    else if (strcmp(m->cmd, "FILE_ACCEPT") == 0 && m->parts >= 2) files_handler_accept(c, m);
    else if (strcmp(m->cmd, "FILE_REJECT") == 0 && m->parts >= 3) files_handler_reject(c, m);
    else if (strcmp(m->cmd, "LOGOUT") == 0)                       h_logout(c, m);
}
