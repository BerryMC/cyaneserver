#!/usr/bin/env python3
"""物品栏同步探针：验证登入时 WindowItems(0x14) 下发、CreativeInventoryAction 写入后
SetSlot(0x16) 反映、ClickWindow(0x07) 回 ConfirmTransaction(0x11)+SetSlot 重同步。
"""
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


def parse_varint(data: bytes, offset: int) -> tuple[int, int]:
    result = 0
    for shift in range(0, 35, 7):
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, offset
    raise ValueError("VarInt too long")


def read_varint(sock: socket.socket) -> int:
    result = 0
    for shift in range(0, 35, 7):
        chunk = sock.recv(1)
        if not chunk:
            raise EOFError("closed while reading VarInt")
        result |= (chunk[0] & 0x7F) << shift
        if not chunk[0] & 0x80:
            return result
    raise ValueError("VarInt too long")


def read_exact(sock: socket.socket, count: int) -> bytes:
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError(f"closed after {len(data)}/{count} bytes")
        data += chunk
    return bytes(data)


def read_packet(sock: socket.socket, threshold: int | None) -> tuple[int, bytes]:
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


def send_packet(sock: socket.socket, pid: int, body: bytes, threshold: int | None) -> None:
    payload = write_varint(pid) + body
    if threshold is None:
        sock.sendall(write_varint(len(payload)) + payload)
        return
    if len(payload) >= threshold:
        frame = write_varint(len(payload)) + zlib.compress(payload)
    else:
        frame = write_varint(0) + payload
    sock.sendall(write_varint(len(frame)) + frame)


def parse_slot(data: bytes, off: int) -> tuple[dict, int]:
    item_id = struct.unpack(">h", data[off:off + 2])[0]; off += 2
    if item_id < 0:
        return {"empty": True}, off
    count = data[off]; off += 1
    damage = struct.unpack(">h", data[off:off + 2])[0]; off += 2
    nbt = data[off]; off += 1  # 0 = TAG_End
    return {"id": item_id, "count": count, "damage": damage, "nbt": nbt}, off


class Client:
    def __init__(self, host: str, port: int, username: str = "InvTester"):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(6)
        self.threshold: int | None = None
        self.window_items: list | None = None
        self.set_slots: list[dict] = []
        self.transactions: list[dict] = []
        self.lock = threading.Lock()
        self._stop = False
        name = username.encode()
        handshake = (write_varint(0x00) + write_varint(340)
                     + write_varint(len(host.encode())) + host.encode()
                     + struct.pack(">H", port) + write_varint(2))
        self.sock.sendall(write_varint(len(handshake)) + handshake)
        login = write_varint(0x00) + write_varint(len(name)) + name
        self.sock.sendall(write_varint(len(login)) + login)
        self._reader = threading.Thread(target=self._loop, daemon=True)
        self._reader.start()

    def _loop(self) -> None:
        try:
            while not self._stop:
                pid, body = read_packet(self.sock, self.threshold)
                if pid == 0x03:
                    self.threshold, _ = parse_varint(body, 0)
                elif pid == 0x2F:
                    off = 8 * 3 + 4 * 2 + 1
                    tp_id, _ = parse_varint(body, off)
                    send_packet(self.sock, 0x00, write_varint(tp_id), self.threshold)
                elif pid == 0x1F:
                    send_packet(self.sock, 0x0B, body, self.threshold)
                elif pid == 0x14:  # WindowItems
                    window = body[0]
                    count = struct.unpack(">h", body[1:3])[0]
                    off = 3
                    slots = []
                    for _ in range(count):
                        slot, off = parse_slot(body, off)
                        slots.append(slot)
                    with self.lock:
                        self.window_items = {"window": window, "count": count, "slots": slots}
                elif pid == 0x16:  # SetSlot
                    window = body[0]
                    slot_id = struct.unpack(">h", body[1:3])[0]
                    item, _ = parse_slot(body, 3)
                    with self.lock:
                        self.set_slots.append({"window": window, "slot": slot_id, "item": item})
                elif pid == 0x11:  # ConfirmTransaction
                    window = body[0]
                    action = struct.unpack(">h", body[1:3])[0]
                    accepted = body[3]
                    with self.lock:
                        self.transactions.append({"window": window, "action": action,
                                                  "accepted": bool(accepted)})
        except (TimeoutError, EOFError, OSError):
            pass

    def creative_set(self, window_slot: int, item_id: int, count: int = 1, damage: int = 0) -> None:
        body = struct.pack(">h", window_slot) + struct.pack(">h", item_id) \
            + bytes([count]) + struct.pack(">h", damage) + bytes([0])
        send_packet(self.sock, 0x1B, body, self.threshold)

    def click(self, window: int, slot: int, button: int, action: int, mode: int,
              item_id: int = -1) -> None:
        body = bytes([window]) + struct.pack(">h", slot) + bytes([button]) \
            + struct.pack(">h", action) + write_varint(mode)
        if item_id < 0:
            body += struct.pack(">h", -1)
        else:
            body += struct.pack(">h", item_id) + bytes([1]) + struct.pack(">h", 0) + bytes([0])
        send_packet(self.sock, 0x07, body, self.threshold)

    def close(self) -> None:
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
    time.sleep(1.0)

    # 登入后应已收到 WindowItems
    with c.lock:
        wi = c.window_items

    # 创造给槽 36（热区栏0）放石头，服务端接收后不回 SetSlot（创造直接写），
    # 但随后 ClickWindow 触发一次权威重发
    c.creative_set(36, item_id=1, count=64)
    time.sleep(0.3)
    # 左键点击槽 36：期待 ConfirmTransaction(accepted=false) + SetSlot(36) 重同步
    c.click(window=0, slot=36, button=0, action=1, mode=0)
    time.sleep(0.6)

    c.close()
    with c.lock:
        result = {
            "window_items_on_login": {
                "received": wi is not None,
                "window": wi["window"] if wi else None,
                "count": wi["count"] if wi else None,
            },
            "set_slots": c.set_slots,
            "transactions": c.transactions,
        }
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
