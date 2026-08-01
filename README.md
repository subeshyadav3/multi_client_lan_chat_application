# Multi-Client LAN Chat Application

A networked chat application built with **zero external dependencies** (plain C,
Berkeley sockets, POSIX threads). This repository contains two front-ends that
share the same line-based wire protocol:

## Projects

| Folder | Type | Description |
|--------|------|-------------|
| [`connect_gui/`](connect_gui/README.md) | **GTK3 GUI** | The original windowed client (`client/`, `server/`, raw-socket tests). |
| [`connect_cli/`](connect_cli/README.md) | **Terminal (CLI/TUI)** | A `termios` + ANSI-escape TUI driven by a single-threaded `select()` loop. Adds SHA-256 password hashing, offer-number file accepts, `/who`, `/history`, and room presence notices. |

Both run against the same threaded TCP server logic and share
`shared/protocol.h/c` and `shared/constants.h`.

## Build & run

Each sub-project builds independently with `make` (both only need `gcc`):

```bash
# GUI client/server
cd connect_gui && make clean && make

# CLI client/server
cd connect_cli && make clean && make
```

See each folder's `README.md` for usage, accounts, commands, and examples.

## Test

```bash
cd connect_cli && python3 tests/smoke.py    # CLI: SMOKE TEST PASSED
```

## Notes

- Passwords are stored as SHA-256 hashes (CLI server); GUI stores plaintext.
- Received files land in each project's `files/` directory (gitignored).
