# ConnectHub Chat Application

Multi-client LAN chat application built with C, TCP sockets, POSIX threads, and GTK3.

## Project Structure

```
ConnectHub/
  bin/           - Compiled binaries
  build/         - Object files
  client/        - Client source code (GTK UI and network logic)
  server/        - Server source code (room management, logger, server core)
  shared/        - Protocol definitions and shared constants
  tests/         - Integration smoke test
  docs/          - Detailed technical documentation
  logs/          - Server log files
  files/         - Transferred files
  Makefile       - Build system
  README.md      - This file
```

## Building

Requires GCC, Make, POSIX threads, and GTK3 development libraries:

```bash
# Debian/Ubuntu
sudo apt-get install build-essential libgtk-3-dev

# Fedora
sudo dnf install gcc make gtk3-devel

cd ConnectHub
make clean && make
```

After building, the executables are placed in `bin/`:

```bash
ls bin/
# chatclient  chatserver
```

## Running

1. **Start the server** (binds to port 8080 by default):

   ```bash
   ./bin/chatserver
   ```

2. **Start one or more clients**:

   ```bash
   ./bin/chatclient
   ```

   The login window lets you choose a username, host, and port. The defaults are `127.0.0.1:8080`.

## Chat Commands

| Command | Description |
|---------|-------------|
| `/msg <user> <text>` | Send a private message |
| `/join <room>` | Join a room |
| `/create <room>` | Create a new room |
| `/announce <text>` | Admin only: broadcast an announcement (username must be `admin`) |
| `/kick <user> <reason>` | Admin only: kick a user |
| `/createuser <user> <password>` | Admin only: create a new user account |
| `/deleteuser <user>` | Admin only: delete a user account |
| `/resetpass <user> <password>` | Admin only: reset a user's password |
| `/listaccounts` | Admin only: list registered accounts |

Click a user in the sidebar to open their profile and start a private message or send a file directly.

## Documentation

For architecture, protocol details, command reference, troubleshooting, and developer notes, see [`docs/DOCUMENTATION.md`](docs/DOCUMENTATION.md).

## Testing

A Python-based integration smoke test builds the project, starts the server, and verifies login, user lists, public messages, and private messages:

```bash
python3 tests/smoke.py
```

Expected output: `SMOKE TEST PASSED`.

## Features

- Username/password authentication with duplicate prevention
- Public chat rooms (with optional passwords)
- Private messaging (`/msg username text`) and PM mode from the user list
- Real-time online user list with profile cards
- Typing indicators
- File transfer with base64 chunks
  - Send files to everyone or to a specific user
  - Incoming file accept/reject dialog
  - Transfer progress notifications
  - Received-files list in the sidebar with one-click Open
  - Automatic filename collision handling
- Server announcements
- Admin controls (kick, broadcast, view stats, user account management)
- Server-side logging in `logs/server.log`
- Graceful connect/disconnect handling
- Dark mode GTK3 UI with host/port configuration

## Protocol

Messages are line-based newline-delimited strings in the format:

```
TYPE|field1|field2|field3|field4
```

Common message types used by the client/server:

- `LOGIN|username`
- `LOGIN_OK|username`
- `LOGIN_FAIL|reason`
- `PUBLIC|room|sender|text|timestamp`
- `PRIVATE|sender|recipient|text|timestamp`
- `USERS|user1:1,user2:1`
- `ROOMS|room1,room2`
- `NOTIFY|text`
- `ANNOUNCE|sender|text|timestamp`
- `KICK|reason`

## License

Academic project for Systems Programming.
