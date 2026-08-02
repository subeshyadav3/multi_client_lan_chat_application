# ConnectHub — Complete Function-by-Function Guide

**Project:** Multi-client LAN chat application written in **C** with **zero external
dependencies** (Berkeley sockets + POSIX threads, plus GTK3 only for the GUI
front-end). This document explains *what every file and every function does*, so
you can walk through the whole codebase confidently in a viva.

> There are **two front-ends** that talk to the **same wire protocol**:
>
> - `connect_cli/` — terminal (TUI) client built on `termios` + ANSI escape codes, single-threaded `select()` loop.
> - `connect_gui/` — GTK3 windowed client.
>
> Both run against a very similar threaded TCP server. The **CLI version is the
> newest and most complete**, so this guide goes into most detail there.

---

## 1. Repository Layout

```
ConnectHub/
├── connect_cli/                 # CLI/TUI front-end (most features)
│   ├── Makefile                 # Build rules (gcc, no external libs)
│   ├── bin/                     # Built executables: chatclient, chatserver
│   ├── build/                   # .o object files
│   ├── client/                  # CLI client code
│   │   ├── client.c / client.h  # main() + the single select() loop
│   │   ├── protocol.c           # handle_line(): turns server lines into screen events
│   │   ├── commands.c           # /slash commands + input processor
│   │   ├── files.c              # file send/receive state machine + base64
│   │   ├── net.c / net.h        # Socket helpers
│   │   └── tui.c                # Raw-mode terminal drawing (ANSI)
│   ├── server/                  # Threaded TCP server
│   │   ├── server.c             # main() + select() accept loop (small!)
│   │   ├── server.h             # Shared types, globals, and prototypes
│   │   ├── connection.c         # Per-client receive thread (reads lines)
│   │   ├── handlers.c           # One small function per chat command
│   │   ├── files.c              # Upload slots, FIFO queue, FILE_* handlers
│   │   ├── users.c              # Account storage (config/users.cred)
│   │   ├── history.c            # Per-room recent-message history
│   │   ├── net.c                # Client list + broadcast helpers
│   │   ├── room.c / room.h      # Room list management
│   │   ├── room_access.c        # "Has joined protected room" tracking
│   │   └── logger.c / logger.h  # File logging to logs/server.log
│   ├── shared/                  # Code used by both client & server
│   │   ├── constants.h          # All magic numbers / limits
│   │   ├── protocol.c / .h      # Message enums + format helpers
│   │   └── sha256.c / .h        # Password hashing (no libcrypto)
│   ├── config/                  # admin.cred, users.cred
│   ├── logs/                    # server.log (runtime)
│   ├── files/                   # received files (runtime)
│   ├── docs/DOCUMENTATION.md    # architecture-focused doc
│   └── tests/smoke.py           # integration test
├── connect_gui/                 # GTK3 front-end (original)
│   ├── client/ (client.c, chat.c/h, ui.c/h)
│   ├── server/ (server.c, room.c/h, logger.c/h)
│   ├── shared/ (protocol, constants)
│   └── tests/ (smoke.py, file_transfer.py)
├── README.md
└── documentation.md             # ← you are here
```

---

## 2. Shared Code (both projects)

### 2.1 `shared/constants.h` — configuration

Defines every limit as a macro:

| Macro | Value | Meaning |
|-------|-------|---------|
| `PORT` | `8080` | Default TCP port |
| `MAX_CLIENTS` | `128` | Max simultaneous connections |
| `MAX_USERNAME` | `32` | Username length limit |
| `MAX_MESSAGE` | `2048` | Max chat message size |
| `MAX_ROOM_NAME` | `64` | Room name length limit |
| `MAX_ROOMS` | `32` | Max rooms in the list |
| `BUFFER_SIZE` | `4096` | Network read buffer |
| `MAX_FILE_SIZE` | `64 MB` | Max allowed file |
| `FILE_CHUNK_SIZE` | `2048` | Bytes per file chunk before base64 |
| `MAX_FILENAME` | `256` | Filename length limit |

