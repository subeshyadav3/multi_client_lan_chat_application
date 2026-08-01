# ConnectHub — Detailed Technical Documentation

## 1. Introduction

ConnectHub is a multi-client LAN chat application written in C. It uses TCP sockets for transport, POSIX threads (`pthreads`) for concurrency, and GTK3 for the client user interface. The project is intentionally small and self-contained: there are no runtime dependencies beyond a standard POSIX environment, GTK3, and `pkg-config` at build time.

### 1.1 What this document covers

- Overall architecture and module responsibilities
- The line-based wire protocol
- Build system and flags
- How to run the server, the client, and the integration test
- Chat commands and admin features
- File transfer implementation
- Logging, error handling, and known limitations
- Troubleshooting common problems

## 2. Project Layout

```
ConnectHub/
├── bin/                # Compiled executables (created by make)
├── build/              # Object files (created by make)
├── client/             # GTK client
│   ├── client.c        # Message dispatch / main()
│   ├── chat.c          # Network I/O thread
│   ├── chat.h          # Client network API
│   ├── ui.c            # GTK windows, widgets, styling
│   └── ui.h            # UI public API
├── server/             # Chat server
│   ├── server.c        # accept() loop, client threads, protocol logic
│   ├── room.c / room.h # Room list management
│   └── logger.c / .h   # File-based server logging
├── shared/             # Code shared by client and server
│   ├── constants.h     # Buffer sizes, port, limits
│   ├── protocol.c / .h # Protocol formatter helpers (mostly unused at runtime)
├── tests/
│   └── smoke.py        # Integration smoke test
├── logs/               # server.log (created at runtime)
├── files/              # Received files (created at runtime)
├── Makefile
└── README.md
```

## 3. Architecture

### 3.1 Server (`bin/chatserver`)

The server is a traditional threaded TCP server:

1. `main()` initializes the logger and room list, creates a listening socket on `PORT` (8080), and enters an `accept()` loop.
2. For every incoming connection a `Client` struct is allocated and added to a global linked list protected by `client_mutex`.
3. A detached POSIX thread (`handle_client`) is spawned for each client. The thread reads newline-delimited messages, dispatches commands, and sends replies/broadcasts.
4. When a client disconnects, the thread marks the client inactive, broadcasts a leave notification, closes the socket, and removes the struct from the list.

Key design points:

- **Mutex usage:** `client_mutex` protects the client linked list. `broadcast()`, `broadcast_room()`, `send_to_user()`, and `client_find()` all acquire this lock. To avoid deadlock, command handlers never call `broadcast()` while holding `client_mutex`.
- **Detached threads:** Client threads are detached so the server never has to join them.
- **Graceful shutdown:** `SIGINT` / `SIGTERM` close the listening socket; `server_shutdown()` frees the client list.

### 3.2 Client (`bin/chatclient`)

The client is a single-threaded GTK application plus one background receive thread:

1. `main()` initializes GTK/UI, registers message callbacks, shows the login window, and runs `gtk_main()`.
2. When the user clicks **Connect**, `client_connect()` resolves the host, opens a TCP socket, and starts `client_receive_loop()` in a background thread.
3. The receive thread reads lines from the server and calls `on_message_received()` in `client.c`, which parses the line and forwards it to the UI.
4. UI callbacks (`on_send`, `on_file`, etc.) call the network helpers in `chat.c` to send commands.
5. Closing the window triggers `client_disconnect()` and quits GTK.

### 3.3 Shared code

- `constants.h` defines sizes such as `BUFFER_SIZE 4096`, `MAX_MESSAGE 2048`, `MAX_USERNAME 32`, `MAX_ROOM_NAME 64`, and the default `PORT 8080`.
- `protocol.c` provides static-buffer format helpers and a `parse_message()` function. Most message formatting is done inline in the server/client for simplicity.

## 4. Wire Protocol

All messages are plain-text, newline-terminated (`\n`) strings of the form:

```
TYPE|field1|field2|...
```

The pipe character `|` is the field separator. There is no escaping, so fields must not contain `|` or newlines.

### 4.1 Client → Server messages

| Message | Fields | Description |
|---------|--------|-------------|
| `LOGIN` | `username` | Authenticate with a username. Rejected if duplicate or invalid. |
| `PUBLIC` | `room\|text` | Send a public message to the sender's current room. |
| `PRIVATE` | `recipient\|text` | Send a private message to another user. |
| `TYPING` | `room` | Notify others that the user is typing. |
| `JOIN` | `room` | Change the sender's current room. |
| `LEAVE` | `room` | Reset the sender's current room to `general`. |
| `CREATE` | `room` | Create a new room. |
| `LIST_USERS` | none | Request the online user list. |
| `LIST_ROOMS` | none | Request the room list. |
| `STATS` | none | Admin only: request server statistics. |
| `ANNOUNCE` | `text` | Admin only: broadcast an announcement. |
| `KICK` | `username\|reason` | Admin only: kick a user. |
| `FILE_OFFER` | `filename\|size\|target` | Offer a file. `target` empty = broadcast. |
| `FILE_DATA` | `filename\|base64chunk` | One chunk of file data. |
| `FILE_END` | `filename` | End of file transfer. |
| `FILE_REJECT` | `sender\|filename\|reason` | Decline an incoming file offer. |
| `FILE_ACCEPT` | `sender\|filename` | Accept an incoming file offer. |
| `LOGOUT` | none | Disconnect cleanly. |

