#!/usr/bin/env python3
"""多连接互见探针：两个客户端登入同一服务器，抓取彼此的实体包并解码。

用途：诊断"某些角度玩家消失"一类问题时，直接观察服务端为对端发出的
SpawnPlayer(0x05)/EntityTeleport(0x4C)/EntityHeadLook(0x36)/EntityRelMove(0x26)
等实体包的原始字节与解码字段（坐标、角度字节、元数据终止符）。

流程：
1. A 登入并进入世界（读若干包，进入 play）。
2. B 登入并进入世界，同时移动几次（PositionLook）。
3. 打印 A 收到的、与 B 的 entity_id 相关的实体包解码结果。
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
        packet_id, offset = parse_varint(frame, 0)
        return packet_id, frame[offset:]
    data_length, offset = parse_varint(frame, 0)
    if data_length == 0:
        packet_id, offset2 = parse_varint(frame, offset)
        return packet_id, frame[offset2:]
    decompressed = zlib.decompress(frame[offset:])
    packet_id, offset2 = parse_varint(decompressed, 0)
    return packet_id, decompressed[offset2:]


def send_packet(sock: socket.socket, packet_id: int, body: bytes, threshold: int | None) -> None:
    payload = write_varint(packet_id) + body
    if threshold is None:
        sock.sendall(write_varint(len(payload)) + payload)
        return
    if len(payload) >= threshold:
        compressed = zlib.compress(payload)
        frame = write_varint(len(payload)) + compressed
    else:
        frame = write_varint(0) + payload
    sock.sendall(write_varint(len(frame)) + frame)


def angle_to_deg(byte: int) -> float:
    # 服务端字节角 = deg*256/360 & 0xFF；此处反解回 0..360 度
    return byte * 360.0 / 256.0


def decode_entity_packet(packet_id: int, body: bytes) -> dict | None:
    """只解码与实体可见性相关的包，返回含 entity_id 的字典；其余返回 None。"""
    if packet_id == 0x05:  # SpawnPlayer
        eid, off = parse_varint(body, 0)
        uuid_hex = body[off : off + 16].hex()
        off += 16
        x, y, z = struct.unpack(">ddd", body[off : off + 24])
        off += 24
        yaw_b = body[off]
        pitch_b = body[off + 1]
        off += 2
        meta_tail = body[off:].hex()
        return {
            "packet": "SpawnPlayer",
            "entity_id": eid,
            "uuid_hex": uuid_hex,
            "x": x, "y": y, "z": z,
            "yaw_byte": yaw_b, "pitch_byte": pitch_b,
            "yaw_deg": round(angle_to_deg(yaw_b), 2),
            "pitch_deg": round(angle_to_deg(pitch_b), 2),
            "metadata_tail_hex": meta_tail,
        }
    if packet_id == 0x4C:  # EntityTeleport
        eid, off = parse_varint(body, 0)
        x, y, z = struct.unpack(">ddd", body[off : off + 24])
        off += 24
        yaw_b = body[off]
        pitch_b = body[off + 1]
        on_ground = body[off + 2]
        return {
            "packet": "EntityTeleport",
            "entity_id": eid,
            "x": x, "y": y, "z": z,
            "yaw_byte": yaw_b, "pitch_byte": pitch_b,
            "yaw_deg": round(angle_to_deg(yaw_b), 2),
            "pitch_deg": round(angle_to_deg(pitch_b), 2),
            "on_ground": on_ground,
        }
    if packet_id == 0x36:  # EntityHeadLook
        eid, off = parse_varint(body, 0)
        head_b = body[off]
        return {
            "packet": "EntityHeadLook",
            "entity_id": eid,
            "head_yaw_byte": head_b,
            "head_yaw_deg": round(angle_to_deg(head_b), 2),
        }
    if packet_id == 0x26:  # EntityRelMove
        eid, off = parse_varint(body, 0)
        dx, dy, dz = struct.unpack(">hhh", body[off : off + 6])
        return {"packet": "EntityRelMove", "entity_id": eid, "dx": dx, "dy": dy, "dz": dz}
    if packet_id == 0x32:  # DestroyEntities
        count, off = parse_varint(body, 0)
        ids = []
        for _ in range(count):
            v, off = parse_varint(body, off)
            ids.append(v)
        return {"packet": "DestroyEntities", "entity_ids": ids}
    if packet_id == 0x2E:  # PlayerInfo
        action, off = parse_varint(body, 0)
        count, off = parse_varint(body, off)
        return {"packet": "PlayerInfo", "action": action, "count": count}
    return None


def handshake_and_login(sock: socket.socket, host: str, port: int, username: str, protocol: int) -> None:
    name = username.encode()
    handshake = (
        write_varint(0x00)
        + write_varint(protocol)
        + write_varint(len(host.encode()))
        + host.encode()
        + struct.pack(">H", port)
        + write_varint(2)
    )
    sock.sendall(write_varint(len(handshake)) + handshake)
    login_start = write_varint(0x00) + write_varint(len(name)) + name
    sock.sendall(write_varint(len(login_start)) + login_start)


class Client:
    def __init__(self, host: str, port: int, username: str, protocol: int = 340):
        self.host = host
        self.port = port
        self.username = username
        self.protocol = protocol
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(6)
        self.threshold: int | None = None
        self.entity_id: int | None = None
        self.captured: list[dict] = []
        self.lock = threading.Lock()
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)

    def login(self) -> None:
        handshake_and_login(self.sock, self.host, self.port, self.username, self.protocol)
        self._reader.start()

    def _read_loop(self) -> None:
        try:
            while not self._stop:
                packet_id, body = read_packet(self.sock, self.threshold)
                if packet_id == 0x03:  # SetCompression
                    self.threshold, _ = parse_varint(body, 0)
                    continue
                if packet_id == 0x23:  # JoinGame -> 自己的 entity_id（int，非 varint）
                    self.entity_id = int.from_bytes(body[:4], "big", signed=True)
                    continue
                if packet_id == 0x2F:  # PlayerPositionLook -> 需回 ConfirmTeleport
                    self._confirm_teleport(body)
                    continue
                if packet_id == 0x1F:  # KeepAlive(cb) -> 原样回
                    send_packet(self.sock, 0x0B, body, self.threshold)
                    continue
                decoded = decode_entity_packet(packet_id, body)
                if decoded is not None:
                    with self.lock:
                        self.captured.append(decoded)
        except (TimeoutError, EOFError, OSError):
            pass

    def _confirm_teleport(self, body: bytes) -> None:
        # PlayerPositionLook: double x/y/z | float yaw/pitch | byte flags | varint teleportId
        off = 8 * 3 + 4 * 2 + 1
        tp_id, _ = parse_varint(body, off)
        send_packet(self.sock, 0x00, write_varint(tp_id), self.threshold)

    def move(self, x: float, y: float, z: float, yaw: float, pitch: float) -> None:
        # PositionLook(0x0E): double x/y/z | float yaw | float pitch | bool onGround
        body = struct.pack(">ddd", x, y, z) + struct.pack(">ff", yaw, pitch) + bytes([1])
        send_packet(self.sock, 0x0E, body, self.threshold)

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

    a = Client(host, port, "Alice")
    a.login()
    time.sleep(1.0)

    b = Client(host, port, "Bob")
    b.login()
    time.sleep(1.0)

    # Bob 依次转向多个角度（含负 yaw / 大 yaw）并小步移动，触发实体包
    for yaw, pitch in [(0.0, 0.0), (-90.0, 45.0), (179.0, -45.0), (270.0, 0.0), (-179.0, 89.0)]:
        b.move(0.5, 4.0, 0.5, yaw, pitch)
        time.sleep(0.4)

    time.sleep(1.0)
    a.close()
    b.close()

    result = {
        "alice_entity_id": a.entity_id,
        "bob_entity_id": b.entity_id,
        "alice_all_entity_packets": a.captured,
    }
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
