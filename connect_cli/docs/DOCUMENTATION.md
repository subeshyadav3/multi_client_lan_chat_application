# ConnectHub v2 — Documentation

A terminal (CLI/TUI) chat application written in C with no external dependencies:
threaded TCP server + a raw-ANSI/`termios` client driven by `select()`.

This document describes the architecture, the wire protocol, and the TUI
design. It mirrors the original ConnectHub feature set so it can be used for
the same Systems Programming coursework.

## 1. Architecture

```
                ┌──────────────┐
                │    Server     │  1 process, thread-per-client (pthreads),
                │  (1 process,  │  mutex-protected shared lists
                │  N threads)   │
                └──┬───┬───┬────┘
                   │   │   │  TCP sockets (port 8080)
              ┌────┘   │   └────┐
              ▼        ▼        ▼
          Client 1  Client 2  Client 3     raw termios TUI + select() loop
```

### 1.1 Server (`bin/chatserver`)

- `main()` loads `config/admin.cred`, seeds rooms/users, binds & listens on
  port `8080`, then accepts connections.
- Each accepted socket becomes a `Client` pushed onto a global linked list
  (`client_mutex` protects it) and gets its own **detached** pthread running
  `handle_client()`.
- `handle_client()` reads newline-delimited lines, splits on `|`, and dispatches
  commands. Replies/broadcasts are sent with `safe_send()` which marks a client
  inactive on failure.
- Shared data (`client_mutex`, `user_mutex`, `room_mutex`, `transfer_mutex`,
  `upload_mutex`) prevents race conditions. Handlers never hold `client_mutex`
  while calling a broadcast.

### 1.2 Client (`bin/chatclient`)

- Single-threaded and **driven by `select()`**: one loop watches `STDIN_FILENO`
  (keyboard) and the socket. File-transfer chunks are pushed in the same loop,
  so the UI stays responsive while uploading.
- Terminal is put in **raw mode** (`termios`: `ICANON`/`ECHO` off, `VMIN=1`)
  so each keystroke is readable immediately; it is restored on exit.
- All drawing uses **ANSI escape codes** (cursor move, colors, `?25l/h` cursor
  hiding, `7m` reverse-video bars) into a chat area, a right sidebar (online
  users + rooms), a status bar, and an input line.
- Command history is stored, navigable with `↑`/`↓`.

## 2. Wire protocol

All messages are plain-text, newline-terminated lines:

```
TYPE|field1|field2|...
```

The pipe `|` separates fields; fields must not contain `|` or newlines.

### 2.1 Client → Server

| Message | Fields | Description |
|---------|--------|-------------|
| `LOGIN` | `username\|password` | Authenticate (required lookup). |
| `PUBLIC` | `text` | Send to the sender's current room. |
| `PRIVATE` | `recipient\|text` | Private message (both parties get it). |
| `TYPING` | `room` | Typing indicator. |
| `JOIN` | `room[|password]` | Join a room. |
| `LEAVE` | `room` | Return to `general`. |
| `CREATE` | `room` | Create a simple room. |
| `CREATE_ROOM` | `name\|title\|desc\|password` | Create a detailed/protected room. |
| `DELETE_ROOM` | `name` | Delete a room (creator or admin). |
| `UPDATE_ROOM` | `name\|field\|value` | Update title/description/password. |
| `LIST_USERS` / `LIST_ROOMS` | — | Request lists. |
| `STATS` | — | Admin: server statistics. |
| `ANNOUNCE` | `text` | Admin: broadcast announcement. |
| `KICK` | `username\|reason` | Admin: kick a user. |
| `CREATE_USER` | `u\|p` | Admin: create account. |
| `DELETE_USER` | `u` | Admin: delete account. |
| `RESET_PASS` | `u\|p` | Admin: reset password. |
| `LIST_ACCOUNTS` | — | Admin: list accounts. |
| `FILE_REQUEST` | `filename\|size\|target` | Request to send (target empty = room). |
| `FILE_DATA` | `filename\|token\|base64` | One chunk of file data. |
| `FILE_END` | `filename` | End of file transfer. |
| `FILE_ACCEPT` | `sender\|filename` | Accept an incoming offer. |
| `FILE_REJECT` | `sender\|filename\|reason` | Decline an incoming offer. |
| `LOGOUT` | — | Clean disconnect. |

