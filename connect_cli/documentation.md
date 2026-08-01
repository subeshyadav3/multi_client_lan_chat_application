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
│   │   ├── client.c / client.h  # Protocol dispatch, commands, file transfer
│   │   ├── net.c / net.h        # Socket helpers
│   │   └── tui.c                # Raw-mode terminal drawing (ANSI)
│   ├── server/                  # Threaded TCP server
│   │   ├── server.c             # accept() loop + command dispatch
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

### 3.1 `server.c` — everything the server does

**Data structures**
- `UploadSlot` (line 27): a granted "permission to upload" — token, sender,
  filename, recipient, size, timestamp, active flag. Only 5 concurrent uploads.
- `Client` (line 54): one per socket — fd, address, username, room, admin flag,
  thread id, linked-list `next`.
- `UserAccount` (line 77): username/password/active for normal users.
- `RoomHistory` (line 93): per-room ring of the last 50 message lines.
- `FileTransfer` (line 157): record of a pending offer (sender, filename,
  recipient, size).

Global state is protected by named mutexes: `client_mutex`, `user_mutex`,
`upload_mutex`, `transfer_mutex` (+ per-history `lock`).

**Housekeeping helpers**

| Function | Line | What it does |
|----------|------|--------------|
| `upload_expire_stale()` | 42 | Every second in the main loop, deactivate upload slots older than 30 s so dead senders don't block the 5 slots |
| `history_init()` | 103 | Reset all room histories |
| `history_for_room()` | 108 | Look up a room's history or create one (max 32) |
| `history_add()` | 122 | Append a message line; drop oldest when count > 50 |
| `history_replay()` | 146 | Send all stored lines of a room to a fresh joiner (`/history`, late joiners) |
| `transfer_add()` / `transfer_remove()` / `transfer_find()` | 168/182/198 | Insert/delete/lookup a pending file offer in the transfer list |
| `sanitize_filename()` | 208 | Strip `/ \ | \n \r \0` from a filename (path-traversal protection) |
| `get_timestamp()` | 219 | Current time as `"02:30 PM"` (12-hour) |

**User / account management**

| Function | Line | What it does |
|----------|------|--------------|
| `account_remove()` | 224 | Delete a `UserAccount` node from the list |
| `user_exists()` | 238 | Is a username already registered? |
| `is_hex64()` | 247 | Is a string a 64-char hex value (i.e. already a SHA-256 hash)? |
| `user_create_plain()` | 258 | Add account storing the password exactly as given |
| `user_create()` | 271 | Add account but store `sha256_hex(password)` instead |
| `user_reset_pass()` | 277 | Set a new (hashed) password for a user |
| `user_validate()` | 289 | Hash the given password and compare to the stored hash |
| `save_users()` | 301 | Rewrite `config/users.cred` with `user:hash` lines |
| `load_users()` | 311 | Read `users.cred`; hashes any plaintext entries (auto-upgrades the file) |

**Networking / delivery helpers**

| Function | Line | What it does |
|----------|------|--------------|
| `safe_send()` | 331 | `send()` with error checking — marks client inactive if it fails (broken pipe handling) |
| `broadcast_room()` | 341 | Send a line to every active client currently in a given room |
| `broadcast()` | 351 | Send a line to *every* active client |
| `send_to_user()` | 361 | Send a line to one specific username |
| `client_find()` | 374 | Look up a `Client` by username (caller must hold `client_mutex`) |
| `client_remove()` | 382 | Unlink + free a `Client` and close its socket |
| `broadcast_user_list()` | 397 | Build `USERS|a:1,b:1,…` and send to everyone (keeps sidebars in sync) |
| `broadcast_room_list()` | 412 | Build `ROOMS|r1,r2:…` and broadcast |
| `send_status_to()` | 420 | Admin-only stats: online users, message/file counters |
| `finish_join()` | 433 | Set a client's room, replay history, send `JOIN_OK`, notify the room |

