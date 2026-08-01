#!/usr/bin/env python3
"""Integration smoke test for ConnectHub server."""
import os
import re
import select
import signal
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
        data = (line + "\n").encode("utf-8")
        self.sock.sendall(data)

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
            lines = self.read_lines(timeout=0.5)
            for line in lines:
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

    # Ensure test user accounts exist.
    config_dir = os.path.join(ROOT, "config")
    os.makedirs(config_dir, exist_ok=True)
    with open(os.path.join(config_dir, "users.cred"), "w") as f:
        f.write("alice:alice\n")
        f.write("bob:bob\n")

    server = subprocess.Popen(["./bin/chatserver"], cwd=ROOT,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        wait_for_server()

        alice = Client()
        time.sleep(0.2)
        bob = Client()
        time.sleep(0.2)

        # Login
        alice.send("LOGIN|alice|alice")
        alice.expect("LOGIN_OK|alice")

        bob.send("LOGIN|bob|bob")
        bob.expect("LOGIN_OK|bob")

        # Bob's join may have produced a NOTIFY on alice; drain it.
        alice.drain()

        # List users
        alice.send("LIST_USERS")
        users = alice.expect("USERS|")
        assert "alice:1" in users, f"alice missing from user list: {users}"
        assert "bob:1" in users, f"bob missing from user list: {users}"

        # List rooms
        alice.send("LIST_ROOMS")
        rooms = alice.expect("ROOMS|")
        assert "general" in rooms, f"general missing from room list: {rooms}"

        # Public message: only bob should receive the broadcast.
        alice.send("PUBLIC|general|hello everyone")
        public = bob.expect("PUBLIC|")
        assert "alice" in public and "hello everyone" in public, public

        # Private message: alice should receive it.
        bob.send("PRIVATE|alice|hi alice")
        private = alice.expect("PRIVATE|")
        assert "bob" in private and "alice" in private and "hi alice" in private, private

        # Bob logs out.
        bob.send("LOGOUT")
        bob.close()
        # Alice may get a leave notification.
        alice.drain()

        # After bob leaves, only alice should remain.
        alice.send("LIST_USERS")
        users2 = alice.expect("USERS|")
        assert "alice:1" in users2, users2
        assert "bob" not in users2, f"bob still in user list: {users2}"

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
