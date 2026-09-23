#!/usr/bin/env python3
"""M3c 探针：验证 ClickWindow 真实拿放、虚空死亡+重生、生存破坏掉落物+拾取。"""
import json
import socket
import struct
import sys
import threading
import time
import zlib


def write_varint(v: int) -> bytes:
    out = bytearray()
    b = v & 0xFFFFFFFF
    while True:
        if b & ~0x7F == 0:
            out.append(b)
            return bytes(out)
        out.append((b & 0x7F) | 0x80)
        b >>= 7


def parse_varint(data: bytes, off: int) -> tuple[int, int]:
    r = 0
    for s in range(0, 35, 7):
        byte = data[off]; off += 1
        r |= (byte & 0x7F) << s
        if not byte & 0x80:
            return r, off
    raise ValueError("varint too long")


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
        sock.sendall(write_varint(len(payload)) + payload)
        return
    if len(payload) >= threshold:
        frame = write_varint(len(payload)) + zlib.compress(payload)
    else:
        frame = write_varint(0) + payload
    sock.sendall(write_varint(len(frame)) + frame)


def parse_slot(data, off):
    iid = struct.unpack(">h", data[off:off + 2])[0]; off += 2
    if iid < 0:
        return {"empty": True}, off
    cnt = data[off]; off += 1
    dmg = struct.unpack(">h", data[off:off + 2])[0]; off += 2
    off += 1  # nbt TAG_End
    return {"id": iid, "count": cnt, "damage": dmg}, off


def encode_position(x, y, z):
    packed = ((x & 0x3FFFFFF) << 38) | ((y & 0xFFF) << 26) | (z & 0x3FFFFFF)
    return struct.pack(">q", packed)


class Client:
    def __init__(self, host, port, username="M3c"):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(6)
        self.threshold = None
        self.lock = threading.Lock()
        self.set_slots = []
        self.transactions = []
        self.health_updates = []
        self.respawns = 0
        self.spawn_objects = []
        self.collect_items = []
        self.destroys = []
        self.open_windows = []
        self.window_items = []
        self._stop = False
        name = username.encode()
        hs = (write_varint(0x00) + write_varint(340) + write_varint(len(host.encode()))
              + host.encode() + struct.pack(">H", port) + write_varint(2))
        self.sock.sendall(write_varint(len(hs)) + hs)
        ls = write_varint(0x00) + write_varint(len(name)) + name
        self.sock.sendall(write_varint(len(ls)) + ls)
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        try:
            while not self._stop:
                pid, body = read_packet(self.sock, self.threshold)
                if pid == 0x03:
                    self.threshold, _ = parse_varint(body, 0)
                elif pid == 0x2F:
                    off = 8 * 3 + 4 * 2 + 1
                    tp, _ = parse_varint(body, off)
                    send_packet(self.sock, 0x00, write_varint(tp), self.threshold)
                elif pid == 0x1F:
                    send_packet(self.sock, 0x0B, body, self.threshold)
                elif pid == 0x16:  # SetSlot
                    win = struct.unpack(">b", body[0:1])[0]
                    slot = struct.unpack(">h", body[1:3])[0]
                    item, _ = parse_slot(body, 3)
                    with self.lock:
                        self.set_slots.append({"window": win, "slot": slot, "item": item})
                elif pid == 0x11:  # ConfirmTransaction
                    win = body[0]
                    action = struct.unpack(">h", body[1:3])[0]
                    accepted = body[3]
                    with self.lock:
                        self.transactions.append({"window": win, "action": action,
                                                  "accepted": bool(accepted)})
                elif pid == 0x41:  # UpdateHealth
                    health = struct.unpack(">f", body[0:4])[0]
                    with self.lock:
                        self.health_updates.append(round(health, 1))
                elif pid == 0x35:  # Respawn
                    with self.lock:
                        self.respawns += 1
                elif pid == 0x00:  # SpawnObject
                    eid, off = parse_varint(body, 0)
                    off += 16  # uuid
                    otype = body[off]; off += 1
                    x, y, z = struct.unpack(">ddd", body[off:off + 24])
                    with self.lock:
                        self.spawn_objects.append({"eid": eid, "type": otype,
                                                   "x": round(x, 1), "y": round(y, 1), "z": round(z, 1)})
                elif pid == 0x4B:  # CollectItem
                    collected, off = parse_varint(body, 0)
                    collector, off = parse_varint(body, off)
                    count, off = parse_varint(body, off)
                    with self.lock:
                        self.collect_items.append({"collected": collected, "collector": collector,
                                                   "count": count})
                elif pid == 0x32:  # DestroyEntities
                    cnt, off = parse_varint(body, 0)
                    ids = []
                    for _ in range(cnt):
                        v, off = parse_varint(body, off)
                        ids.append(v)
                    with self.lock:
                        self.destroys.append(ids)
                elif pid == 0x13:  # OpenWindow
                    win = body[0]
                    wtype_len, off = parse_varint(body, 1)
                    wtype = body[off:off + wtype_len].decode("utf-8", "replace")
                    with self.lock:
                        self.open_windows.append({"window": win, "type": wtype})
                elif pid == 0x14:  # WindowItems
                    win = body[0]
                    count = struct.unpack(">h", body[1:3])[0]
                    with self.lock:
                        self.window_items.append({"window": win, "count": count})
        except (TimeoutError, EOFError, OSError):
            pass

    def creative_set(self, win_slot, iid, count=1, damage=0):
        body = struct.pack(">h", win_slot) + struct.pack(">h", iid) + bytes([count]) \
            + struct.pack(">h", damage) + bytes([0])
        send_packet(self.sock, 0x1B, body, self.threshold)

    def click(self, win, slot, button, action, mode, item=None):
        body = bytes([win]) + struct.pack(">h", slot) + bytes([button]) \
            + struct.pack(">h", action) + write_varint(mode)
        if item is None:
            body += struct.pack(">h", -1)
        else:
            body += struct.pack(">h", item[0]) + bytes([item[1]]) + struct.pack(">h", 0) + bytes([0])
        send_packet(self.sock, 0x07, body, self.threshold)

    def move(self, x, y, z):
        body = struct.pack(">ddd", x, y, z) + struct.pack(">ff", 0.0, 0.0) + bytes([1])
        send_packet(self.sock, 0x0E, body, self.threshold)

    def dig(self, x, y, z, status=0, face=1):
        body = write_varint(status) + encode_position(x, y, z) + bytes([face])
        send_packet(self.sock, 0x14, body, self.threshold)

    def place(self, x, y, z, face=1, hand=0):
        body = encode_position(x, y, z) + write_varint(face) + write_varint(hand) \
            + struct.pack(">fff", 0.5, 1.0, 0.5)
        send_packet(self.sock, 0x1F, body, self.threshold)

    def client_status(self, action=0):  # 0 = respawn
        send_packet(self.sock, 0x03, write_varint(action), self.threshold)

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
    result = {}

    # 1) 点击拿放（创造）：给槽 36 放石头 64，左键拿起→左键放到槽 9
    c = Client(host, port, "Clicker")
    time.sleep(1.0)
    c.creative_set(36, iid=1, count=64)
    time.sleep(0.3)
    c.click(0, 36, 0, 1, 0)   # 左键槽36：拿起到游标
    time.sleep(0.2)
    c.click(0, 9, 0, 2, 0)    # 左键槽9：放下游标
    time.sleep(0.4)
    with c.lock:
        result["click_set_slots"] = c.set_slots[-6:]
        result["click_transactions_accepted"] = [t["accepted"] for t in c.transactions]
    c.close()

    # 2) 虚空死亡 + 重生
    d = Client(host, port, "Faller")
    time.sleep(1.0)
    d.move(0.5, -70.0, 0.5)   # 掉到 y<-64
    time.sleep(0.5)
    d.client_status(0)         # 请求重生
    time.sleep(0.6)
    with d.lock:
        result["death_health_updates"] = d.health_updates
        result["respawns"] = d.respawns
    d.close()

    return _finish(result, host, port)


