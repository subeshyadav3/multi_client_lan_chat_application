# ConnectHub — Multi-Client LAN Chat & File Sharing (TUI)

A terminal-based (CLI/TUI) chat application written in **plain C** with **no external dependencies**: raw Berkeley TCP sockets, POSIX threads with mutex synchronization (server), and a single-threaded `select()` loop driving a `termios` + ANSI-escape interface (client).

Designed and implemented for systems programming, computer networking, and operating systems defense at Pulchowk Campus, IOE, Tribhuvan University.

---

## 🌟 Key Features

- **Zero External Dependencies:** Built strictly using C11 standard library and POSIX system calls (`socket`, `bind`, `listen`, `accept`, `pthread_create`, `select`, `termios`).
- **Concurrent Multithreaded TCP Server:** One detached POSIX thread per client connection with fine-grained mutex synchronization across 6 locking domains (clients, users, rooms, uploads, transfers, history).
- **Single-Threaded Non-Blocking Client:** Driven by a single `select()` loop multiplexing keyboard `stdin` and TCP socket descriptors.
- **Custom Wire Protocol:** Human-readable pipe-delimited line protocol (`TYPE|field1|field2|...\n`) debuggable via `netcat` or Wireshark.
- **Token-Guarded Chunked File Transfer:** Rate-limited upload subsystem (2 concurrent uploads, 1 MiB byte budget) streaming 2 KB binary chunks encoded in Base64 (~2.7 KB payload) to prevent stream delimiter collisions.
- **In-House Cryptography:** NIST FIPS 180-4 compliant SHA-256 implementation written from scratch in `shared/sha256.c` (no OpenSSL dependency).
- **Room Management & History:** Password-protected custom rooms, presence notifications (`NOTIFY`), and ring-buffer history replay (last 50 messages).
- **Admin Dashboard:** Real-time server statistics (`/stats`), global broadcast announcements (`/announce`), account management (`/createuser`, `/deleteuser`, `/resetpass`), and user moderation (`/kick`).

---

## 🏛️ System Architecture

```
                    ┌────────────────────────────────────────────────────────┐
                    │               CONNECTHUB TCP SERVER                    │
                    │               (Port 8080 by default)                   │
                    │                                                        │
                    │   ┌────────────────────────────────────────────────┐   │
                    │   │  main() -> select() accept loop (1s timeout)   │   │
                    │   │  (Runs periodic upload cleanup on timeout)     │   │
                    │   └──────────────────────┬─────────────────────────┘   │
                    │                          │                             │
                    │              accept()    │ spawns                      │
                    │                          ▼                             │
                    │   ┌────────────────────────────────────────────────┐   │
                    │   │ Detached POSIX Thread per Client (connection.c)│   │
                    │   │ - Accumulates raw bytes into '\n'-ended lines  │   │
                    │   │ - Parses TYPE|arg1|arg2 into Cmd struct        │   │
                    │   │ - Dispatches to handlers.c / files.c           │   │
                    │   └──────────────────────┬─────────────────────────┘   │
                    │                          │                             │
                    │            Guarded by Dedicated Mutexes                │
                    │   ┌────────────────────────────────────────────────┐   │
                    │   │ • client_mutex   -> clients[] registry         │   │
                    │   │ • user_mutex     -> accounts & SHA-256 hashes  │   │
                    │   │ • room_mutex     -> rooms[], passwords, owners │   │
                    │   │ • upload_mutex   -> 2 upload slots + FIFO queue│   │
                    │   │ • transfer_mutex -> active file offer routing  │   │
                    │   │ • Per-room locks -> ring buffer (50 msgs)      │   │
                    │   └────────────────────────────────────────────────┘   │
                    └──────────────────────────▲─────────────────────────────┘
                                               │
                                 TCP Sockets (Plaintext Wire)
                     Protocol: TYPE|arg1|arg2...\n (Base64 file chunks)
                                               │
                    ┌──────────────────────────┴─────────────────────────────┐
                    │               CONNECTHUB CLI CLIENT                    │
                    │                                                        │
                    │   ┌────────────────────────────────────────────────┐   │
                    │   │ Single-Threaded select() Event Loop (client.c) │   │
                    │   │ • STDIN_FILENO -> Keypresses (termios raw mode)│   │
                    │   │ • Server Socket -> Incoming wire lines         │   │
                    │   │ • 100ms Timeout -> File chunking & typing timer│   │
                    │   └──────────────────────┬─────────────────────────┘   │
                    │                          │                             │
                    │   ┌──────────────────────▼─────────────────────────┐   │
                    │   │ 4-Region Terminal UI (tui.c):                  │   │
                    │   │ 1. Scrollable Chat History Area                │   │
                    │   │ 2. Right-hand Online Users & Rooms Sidebar     │   │
                    │   │ 3. System Status Bar                           │   │
                    │   │ 4. Masked Input Buffer & Command History       │   │
                    │   └────────────────────────────────────────────────┘   │
                    └────────────────────────────────────────────────────────┘
```

