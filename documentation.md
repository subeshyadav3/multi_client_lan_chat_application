# ConnectHub — Technical Documentation & Viva Reference

A terminal (CLI/TUI) chat and file-sharing application written in plain C with zero external dependencies: a multithreaded TCP server and a raw-ANSI/`termios` client driven by a single `select()` event loop.

---

## 1. System Architecture

ConnectHub is a two-tier **client–server** architecture over TCP:

- **Centralized Server:** The server (`bin/chatserver`) listens on TCP port `8080` (by default). It is the single source of truth for online users, accounts, rooms, history, and active file uploads.
- **Pipe-Delimited Wire Protocol:** All traffic is formatted as plain-text lines of the form `TYPE|field1|field2|...` terminated with `\n`. Example:
  ```
  PUBLIC|general|subesh|hello everyone|02:30 PM
  ```
- **Server Concurrency (POSIX Threads):**
  - `main()` loads `config/admin.cred` and `config/users.cred`, initializes subsystems, binds the listening socket, and executes an accept loop driven by `select()` with a 1-second timeout.
  - Each accepted socket is handed to its own **detached pthread** (`handle_client()` in `server/connection.c`) which accumulates bytes into newline-delimited lines and dispatches them.
  - Dedicated mutexes protect shared data structures: `client_mutex` (connected clients), `user_mutex` (accounts), `room_mutex` (rooms), `upload_mutex` (upload slots and FIFO queue), `transfer_mutex` (active offers), and per-room locks (message ring buffers).
  - Handlers never hold `client_mutex` while broadcasting over network sockets to prevent deadlocks.
- **Client Event Loop (`select()` Multiplexing):**
  - The client is **single-threaded**. A single `select()` loop monitors both `STDIN_FILENO` (keyboard) and the server socket simultaneously.
  - The terminal is configured in **raw mode** (`termios`: `ICANON` and `ECHO` disabled, `VMIN=1`) so keystrokes (arrow navigation, masked passwords) are captured immediately without waiting for newline.
  - Screen rendering is powered by zero-dependency **ANSI escape codes** dividing the terminal into four regions: chat scrollback, user/room sidebar, status bar, and input line.

---

## 2. Core Operational Flows

### 2.1 Authentication Flow
1. Client connects and transmits `LOGIN|username|password`.
2. Server hashes the password with an in-house **SHA-256** implementation (`shared/sha256.c`) and compares it to the stored hash.
3. Match → `LOGIN_OK|username`; client is marked active, placed in `#general`, receives history replay, and an updated user list is broadcast.
4. Mismatch → `LOGIN_FAIL|reason`.
5. Duplicate logins for already connected usernames are rejected.

### 2.2 Messaging Flow
- **Public Chat:** Client sends `PUBLIC|room|text`. The server timestamps the message, records it in the room's 50-message ring buffer, and forwards `PUBLIC|room|sender|text|timestamp` to all room members except the sender.
- **Private DM:** Client sends `PRIVATE|recipient|text`. The server routes `PRIVATE|sender|recipient|text|timestamp` to the recipient and sends an echo copy back to the sender.
- **Typing Indicator:** Client sends `TYPING|room` on keypress; server broadcasts `TYPING|room|username`. Client auto-expires the indicator after 2 seconds of inactivity.

### 2.3 Room Lifecycle & Access Control
- `#general` is the default root room created on startup.
- `/create <name>` or `/createroom <name> [title|desc|password]` creates custom rooms.
- `/join <room> [password]` validates credentials, switches the active room, notifies both previous and target rooms, and automatically replays the last 50 messages.
- `/leave` returns the client to `#general`.
- `/deleteroom <name>` removes the room (room creator or admin only).

### 2.4 Token-Guarded Chunked File Transfer
Sender → Server → Recipient:
1. Sender initiates: `FILE_REQUEST|filename|size|target`.
2. Server verifies the size (≤ 2 MiB limit). If upload slots (max 2) or global budget (4 MiB) are full, the request is queued in a FIFO queue (cap 16) with `FILE_WAIT`.
3. Server mints a pseudo-random **upload token**, sends `FILE_GRANTED|sender|filename|TOKEN|size` to the sender, and forwards `FILE_OFFER|sender|filename|size|target` to the recipient.
4. Recipient responds: `/accept <n>` → `FILE_ACCEPT` or `/reject <n>` → `FILE_REJECT`.
5. On acceptance, sender streams `FILE_DATA|filename|TOKEN|<base64_chunk>` in 2048-byte binary chunks (~2732 Base64 characters). Server verifies the token on every chunk before forwarding.
### 2.5 Default Accounts & Test Credentials
The server pre-loads default credentials on startup:
| Username | Password | Role | Configuration File | Purpose / Permissions |
| :--- | :--- | :--- | :--- | :--- |
| `admin` | `admin123` | Administrator | `config/admin.cred` | Access to `/stats`, `/announce`, `/kick`, `/createuser`, `/deleteuser`, `/resetpass` |
| `subesh` | `subesh` | Standard User | `config/users.cred` | General messaging, file transfer, room creation |
| `saroj` | `saroj` | Standard User | `config/users.cred` | General messaging, file transfer, room creation |
| `prabesh`| `prabesh`| Standard User | `config/users.cred` | General messaging, file transfer, room creation |

---

## 3. Complete Wire Protocol Reference