**`handle_client()` — the per-client thread (line 444)**

This is the heart of the server. Each client gets its own detached thread running
this loop:

1. `recv()` up to `BUFFER_SIZE` bytes (line 451).
2. Split the stream into newline-terminated lines (lines 455–1024).
3. For each line, split on `|` into up to 5 tokens (preserving empty fields —
   important for `CREATE_ROOM` with empty description) (lines 461–483).
4. **Dispatch on `cmd`** — the first token. Major branches:

| Command | Line | Behaviour |
|---------|------|-----------|
| `LOGIN` | 487 | Reject duplicates; admin (`admin` user) validated against hashed admin password, normal users against `user_validate()`. On success: mark active, join `general`, replay history, broadcast user/room lists |
| `PUBLIC` | 542 | Store in room history and broadcast to the sender's room |
| `PRIVATE` | 549 | Deliver to both recipient and sender (both see the conversation) |
| `TYPING` | 556 | Broadcast "X is typing" to the room (except sender) |
| `JOIN` | 560 | Check room exists; if protected, require password once, then `room_grant_access()`; `finish_join()` |
| `LEAVE` | 583 | Return to `general`, notify old room |
| `CREATE` | 591 | Simple room creation |
| `CREATE_ROOM` | 598 | Extended creation with title/description/password |
| `DELETE_ROOM` | 615 | Only creator or admin; boot members back to `general` |
| `UPDATE_ROOM` | 633 | Change title/description/password |
| `LIST_USERS` / `LIST_ROOMS` | 643/657 | Send the lists to the requester |
| `WHO` | 663 | List users currently in a room |
| `HISTORY` | 686 | Replay current room history (no cross-room leak) |
| `STATS` | 689 | Admin only |
| `ANNOUNCE` | 697 | Admin only — broadcast to all |
| `KICK` | 708 | Admin only — send `KICK`, shut the socket down |
| `CREATE_USER` | 731 | Admin only — add account, persist |
| `DELETE_USER` | 752 | Admin only — kick if online, remove account, persist |
| `RESET_PASS` | 784 | Admin only — new hashed password |
| `LIST_ACCOUNTS` | 805 | Admin only |
| `FILE_REQUEST` | 822 | Validate size, allocate an upload slot + random 16-hex token, reply `FILE_GRANTED`, forward `FILE_OFFER` to target (or room) |
| `FILE_OFFER` | 884 | Legacy direct offer path (no slot) |
| `FILE_DATA` | 894 | Forward one base64 chunk to the recipient, but only if the embedded token matches the slot (authorization) |
| `FILE_END` | 946 | Notify recipient transfer complete, free slot + transfer record |
| `FILE_ACCEPT` | 970 | Tell sender the recipient accepted (starts streaming) |
| `FILE_REJECT` | 988 | Tell sender, free slot + record |
| `LOGOUT` | 1015 | Mark inactive and break |

5. **Cleanup on disconnect** (lines 1028–1066): remove from client list, free
   any upload slots / transfer records owned by the user, broadcast updated user
   list and a "disconnected" notice.

**Server lifecycle**

| Function | Line | What it does |
|----------|------|--------------|
| `server_shutdown()` | 1068 | Set running flag off, close all sockets, free all clients, close logger, free rooms/access |
| `signal_handler()` | 1085 | `SIGINT`/`SIGTERM` → set flag + close listening socket (async-signal-safe: no locks) |
| `main()` | 1094 | Parse optional port arg; load `admin.cred`, hash admin password; init logger/rooms/users/history; create socket, `SO_REUSEADDR`, `bind`, `listen`; then an **event loop** using `select()` with 1 s timeout so it can run `upload_expire_stale()` as housekeeping; `accept()` each connection and spawn a detached `handle_client()` thread |