*Viva point:* these caps prevent buffer overruns and keep every wire line under
`BUFFER_SIZE` even after base64 encoding (2048 bytes → ~2732 chars, well below 4096).

### 2.2 `shared/protocol.h` — message types & struct

- **`MessageType` enum** (line 7): the full set of message kinds —
  `MSG_LOGIN`, `MSG_PUBLIC`, `MSG_PRIVATE`, `MSG_FILE_OFFER`, `MSG_ACCEPT`,
  `MSG_KICK`, … used as a tag for each message.
- **`Message` struct** (line 20): one logical message —
  `type, sender, recipient, room, body, timestamp`.
- **Function prototypes** (line 29+): the format helpers below.

### 2.3 `shared/protocol.c` — message formatters

All helpers write a complete protocol line into a `static` buffer and return a
pointer to it (used mainly as reference/tests; the live client & server build
lines inline with `snprintf`):

| Function | Line | What it does |
|----------|------|--------------|
| `format_public_msg()` | 5 | `PUBLIC\|room\|sender\|body\|timestamp` |
| `format_private_msg()` | 11 | `PRIVATE\|sender\|recipient\|body\|timestamp` |
| `format_login()` | 17 | `LOGIN\|username` |
| `format_login_ok()` / `format_login_fail()` | 23 / 29 | login result |
| `format_announce()` | 35 | `ANNOUNCE\|text\|timestamp` |
| `format_notify()` | 41 | `NOTIFY\|text` (system notice) |
| `format_kick()` | 47 | `KICK\|reason` |
| `format_user_list()` | 53 | comma-joined user list |
| `format_error()` | 63 | `ERROR\|reason` |
| `parse_message()` | 72 | reverse: `sscanf` a line back into a `Message` struct |

### 2.4 `shared/sha256.c` — password hashing (CLI server only)

A from-scratch SHA-256 implementation (no `libcrypto`), used to store and verify
passwords so plaintext is never saved.

| Function | Line | What it does |
|----------|------|--------------|
| `rotr()` | 25 | Circular bit-shift used by the compression round |
| `sha256_transform()` | 29 | The core 64-round compression of one 64-byte block |
| `sha256_init()` | 55 | Sets the 8 initial hash constants (H0..H7) |
| `sha256_update()` | 64 | Feeds bytes into the 64-byte block; calls `transform()` when full |
| `sha256_final()` | 75 | Applies padding (`0x80`, zeros, 64-bit length) and outputs 32 raw bytes |
| `sha256_hex()` | 91 | Public API: hash a string and produce a 64-char hex string |

*Viva point:* the 64-round schedule is defined by the well-known K-table
(round constants) on line 14, and the message schedule `w[16..63]` on line 35.

---

## 3. CLI Server — `connect_cli/server/`

> **Architecture at a glance.** The old single 1400-line `server.c` was split into
> small modules, each owning one job. `server.c` is now ~130 lines and only does
> two things: **open the socket** and **run the `select()` accept loop**. Every
> other piece of behaviour lives in its own file, listed below.
>
> **Concurrency model** — the whole thing to explain in a viva:
> - the main thread uses **`select()`** with a **1-second timeout** on the
>   listening socket, so it can also run the upload-queue housekeeping every tick;
> - each accepted connection is handled by **its own detached thread**
>   (`connection.c`) using **blocking** `recv()`/`send()`;
> - shared state (client list, users, upload slots, transfers) is guarded by
>   named mutexes: `client_mutex`, `user_mutex`, `upload_mutex`, `transfer_mutex`
>   (+ a per-room-history `lock`).
>
> Why `select()` and not `epoll`/`poll`? For a small LAN chat server (dozens of
> clients at most) `select()` is simpler and perfectly adequate; each client
> already runs in its own thread, so the main loop never has to juggle thousands
> of sockets.

### 3.1 `server.c` — entry point (small on purpose)