### 4.2 Server → Client messages

| Message | Fields | Description |
|---------|--------|-------------|
| `LOGIN_OK` | `username` | Login accepted. |
| `LOGIN_FAIL` | `reason` | Login rejected. |
| `PUBLIC` | `room\|sender\|text\|timestamp` | Public chat message. |
| `PRIVATE` | `sender\|recipient\|text\|timestamp` | Private message. |
| `NOTIFY` | `text` | Server/user notification (joins, leaves, room events). |
| `ANNOUNCE` | `sender\|text\|timestamp` | Administrator announcement. |
| `USERS` | `user1:1,user2:1` | Comma-separated online users. |
| `ROOMS` | `room1,room2` | Comma-separated room names. |
| `TYPING` | `room\|username` | Typing indicator. |
| `FILE_OFFER` | `sender\|filename\|size\|target` | Incoming file offer. |
| `FILE_DATA` | `sender\|filename\|base64chunk` | Incoming file chunk. |
| `FILE_END` | `sender\|filename` | File transfer complete. |
| `FILE_REJECT` | `recipient\|filename\|reason` | File offer was rejected by recipient. |
| `KICK` | `reason` | Kicked by administrator. |

### 4.3 Protocol notes

- Timestamps are formatted as `%I:%M %p` (e.g. `02:30 PM`).
- User list entries append `:1` as a placeholder status value; the UI strips it.
- Public messages are broadcast to all clients in the same room **except** the sender.
- Private messages are delivered to both the recipient and the sender so each inbox shows the conversation.
- `FILE_DATA` chunks are encoded with GLib `g_base64_encode`. The receiver appends each chunk to `files/<filename>.tmp` and renames it on `FILE_END`.

## 5. Build System

The `Makefile` produces two binaries:

```
make clean && make
```

### 5.1 Targets

| Target | Description |
|--------|-------------|
| `all` | Build `bin/chatclient` and `bin/chatserver`. |
| `directories` | Create `build/`, `bin/`, `logs/`, `files/`. |
| `clean` | Remove `build/` and `bin/`. |

### 5.2 Compiler flags

- **Common:** `-Wall -Wextra -O2`
- **Client only:** `pkg-config --cflags gtk+-3.0` and `pkg-config --libs gtk+-3.0`
- **Server only:** `-lpthread`

The server no longer links GTK libraries, which keeps the server binary smaller and avoids unnecessary GUI dependencies on a headless host.

## 6. Running the Application

### 6.1 Server

```bash
./bin/chatserver
```

Output:

```
[SERVER] Listening on port 8080
```

The server writes logs to `logs/server.log`.

### 6.2 Client

```bash
./bin/chatclient
```

A login window appears. Defaults:

- **Username:** (empty)
- **Host:** `127.0.0.1`
- **Port:** `8080`

Enter a unique username and click **Connect**. If the server is reachable, the main chat window opens.

### 6.3 Running on two machines

1. Start the server on machine A:

   ```bash
   ./bin/chatserver
   ```

2. On machine B (and/or A), run the client and set **Host** to machine A's IP address:

   ```bash
   ./bin/chatclient
   # Host: 192.168.1.10   Port: 8080
   ```

3. Make sure the firewall on machine A allows TCP port 8080.

## 7. Chat Commands

Commands are typed into the message entry at the bottom of the main window.

| Command | Example | Effect |
|---------|---------|--------|
| `/msg` | `/msg alice hello` | Send a private message to `alice`. |
| `/join` | `/join dev` | Join the `dev` room. |
| `/create` | `/create dev` | Create a new room called `dev`. |
| `/announce` | `/announce Server restart at noon` | Admin only: broadcast to everyone. |
| `/kick` | `/kick bob spam` | Admin only: kick `bob` with reason `spam`. |

Any text that does not start with `/` is sent as a public message to the current room.

### 7.1 Admin mode

A user whose username is exactly `admin` is marked as an administrator on login and can use `/announce` and `/kick`. The `STATS` command also requires admin status.

## 8. File Transfer

### 8.1 Sending a file

1. Click the **File** button in the main window.
2. Choose a file.
3. The client sends:
   - `FILE_OFFER|filename|size|`
   - `FILE_DATA|filename|<base64-chunk>` (repeated)
   - `FILE_END|filename`

### 8.2 Receiving a file

When the client sees a `FILE_OFFER`, it shows a notification. Each `FILE_DATA` chunk is base64-decoded and appended to `files/<filename>.tmp`. When `FILE_END` arrives, the temporary file is renamed to `files/<filename>` and a notification is displayed.