*Viva point — concurrency model:* one thread per client, shared state guarded by
mutexes. The main thread only accepts; heavy work happens in client threads.
Handlers never call a broadcast while holding `client_mutex` (avoids deadlock).

### 3.2 `room.c` / `room.h` — rooms

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

### 4.2 `client.c` — protocol + commands + file transfer

**Parsing / encoding helpers**

| Function | Line | What it does |
|----------|------|--------------|
| `parse_pipe()` | 20 | Split a protocol line on `\|` into up to `max` fields |
| `after_pipes()` | 37 | Return pointer to text after the Nth `\|` (used to grab base64) |
| `b64tab` / `b64encode()` | 48/50 | Encode raw bytes to base64 (3→4 chars, `=` padding) |
| `b64val()` / `b64decode()` | 65/74 | Decode base64 back to raw bytes |
| `update_user_list()` / `update_room_list()` | 91/108 | Parse the CSV user/room lists from `USERS`/`ROOMS` into the App arrays |
| `request_lists()` | 125 | Send `LIST_USERS` + `LIST_ROOMS` (keeps sidebar fresh) |
| `path_basename()` | 129 | Strip directory part from a path |

**File send**

| Function | Line | What it does |
|----------|------|--------------|
| `request_send_file()` | 137 | `stat()` the file (must be a regular file, ≤ 64 MB), set send state=1, send `FILE_REQUEST|name|size|target` |
| `find_offer()` / `add_offer()` / `remove_offer()` | 331/340/358 | Manage the local list of incoming offers (shown as `[1] alice offers 'x' …`) |
| `finalize_received()` | 371 | Close `.tmp` file, pick a unique name (` (n)` suffix if taken), `rename()` into `files/` |
| `try_send_chunk()` | 538 | Called each loop when streaming (state 3): read up to 2048 raw bytes, base64-encode, send `FILE_DATA|filename|token|b64`, send `FILE_END` when done |

**`cmd_process()` — slash-command handler (line 161)**

Parses `/command args` and sends the matching protocol line:

| Command | Line | Sends |
|---------|------|-------|
| `/help` | 166 | prints command list |
| `/quit`, `/exit` | 177 | `LOGOUT` + exit |
| `/logout` | 180 | `LOGOUT`, waits for reconnect |
| `/msg <u> <t>` | 188 | `PRIVATE\|u\|t` |
| `/join <room> [pw]` | 198 | `JOIN\|room\|pw` |
| `/leave` | 207 | `LEAVE\|general` |
| `/create <room>` | 209 | `CREATE\|room` |
| `/createroom` | 216 | `CREATE_ROOM\|name\|title\|desc\|pw` |
| `/rooms` / `/users` | 224/242 | `LIST_ROOMS` / `LIST_USERS` |
| `/who [room]` | 226 | `WHO\|room` |
| `/history` | 232 | `HISTORY` |
| `/deleteroom <r>` | 235 | `DELETE_ROOM\|r` |
| `/clear` | 244 | clears local chat buffer |
| `/typing` | 246 | `TYPING\|general` |
| `/stats` | 248 | `STATS` |
| `/announce <t>` | 250 | `ANNOUNCE\|t` |
| `/kick <u> <why>` | 255 | `KICK\|u\|why` |
| `/createuser <u> <p>` | 262 | `CREATE_USER\|u\|p` |
| `/deleteuser <u>` | 269 | `DELETE_USER\|u` |
| `/resetpass <u> <p>` | 276 | `RESET_PASS\|u\|p` |
| `/accounts` | 283 | `LIST_ACCOUNTS` |
| `/sendfile [@u] <path>` | 285 | `request_send_file()` |
| `/accept <n>` / `/reject <n>` | 295/309 | `FILE_ACCEPT` / `FILE_REJECT` by offer number |

**`handle_line()` — incoming message dispatcher (line 399)**

The inverse of `cmd_process`. Parses the first field and reacts:

