# ConnectHub — How it Works (Viva Guide)

A multi-client **LAN chat application** written in **plain C** with **no
external libraries** — raw Berkeley sockets, POSIX threads, and raw terminal
(`termios`) drawing. This guide explains the architecture and the main flows
in plain English, so you can walk through the code confidently in a viva.

The most complete front-end is the **CLI/TUI** one in `connect_cli/` (this
folder). It talks to a threaded TCP server using a simple text protocol.

---

## 1. Architecture overview

```
                    +------------------------------+
                    |          SERVER               |
                    |  (1 process, N threads)       |
                    |  main: accept + one thread    |
                    |  per client (pthread)         |
                    +--+--------+--------+---------+
                       |        |        |   TCP sockets (port 8080)
              +--------+   +----+        +--------+
              v             v                     v
          Client 1       Client 2             Client 3
          (select()      (select()             (select()
           loop + TUI)    loop + TUI)           loop + TUI)
```

There are two different concurrency styles here — a common viva question:

- **Server: thread-per-client.** The main thread opens the listening socket
  and accepts new connections. Each accepted client is given its **own
detached thread** that reads that client's messages. Mutexes protect the
shared lists (users, rooms, transfers) so threads do not race.
- **Client: one `select()` loop.** The client uses a single thread that
  watches **both** the keyboard (`stdin`) and the server socket with
  `select()`. Whichever has input ready gets handled; nothing blocks.

Because TCP is a **stream** (not packets), both sides read **lines**: messages
are newline-terminated, so `recv()` keeps going until a `\n` arrives.

## 2. The pipe-delimited protocol

Every message is one plain-text line with fields separated by `|` and ended
with a newline:

```
TYPE|field1|field2|...
PUBLIC|general|alice|hello everyone|02:30 PM
```

- The first field is always the **type** (`LOGIN`, `LOGIN_OK`, `PUBLIC`,
  `PRIVATE`, `JOIN`, `KICK`, `FILE_OFFER`, …).
- Fields must not contain `|` or newlines.
- Timestamps are like `02:30 PM`.

This is defined in `shared/protocol.h` (the `MessageType` list, the `Message`
struct, and the `format_*` helpers that build the lines) and
`shared/constants.h` (all size limits and `PORT`).

**Client → Server** (what you send): `LOGIN`, `PUBLIC`, `PRIVATE`, `TYPING`,
`JOIN`, `LEAVE`, `CREATE_ROOM`, `LIST_USERS`, `WHO`, `HISTORY`, `STATS`,
`ANNOUNCE`, `KICK`, `CREATE_USER`, `DELETE_USER`, `RESET_PASS`, `FILE_REQUEST`,
`FILE_DATA`, `FILE_END`, `FILE_ACCEPT`, `FILE_REJECT`, `LOGOUT`.

**Server → Client** (what you receive): `LOGIN_OK`, `LOGIN_FAIL`, `PUBLIC`,
`PRIVATE`, `NOTIFY`, `ANNOUNCE`, `TYPING`, `USERS`, `ROOMS`, `JOIN_OK`,
`JOIN_FAIL`, `ROOM_CREATED`, `STATUS`, `KICK`, `ERROR`, `FILE_GRANTED`,
`FILE_OFFER`, `FILE_ACCEPT`, `FILE_REJECT`, `FILE_DATA`, `FILE_END`.

## 3. The login flow

1. Client connects and sends `LOGIN|username|password`.
2. Server hashes the password with **SHA-256** (`shared/sha256.c`) and
   compares it to the stored hash.
3. Success → `LOGIN_OK|username` and the user is added to the online list
   (broadcast via `USERS`).
4. Failure → `LOGIN_FAIL|reason` and the connection is closed or retried.

## 4. The chat flow

**Public message:** you type text and press Enter. The client sends
`PUBLIC|text` (the room follows from your current room). The server looks up
who is in that room and forwards a `PUBLIC|room|sender|text|timestamp` to
everyone there except you.

**Private message:** `/msg bob hello` sends `PRIVATE|...`; the server forwards
it only to you and the recipient, shown as a `(PM)` line.

**Typing indicator:** `/typing` broadcasts `TYPING` so others see
`typing: <you>`.

## 5. Rooms

- `#general` is the default room every user starts in.
- `/create <name>` or `/createroom <name> [title|desc|pw]` creates a room
  (optionally password-protected).
- `/join <room> [pw]` moves you in; `/leave` returns you to `#general`.
- `/rooms` refreshes the room list, `/who [room]` lists members, and
  `/history` replays the last ~50 messages of your current room.
- `/deleteroom <name>` removes a room (members return to `#general`); only the
  creator or an admin can do this.
- Join/leave/disconnect produce `NOTIFY` presence lines so everyone sees who
  is around.

## 6. File transfer

1. `/sendfile [@user] <path>` sends a `FILE_REQUEST` to the server.
2. The server checks its upload slots, mints a **token**, replies
   `FILE_GRANTED` to the sender, and sends `FILE_OFFER` to the recipient.
3. The recipient picks `/accept <n>` or `/reject <n>`.
4. On accept, the sender streams the file in ~2 KB chunks, base64-encoded in
   `FILE_DATA|...|token|...`, then `FILE_END`. The token in every chunk stops
   random clients injecting data.
5. The receiver decodes each chunk into `files/<name>.tmp` and renames it to
   `files/<name>` on `FILE_END` (duplicates get a ` (n)` suffix).

## 7. Project layout

```
connect_cli/
  shared/        code used by both sides: constants.h, protocol.h/c, sha256.h/c
  client/        the TUI client (select() loop, TUI drawing, commands, files)
  server/        the threaded TCP server (accept loop, per-client threads)
  config/        admin.cred, users.cred (accounts, SHA-256 hashed)
  docs/          this documentation
  files/         received files
  logs/          server.log
  tests/         smoke.py integration test
  Makefile
```

## 8. Quick viva cheat sheet

- **What did you build?** A LAN chat app in C — a threaded TCP server plus a
  raw-terminal TUI client, custom SHA-256, no external deps.
- **Concurrency?** Server = thread-per-client with mutex-protected shared
  lists. Client = single-threaded `select()` loop.
- **How do clients talk?** Newline-terminated `TYPE|field|…` lines over TCP
  port 8080.
- **Passwords?** Stored as SHA-256 hex hashes (never plaintext after the
  first admin user action).
- **File transfer?** Offer/accept handshake + upload token + base64 chunks
  (~2 KB), reassembled into `files/`.
- **Security features?** Filename sanitization, upload tokens, duplicate-login
  rejection, admin-only commands, mutex discipline to avoid races.
- **Limitations?** No encryption (plain text on the wire), no persistence for
  rooms/users/history created at runtime, no field escaping (`|` is forbidden
  in text). Intended for a trusted LAN demo, not production.