---

## 📁 Repository Structure

```
ConnectHub/
├── bin/              # Compiled executables (chatclient, chatserver)
├── build/            # Object files (.o)
├── client/           # Terminal UI client source code
│   ├── client.c      # Entry point & select() event loop
│   ├── commands.c    # Slash-command parser & handlers
│   ├── files.c       # File chunking & Base64 encode/decode
│   ├── net.c         # Socket connection helpers
│   ├── protocol.c    # Message formatting & parsing
│   └── tui.c         # Raw termios mode & ANSI screen rendering
├── server/           # Multithreaded TCP server source code
│   ├── server.c      # Entry point, config loader & accept loop
│   ├── connection.c  # Per-client reader pthread & line accumulator
│   ├── handlers.c    # Protocol command dispatcher & handlers
│   ├── files.c       # Upload slot manager, FIFO queue & tokens
│   ├── room.c        # Room registry & membership management
│   ├── room_access.c # Room permissions & password validation
│   ├── users.c       # User account credentials loader & validator
│   ├── history.c     # Per-room message ring buffer (last 50 lines)
│   ├── logger.c      # File logger (logs/server.log)
│   └── net.c         # Socket broadcasting & routing helpers
├── shared/           # Shared components between client and server
│   ├── constants.h   # Global limits, buffer sizes, and ports
│   ├── protocol.c/h  # Wire protocol definitions
│   └── sha256.c/h    # NIST FIPS 180-4 SHA-256 implementation from scratch
├── config/           # Seed credentials (admin.cred, users.cred)
├── docs/             # Project reports, figures & defense resources
│   ├── final_report_v4.docx  # Final academic project report (Word)
│   ├── final_report_v4.pdf   # Final academic project report (PDF)
│   ├── final_report_v4.tex   # LaTeX source for final report
│   ├── defense_guide.html    # Interactive defense & viva portal
│   ├── defense_guide.docx    # Complete defense theory notes
│   └── fig_*.jpg             # Architecture & flow diagrams
├── files/            # Destination folder for received files
├── logs/             # Server operational logs
├── tests/            # Automated integration & smoke tests (smoke.py)
├── Makefile          # GNU Makefile build system
├── documentation.md  # Comprehensive systems walkthrough & viva notes
└── README.md         # Project documentation
```

---

## 🛠️ Build & Execution

Requires only standard `gcc` and `make`:

```bash
# Build both client and server binaries
make

# Clean build artifacts
make clean
```

### Running the Server
```bash
# Default port 8080
./bin/chatserver 8080
```

### Running the Client
```bash
# Interactive login wizard:
./bin/chatclient 127.0.0.1 8080

# Auto-login with command flags:
./bin/chatclient --host 127.0.0.1 --port 8080 --user alice --pass alice

# Connect as administrator:
./bin/chatclient --host 127.0.0.1 --port 8080 --admin --pass admin123
```

---

## 🔑 Default Accounts & Test Credentials