### 2.2 Server → Client

| Message | Fields | Description |
|---------|--------|-------------|
| `LOGIN_OK` | `username` | Accepted. |
| `LOGIN_FAIL` | `reason` | Rejected. |
| `PUBLIC` | `room\|sender\|text\|timestamp` | Public chat message. |
| `PRIVATE` | `sender\|recipient\|text\|timestamp` | Private message. |
| `NOTIFY` | `text` | System/user notification. |
| `ANNOUNCE` | `sender\|text\|timestamp` | Admin announcement. |
| `TYPING` | `room\|username` | Typing indicator. |
| `USERS` | `a:1,b:1` | Comma-separated online users. |
| `ROOMS` | `r1,r2` | Comma-separated rooms. |
| `JOIN_OK` / `JOIN_FAIL` | `room` / `reason` | Join result. |
| `ROOM_CREATED` | `name` | Room created. |
| `STATUS` | `text` | Stats/admin info. |
| `ACCOUNT_LIST` | `list` | Account usernames. |
| `KICK` | `reason` | This client was kicked. |
| `FILE_GRANTED` | `sender\|filename\|token\|size` | Upload slot granted. |
| `FILE_DENIED` | `filename\|sender\|reason` | Upload rejected. |
| `FILE_OFFER` | `sender\|filename\|size\|target` | Incoming offer. |
| `FILE_ACCEPT` | `recipient\|filename` | Recipient accepted. |
| `FILE_REJECT` | `recipient\|filename\|reason` | Recipient declined. |
| `FILE_DATA` | `sender\|filename\|base64` | Incoming chunk. |
| `FILE_END` | `sender\|filename` | Transfer complete. |
| `ERROR` | `reason` | Generic error. |

### 2.3 Notes

- Timestamps are `%I:%M %p` (e.g. `02:30 PM`).
- Public messages are delivered to everyone in the sender's room except the
  sender.
- `FILE_DATA` is base64-encoded; raw bytes are chunked at `FILE_CHUNK_SIZE`
  (2 KB) before encoding so each line stays under `BUFFER_SIZE`.
- Room history is kept per room (50 lines) and replayed when a user joins.

## 3. File transfer flow

Sender → Server → Recipient:

1. Sender: `FILE_REQUEST|name|size|target`.
2. Server allocates an upload slot + token and replies `FILE_GRANTED`, and
   forwards `FILE_OFFER` to the recipient.
3. Recipient chooses `/accept` or `/reject`.
   - Accept → server forwards `FILE_ACCEPT` to sender; sender starts streaming
     `FILE_DATA|name|token|<b64>` chunks, then `FILE_END|name`.
   - Reject → server forwards `FILE_REJECT` and frees the slot.
4. Recipient appends decoded chunks to `files/<name>.tmp` and renames it on
   `FILE_END` (appending ` (n)` if the name exists).

## 4. Logging

The server writes to `logs/server.log`:

```
[2026-08-01 10:40:00] [INFO] Server started on port 8080
[2026-08-01 10:40:05] [INFO] User 'alice' logged in from 127.0.0.1
[2026-08-01 10:40:09] [MSG] [general] alice: hello
```

Levels: `INFO`, `MSG`, `PRIV`, `CTRL`, `FILE`.

## 5. Security / limitations

- No encryption; plain-text on the wire (academic project).
- Admin is whoever logs in as the `/admin` credential (default `admin`).
- Passwords are stored in plain text in `config/*.cred`.
- Filenames are sanitized against path separators; fields cannot contain `|`.
- No persistence across server restarts for rooms/history/accounts created at
  runtime (only `config/*.cred` seeds on start).
- Intended for a trusted LAN demo, not production.

