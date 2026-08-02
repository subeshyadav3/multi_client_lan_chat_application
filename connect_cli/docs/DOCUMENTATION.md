# ConnectHub — Technical Documentation

A terminal (CLI/TUI) chat application written in C with no external
dependencies: a threaded TCP server and a raw-ANSI/`termios` client driven by
a single `select()` loop. This document covers the architecture and the wire
protocol in detail.

For the short, plain-English walkthrough see `../documentation.md`. For usage
and commands see `../README.md`.

---

## 1. How it works (architecture)

ConnectHub is a classic **client–server chat over TCP**:

- **One server, many clients.** The server listens on a TCP port (`8080` by
  default). Clients connect, log in, then send and receive messages.

- **Pipe-delimited protocol.** All traffic is plain-text lines of the form
  `TYPE|field1|field2|...`, ended with `\n`. The first field is the message
  type; the rest are its arguments. Example:

  ```
  PUBLIC|general|alice|hello everyone|02:30 PM
  ```
  Both sides build and parse these identical lines, so they interoperate.

### 1.1 The server is multithreaded (`bin/chatserver`)

- `main()` loads `config/admin.cred`, seeds the default users/rooms, binds and
  listens on the port, then runs a `select()` accept loop.
- Each accepted socket becomes a `Client` in a global linked list and is given
  its **own detached pthread** that reads that client's lines and dispatches
  them.
- Because many threads touch the same data, mutexes protect the shared lists:
  `client_mutex` (connected clients), `user_mutex` (accounts), `room_mutex`
  (rooms), and transfer/upload mutexes (files). Handlers never hold
  `client_mutex` while broadcasting, to avoid deadlocks.
- A failing `send()` marks the client inactive so its thread can clean up
  safely.

### 1.2 The client uses a `select()` loop (`bin/chatclient`)

- The client is **single-threaded**. One `select()` loop watches **both**
  `stdin` (keyboard) and the server socket, so it reads keystrokes and network
  data without blocking, and stays responsive while uploading a file.
- The terminal is put in **raw mode** (`termios`: `ICANON`/`ECHO` off,
  `VMIN=1`) so each keypress is readable immediately; it is restored on exit.
- Drawing uses **ANSI escape codes** (cursor movement, colours, cursor
  hide/show, reverse-video bars) into a chat area, a right-sidebar (users +
  rooms), a status bar, and an input line.

## 2. Login flow

1. Client connects and sends `LOGIN|username|password`.
2. Server hashes the password with SHA-256 (`shared/sha256.c`) and compares it
   to the stored hash.
3. Match → `LOGIN_OK|username`; the user is added to the online list and a
   `USERS` update is broadcast. No match → `LOGIN_FAIL|reason`.
4. Passwords are stored hashed (never plaintext after the first admin action).

## 3. Chat flow

- **Public:** client sends `PUBLIC|text` (its current room is implied). The
  server forwards `PUBLIC|room|sender|text|timestamp` to everyone in the room
  except the sender.
- **Private:** `/msg bob hi` sends `PRIVATE|...`; the server delivers it only
  to you and the recipient.
- **Typing:** `/typing` broadcasts `TYPING|room|username`.

## 4. Rooms

- `#general` is the default start room.
- `/create <name>` or `/createroom <name> [title|desc|pw]` adds a room
  (optionally protected).
- `/join <room> [pw]` moves you in; `/leave` returns to `#general`.
- `CREATE_ROOM` → `ROOM_CREATED|name`; `JOIN` → `JOIN_OK|room` or
  `JOIN_FAIL|reason`.
- `/who [room]`, `/history` show members and the last ~50 lines.
- `/deleteroom <name>` removes a room (creator or admin only).
- Join/leave/disconnect emit `NOTIFY` presence lines.

## 5. File transfer flow

Sender → Server → Recipient:

1. Sender: `FILE_REQUEST|name|size|target`.
2. Server mints a **token**, replies `FILE_GRANTED` to the sender, and sends
   `FILE_OFFER` to the recipient.
3. Recipient: `/accept <n>` → `FILE_ACCEPT`; `/reject <n>` → `FILE_REJECT`.
4. On accept, the sender streams `FILE_DATA|name|token|<base64 chunk>`
   (~2 KB per chunk, under `BUFFER_SIZE` after encoding), then `FILE_END|name`.
   The token in every chunk prevents a random client injecting file data.
5. Receiver decodes each chunk into `files/<name>.tmp` and renames it on
   `FILE_END` (adding ` (n)` if the name already exists).

## 6. Wire protocol reference

**Client → Server:**

| Message | Fields | Description |
|---------|--------|-------------|
| `LOGIN` | `username\|password` | Authenticate |
| `PUBLIC` | `text` | Send to your current room |
| `PRIVATE` | `recipient\|text` | Private message |
| `TYPING` | — | Typing indicator |
| `JOIN` | `room[\|password]` | Join a room |
| `LEAVE` | — | Return to `general` |
| `CREATE_ROOM` | `name\|title\|desc\|password` | Create a room |
| `DELETE_ROOM` | `name` | Delete a room |
| `LIST_USERS` / `LIST_ROOMS` | — | Request lists |
| `WHO` / `HISTORY` | `room` | Room members / recent messages |
| `STATS` | — | Admin: server statistics |
| `ANNOUNCE` | `text` | Admin: announcement |
| `KICK` | `username\|reason` | Admin: kick a user |
| `CREATE_USER` / `DELETE_USER` / `RESET_PASS` | `u[\|p]` | Admin: accounts |
| `FILE_REQUEST` | `filename\|size\|target` | Offer a file (target empty = room) |
| `FILE_DATA` | `filename\|token\|base64` | One file chunk |
| `FILE_END` | `filename` | End of transfer |
| `FILE_ACCEPT` / `FILE_REJECT` | `sender\|filename[\|reason]` | Accept / decline |
| `LOGOUT` | — | Clean disconnect |

**Server → Client:**

| Message | Fields | Description |
|---------|--------|-------------|
| `LOGIN_OK` | `username` | Login accepted |
| `LOGIN_FAIL` | `reason` | Login rejected |
| `PUBLIC` | `room\|sender\|text\|timestamp` | Public message |
| `PRIVATE` | `sender\|recipient\|text\|timestamp` | Private message |
| `NOTIFY` / `ANNOUNCE` | `text[\|timestamp]` | Notice / announcement |
| `TYPING` | `room\|username` | Typing indicator |
| `USERS` / `ROOMS` | `list` | Online users / rooms |
| `JOIN_OK` / `JOIN_FAIL` | `room` / `reason` | Join result |
| `ROOM_CREATED` | `name` | Room created |
| `STATUS` | `text` | Stats / status |
| `KICK` | `reason` | You were kicked |
| `ERROR` | `reason` | Generic error |
| `FILE_GRANTED` | `sender\|filename\|token\|size` | Upload slot granted |
| `FILE_OFFER` | `sender\|filename\|size\|target` | Incoming offer |
| `FILE_ACCEPT` / `FILE_REJECT` | `recipient\|filename[\|reason]` | Result |
| `FILE_DATA` | `sender\|filename\|base64` | Incoming chunk |
| `FILE_END` | `sender\|filename` | Transfer complete |

## 7. Notes & limitations

- Timestamps are `%I:%M %p` (e.g. `02:30 PM`).
- No encryption — plain text on the wire (academic project for a trusted LAN).
- Filename sanitisation and upload tokens provide basic safety; fields cannot
  contain `|`.
- No persistence for rooms/users/history created at runtime (only
  `config/*.cred` seeds on start).