| Function | Line | What it does |
|----------|------|--------------|
| `main()` | 42 | Parse optional port arg; load `admin.cred` and hash the admin password; init logger/rooms/users/history; create socket, `SO_REUSEADDR`, `bind`, `listen`; then run the **`select()` loop** with a 1 s timeout. On timeout it calls `files_expire_stale()` (housekeeping); on a readable socket it calls `accept()` and hands the socket to `net_spawn_client()` |
| `signal_handler()` | 33 | `SIGINT`/`SIGTERM` → set `server_running = 0` and close the listening socket (async-signal-safe: no locks) |
| `server_shutdown()` | 24 | Set the running flag off, close the listening socket, free every client, close the logger, free rooms/access |

### 3.2 `connection.c` — the per-client thread

| Function | Line | What it does |
|----------|------|--------------|
| `handle_client()` | 37 | Runs in its own detached thread per client. Loops `recv()`, splits the byte stream into newline-terminated lines, splits each on `|` into a `Cmd` struct, and calls `dispatch_command()`. On disconnect it releases everything the user owned (`files_*` helpers) and broadcasts an updated user list |
| `parse_command()` | 11 | Split `line` on `|` into `cmd/a1/a2/a3/a4`, preserving empty fields (needed for `CREATE_ROOM` with an empty description) |

### 3.3 `handlers.c` — one small function per chat command

`dispatch_command()` (line 402) matches the command name and calls one tiny
handler. Chat commands live here; `FILE_*` commands are forwarded to `files.c`:

| Handler | Line | Behaviour |
|---------|------|-----------|
| `h_login` | 16 | Reject duplicates; admin (`admin`) validated against the hashed admin password, normal users against `user_validate()`. On success: mark active, join `general`, replay history, broadcast user/room lists |
| `h_public` | 72 | Store in room history and broadcast to the sender's room |
| `h_private` | 83 | Deliver to both recipient and sender (both see the conversation) |
| `h_typing` | 94 | Broadcast "X is typing" to the room (except sender) |
| `h_join` | 103 | Check the room exists; if protected, require the password once, then grant access and `net_finish_join()` |
| `h_leave` | 128 | Return to `general`, notify the old room |
| `h_create` / `h_create_room` | 139/148 | Simple and extended (title/description/password) room creation |
| `h_delete_room` | 168 | Only creator or admin; boot members back to `general` |
| `h_update_room` | 188 | Change title/description/password |
| `h_list_users` / `h_list_rooms` / `h_who` | 200/217/226 | Send the live lists / users-in-a-room to the requester |
| `h_history` | 251 | Replay current room history (no cross-room leak) |
| `h_stats` | 259 | Admin only — online users, message/file counters |
| `h_announce` | 268 | Admin only — broadcast to all |
| `h_kick` | 281 | Admin only — send `KICK`, shut the socket down |
| `h_create_user` / `h_delete_user` / `h_reset_pass` / `h_list_accounts` | 303/324/356/377 | Admin-only account management |
| `h_logout` | 395 | Mark the client inactive (the connection thread then exits) |

### 3.4 `files.c` — file transfer (slots, FIFO queue, `FILE_*` handlers)

Three pieces of state, each with its own mutex: `upload_slots[]` (max 2
concurrent uploads), the **FIFO upload queue**, and `transfer_list` (active
offers). The wire handlers are:

| Function | Line | What it does |
|----------|------|--------------|
| `files_handler_request` | 320 | Validate size (≤ 64 MB). If the limits allow (≤ 2 slots, combined ≤ 1 MB budget), grant a slot + random token and reply `FILE_GRANTED`; otherwise enqueue and reply `FILE_WAIT|filename|position` |
| `files_handler_offer` | 381 | Legacy direct offer path (no slot) |
| `files_handler_data` | 395 | Forward one base64 chunk to the recipient, but only if the embedded token matches the slot (authorization) |
| `files_handler_end` / `files_handler_accept` / `files_handler_reject` | 452/469/490 | Finish / accept / reject a transfer; `files_release()` frees the slot + record and promotes the queue |
| `files_expire_stale` | 252 | Deactivate slots idle > 30 s, drop queue entries waiting > 120 s, then promote the queue (called every second from `main`) |
| `files_process_queue` | 199 | While limits allow, promote queued entries to granted slots |
| `files_queue_remove_client` / `files_remove_slots` / `files_remove_transfers` | 267/290/74 | Disconnect cleanup — purge everything a leaving user owned |
| `upload_active_bytes` / `upload_can_accept` / `upload_enqueue` / `upload_grant_one` | 92/100/119/157 | The queue machinery itself |

