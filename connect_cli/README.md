# ConnectHub v2 — CLI Chat (TUI)

A **terminal-based** (CLI/TUI) re-implementation of the original *ConnectHub*
GTK chat program, rebuilt from scratch with **zero external dependencies**:
plain C, TCP sockets, POSIX threads (server), and a raw `termios` + ANSI-escape
TUI driven by a single-threaded `select()` loop (client).

> `connect_v2` is a separate project. The original `ConnectHub` folder is left
> untouched. Everything here was written fresh from the README, proposal notes
> and the wire protocol.

## Why "CLI instead of GUI"?

The original client was a GTK3 windowed app. This version keeps **all** the
functionality (rooms, private messages, admin tools, file transfer, typing
indicators) but presents it in the terminal using raw ANSI escape codes and
`termios` — no ncurses, no GTK, no X11. It is also the cleanest way to exercise
the course concepts:

- **Berkeley Sockets API**: `socket → bind → listen → accept`, `send/recv`.
- **POSIX Threads**: the server handles each client in its own thread, with
  `pthread_mutex_t` protecting the shared client/user/room/transfer lists.
- **`select()` I/O multiplexing**: the client watches **both** stdin (keyboard)
  and the socket in one loop — one thread, no blocking reads.

## Project layout

```
connect_v2/
  bin/          Compiled executables (chatclient, chatserver)
  build/        Object files
  client/       TUI client
    client.c    State, command dispatch, message handling, select() main loop
    net.c/h     Socket connect + line send
    tui.c/h     Raw terminal, ANSI drawing, input
  server/       Threaded TCP server
    server.c    accept loop, client threads, protocol dispatch
    room.c/h    Room list (create/delete/update, protected rooms)
    room_access.c  Per-user access to protected rooms
    logger.c/h  File-based logging
  shared/       constants.h, protocol.h/c (line protocol), sha256.h/c (password hashing)
  config/       admin.cred, users.cred
  tests/        smoke.py (integration test)
  files/        Received files
  logs/         server.log
  Makefile
```

## Build

Requires only `gcc` and `make` (no GTK, no ncurses):

```bash
cd connect_v2
make clean && make
```

Produces `bin/chatclient` and `bin/chatserver`.

## Run

### 1. Start the server

```bash
./bin/chatserver            # listens on port 8080 by default
./bin/chatserver 9000       # or pick any port
```

### 2. Start one or more clients

```bash
./bin/chatclient                            # connect to 127.0.0.1:8080
./bin/chatclient --host 192.168.1.5 --port 9000
```

### Auto-login

Log in automatically by passing credentials up front (flags or positional):

```bash
./bin/chatclient --user alice --pass alice
./bin/chatclient 127.0.0.1 8080 alice alice    # legacy positional form
./bin/chatclient --admin                       # shortcut for admin/admin123
```

`./bin/chatclient --help` prints the usage summary.

## Test accounts

Accounts come from `config/users.cred` (10+ ready to try):

| Username | Password |
|----------|----------|
| `alice`  | `alice`  |
| `bob`    | `bob`    |
| `carol`  | `carol`  |
| `dave`   | `dave`   |
| `erin`   | `erin`   |
| `frank`  | `frank`  |
| `grace`  | `grace`  |
| `heidi`  | `heidi`  |
| `ivan`   | `ivan`   |
| `judy`   | `judy`   |
| `saroj`  | `saroj`  |
| `abc`    | `abc`    |

> Administrator: login as **`admin`** with password **`admin123`** (from
> `config/admin.cred`). Only the admin can use `/announce`, `/kick`, and the
> user-management commands.

> **Passwords are hashed (SHA-256).** On server start, plaintext entries in
> `config/users.cred` are hashed in memory, and any admin `/createuser`,
> `/resetpass`, or `/deleteuser` rewrites the file with hashed values, so
> credentials are never stored in plaintext after the first admin action.

## Keyboard

| Key           | Action                          |
|---------------|---------------------------------|
| `Enter`       | Send message / submit command   |
| `Backspace`   | Delete previous character       |
| `↑` / `↓`     | Scroll command history          |
| `Ctrl-C`      | Quit                            |

## Features

### 1. Messaging

- **Public chat (broadcast)** — Just type a message and press `Enter`. It is
  sent to **everyone in your current room** (the status bar shows `room: #general`).
  ```
  > hello everyone!                ← shown to the whole room in white
  [10:12 AM] carol: hello everyone!
  ```
- **Private 1-on-1 message** — `/msg <username> <text>`. Only you and that user
  see it (shown in magenta as `(PM)`).
  ```
  > /msg bob wanna test file transfer?
  [10:12 AM] (PM) -> bob: wanna test file transfer?
  ```
- **Typing indicator** — `/typing`. Other members of your room see
  `typing: <you>` in the status bar.
  ```
  > /typing
  (bob's status bar shows:  typing: alice)
  ```