### 8.3 File-transfer improvements

- The recipient sees an accept/reject dialog for each incoming file offer.
- Accepted files are written to `files/<filename>`; if the name already exists, a unique suffix such as ` (1)` is appended automatically.
- File data is routed only to the intended recipient (or broadcast if no target was specified), so unrelated clients no longer receive the chunks.
- Transfer progress is shown as a percentage notification.
- Received files appear in the sidebar Files list with an **Open** button.

### 8.4 Limitations

- Very large files are split into ~2 KB raw chunks before base64 encoding, so the final line stays under `BUFFER_SIZE`.
- The server does not persist files; they are stored only on the recipient's local disk.

## 9. Logging

The server logs to `logs/server.log` in append mode. Example:

```
[2026-07-04 10:45:12] [INFO] Server started on port 8080
[2026-07-04 10:45:15] [INFO] User 'alice' logged in from 127.0.0.1
[2026-07-04 10:45:20] [MSG] [general] alice: hello
```

Log levels:

- `INFO` — server lifecycle and connect/disconnect events
- `MSG` — public messages
- `PRIV` — private messages
- `CTRL` — announcements and kicks
- `FILE` — file offers

## 10. Testing

A Python integration test is included in `tests/smoke.py`.

### 10.1 What the test checks

1. `make clean && make` succeeds.
2. The server starts and listens on port 8080.
3. Two clients can log in as `alice` and `bob`.
4. `LIST_USERS` returns both users.
5. `LIST_ROOMS` contains `general`.
6. A public message from `alice` reaches `bob`.
7. A private message from `bob` reaches `alice`.
8. After `bob` logs out, `LIST_USERS` no longer contains `bob`.

### 10.2 Running the test

```bash
python3 tests/smoke.py
```

Expected output:

```
SMOKE TEST PASSED
--- server output ---
[SERVER] Listening on port 8080
[SERVER] Shutting down...
```

## 11. Error Handling

### 11.1 Client

- **Connection failure:** The login window shows "Connection failed".
- **Duplicate username:** The server replies `LOGIN_FAIL|Username already exists or invalid` and the UI displays the error.
- **Unexpected disconnect:** The receive thread detects a closed socket and shows a notification in the chat area.
- **Thread safety:** `client_disconnect()` only cancels/joins the receive thread if it was actually created.

### 11.2 Server

- **Invalid/dup username:** Rejected with `LOGIN_FAIL`.
- **Client disconnect:** The handler thread sets the client inactive, broadcasts a leave notification, and frees the struct.
- **Mutex safety:** `client_find()` is called only while the caller holds `client_mutex`, preventing the deadlock that existed in earlier versions.

## 12. Security Considerations

ConnectHub is an academic project and is **not hardened for production** or untrusted networks:

- No encryption — all messages and file data travel in plain text.
- No authentication beyond a self-declared username.
- Admin status is granted simply by logging in as `admin`.
- File transfers write directly to `files/` without sanitizing filenames.
- Input is not rigorously validated; very long lines may be truncated.

For a classroom or trusted LAN demo these limitations are acceptable, but a real deployment would need TLS, proper authentication, path sanitization, and rate limiting.

## 13. Known Limitations

- The protocol has no field escaping, so messages cannot contain `|` or newlines.
- User status values are hard-coded to `:1`.
- There is no persistence: rooms, users, and message history disappear when the server restarts.
- The GTK client requires an X11/Wayland display and cannot run headlessly.
- `/stats` is only available to the `admin` user.

## 14. Troubleshooting

### 14.1 `make` fails with GTK errors

Install GTK3 development headers:

```bash
sudo apt-get install libgtk-3-dev     # Debian/Ubuntu
sudo dnf install gtk3-devel           # Fedora
```

### 14.2 Server says `bind: Address already in use`

Another process (maybe a previous server) is using port 8080. Find and stop it:

```bash
sudo lsof -i :8080
# or
pkill -f chatserver
```

### 14.3 Client cannot connect

- Verify the server is running.
- Check that the host/port in the login window match the server's address.
- Check firewall rules between the machines.

### 14.4 Two clients with the same username

The server rejects the second login with `LOGIN_FAIL|Username already exists or invalid`.

### 14.5 Smoke test fails

Run it with full output to see the failing assertion:

```bash
python3 tests/smoke.py
```

Common causes: another server on port 8080, or a stale `chatserver` process.

## 15. Developer Notes

- The `shared/protocol.c` format helpers are kept for reference but are not thread-safe (they use static buffers). The live code formats messages inline with `snprintf`.
- `pthread_detach()` is used so the main thread never joins client handler threads.
- GTK CSS is loaded at startup in `ui_init()`; the provider is added to the default screen.
- The receive thread in `client/chat.c` performs blocking `recv()`; `client_disconnect()` calls `shutdown()` to unblock it cleanly.

## 16. License

Academic project for Systems Programming.
