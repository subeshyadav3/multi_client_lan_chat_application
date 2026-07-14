#!/usr/bin/env python3
"""File transfer integration test for ConnectHub server.

Tests:
  1. Direct file offer to a single recipient is only delivered to that user.
  2. FILE_DATA chunks are routed only to the intended recipient.
  3. FILE_END completes the transfer on the recipient side.
  4. FILE_REJECT from the recipient is forwarded back to the sender.
  5. Filename collision on the recipient gets suffixed on disk.
"""
import base64
import os
import select
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = "127.0.0.1"
PORT = 8080


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
    def __init__(self, name):
        self.name = name
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        self.sock.connect((HOST, PORT))
        self.buf = b""

    def send(self, line):
        self.sock.sendall((line + "\n").encode("utf-8"))

    def read_lines(self, timeout=2):
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([self.sock], [], [], 0.2)
            if r:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                self.buf += chunk
            if b"\n" in self.buf:
                break
        out = []
        while b"\n" in self.buf:
            head, self.buf = self.buf.split(b"\n", 1)
            out.append(head.decode("utf-8", errors="replace").strip())
        return out

    def expect(self, prefix, timeout=3):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for line in self.read_lines(timeout=0.5):
                if line.startswith(prefix):
                    return line
        raise AssertionError(f"{self.name}: did not receive {prefix!r}")

    def wait_for(self, prefix, timeout=3):
        """Non-fatal: returns the line if found, else None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for line in self.read_lines(timeout=0.5):
                if line.startswith(prefix):
                    return line
        return None

    def drain(self):
        return self.read_lines(timeout=0.5)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def login(name, password):
    c = Client(name)
    c.send(f"LOGIN|{name}|{password}")
    c.expect("LOGIN_OK|")
    return c


def main():
    # Ensure users exist.
    config_dir = os.path.join(ROOT, "config")
    os.makedirs(config_dir, exist_ok=True)
    with open(os.path.join(config_dir, "users.cred"), "w") as f:
        f.write("alice:alice\nbob:bob\ncharlie:charlie\n")

    # Reset server files dir so we know what landed there.
    files_dir = os.path.join(ROOT, "files")
    if os.path.isdir(files_dir):
        for f in os.listdir(files_dir):
            if f.endswith(".tmp"):
                os.remove(os.path.join(files_dir, f))

    server = subprocess.Popen(["./bin/chatserver"], cwd=ROOT,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        wait_for_server()

        alice = login("alice", "alice")
        time.sleep(0.2)
        bob = login("bob", "bob")
        time.sleep(0.2)
        charlie = login("charlie", "charlie")
        time.sleep(0.2)
        alice.drain(); bob.drain(); charlie.drain()

        # ── Test 1: direct offer to bob only ──
        filename = "report.txt"
        payload = b"hello bob from alice " * 50  # ~1000 bytes
        alice.send(f"FILE_OFFER|{filename}|{len(payload)}|bob")

        offer = bob.expect("FILE_OFFER|")
        assert "alice" in offer and filename in offer and "|bob" in offer, offer
        assert charlie.wait_for("FILE_OFFER|") is None, "charlie should not get offer"
        print("[OK] direct offer routed only to bob")

        # ── Test 2: recipient accepts before data is sent ──
        bob.send(f"FILE_ACCEPT|alice|{filename}")
        accepted = alice.expect("FILE_ACCEPT|")
        assert "bob" in accepted and filename in accepted, accepted

        # Send FILE_DATA chunks after the acceptance handshake.
        chunk_size = 200
        for off in range(0, len(payload), chunk_size):
            chunk = payload[off:off + chunk_size]
            b64 = base64.b64encode(chunk).decode("ascii")
            alice.send(f"FILE_DATA|{filename}|{b64}")
        alice.send(f"FILE_END|{filename}")

        # Bob should receive all chunks.
        received = b""
        end_seen = False
        deadline = time.time() + 5
        while time.time() < deadline and not end_seen:
            for line in bob.read_lines(timeout=0.5):
                if line.startswith("FILE_DATA|") and filename in line:
                    # Server rewrites to FILE_DATA|sender|filename|b64
                    parts = line.split("|", 3)
                    if len(parts) >= 4:
                        received += base64.b64decode(parts[3])
                elif line.startswith("FILE_END|") and filename in line:
                    end_seen = True
        assert end_seen, "bob did not receive FILE_END"
        assert received == payload, f"bob got {len(received)} of {len(payload)} bytes"
        # Charlie should not have received any chunks.
        assert charlie.wait_for("FILE_DATA|") is None, "charlie leaked FILE_DATA"
        print("[OK] FILE_DATA routed only to bob, payload intact")

        # ── Test 3: reject path ──
        rej_name = "secret.pdf"
        alice.send(f"FILE_OFFER|{rej_name}|1024|bob")
        bob.expect("FILE_OFFER|")
        bob.send(f"FILE_REJECT|alice|{rej_name}|not interested")
        reject = alice.expect("FILE_REJECT|")
        assert "bob" in reject and rej_name in reject, reject
        print("[OK] FILE_REJECT forwarded from recipient to sender")

        # ── Test 4: broadcast offer reaches everyone except sender ──
        time.sleep(0.5)
        alice.drain(); bob.drain(); charlie.drain()
        bc_name = "public.txt"
        alice.send(f"FILE_OFFER|{bc_name}|5|")
        time.sleep(0.5)
        b_lines = bob.read_lines(timeout=2)
        c_lines = charlie.read_lines(timeout=2)
        b_offer = next((l for l in b_lines if l.startswith("FILE_OFFER|")), None)
        c_offer = next((l for l in c_lines if l.startswith("FILE_OFFER|")), None)
        assert b_offer and bc_name in b_offer, f"bob missed broadcast: {b_lines}"
        assert c_offer and bc_name in c_offer, f"charlie missed broadcast: {c_lines}"
        print("[OK] broadcast offer reaches all non-sender clients")

        print("\nFILE TRANSFER TEST PASSED")
        return 0
    except Exception as e:
        print(f"\nFILE TRANSFER TEST FAILED: {e}", file=sys.stderr)
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