### 2. Rooms (channels)

| Command | What it does |
|---------|--------------|
| `/rooms` | Refresh the room list (sidebar shows `*` on your current room) |
| `/create <name>` | Create a simple room |
| `/createroom <name> [title\|desc\|pw]` | Create a room with a title, description and/or password |
| `/join <room> [password]` | Join a room (password needed if protected) |
| `/leave` | Return to `#general` |
| `/deleteroom <name>` | Delete a room (members go back to `#general`) |
| `/who [room]` | List the users in a room (defaults to current) |
| `/history` | Replay the last ~50 messages of your current room |

Example — create, join, and check who's there:
```
> /create linux
> /join linux
Now in room #linux
> /who linux
STATUS|Users in #linux: alice, bob
```

Password-protected rooms need the password to join:
```
> /createroom secret Project X|only members|hunter2
> /join secret hunter2
Now in room #secret
```

You'll also see live presence notices as people move around:
```
NOTIFY|bob joined the room.
NOTIFY|carol left room secret.
NOTIFY|dave disconnected.
```

### 3. People

- **Who is online** — `/users` (and the sidebar shows the online count).
- **Who is in a room** — `/who dev`.
  ```
  > /who dev
  STATUS|Users in #dev: alice, bob, carol
  ```

### 4. File transfer

| Command | What it does |
|---------|--------------|
| `/sendfile @<user> <path>` | Offer a file to a specific user |
| `/sendfile <path>` | Offer a file to the whole room |
| `/accept <offer#>` | Accept an incoming file (use the `[#]` shown) |
| `/accept <sender> <file>` | Accept (long form) |
| `/reject <offer#> [reason]` | Decline an incoming file |
| `/reject <sender> <file> [reason]` | Decline (long form) |

When someone offers you a file, an offer appears with a number — just accept by
that number:
```
[1] alice offers 'report.pdf' (45823 bytes) to you. /accept 1 or /reject 1
> /accept 1
Received 'report.pdf' (45823 bytes) -> files/report.pdf
```

Sender's side, the transfer streams automatically once accepted:
```
bob accepted 'report.pdf'. Sending...
Uploading 'report.pdf': 50%
Sent 'report.pdf' (45823 bytes).
```

- Files are saved in the **`files/`** directory (duplicates become
  `name (1)`, `name (2)`, …).
- A bad accept/reject tells you why:
  `Error: No active file offer from 'ghost' named 'nofile.txt'`.

### 5. Admin tools (admin only)

| Command | What it does |
|---------|--------------|
| `/announce <text>` | Broadcast a yellow announcement to everyone |
| `/kick <user> <why>` | Kick a user off the server |
| `/createuser <u> <p>` | Create a new account (saved hashed) |
| `/deleteuser <u>` | Delete an account (and disconnect them if online) |
| `/resetpass <u> <p>` | Reset a user's password |
| `/accounts` | List all accounts |
| `/stats` | Server statistics |

```
> /announce System will restart in 5 minutes.
> /kick bob spamming the room
> /stats
STATUS|Online users: 5 | Messages: 12 | Private msgs: 3 | Files offered: 1
```

### 6. Permissions summary

| Action | Normal user | Room creator | Admin |
|--------|-------------|--------------|-------|
| Send public/private messages | ✅ | ✅ | ✅ |
| Create / join / leave rooms | ✅ | ✅ | ✅ |
| `/deleteroom <name>` | ❌ | ✅ own room only | ✅ any room |
| `/who`, `/history`, `/users`, `/rooms` | ✅ | ✅ | ✅ |
| `/sendfile`, `/accept`, `/reject` | ✅ | ✅ | ✅ |
| `/announce`, `/kick`, `/createuser`, `/deleteuser`, `/resetpass`, `/accounts`, `/stats` | ❌ | ❌ | ✅ |

## Example sessions

### Chatting

**Example 1 — broadcast + private message + typing indicator**

```
$ ./bin/chatclient --user alice --pass alice
Connected to 127.0.0.1:8080.
Logged in as alice.

> hello everyone!                     ← every room member sees this
> /msg bob wanna test file transfer?  ← only bob sees it (magenta, PM)
> /typing                              ← others see "typing: alice"
> /join dev                            ← hop into the #dev room
> /leave                               ← back to #general
```

**Example 2 — PM reply and screen clean-up**

```
$ ./bin/chatclient --user bob --pass bob

(alice's PM arrives)
[10:12 AM] (PM) alice: wanna test file transfer?

> /msg alice sure, send it over!      ← reply privately
> /clear                               ← wipe the scrollback
> /quit                                 ← log out and close
```

### Rooms

**Example 1 — simple room + join**

```
> /create linux                        ← create a room
Now in room #general                   (you stay put; room is created)
> /join linux
Now in room #linux
> /rooms                               ← refresh the room list
```