def _finish(result, host, port) -> int:
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


def survival_drops(host, port) -> dict:
    """生存模式：给热区栏放石头→放置→破坏→应生成掉落物并被拾取。"""
    c = Client(host, port, "Miner")
    time.sleep(1.0)
    # 生存下客户端手持需从背包给，用 creative_set 直接塞（服务端不校验模式）
    c.creative_set(36, iid=1, count=1)
    time.sleep(0.3)
    # 破坏出生点草方块 (0,3,0)：生存 status=2 完成挖掘
    c.dig(0, 3, 0, status=2)
    time.sleep(0.4)
    # 移动到方块处触发拾取
    c.move(0.5, 4.0, 0.5)
    time.sleep(0.8)
    with c.lock:
        out = {
            "spawn_objects": c.spawn_objects,
            "collect_items": c.collect_items,
            "destroys": c.destroys,
            "set_slots_tail": c.set_slots[-4:],
        }
    c.close()
    return out


def chest_open(host, port) -> dict:
    """创造：给热区栏放箱子(id=54)→放置→右键打开→期待 OpenWindow + 容器 WindowItems。"""
    c = Client(host, port, "Chester")
    time.sleep(1.0)
    c.creative_set(36, iid=54, count=1)   # 箱子
    time.sleep(0.3)
    c.place(2, 3, 2, face=1)              # 放到 (2,4,2)
    time.sleep(0.4)
    c.place(2, 4, 2, face=1)             # 右键箱子本身触发打开
    time.sleep(0.5)
    with c.lock:
        out = {"open_windows": c.open_windows,
               "window_items": [w for w in c.window_items if w["window"] == 1]}
    c.close()
    return out


if __name__ == "__main__":
    if len(sys.argv) > 3 and sys.argv[3] == "survival":
        print(json.dumps(survival_drops(sys.argv[1], int(sys.argv[2])), indent=2, ensure_ascii=False))
        sys.exit(0)
    if len(sys.argv) > 3 and sys.argv[3] == "chest":
        print(json.dumps(chest_open(sys.argv[1], int(sys.argv[2])), indent=2, ensure_ascii=False))
        sys.exit(0)
    sys.exit(main())

