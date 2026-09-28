#!/usr/bin/env python3
"""合成探针：向 2x2 合成格放原木 → 验证结果槽出现木板 → 点击结果槽拿取 →
验证材料被消耗。全部通过协议层完成。"""
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


def parse_slot(data, off):
    iid = struct.unpack(">h", data[off:off + 2])[0]; off += 2
    if iid < 0:
        return {"empty": True}, off
    cnt = data[off]; off += 1
    dmg = struct.unpack(">h", data[off:off + 2])[0]; off += 2
    off += 1  # nbt TAG_End
    return {"id": iid, "count": cnt, "damage": dmg}, off


class Client:
    def __init__(self, host, port, username="Crafter"):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(6)
        self.threshold = None
        self.lock = threading.Lock()
        self.slots = {}       # 最近 SetSlot 快照
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
                if pid == 0x03:
                    self.threshold, _ = parse_varint(body, 0)
                elif pid == 0x2F:
                    tp, _ = parse_varint(body, 8 * 3 + 4 * 2 + 1)
                    send_packet(self.sock, 0x00, write_varint(tp), self.threshold)
                elif pid == 0x1F:
                    send_packet(self.sock, 0x0B, body, self.threshold)
                elif pid == 0x16:  # SetSlot
                    win = struct.unpack(">b", body[0:1])[0]
                    slot = struct.unpack(">h", body[1:3])[0]
                    item, _ = parse_slot(body, 3)
                    with self.lock:
                        self.slots[slot] = item
        except (TimeoutError, EOFError, OSError):
            pass

    def creative_set(self, win_slot, iid, count=1, damage=0):
        body = struct.pack(">h", win_slot) + struct.pack(">h", iid) \
            + bytes([count]) + struct.pack(">h", damage) + bytes([0])
        send_packet(self.sock, 0x1B, body, self.threshold)

    def click(self, win, slot, button, action, mode, item=None):
        body = bytes([win]) + struct.pack(">h", slot) + bytes([button]) \
            + struct.pack(">h", action) + write_varint(mode)
        if item is None:
            body += struct.pack(">h", -1)
        else:
            body += struct.pack(">h", item[0]) + bytes([item[1]]) + struct.pack(">h", 0) + bytes([0])
        send_packet(self.sock, 0x07, body, self.threshold)

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
    time.sleep(1.2)

    # 1) 往合成格槽 2 放一个原木 (id=17)
    c.creative_set(2, iid=17, count=3)
    time.sleep(0.5)
    with c.lock:
        result_after_place = c.slots.get(0)

    # 2) 左键点击结果槽 0 拿取（客户端报告游标为空）
    c.click(0, 0, 0, 1, 0)
    time.sleep(0.5)

    # 3) 用游标上的产物点击背包槽 9 放下
    with c.lock:
        cursor = c.slots.get(-1)
    if cursor and not cursor.get("empty"):
        c.click(0, 9, 0, 2, 0, item=(cursor["id"], cursor["count"]))
        time.sleep(0.4)

    with c.lock:
        result = {
            "result_slot_after_place": result_after_place,
            "cursor_after_take": c.slots.get(-1),
            "grid_slot_after_take": c.slots.get(2),   # 应剩 2 个原木
            "inventory_slot9": c.slots.get(9),        # 应有 4 木板
        }
    c.close()
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