**Example 2 — protected room with a password**

```
> /createroom secret Room title|Only admins|hunter2    ← protected room
> /join secret hunter2                 ← protected rooms need the password
Now in room #secret
> /leave                               ← back to #general
```

### Sending a file — one-to-one

**Example 1 — accept by offer number**

```
alice: > /sendfile @bob ~/report.pdf
       Offering 'report.pdf' (45823 bytes) to bob ...
       Granted to send 'report.pdf'. Waiting for the recipient to accept...

bob:   [1] alice offers 'report.pdf' (45823 bytes) to you. /accept 1 or /reject 1
       > /accept 1
       (file streams in)  Received 'report.pdf' (45823 bytes) -> files/report.pdf

alice: bob accepted 'report.pdf'. Sending...
       Uploading 'report.pdf': 50%
       Sent 'report.pdf' (45823 bytes).
```

**Example 2 — reject the offer**

```
alice: > /sendfile @bob ~/report.pdf
       Offering 'report.pdf' (45823 bytes) to bob ...
       Granted to send 'report.pdf'. Waiting for the recipient to accept...

bob:   [1] alice offers 'report.pdf' (45823 bytes) to you. /accept 1 or /reject 1
       > /reject 1 already have it

alice: bob rejected 'report.pdf': already have it
```

### Sending a file — to the whole room

**Example 1 — accept a room offer**

```
alice: > /sendfile ~/notes.txt
       Offering 'notes.txt' (2048 bytes) to room ...

bob:   [1] alice offers 'notes.txt' (2048 bytes) to room. /accept 1 or /reject 1
       > /accept 1
       Received 'notes.txt' (2048 bytes) -> files/notes.txt
```

**Example 2 — long form / duplicate-name handling**

```
alice: > /sendfile @bob ~/docs/notes.txt
       Offering 'notes.txt' (2048 bytes) to bob ...

bob:   [1] alice offers 'notes.txt' (2048 bytes) to you. /accept 1 or /reject 1
       > /accept alice notes.txt        ← long form still works
       (files/notes.txt already exists → auto-renamed)
       Received 'notes.txt' (2048 bytes) -> files/notes.txt (1)
```

No more typing `/accept <sender> <exact-filename>` by hand — just use the offer
number shown in the `[1]` prompt. (The long form still works if you prefer it:
`/accept alice report.pdf`.)

### Admin

**Example 1 — announcements and user management**

```
> /announce System will restart in 5 minutes.
> /createuser newbie letmein
> /kick troublemaker spamming the room
> /stats
STATUS|Online users: 5 | Messages: 12 | Private msgs: 3 | Files offered: 1
```

**Example 2 — accounts and passwords**

```
> /accounts
Accounts: alice,bob,carol,newbie
> /resetpass alice s3cret
> /deleteuser old_account
```

## Commands

| Command | Effect |
|---------|--------|
| `/msg <user> <text>` | Send a private (1-on-1) message |
| `/join <room> [pw]` | Join a room (password if protected) |
| `/leave` | Return to `#general` |
| `/create <room>` | Create a room |
| `/createroom <name> [title\|desc\|pw]` | Create a room with metadata/password |
| `/deleteroom <room>` | Delete a room (returns members to `#general`) |
| `/rooms` / `/users` | Refresh room / online-user lists |
| `/who [room]` | List who is in a room (defaults to current) |
| `/history` | Replay recent messages in your current room |
| `/clear` | Clear the chat scrollback |
| `/help` | Show command help |
| `/quit` | Log out and quit |

**Admin only:**

| Command | Effect |
|---------|--------|
| `/announce <text>` | Broadcast an announcement |
| `/kick <user> <why>` | Kick a user |
| `/createuser <u> <p>` | Create an account |
| `/deleteuser <u>` | Delete an account |
| `/resetpass <u> <p>` | Reset a password |
| `/accounts` | List accounts |
| `/stats` | Server statistics |

**File transfer:**

| Command | Effect |
|---------|--------|
| `/sendfile [@user] <path>` | Offer a file (to a user or the room) |
| `/accept <offer#>` | Accept an incoming file by its `[#]` offer number |
| `/accept <sender> <file>` | Accept (long form, still supported) |
| `/reject <offer#> [why]` | Decline an incoming file |
| `/reject <sender> <file> [why]` | Decline (long form) |

Received files land in `files/` (duplicates get a ` (n)` suffix).

## Test

```bash
python3 tests/smoke.py
```

Builds the project, starts the server, and uses raw sockets to verify login,
online-user list, room list, public & private messaging, room create/join, and
logout. Expected output: `SMOKE TEST PASSED`.

See [`docs/DOCUMENTATION.md`](docs/DOCUMENTATION.md) for the wire protocol and
architecture details.
