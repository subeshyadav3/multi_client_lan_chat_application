# ConnectHub — CLI Chat (TUI)

A terminal-based (CLI/TUI) chat application written in **plain C** with **no
external dependencies**: raw TCP sockets, POSIX threads (server), and a
single-threaded `select()` loop driving a `termios` + ANSI-escape interface
(client).

It is a from-scratch CLI re-implementation of the original (GTK) ConnectHub,
keeping all the features — rooms, private messages, typing indicators, admin
tools and file transfer — but in the terminal.

---

## How it works (architecture in plain English)

ConnectHub is a classic **client–server** chat over **TCP**:

- **One server, many clients.** The server runs on a TCP port (8080 by
  default). Clients connect to it, log in, then exchange messages.*

- **The pipe-delimited protocol.** Every message is a single line of text with
  its parts separated by the `|` (pipe) character and ended with a newline.
  For example a public chat message looks like:

  ```
  PUBLIC|general|alice|hello everyone|02:30 PM
  ```

  The first field is always the *type* (e.g. `LOGIN`, `PUBLIC`, `PRIVATE`,
  `JOIN`, `KICK`). Because both sides build and read these lines the exact
  same way, they can talk to each other.

- **The server is multithreaded.** `chatserver` opens the listening socket,
  then gives **each connected client its own thread** (a *detached pthread*).
  That thread just reads lines from its own socket and does what the message
  asks. Shared data (the user list, room list, file transfers) is protected
  with mutexes so the threads do not corrupt each other's state.

- **The client is a single `select()` loop.** Instead of using many threads,
  the `chatclient` watches **two things at once** in one loop: the keyboard
  (`stdin`) and the server socket. `select()` tells it which one has new input
  ready. This keeps the client simple and lets it stay responsive while a
  file is uploading.

### The login flow

1. Client connects and sends `LOGIN|username|password`.
2. The server hashes the password (SHA-256) and compares it to the stored
   hash.
3. On success the server replies `LOGIN_OK|username`; otherwise
   `LOGIN_FAIL|reason`.

### The chat flow

1. You type a message; the client sends `PUBLIC|text` (their current room is
   implied) or `/msg <user> <text>` for a private `PRIVATE|...`.
2. The server finds the recipients (everyone in the room, or the one private
   target) and forwards the message to each of them.
3. Each receiving client prints it on screen with a timestamp.

### Rooms

- `#general` always exists and is every user's starting room.
- `/create <name>` or `/createroom <name> [title|desc|pw]` makes a new room
  (optionally password-protected).
- `/join <room> [pw]` moves you into a room; `/leave` returns you to
  `#general`. `/who` and `/history` show members and recent messages.

### File transfer

1. `/sendfile [@user] <path>` offers a file (to one user or the whole room).
2. The server reserved a slot and tells the recipient `FILE_OFFER`.
3. The recipient accepts `/accept <n>` or declines `/reject <n>`.
4. If accepted, the sender streams the file in ~2 KB base64-encoded chunks
   (`FILE_DATA`), then `FILE_END`; the receiver reassembles it into `files/`.

---

## Project layout

```
connect_cli/
  bin/          built executables (chatclient, chatserver)
  build/        .o object files
  client/       the TUI client (select() loop, TUI drawing, commands, files)
  server/       the threaded TCP server (per-client threads, rooms, users)
  shared/       code used by both: constants, protocol helpers, sha256
  config/       admin.cred, users.cred (accounts, hashed)
  docs/         this documentation
  files/        received files
  logs/         server.log
  tests/        smoke.py integration test
  Makefile
```

## Build

Requires only `gcc` and `make`:

```bash
cd connect_cli
make clean && make
```

This produces `bin/chatclient` and `bin/chatserver`.

## Run

**1. Start the server** (listens on port 8080 by default):

```bash
./bin/chatserver          # port 8080
./bin/chatserver 9000     # or any port
```

**2. Start one or more clients:**

```bash
./bin/chatclient                          # connect to 127.0.0.1:8080
./bin/chatclient --host 192.168.1.5 --port 9000
```

**Auto-login** by passing credentials:

```bash
./bin/chatclient --user alice --pass alice
./bin/chatclient 127.0.0.1 8080 alice alice   # legacy positional form
./bin/chatclient --admin                     # shortcut for admin/admin123
./bin/chatclient --help                      # usage summary
```

## Test accounts

Accounts come from `config/users.cred` (password == username, or `admin123`):

| Username | Password  |
|----------|-----------|
| subesh, prabesh, saroj, alice, bob, carol, dave | same as username |
| admin    | admin123  |

Passwords are stored as **SHA-256 hashes**, never plaintext.

## Keyboard

| Key        | Action                        |
|------------|-------------------------------|
| `Enter`    | Send message / submit command |
| `Backspace`| Delete previous character     |
| `↑` / `↓`  | Scroll command history        |
| `Ctrl-C`   | Quit                          |

## Commands

| Command | Effect |
|---------|--------|
| `/msg <user> <text>` | Send a private (1-on-1) message |
| `/join <room> [pw]` | Join a room (password if protected) |
| `/leave` | Return to `#general` |
| `/create <room>` | Create a simple room |
| `/createroom <name> [title|desc|pw]` | Create a room with metadata/password |
| `/deleteroom <room>` | Delete a room (members return to `#general`) |
| `/rooms` / `/users` | Refresh room / online-user lists |
| `/who [room]` | List who is in a room (defaults to current) |
| `/history` | Replay recent messages in your current room |
| `/typing` | Show a typing indicator to your room |
| `/clear` | Clear the chat scrollback |
| `/help` | Show command help |
| `/quit` | Log out and quit |

**File transfer:**

| Command | Effect |
|---------|--------|
| `/sendfile [@user] <path>` | Offer a file (to a user or the room) |
| `/accept <offer#>` | Accept an incoming file by its `[#]` offer number |
| `/reject <offer#> [why]` | Decline an incoming file |

Received files land in `files/` (duplicates get a ` (n)` suffix).

**Admin only** (log in as `admin`):

| Command | Effect |
|---------|--------|
| `/announce <text>` | Broadcast an announcement |
| `/kick <user> <why>` | Kick a user |
| `/createuser <u> <p>` | Create an account |
| `/deleteuser <u>` | Delete an account |
| `/resetpass <u> <p>` | Reset a password |
| `/accounts` | List accounts |
| `/stats` | Server statistics |

## Test

```bash
python3 tests/smoke.py
```

Builds the project, starts the server, and verifies login, room lists, public
and private messaging, and logout with raw sockets. Expected output:
`SMOKE TEST PASSED`.

See [`docs/DOCUMENTATION.md`](docs/DOCUMENTATION.md) for the full wire protocol
and `documentation.md` for the architecture walkthrough.