### 3.5 `users.c` — account storage

| Function | Line | What it does |
|----------|------|--------------|
| `users_load()` / `users_save()` | 98/89 | Read/write `config/users.cred` as `user:sha256hex` lines; hashing plaintext entries on load auto-upgrades the file |
| `user_create()` / `user_reset_pass()` / `user_validate()` | 45/51/63 | Create an account (stores `sha256_hex(password)`), set a new hashed password, verify a login |
| `account_remove()` | 74 | Delete a `UserAccount` node |
| `user_create_plain()` / `user_exists()` / `is_hex64()` | 32/12/21 | Internals used while loading |

### 3.6 `history.c` — recent-message history

| Function | Line | What it does |
|----------|------|--------------|
| `history_init()` | 24 | Reset all room histories |
| `history_add()` | 43 | Append a message line; drop the oldest when a room has more than 50 |
| `history_replay()` | 67 | Send a room's stored lines to a fresh joiner (`/history`, late joiners) |

### 3.7 `net.c` — client list + delivery

| Function | Line | What it does |
|----------|------|--------------|
| `net_spawn_client()` | 146 | Allocate a `Client`, add it to the list, run `handle_client()` in a detached thread |
| `safe_send()` / `net_broadcast()` / `net_broadcast_room()` / `net_send_to_user()` | 13/54/44/32 | Deliver a line to one client, everyone, a room, or a username — with broken-pipe handling |
| `net_client_find()` / `net_client_remove()` | 24/64 | Look up / unlink a `Client` (find expects the caller to hold `client_mutex`) |
| `net_broadcast_user_list()` / `net_broadcast_room_list()` | 79/94 | Build `USERS|a:1,b:1,…` / `ROOMS|…` and broadcast so sidebars stay in sync |
| `net_send_status()` / `net_finish_join()` | 102/115 | Admin stats; and set a client's room + replay history + send `JOIN_OK` + notify the room |
| `net_sanitize_filename()` / `net_get_timestamp()` | 128/139 | Strip `/ \ | \n` from filenames (path-traversal protection); format the time as `"02:30 PM"` |
| `net_close_all_clients()` | 170 | Shutdown path — close and free every client |

### 3.8 `room.c` / `room.h` — rooms

| Function | Line | What it does |
|----------|------|--------------|
| `room_init()` | 11 | Create the list and seed the default `general` room |
| `room_destroy()` | 26 | Free the whole room list |
| `room_create()` | 38 | Thin wrapper calling `room_create_extended()` with empty extras |
| `room_create_extended()` | 42 | Add a room (title/desc/optional password → `is_protected`) |
| `room_delete()` | 67 | Remove a room, but only if requester is its creator or `admin` |
| `room_update_field()` | 89 | Edit title / description / password |
| `room_check_password()` | 114 | Compare a given password to the room's |
| `room_is_protected()` | 125 | Does the room need a password? |
| `room_exists()` | 134 | Room present? |
| `room_find()` | 142 | Get the `RoomNode*` for a name (linear scan) |
| `room_list()` | 150 | Format `r1,r2:p` (protected rooms get `:p`) |

All access is guarded by `room_mutex`.

### 3.3 `room_access.c` — protected-room entry tracking

Remembers who has *successfully* entered a protected room so they are not asked
for the password again on re-join. In-memory only.

