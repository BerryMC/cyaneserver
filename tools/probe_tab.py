#!/usr/bin/env python3
"""探测 Tab 补全：测试 /g（部分命令名）、/gamemode <空格>、/kill <空格>。"""
import json
import socket
import struct
import sys
import threading
import time
import zlib


def write_varint(value: int) -> bytes:
    out = bytearray()
    bits = value & 0xFFFFFFFF
    while True:
        if bits & ~0x7F == 0:
            out.append(bits)
            return bytes(out)
        out.append((bits & 0x7F) | 0x80)
        bits >>= 7


def parse_varint(data: bytes, off: int) -> tuple[int, int]:
    result = 0
    for shift in range(0, 35, 7):
        byte = data[off]; off += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, off
    raise ValueError("VarInt too long")


def read_varint(sock) -> int:
    result = 0
    for shift in range(0, 35, 7):
        chunk = sock.recv(1)
        if not chunk:
            raise EOFError("closed")
        result |= (chunk[0] & 0x7F) << shift
        if not chunk[0] & 0x80:
            return result
    raise ValueError("VarInt too long")


def read_exact(sock, count: int) -> bytes:
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError("closed")
        data += chunk
    return bytes(data)


def read_packet(sock, threshold):
    length = read_varint(sock)
    frame = read_exact(sock, length)
    if threshold is None:
        pid, off = parse_varint(frame, 0)
        return pid, frame[off:]
    data_length, off = parse_varint(frame, 0)
    if data_length == 0:
        pid, off2 = parse_varint(frame, off)
        return pid, frame[off2:]
    dec = zlib.decompress(frame[off:])
    pid, off2 = parse_varint(dec, 0)
    return pid, dec[off2:]


def send_packet(sock, pid: int, body: bytes, threshold):
    payload = write_varint(pid) + body
    if threshold is None:
        sock.sendall(write_varint(len(payload)) + payload)
        return
    if len(payload) >= threshold:
        frame = write_varint(len(payload)) + zlib.compress(payload)
    else:
        frame = write_varint(0) + payload
    sock.sendall(write_varint(len(frame)) + frame)


class Client:
    def __init__(self, host, port, username):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(6)
        self.threshold = None
        self.lock = threading.Lock()
        self.tab_results = []
        self._stop = False
        name = username.encode()
        hs = (write_varint(0x00) + write_varint(340) + write_varint(len(host.encode()))
              + host.encode() + struct.pack(">H", port) + write_varint(2))
        self.sock.sendall(write_varint(len(hs)) + hs)
        ls = write_varint(0x00) + write_string("CmdTester")
        self.sock.sendall(write_varint(len(ls)) + ls)
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        try:
            while not self._stop:
                pid, body = read_packet(self.sock, self.threshold)
                if pid == 0x03:
                    self.threshold, _ = parse_varint(body, 0)
                elif pid == 0x2F:
                    tp, _ = parse_varint(body, 8 * 3 + 4 * 2 + 1)
                    send_packet(self.sock, 0x00, write_varint(tp), self.threshold)
                elif pid == 0x1F:
                    send_packet(self.sock, 0x0B, body, self.threshold)
                elif pid == 0x0E:  # TabComplete
                    count, off = parse_varint(body, 0)
                    matches = []
                    for _ in range(count):
                        n, off = parse_varint(body, off)
                        matches.append(body[off:off + n].decode("utf-8", "replace"))
                        off += n
                    with self.lock:
                        self.tab_results.append({"matches": matches})
        except (TimeoutError, EOFError, OSError):
            pass

    def tab_complete(self, text, tx=1):
        body = write_string(text) + bytes([1])  # assumeCommand: true
        body += struct.pack(">iii", 0, 63, 0)  # lookedAtBlock: optional position
        send_packet(self.sock, 0x01, body, self.threshold)

    def _string(self, s):
        b = s.encode("utf-8")
        return write_varint(len(b)) + b

    def close(self):
        self._stop = True
        try:
            self.sock.close()
        except OSError:
            pass


def write_string(s):
    b = s.encode("utf-8")
    return write_varint(len(b)) + b


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <host> <port>", file=sys.stderr)
        return 2
    host, port = sys.argv[1], int(sys.argv[2])

    c = Client(host, port, "TabTester")
    time.sleep(1.2)

    # Test 1: partial command "/g"
    c.tab_complete("/g", tx=1)
    time.sleep(0.4)

    # Test 2: "/gamemode " (space after command)
    c.tab_complete("/gamemode ", tx=2)
    time.sleep(0.4)

    # Test 3: "/kill " (player name completion)
    c.tab_complete("/kill ", tx=3)
    time.sleep(0.4)

    # Test 4: "/tp " (player name completion)
    c.tab_complete("/tp ", tx=4)
    time.sleep(0.4)

    # Test 5: empty "/"
    c.tab_complete("/", tx=5)
    time.sleep(0.4)

    with c.lock:
        result = {"tab_completions": c.tab_results}
    c.close()
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
