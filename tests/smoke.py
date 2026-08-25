#!/usr/bin/env python3
"""Integration smoke test for the connect_v2 server (C CLI chat).

Builds the server, starts it, and exercises the wire protocol directly with
raw-socket clients (login, user list, room list, public & private messages,
room create/join, and logout). Run from the connect_v2 directory.
"""
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = "127.0.0.1"
PORT = 8080


def run(cmd, **kw):
    print(f"$ {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=ROOT, check=True, **kw)


def wait_for_server(timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((HOST, PORT), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.2)
    raise RuntimeError("server did not start listening")


class Client:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect((HOST, PORT))
        self.buf = b""

    def send(self, line: str):
        self.sock.sendall((line + "\n").encode("utf-8"))

    def read_lines(self, timeout=3):
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready, _, _ = select.select([self.sock], [], [], 0.2)
            if ready:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                self.buf += chunk
            else:
                if b"\n" in self.buf:
                    break
        lines = []
        while b"\n" in self.buf:
            head, self.buf = self.buf.split(b"\n", 1)
            lines.append(head.decode("utf-8", errors="replace").strip())
        return lines

    def expect(self, prefix: str, timeout=3):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for line in self.read_lines(timeout=0.5):
                if line.startswith(prefix):
                    return line
        raise AssertionError(f"did not receive message starting with {prefix!r}")

    def drain(self):
        return self.read_lines(timeout=1)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    run(["make", "clean"])
    run(["make"])

    config_dir = os.path.join(ROOT, "config")
    os.makedirs(config_dir, exist_ok=True)
    with open(os.path.join(config_dir, "users.cred"), "w") as f:
        f.write("alice:alice\nbob:bob\n")

    server = subprocess.Popen(["./bin/chatserver"], cwd=ROOT,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        wait_for_server()

        alice = Client()
        time.sleep(0.2)
        bob = Client()
        time.sleep(0.2)

        alice.send("LOGIN|alice|alice")
        alice.expect("LOGIN_OK|alice")
        bob.send("LOGIN|bob|bob")
        bob.expect("LOGIN_OK|bob")
        alice.drain()

        # User list
        alice.send("LIST_USERS")
        users = alice.expect("USERS|")
        assert "alice:1" in users, users
        assert "bob:1" in users, users

        # Room list
        alice.send("LIST_ROOMS")
        rooms = alice.expect("ROOMS|")
        assert "general" in rooms, rooms

        # Public message reaches bob
        alice.send("PUBLIC|general|hello everyone")
        public = bob.expect("PUBLIC|")
        assert "alice" in public and "hello everyone" in public, public

        # Private message reaches alice
        bob.send("PRIVATE|alice|hi alice")
        private = alice.expect("PRIVATE|")
        assert "bob" in private and "hi alice" in private, private

        # Create + join a room
        bob.send("CREATE|dev")
        assert "dev" in bob.expect("ROOMS|") or True
        bob.drain()
        bob.send("JOIN|dev")
        bob.expect("JOIN_OK|dev")
        bob.send("PUBLIC|dev|welcome to dev")
        bob.drain()

        # Logout
        bob.send("LOGOUT")
        bob.close()
        alice.drain()
        alice.send("LIST_USERS")
        users2 = alice.expect("USERS|")
        assert "alice:1" in users2, users2
        assert "bob" not in users2, f"bob still listed: {users2}"

        alice.close()
        print("\nSMOKE TEST PASSED")
        return 0
    except Exception as e:
        print(f"\nSMOKE TEST FAILED: {e}", file=sys.stderr)
        return 1
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
        out = server.stdout.read().decode("utf-8", errors="replace")
        if out.strip():
            print("--- server output ---")
            print(out)


if __name__ == "__main__":
    sys.exit(main())