| Function | Line | What it does |
|----------|------|--------------|
| `room_access_clear()` | 20 | Free the whole access list |
| `room_access_load()` | 28 | "Load" = clear (no persistence) |
| `room_has_access()` | 33 | Has user entered this room before? |
| `room_grant_access()` | 46 | Add a (user, room) entry |

### 3.4 `logger.c` / `logger.h` — server logging

| Function | Line | What it does |
|----------|------|--------------|
| `logger_init()` | 11 | Open `logs/server.log` in append mode (unbuffered) |
| `logger_close()` | 22 | Close the log file |
| `log_message()` | 29 | Write `[timestamp] [LEVEL] message` line |

Levels used: `INFO`, `MSG`, `PRIV`, `CTRL`, `FILE`.

---

## 4. CLI Client — `connect_cli/client/`

### 4.1 `client.h` — the `App` state

- `App` struct (line 30) holds *all* client state: socket, login state,
  username, current room, the scrolling chat buffer (`ChatLine lines[]`), the
  online user list, room list, typing indicator, the input line, command
  history, pending file offers, and the send/receive state machines for file
  transfer.

> **Architecture at a glance.** The client is split so the entry point is tiny
> and each concern has its own file: `client.c` (main + select loop),
> `protocol.c` (incoming server lines), `commands.c` (slash commands + input),
> `files.c` (file transfer), `net.c` (socket), `tui.c` (terminal drawing).

### 4.2 `client.c` — main() + the single `select()` loop (entry point)

| Function | Line | What it does |
|----------|------|--------------|
| `main()` | 179 | Parse `--host/--port/--user/--pass/--admin` (with positional fallback); init the `App`; enter raw mode; then a **reconnect loop** that calls `run_session()` again after `/logout` |
| `run_session()` | 96 | Connect, reset the app state to the login screen, optionally auto-login (`--user/--pass` on the first connect only), then run the inner **`select()` loop** until the session ends |
| `read_from_server()` | 38 | `recv()` → assemble newline-terminated lines → `handle_line()` (protocol.c); returns false when the loop must exit (disconnect/logout) |
| `read_from_stdin()` | 60 | Read one keystroke: Enter → `process_input()`; backspace; arrow keys for command history; Ctrl-C to quit; printable chars → append to the input line + `send_typing()` |
| `client_quit()` | 30 | Restore the terminal, close the socket, exit |
| `on_signal()` | (top) | `SIGINT`/`SIGTERM` set a flag so the loop exits cleanly |

The **`select()` call lives in `run_session()`** (line 124): it watches `stdin`
and the socket with a **100 ms timeout**, so the UI stays responsive and file
chunks stream while the user types. After each select tick the loop also clears a
stale "typing…" indicator, calls `files_try_send_chunk()` (keeps uploads moving),
and redraws via `tui_draw()`.

*Viva point — single-threaded client:* everything (network, keyboard, drawing,
file streaming) lives in ONE `select()` loop, so the interface never freezes.

### 4.3 `protocol.c` — turning server lines into screen events

| Function | Line | What it does |
|----------|------|--------------|
| `handle_line()` | 84 | The inverse of the server's dispatch. Splits a received line on `\|` and reacts to each message type: prints chat/PM/notify lines, refreshes the user/room sidebars, handles login/join results, and routes `FILE_*` messages to `files.c` |
| `parse_pipe()` | 17 | Split a protocol line on `\|` into up to `max` fields |
| `after_pipes()` | 34 | Return a pointer to the text after the Nth `\|` (used to grab large base64 chunks) |
| `update_user_list()` / `update_room_list()` | 45/62 | Parse the CSV lists from `USERS`/`ROOMS` into the App arrays |
| `request_lists()` | 79 | Send `LIST_USERS` + `LIST_ROOMS` to keep the sidebar fresh |

### 4.4 `commands.c` — slash commands + the input processor