| Message | Line | Effect |
|---------|------|--------|
| `LOGIN_OK` | 403 | mark logged in, set room, request lists |
| `LOGIN_FAIL` | 413 | show reason, go back to password step |
| `PUBLIC` | 418 | print `[time] user: text` (normal line) |
| `PRIVATE` | 423 | print as PM (magenta) |
| `NOTIFY` / `ANNOUNCE` | 428/433 | system / admin notices |
| `TYPING` | 440 | record who is typing (shown in status bar) |
| `USERS` / `ROOMS` | 446/448 | refresh sidebar lists |
| `JOIN_OK` / `JOIN_FAIL` | 450/457 | room change result |
| `ROOM_CREATED` | 459 | success notice |
| `STATUS` / `ACCOUNT_LIST` | 462/464 | info lines |
| `KICK` | 466 | show reason, disconnect |
| `ERROR` | 469 | show error |
| `FILE_GRANTED` | 471 | store token, wait for recipient accept (state 2) |
| `FILE_DENIED` | 478 | show reason, abort |
| `FILE_OFFER` | 482 | push a pending offer |
| `FILE_ACCEPT` | 484 | recipient accepted → open file, start streaming (state 3) |
| `FILE_REJECT` | 496 | recipient declined → abort |
| `FILE_DATA` | 500 | decode + append chunk to `files/name.tmp`, update progress |
| `FILE_END` | 532 | finalize received file |

**Input loop helpers**

| Function | Line | What it does |
|----------|------|--------------|
| `push_history()` | 564 | Store typed line in history ring (max 100, dedupes consecutive) |
| `send_typing()` | 579 | Send `TYPING` while typing (throttled by the main loop) |
| `process_input()` | 586 | If not logged in → login flow (username → masked password → `LOGIN`). Else `/cmd` → `cmd_process`, plain text → `PUBLIC` |
| `client_quit()` | 628 | Restore terminal, close socket, exit |
| `on_signal()` | 634 | `SIGINT`/`SIGTERM` set a flag to exit the loop cleanly |
| `main()` | 636 | Parse `--host/--port/--user/--pass/--admin` (with positional fallback); enter raw mode; **reconnect loop** (re-logs-in after `/logout`); inside: `select()` on stdin+socket with 100 ms timeout; keystrokes handled (Enter, backspace, arrows for history, Ctrl-C); on socket data, assemble lines → `handle_line()`; every iteration calls `try_send_chunk()` + `tui_draw()` so the UI stays live while uploading |

*Viva point — single-threaded client:* everything (network, keyboard, drawing,
file streaming) lives in ONE `select()` loop, so the interface never freezes.

### 4.3 `net.c` — socket helpers

| Function | Line | What it does |
|----------|------|--------------|
| `net_connect()` | 13 | `gethostbyname()` → `socket()` → `connect()`; returns fd or -1 |
| `net_send_line()` | 32 | Send a whole line + `\n`, looping until fully sent (partial-write safe) |
| `net_close()` | 49 | `shutdown()` + `close()` |

### 4.4 `tui.c` — terminal user interface

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
`FILE_DENIED`, `FILE_OFFER`, `FILE_ACCEPT`, `FILE_REJECT`, `FILE_DATA`,
`FILE_END`.

---

## 7. File Transfer Flow (step by step)

```
Sender                     Server                        Recipient
  │  FILE_REQUEST|name|size|target ──►
  │                     allocates slot + token
  │  ◄── FILE_GRANTED|sender|name|token|size
  │  ◄── FILE_OFFER|sender|name|size|target ────────► shows [1] offer
  │                                                    /accept or /reject
  │  ◄── FILE_ACCEPT|recipient|name ◄──────────────── /
  │  FILE_DATA|name|token|<b64 chunk> ──► (validates token) ──► decode+append
  │  ...repeat (2048-byte chunks)...
  │  FILE_END|name ──► ─────────────────────────────► rename .tmp → files/name
```

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