The server loads default credentials on startup from the `config/` directory. Use these pre-configured accounts for testing and verification:

| Username | Password | Role | Configuration File | Direct Login Command |
| :--- | :--- | :--- | :--- | :--- |
| `alice` | `alice` | Standard User | `config/users.cred` | `./bin/chatclient --host 127.0.0.1 --port 8080 --user alice --pass alice` |
| `bob` | `bob` | Standard User | `config/users.cred` | `./bin/chatclient --host 127.0.0.1 --port 8080 --user bob --pass bob` |
| `admin` | `admin123` | Administrator | `config/admin.cred` | `./bin/chatclient --host 127.0.0.1 --port 8080 --admin --pass admin123` |

> **Note:** Administrators can also register new user accounts at runtime using `/createuser <username> <password>` and reset passwords using `/resetpass <username> <new_password>`.

---

## 💬 Command Reference

### Chat & Rooms
| Command | Usage | Description |
| :--- | :--- | :--- |
| `<message>` | Type text + Enter | Broadcast message to your active room. |
| `/msg` | `/msg <user> <message>` | Send a private 1-on-1 direct message. |
| `/join` | `/join <room> [password]` | Switch active room (auto-replays last 50 messages). |
| `/leave` | `/leave` | Return to default `#general` room. |
| `/create` | `/create <room>` | Create a simple public room. |
| `/createroom` | `/createroom <name> [title\|desc\|pw]` | Create a password-protected room with metadata. |
| `/deleteroom` | `/deleteroom <room>` | Delete a room (owner or admin only). |
| `/who` | `/who [room]` | List active members of a room. |
| `/history` | `/history` | Replay the last 50 messages of the current room. |
| `/rooms` | `/rooms` | Refresh online room directory. |
| `/users` | `/users` | Refresh online user list. |
| `/clear` | `/clear` | Clear terminal chat scrollback. |
| `/quit` | `/quit` or `/exit` | Clean disconnect from server. |

### File Transfer
| Command | Usage | Description |
| :--- | :--- | :--- |
| `/sendfile` | `/sendfile [@user] <path>` | Offer a file to a specific user or active room. |
| `/accept` | `/accept <offer#>` or `/accept <user> <file>` | Accept incoming file transfer offer. |
| `/reject` | `/reject <offer#> [reason]` | Decline incoming file transfer offer. |

### Admin Commands
| Command | Usage | Description |
| :--- | :--- | :--- |
| `/stats` | `/stats` | Display total messages, private messages, and file transfers. |
| `/announce` | `/announce <announcement text>` | Broadcast system announcement to all online users. |
| `/kick` | `/kick <user> <reason>` | Disconnect a user from the server. |
| `/createuser`| `/createuser <user> <password>` | Register a new user account at runtime. |
| `/deleteuser`| `/deleteuser <user>` | Delete a registered user account. |
| `/resetpass` | `/resetpass <user> <new_password>` | Reset password for a user account. |
| `/accounts`  | `/accounts` | List all registered user accounts. |

---

## 🧪 Testing & Validation

Run the automated end-to-end integration test suite:

```bash
python3 tests/smoke.py
```

Validates:
- SHA-256 authentication (valid and invalid credentials).
- Concurrent multi-client room routing and DM delivery.
- Room creation, access controls, and presence notifications.
- Token-guarded chunked file transfer with SHA-256 digest validation.
- Clean client disconnection and resource cleanup.

---

## 🎓 Defense & Academic Resources

- **Interactive Defense Portal:** Open [`docs/defense_guide.html`](docs/defense_guide.html) in any browser for live flashcards, interactive Q&A search, and demo checklists.
- **Defense Viva Document:** [`docs/defense_guide.docx`](docs/defense_guide.docx) containing full theoretical proofs and viva model answers.
- **Final Project Report (v4):** [`docs/final_report_v4.docx`](docs/final_report_v4.docx) / [`docs/final_report_v4.pdf`](docs/final_report_v4.pdf).