| Function | Line | What it does |
|----------|------|--------------|
| `cmd_process()` | 80 | Parse `/command args` and send the matching protocol line: `/msg`→`PRIVATE`, `/join`→`JOIN`, `/sendfile`→`files_request_send_file()`, `/accept`/`/reject`→`FILE_ACCEPT`/`FILE_REJECT`, admin commands (`/announce`, `/kick`, `/createuser`, …), and `/help` |
| `process_input()` | 38 | Called on Enter. If not logged in → the login wizard (username → masked password → `LOGIN`). If logged in and the line starts with `/` → `cmd_process()`; otherwise send it as `PUBLIC` and echo it locally |
| `push_history()` | 16 | Store typed lines in the history ring (max 100, consecutive dedupe) for the up/down arrows |
| `send_typing()` | 31 | Send `TYPING|room` so others see the "typing…" indicator |

### 4.5 `files.c` — file send / receive

| Function | Line | What it does |
|----------|------|--------------|
| `b64tab` / `b64encode()` | 19 | Encode raw bytes to base64 (3→4 chars, `=` padding) |
| `b64val()` / `b64decode()` | 34/43 | Decode base64 back to raw bytes |
| `path_basename()` | 60 | Strip the directory part from a path |
| `find_offer()` / `files_add_offer()` / `files_remove_offer()` | 67/76/94 | Manage the local list of incoming offers (`[1] alice offers 'x' …`) |
| `files_request_send_file()` | 108 | `stat()` the file (must be a regular file, ≤ 64 MB), set send state 1, send `FILE_REQUEST|name|size|target` |
| `files_finalize_received()` | 135 | Close the `.tmp` file, pick a unique name (` (n)` suffix if taken), `rename()` into `files/` |
| `files_receive_chunk()` | 164 | Append one decoded base64 chunk to `files/name.tmp`, update progress (state machine for starting a fresh receive) |
| `files_try_send_chunk()` | 199 | Called every select tick while streaming (state 3): read up to 2048 raw bytes, base64-encode, send `FILE_DATA|filename|token|b64`, then `FILE_END` when done |

### 4.6 `net.c` — socket helpers

| Function | Line | What it does |
|----------|------|--------------|
| `net_connect()` | 13 | `gethostbyname()` → `socket()` → `connect()`; returns fd or -1 |
| `net_send_line()` | 32 | Send a whole line + `\n`, looping until fully sent (partial-write safe) |
| `net_close()` | 49 | `shutdown()` + `close()` |

### 4.7 `tui.c` — terminal user interface

Uses `termios` (raw mode) + ANSI escape sequences.

| Function | Line | What it does |
|----------|------|--------------|
| `tui_enter_raw()` | 26 | Save termios, disable canonical mode + echo, `VMIN=1` (read keys immediately) |
| `tui_restore()` | 37 | Show cursor, clear screen, restore original termios |
| `tui_get_size()` | 46 | `ioctl(TIOCGWINSZ)` for rows/cols (with sane defaults) |
| `color_for()` | 57 | Map a `LineKind` to an ANSI color (self=green, PM=magenta, error=red bold, file=blue…) |
| `tui_add_line()` | 72 | Append a `ChatLine`; drop oldest if the buffer is full (ring of 600) |
| `tui_add_notify()` | 88 | Convenience wrapper for notification lines |
| `tui_set_input()` | 97 | Replace the input buffer (used by history navigation) |
| `print_cell()` | 104 | Print a string padded/truncated to a width |
| `draw_title()` | 109 | Reverse-video title bar with user + room |
| `draw_separator()` | 121 | Dim line of `-` |
| `draw_login()` | 128 | Centered login box: username → password (masked with `*`), cursor positioning |
| `tui_draw()` | 181 | Full redraw: title, sidebar (users + rooms), chat area (last N lines), status bar, input line + cursor |

---

## 5. GUI Project — `connect_gui/` (GTK3)

The GUI server is essentially the same logic as the CLI server (thread-per-client,
same command dispatch), but:

- **`server/server.c`** — identical accept loop + `handle_client()`, plus
  `client_add()` (line 999), `client_remove(fd)` (line 1006), `load_admin_creds()`
  (line 332), and `user_create/user_delete/user_reset_pass` stored **in plaintext**
  (lines 262–295). No SHA-256 (no `shared/sha256.c`), no `room_access.c`
  (tracking is inline, lines 297–330), no `/who`, `/history` or `STATUS` stats —
  those are CLI additions.

### 5.1 `client/chat.c` — network layer

| Function | Line | What it does |
|----------|------|--------------|
| `client_connect()` | 32 | Resolve host, open socket, spawn the **receive thread** |
| `client_disconnect()` | 67 | Set flags, `shutdown()` socket, `pthread_join()` the thread (no leak) |
| `client_send_login()` / `client_send_login_pw()` | 84/91 | Send `LOGIN` (old style / with password) |
| `client_send_public()` / `client_send_private()` | 98/105 | Send chat lines |
| `client_send_typing()` | 112 | Send `TYPING` |
| `client_send_file_request()` / `client_send_file_offer()` | 120/127 | File offer paths |
| `client_send_file_reject()` / `client_send_file_accept()` | 134/141 | Accept/decline |
| `client_send_raw()` | 148 | Send any line (admin commands) |
| `client_register_callbacks()` | 153 | Store UI callbacks |
| `client_is_connected()` | 159 | State check |
| `send_line()` | 163 | Full-loop newline-terminated send |
| `client_receive_loop()` | 179 | **Background thread**: blocking `recv()`, splits on `\n`, calls `handle_incoming()` |
| `handle_incoming()` | 207 | Forward the line to the message callback (→ main thread) |

### 5.2 `client/client.c` — GTK glue

| Function | Line | What it does |
|----------|------|--------------|
| `parse_pipe()` | 9 | Split on `\|` |
| `process_on_main_thread()` | 30 | Run on the GTK main loop via `g_idle_add`; dispatches every incoming message type to `ui_*` functions |
| `on_message_received()` | 127 | Called from receive thread; copies the line and schedules `process_on_main_thread` — **this marshals data from the network thread to the GTK thread** |
| `disconnect_on_main_thread()` / `on_disconnect()` | 135/141 | Notify UI when the socket closes |
| `main()` | 146 | Init GTK, register callbacks, show login, run main loop |

### 5.3 `client/ui.c` — GTK windows & widgets

Key handlers: `on_login()` (734), `on_send()` (770), `on_file()` (932),
`send_file_to()` (841), `send_file_data_with_token()` (887),
`join_room()` (1004), `on_create_room()`, `on_admin_panel()`, file management
helpers `make_unique_path()` (203), `update_files_list()` (275), `rerender_chat()`
(571). The public `ui_*` functions (declared in `ui.h`) are called from
`client.c` to update the window: append messages, show typing, update user/room
lists, accept/reject file offers, and list accounts.

---

## 6. Wire Protocol Summary

Every message is a plain-text, newline-terminated line: `TYPE|f1|f2|…`.

**Client → Server:** `LOGIN`, `PUBLIC`, `PRIVATE`, `TYPING`, `JOIN`, `LEAVE`,
`CREATE`, `CREATE_ROOM`, `DELETE_ROOM`, `UPDATE_ROOM`, `LIST_USERS`,
`LIST_ROOMS`, `WHO`, `HISTORY`, `STATS`, `ANNOUNCE`, `KICK`, `CREATE_USER`,
`DELETE_USER`, `RESET_PASS`, `LIST_ACCOUNTS`, `FILE_REQUEST`, `FILE_OFFER`,
`FILE_DATA`, `FILE_END`, `FILE_ACCEPT`, `FILE_REJECT`, `LOGOUT`.

**Server → Client:** `LOGIN_OK`, `LOGIN_FAIL`, `PUBLIC`, `PRIVATE`, `NOTIFY`,
`ANNOUNCE`, `TYPING`, `USERS`, `ROOMS`, `JOIN_OK`, `JOIN_FAIL`,
`ROOM_CREATED`, `STATUS`, `ACCOUNT_LIST`, `KICK`, `ERROR`, `FILE_GRANTED`,
`FILE_DENIED`, `FILE_WAIT`, `FILE_OFFER`, `FILE_ACCEPT`, `FILE_REJECT`,
`FILE_DATA`, `FILE_END`.

