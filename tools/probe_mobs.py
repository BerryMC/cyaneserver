#!/usr/bin/env python3
"""生物探针：登入后应收到一批 SpawnMob(0x03)，并观察到 EntityTeleport(0x4C) 移动。"""
import json
import socket
import struct
import sys
import threading
import time
import zlib


def write_varint(v: int) -> bytes:
    out = bytearray()
    bits = v & 0xFFFFFFFF
    while True:
        if bits & ~0x7F == 0:
            out.append(bits); return bytes(out)
        out.append((bits & 0x7F) | 0x80)
        bits >>= 7


def parse_varint(data: bytes, off: int):
    r = 0
    for s in range(0, 35, 7):
        byte = data[off]; off += 1
        r |= (byte & 0x7F) << s
        if not byte & 0x80:
            return r, off
    raise ValueError


def read_varint(sock) -> int:
    r = 0
    for s in range(0, 35, 7):
        c = sock.recv(1)
        if not c:
            raise EOFError
        r |= (c[0] & 0x7F) << s
        if not c[0] & 0x80:
            return r
    raise ValueError


def read_exact(sock, n) -> bytes:
    d = bytearray()
    while len(d) < n:
        c = sock.recv(n - len(d))
        if not c:
            raise EOFError
        d += c
    return bytes(d)


def read_packet(sock, threshold):
    length = read_varint(sock)
    frame = read_exact(sock, length)
    if threshold is None:
        pid, off = parse_varint(frame, 0)
        return pid, frame[off:]
    dl, off = parse_varint(frame, 0)
    if dl == 0:
        pid, off2 = parse_varint(frame, off)
        return pid, frame[off2:]
    dec = zlib.decompress(frame[off:])
    pid, off2 = parse_varint(dec, 0)
    return pid, dec[off2:]


def send_packet(sock, pid, body, threshold):
    payload = write_varint(pid) + body
    if threshold is None:
        sock.sendall(write_varint(len(payload)) + payload); return
    if len(payload) >= threshold:
        frame = write_varint(len(payload)) + zlib.compress(payload)
    else:
        frame = write_varint(0) + payload
    sock.sendall(write_varint(len(frame)) + frame)


def write_string(s: str) -> bytes:
    b = s.encode()
    return write_varint(len(b)) + b


class Client:
    def __init__(self, host, port, username="MobWatcher"):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(6)
        self.threshold = None
        self.lock = threading.Lock()
        self.in_play = False
        self.spawn_mobs = []   # (eid, type, x, z)
        self.teleports = {}    # eid -> (x, z) 最新位置
        self._stop = False
        hs = (write_varint(0x00) + write_varint(340) + write_varint(len(host.encode()))
              + host.encode() + struct.pack(">H", port) + write_varint(2))
        self.sock.sendall(write_varint(len(hs)) + hs)
        ls = write_varint(0x00) + write_string(username)
        self.sock.sendall(write_varint(len(ls)) + ls)
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        try:
            while not self._stop:
                pid, body = read_packet(self.sock, self.threshold)
                if pid == 0x23:  # JoinGame：进入 play 态
                    self.in_play = True
                elif pid == 0x03 and not self.in_play:
                    self.threshold, _ = parse_varint(body, 0)
                elif pid == 0x2F:
                    tp, _ = parse_varint(body, 8 * 3 + 4 * 2 + 1)
                    send_packet(self.sock, 0x00, write_varint(tp), self.threshold)
                elif pid == 0x1F:
                    send_packet(self.sock, 0x0B, body, self.threshold)
                elif pid == 0x03:  # SpawnMob
                    eid, off = parse_varint(body, 0)
                    off += 16  # uuid
                    mtype, off = parse_varint(body, off)
                    x, y, z = struct.unpack(">ddd", body[off:off + 24])
                    with self.lock:
                        self.spawn_mobs.append({"eid": eid, "type": mtype,
                                                "x": round(x, 1), "z": round(z, 1)})
                elif pid == 0x4C:  # EntityTeleport
                    eid, off = parse_varint(body, 0)
                    x, y, z = struct.unpack(">ddd", body[off:off + 24])
                    with self.lock:
                        self.teleports[eid] = (round(x, 1), round(z, 1))
        except (TimeoutError, EOFError, OSError):
            pass

    def close(self):
        self._stop = True
        try:
            self.sock.close()
        except OSError:
            pass


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <host> <port>", file=sys.stderr)
        return 2
    host, port = sys.argv[1], int(sys.argv[2])
    c = Client(host, port)
    # 观察一段漫游时间
    time.sleep(8.0)
    with c.lock:
        eids = {m["eid"] for m in c.spawn_mobs}
        moved = {eid: pos for eid, pos in c.teleports.items() if eid in eids}
        result = {
            "spawned_mobs": c.spawn_mobs,
            "mobs_that_moved": len(moved),
            "sample_move": list(moved.items())[:3],
        }
    c.close()
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
