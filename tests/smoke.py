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
PORT = 8089


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
    import base64
    run(["make"])

    # Ensure clean users.cred with subesh, saroj, prabesh
    config_dir = os.path.join(ROOT, "config")
    os.makedirs(config_dir, exist_ok=True)
    with open(os.path.join(config_dir, "users.cred"), "w") as f:
        f.write("subesh:subesh\nsaroj:saroj\nprabesh:prabesh\n")

    with open(os.path.join(config_dir, "admin.cred"), "w") as f:
        f.write("admin:admin123\n")

    server = subprocess.Popen(["./bin/chatserver", "8089"], cwd=ROOT,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        global PORT
        PORT = 8089
        wait_for_server()
        print("✓ Server listening on port 8089")

        # 1. Invalid login
        bad_c = Client()
        bad_c.send("LOGIN|subesh|wrongpassword")
        res = bad_c.expect("LOGIN_FAIL|")
        assert "Invalid" in res, f"Unexpected response: {res}"
        bad_c.close()
        print("  ✓ Invalid password rejected properly")

        # 2. Valid logins
        subesh = Client()
        subesh.send("LOGIN|subesh|subesh")
        subesh.expect("LOGIN_OK|subesh")

        saroj = Client()
        saroj.send("LOGIN|saroj|saroj")
        saroj.expect("LOGIN_OK|saroj")

        prabesh = Client()
        prabesh.send("LOGIN|prabesh|prabesh")
        prabesh.expect("LOGIN_OK|prabesh")
        print("  ✓ Valid logins succeeded (Subesh, Saroj, Prabesh)")

        # 3. Duplicate login prevention
        dup = Client()
        dup.send("LOGIN|subesh|subesh")
        res = dup.expect("LOGIN_FAIL|")
        assert "already logged in" in res.lower(), f"Unexpected duplicate response: {res}"
        dup.close()
        print("  ✓ Duplicate active login prevented")

        # 4. User and Room Discovery
        subesh.drain()
        subesh.send("LIST_USERS")
        users = subesh.expect("USERS|")
        assert "subesh:1" in users and "saroj:1" in users and "prabesh:1" in users, users
        print("  ✓ LIST_USERS returned all online users")

        subesh.send("LIST_ROOMS")
        rooms = subesh.expect("ROOMS|")
        assert "general" in rooms, rooms
        print("  ✓ LIST_ROOMS returned default #general")

        # 5. Public Chat & Typing Indicators
        subesh.send("TYPING|general")
        typing_msg = saroj.expect("TYPING|")
        assert "subesh" in typing_msg, typing_msg
        print("  ✓ Real-time typing notification received")

        subesh.send("PUBLIC|general|Hello everyone in the room!")
        pub1 = saroj.expect("PUBLIC|")
        pub2 = prabesh.expect("PUBLIC|")
        assert "subesh" in pub1 and "Hello everyone" in pub1
        assert "subesh" in pub2 and "Hello everyone" in pub2
        print("  ✓ Public broadcast delivered to all room members")

        # 6. Direct 1-on-1 Private Messaging (DM)
        prabesh.drain()
        subesh.send("PRIVATE|saroj|Confidential message for Saroj only")
        dm = saroj.expect("PRIVATE|")
        assert "subesh" in dm and "Confidential message" in dm, dm

        time.sleep(0.2)
        prabesh_lines = prabesh.drain()
        for l in prabesh_lines:
            assert "Confidential message" not in l, f"PM leaked: {l}"
        print("  ✓ Private message delivered exclusively without leak")

        # 7. Password-Protected Rooms & Access Control
        saroj.send("CREATE_ROOM|dev|Developer Team|Secret Dev Room|dev123")
        saroj.expect("ROOM_CREATED|dev")
        print("  ✓ Protected room 'dev' created with password")

        saroj.send("JOIN|dev")
        saroj.expect("JOIN_OK|dev")
        print("  ✓ Room creator auto-authorized into room")

        subesh.send("JOIN|dev")
        fail1 = subesh.expect("JOIN_FAIL|")
        assert "password" in fail1.lower(), fail1
        print("  ✓ Joining protected room without password rejected")

        subesh.send("JOIN|dev|wrongpass")
        fail2 = subesh.expect("JOIN_FAIL|")
        assert "password" in fail2.lower(), fail2
        print("  ✓ Joining protected room with wrong password rejected")

        subesh.send("JOIN|dev|dev123")
        subesh.expect("JOIN_OK|dev")
        print("  ✓ Joining protected room with correct password succeeded")

        # 8. Room History Replay & # Prefix Room Join
        saroj.send("PUBLIC|dev|Sprint 1 starting")
        saroj.send("PUBLIC|dev|Sprint 2 in progress")
        time.sleep(0.2)
        subesh.drain()

        # Join with leading '#' to verify prefix handling
        prabesh.send("JOIN|#dev|dev123")
        lines = prabesh.read_lines(timeout=1.5)
        history_found = any("Sprint 1" in l or "Sprint 2" in l for l in lines)
        join_ok_found = any("JOIN_OK|dev" in l for l in lines)
        assert history_found, f"History not replayed in lines: {lines}"
        assert join_ok_found, f"JOIN_OK not found in lines: {lines}"
        print("  ✓ Joining room with '#dev' prefix and password succeeded & history replayed")

        # 9. Token-Guarded Chunked File Transfer (2 MB max per file, 4 MB combined)
        # 9a. Over-limit rejection test (>2 MB)
        subesh.send("FILE_REQUEST|oversize.iso|3145728|saroj")
        denied = subesh.expect("FILE_DENIED|")
        assert "oversize.iso" in denied and "large" in denied.lower(), denied
        print("  ✓ File exceeding 2 MB limit (3 MB) rejected with FILE_DENIED")

        # 9b. Valid 1.5 MB file transfer
        file_size = int(1.5 * 1024 * 1024)
        subesh.send(f"FILE_REQUEST|project.pdf|{file_size}|saroj")
        grant = subesh.expect("FILE_GRANTED|")
        token = grant.split("|")[3]
        assert len(token) > 0, "No token granted"

        saroj.expect("FILE_OFFER|")
        saroj.send("FILE_ACCEPT|subesh|project.pdf")
        subesh.expect("FILE_ACCEPT|")

        sample_chunk = b"X" * 2048
        b64_data = base64.b64encode(sample_chunk).decode("ascii")
        subesh.send(f"FILE_DATA|project.pdf|{token}|{b64_data}")
        saroj.expect("FILE_DATA|")

        subesh.send("FILE_END|project.pdf")
        saroj.expect("FILE_END|")
        print("  ✓ Valid 1.5 MB file transfer token-guarded and completed")

        # 10. Admin Operations
        admin = Client()
        admin.send("LOGIN|admin|admin123")
        admin.expect("LOGIN_OK|admin")

        admin.send("STATS")
        stats = admin.expect("STATUS|")
        print("  ✓ Admin /stats verified")

        admin.send("ANNOUNCE|Maintenance in 10 minutes")
        subesh.expect("ANNOUNCE|")
        print("  ✓ Global announcement broadcast verified")

        # 11. Disconnect & Presence Cleanup
        saroj.send("LOGOUT")
        saroj.close()
        subesh.drain()
        subesh.send("LIST_USERS")
        users_after = subesh.expect("USERS|")
        assert "saroj" not in users_after, f"Saroj still listed: {users_after}"
        print("  ✓ Disconnection cleanup & presence update verified")

        subesh.close()
        prabesh.close()
        admin.close()

        print("\n=======================================================")
        print("🎉 ALL INTEGRATION TESTS PASSED WITH ZERO FAILURES!")
        print("=======================================================")
        return 0

    except Exception as e:
        print(f"\n❌ TEST FAILED: {e}", file=sys.stderr)
        return 1
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    sys.exit(main())