---

## 7. File Transfer Flow (step by step)

```
Sender                     Server                        Recipient
  │  FILE_REQUEST|name|size|target ──►
  │          limits free? ── yes ──► allocates slot + token
  │  ◄── FILE_GRANTED|sender|name|token|size
  │  ◄── FILE_OFFER|sender|name|size|target ────────► shows [1] offer
  │                                                    /accept or /reject
  │  ◄── FILE_ACCEPT|recipient|name ◄──────────────── /
  │  FILE_DATA|name|token|<b64 chunk> ──► (validates token) ──► decode+append
  │  ...repeat (2048-byte chunks)...
  │  FILE_END|name ──► ─────────────────────────────► rename .tmp → files/name
```

**Queueing (when the server is busy):** if 2 files are already in flight or their
combined size would exceed 1 MB, the server replies
`FILE_WAIT|filename|position|size` and holds the request in a FIFO queue. The
client shows "queued at position N" and keeps `send_state = 1`; the moment a slot
frees (on `FILE_END`, `FILE_REJECT`, 30 s timeout, or disconnect) the queue is
promoted and the client receives a normal `FILE_GRANTED` to start streaming. This
prevents the server being overwhelmed by simultaneous uploads — excess requests
**wait** instead of being dropped, and are themselves denied/timed out only if the
queue is full or they wait longer than 120 s.

Key security detail: the sender must present the **16-char token** in every
`FILE_DATA`, so random clients cannot inject file chunks into another transfer.

---

## 8. Tests

### `connect_cli/tests/smoke.py` (and `connect_gui/tests/smoke.py`)

Raw-socket integration test, no C code needed:

| Function | What it does |
|----------|--------------|
| `run()` | Runs `make clean && make` |
| `wait_for_server()` | Polls TCP connect until the server is listening |
| `class Client` | Thin socket wrapper: `send()` a protocol line, `read_lines()` (with `select`), `expect()` (wait for a line starting with a prefix), `drain()`, `close()` |
| `main()` | Writes test accounts, starts the server, then asserts: two logins OK → user list contains both → `general` room exists → public message reaches the other client → private message works → after logout the user disappears from `USERS` |

`connect_gui/tests/file_transfer.py` additionally exercises the file send flow.

---

## 9. Build System

- `connect_cli/Makefile`: compiles with `gcc -Wall -Wextra -O2 -g`, produces
  `bin/chatclient` (client.o + net.o + tui.o + protocol.o) and
  `bin/chatserver` (+ room.o, room_access.o, logger.o, sha256.o, `-lpthread`).
- `connect_gui/Makefile`: same, but the client links GTK3 (`pkg-config gtk+-3.0`)
  and the server stays dependency-free.

---

## 10. Quick Viva Cheat Sheet

- **What did you build?** A LAN chat app in C — threaded TCP server + two
  clients (GTK GUI and a raw-terminal TUI), custom SHA-256, no external deps
  (except GTK for the GUI).
- **Concurrency?** Server: thread-per-client, mutex-protected shared lists.
  CLI client: single-threaded `select()` loop. GUI client: receive thread +
  GTK main thread, bridged by `g_idle_add`.
- **How do clients talk?** Newline-terminated `TYPE|field|…` lines over TCP port 8080.
- **File transfer?** Offer/accept handshake + token authorization + base64
  chunks (2 KB), reassembled in `files/`.
- **Passwords?** Stored as SHA-256 hex hashes (CLI server), plaintext (GUI
  server) — a known trade-off.
- **Security features?** Filename sanitization, upload tokens, duplicate-login
  rejection, admin-only commands, mutex discipline to avoid races/deadlocks.
- **Limitations?** No encryption (plain text on the wire), no persistence for
  runtime-created rooms/users/history, no field escaping (`|` forbidden in text).