### Client → Server Messages
| Message | Arguments | Purpose |
| :--- | :--- | :--- |
| `LOGIN` | `username\|password` | Authenticate client session |
| `PUBLIC` | `room\|text` | Send message to active room |
| `PRIVATE` | `recipient\|text` | Send 1-on-1 direct message |
| `TYPING` | `room` | Broadcast typing status |
| `JOIN` | `room[\|password]` | Join a named room |
| `LEAVE` | `room` | Leave room and return to `#general` |
| `CREATE` / `CREATE_ROOM` | `name[\|title\|desc\|pw]` | Create a new room |
| `DELETE_ROOM` | `name` | Delete an existing room |
| `LIST_USERS` / `LIST_ROOMS` | *(none)* | Request directory listings |
| `WHO` / `HISTORY` | `room` | List room members / replay history |
| `STATS` | *(none)* | Admin: View server metrics |
| `ANNOUNCE` | `text` | Admin: Global server broadcast |
| `KICK` | `user\|reason` | Admin: Disconnect a user |
| `CREATE_USER` / `DELETE_USER` | `user[\|password]` | Admin: Account management |
| `RESET_PASS` | `user\|new_password` | Admin: Reset account password |
| `LIST_ACCOUNTS` | *(none)* | Admin: List registered accounts |
| `FILE_REQUEST` | `filename\|size\|target` | Request file upload slot |
| `FILE_DATA` | `filename\|token\|base64` | Stream one file chunk |
| `FILE_END` | `filename` | Complete file transfer |
| `FILE_ACCEPT` / `FILE_REJECT` | `sender\|filename[\|why]` | Accept / decline file offer |
| `LOGOUT` | *(none)* | Gracefully disconnect from server |

### Server → Client Messages
| Message | Arguments | Purpose |
| :--- | :--- | :--- |
| `LOGIN_OK` | `username` | Authentication accepted |
| `LOGIN_FAIL` | `reason` | Authentication rejected |
| `PUBLIC` | `room\|sender\|text\|time` | Forwarded public message |
| `PRIVATE` | `sender\|recipient\|text\|time` | Forwarded direct message |
| `NOTIFY` / `ANNOUNCE` | `text[\|time]` | Presence / system announcement |
| `TYPING` | `room\|username` | Remote typing indicator |
| `USERS` / `ROOMS` | `item1,item2,...` | Updated directory list |
| `JOIN_OK` / `JOIN_FAIL` | `room` / `reason` | Room switch status |
| `ROOM_CREATED` | `name` | Room creation confirmation |
| `STATUS` | `text` | Status / statistics payload |
| `KICK` | `reason` | Client disconnection notice |
| `ERROR` | `reason` | Generic error notice |
| `FILE_GRANTED` | `sender\|name\|token\|size`| Upload slot & token granted |
| `FILE_OFFER` | `sender\|name\|size\|target`| Incoming file transfer offer |
| `FILE_WAIT` | `filename\|queue_pos\|size` | Upload queued notification |
| `FILE_DATA` | `sender\|name\|base64` | Forwarded file chunk |
| `FILE_END` | `sender\|name` | File transfer completed |

### 3.3 Client Slash Commands Reference

| Slash Command | Parameters / Formats | Description |
| :--- | :--- | :--- |
| `/help` | *(none)* | Displays available commands and quick-start guide |
| `/msg` | `<username> <message>` | Sends a 1-on-1 private direct message |
| `/create` | `<room> [password]` | Creates a new chat room (optional password) |
| `/createroom` | `<room> [password]`<br>`<room> "Title" [password]`<br>`<room> "Title" "Description" [password]`<br>`<room> Title\|Description\|password` | Creates a rich room with optional title, description, and password |
| `/join` | `<room> [password]` | Joins a room (validates password if protected, replays last 50 messages) |
| `/leave` | *(none)* | Leaves current room and returns to `#general` |
| `/rooms` | *(none)* | Lists all active rooms with occupant counts in chat & sidebar |
| `/who` | `[room]` | Lists active members inside the specified or current room |
| `/history` | *(none)* | Manually requests a replay of recent messages in the room |
| `/deleteroom` | `<room>` | Deletes room (creator or administrator only) |
| `/users` | *(none)* | Lists all currently online users |
| `/sendfile` | `@<username> <file_path>` *(direct)*<br>`<file_path>` *(room broadcast)* | Offers a file for transmission |
| `/accept` | `<offer#>` or `<sender> <filename>` | Accepts an incoming file offer and begins chunk streaming |
| `/reject` | `<offer#> [reason]` or `<sender> <fn> [why]` | Declines an incoming file offer and frees upload slot |
| `/clear` | *(none)* | Clears local terminal chat scrollback buffer |
| `/logout` | *(none)* | Disconnects session and returns to login wizard |
| `/quit`, `/exit` | *(none)* | Restores terminal mode and terminates application |
| `/stats` *(admin)* | *(none)* | Displays server uptime and throughput counters |
| `/announce` *(admin)* | `<message>` | Broadcasts a high-priority banner across all rooms |
| `/accounts` *(admin)* | *(none)* | Lists all registered accounts in `config/users.cred` |
| `/createuser` *(admin)* | `<username> <password>` | Dynamically creates and saves a new user account |
| `/resetpass` *(admin)* | `<username> <new_password>` | Dynamically resets password for an existing account |
| `/kick` *(admin)* | `<username> [reason]` | Immediately force-disconnects an active client socket |
| `/deleteuser` *(admin)* | `<username>` | Deletes an account from the system |

---

## 4. Academic Deliverables & Defense Resources

- **Interactive Defense Portal:** [`docs/defense_guide.html`](docs/defense_guide.html) — Live flashcards, instant Q&A search, protocol reference, and demo checklists.
- **Defense Study Document:** [`docs/defense_guide.docx`](docs/defense_guide.docx) — Formal Word document for viva defense preparation.
- **Final Project Report (v4):** [`docs/final_report_v4.docx`](docs/final_report_v4.docx) / [`docs/final_report_v4.pdf`](docs/final_report_v4.pdf).
