#!/usr/bin/env python3
"""方块破坏/放置探针：登入服务器，发 PlayerDigging 与 BlockPlacement，
抓取回发的 BlockChange(0x0B)，解码坐标与方块状态验证闭环。

创造模式下：破坏发 status=0，放置需先用 CreativeInventoryAction 往热区栏放一个
方块型物品（如石头 id=1），再发 BlockPlacement。
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


def encode_position(x: int, y: int, z: int) -> bytes:
    packed = ((x & 0x3FFFFFF) << 38) | ((y & 0xFFF) << 26) | (z & 0x3FFFFFF)
    return struct.pack(">q", packed)


def decode_position(packed: int) -> tuple[int, int, int]:
    x = packed >> 38
    y = (packed >> 26) & 0xFFF
    z = packed & 0x3FFFFFF
    if x >= 1 << 25: x -= 1 << 26
    if y >= 1 << 11: y -= 1 << 12
    if z >= 1 << 25: z -= 1 << 26
    return x, y, z


class Client:
    def __init__(self, host: str, port: int, username: str, protocol: int = 340):
        self.host = host
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=10)
        self.sock.settimeout(6)
        self.threshold: int | None = None
        self.block_changes: list[dict] = []
        self.lock = threading.Lock()
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        name = username.encode()
        handshake = (write_varint(0x00) + write_varint(protocol)
                     + write_varint(len(host.encode())) + host.encode()
                     + struct.pack(">H", port) + write_varint(2))
        self.sock.sendall(write_varint(len(handshake)) + handshake)
        login_start = write_varint(0x00) + write_varint(len(name)) + name
        self.sock.sendall(write_varint(len(login_start)) + login_start)
        self._reader.start()

    def _read_loop(self) -> None:
        try:
            while not self._stop:
                packet_id, body = read_packet(self.sock, self.threshold)
                if packet_id == 0x03:
                    self.threshold, _ = parse_varint(body, 0)
                elif packet_id == 0x2F:
                    off = 8 * 3 + 4 * 2 + 1
                    tp_id, _ = parse_varint(body, off)
                    send_packet(self.sock, 0x00, write_varint(tp_id), self.threshold)
                elif packet_id == 0x1F:
                    send_packet(self.sock, 0x0B, body, self.threshold)
                elif packet_id == 0x0B:  # BlockChange
                    packed = struct.unpack(">q", body[:8])[0]
                    state, _ = parse_varint(body, 8)
                    x, y, z = decode_position(packed)
                    with self.lock:
                        self.block_changes.append(
                            {"x": x, "y": y, "z": z, "state": state,
                             "block_id": state >> 4, "meta": state & 0xF})
        except (TimeoutError, EOFError, OSError):
            pass

    def set_creative_slot(self, hotbar: int, item_id: int, count: int = 1, damage: int = 0) -> None:
        # CreativeInventoryAction(0x1B)：short slot | item(short id|byte count|short damage|nbt)
        window_slot = 36 + hotbar
        body = struct.pack(">h", window_slot) + struct.pack(">h", item_id) \
            + bytes([count]) + struct.pack(">h", damage) + bytes([0])  # nbt: 0 = TAG_End (无)
        send_packet(self.sock, 0x1B, body, self.threshold)

    def hold(self, hotbar: int) -> None:
        send_packet(self.sock, 0x1A, struct.pack(">h", hotbar), self.threshold)

    def dig(self, x: int, y: int, z: int, status: int = 0, face: int = 1) -> None:
        # PlayerDigging(0x14)：varint status | position | byte face
        body = write_varint(status) + encode_position(x, y, z) + bytes([face])
        send_packet(self.sock, 0x14, body, self.threshold)

    def place(self, x: int, y: int, z: int, face: int = 1, hand: int = 0) -> None:
        # BlockPlacement(0x1F)：position | varint face | varint hand | float cursorX/Y/Z
        body = encode_position(x, y, z) + write_varint(face) + write_varint(hand) \
            + struct.pack(">fff", 0.5, 1.0, 0.5)
        send_packet(self.sock, 0x1F, body, self.threshold)

    def move(self, x: float, y: float, z: float, yaw: float, pitch: float) -> None:
        # PositionLook(0x0E)：double x/y/z | float yaw | float pitch | bool onGround
        body = struct.pack(">ddd", x, y, z) + struct.pack(">ff", yaw, pitch) + bytes([1])
        send_packet(self.sock, 0x0E, body, self.threshold)

    def close(self) -> None:
        self._stop = True
        try:
            self.sock.close()
        except OSError:
            pass

    def read_block(self, cx: int, cz: int) -> dict | None:
        """读取自己刚进入时收到的某区块 (cx,cz) 的 section0，返回若干采样点状态。"""
        return None


def sample_chunk_column(host: str, port: int, cx: int, cz: int, samples: list[tuple[int, int, int]],
                        username: str = "Reader") -> dict:
    """新登入一个客户端，抓取指定区块的 ChunkData(0x20)，解码 section0 并采样几个坐标。"""
    result: dict = {}
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(6)
    name = username.encode()
    handshake = (write_varint(0x00) + write_varint(340)
                 + write_varint(len(host.encode())) + host.encode()
                 + struct.pack(">H", port) + write_varint(2))
    sock.sendall(write_varint(len(handshake)) + handshake)
    login = write_varint(0x00) + write_varint(len(name)) + name
    sock.sendall(write_varint(len(login)) + login)
    threshold: int | None = None
    try:
        deadline = time.time() + 5
        while time.time() < deadline:
            packet_id, body = read_packet(sock, threshold)
            if packet_id == 0x03:
                threshold, _ = parse_varint(body, 0)
            elif packet_id == 0x20:  # ChunkData
                bx = struct.unpack(">i", body[:4])[0]
                bz = struct.unpack(">i", body[4:8])[0]
                if bx == cx and bz == cz:
                    result["found_chunk"] = True
                    result["samples"] = decode_section0_samples(body, samples)
                    break
    except (TimeoutError, EOFError, OSError):
        pass
    finally:
        sock.close()
    return result


def decode_section0_samples(chunk_body: bytes, samples: list[tuple[int, int, int]]) -> list[dict]:
    """解析 ChunkData 载荷的 section0（最低 16 层），取指定局部坐标的方块状态。"""
    off = 8  # skip chunkX/Z
    off += 1  # groundUp bool
    bitmask, off = parse_varint(chunk_body, off)
    data_len, off = parse_varint(chunk_body, off)
    data = chunk_body[off:off + data_len]
    # 仅解析 section0（bit0），它一定存在（含 bedrock/dirt/grass）
    if not (bitmask & 1):
        return [{"error": "section0 not present"}]
    p = 0
    bits_per_block = data[p]; p += 1
    palette_len, p = parse_varint(data, p)
    palette = []
    for _ in range(palette_len):
        v, p = parse_varint(data, p)
        palette.append(v)
    longs_count, p = parse_varint(data, p)
    longs = []
    for _ in range(longs_count):
        longs.append(struct.unpack(">Q", data[p:p + 8])[0]); p += 8

    def block_at(lx: int, ly: int, lz: int) -> int:
        idx = (ly << 8) | (lz << 4) | lx
        bit = idx * bits_per_block
        long_i = bit >> 6
        offset_in = bit & 63
        mask = (1 << bits_per_block) - 1
        val = longs[long_i] >> offset_in
        if offset_in + bits_per_block > 64:
            val |= longs[long_i + 1] << (64 - offset_in)
        pidx = val & mask
        return palette[pidx] if pidx < len(palette) else -1

    out = []
    for (lx, ly, lz) in samples:
        state = block_at(lx, ly, lz)
        out.append({"lx": lx, "ly": ly, "lz": lz, "state": state,
                    "block_id": state >> 4 if state >= 0 else -1})
    return out


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <host> <port>", file=sys.stderr)
        return 2
    host, port = sys.argv[1], int(sys.argv[2])

    c = Client(host, port, "Digger")
    time.sleep(1.0)

    # 破坏出生点草方块层的一个方块 (0,3,0)，创造模式 status=0 即刻生效
    c.dig(0, 3, 0, status=0)
    time.sleep(0.5)

    # 往热区栏 0 放石头(id=1)，选中它，在 (2,3,2) 顶面放置 -> 落在 (2,4,2)
    c.set_creative_slot(0, item_id=1)
    c.hold(0)
    time.sleep(0.5)
    # 移动到 (0,4,0) 脚下，使脚下放置命中自己的碰撞体
    c.move(0.5, 4.0, 0.5, 0.0, 0.0)
    time.sleep(0.5)
    # 1) 合法放置：在 (2,3,2) 顶面放石头 -> (2,4,2)
    c.place(2, 3, 2, face=1)
    time.sleep(0.8)
    # 2) 非法放置：右键自己的脚下 (0,3,0) 顶面 -> 目标 (0,4,0) 落在玩家碰撞体内
    c.place(0, 3, 0, face=1)
    time.sleep(0.8)

    c.close()
    with c.lock:
        changes = list(c.block_changes)

    # 持久化验证：新客户端进入，读区块 (0,0) 的 section0
    # (0,3,0) 应已被破坏为空气(0)；(2,4,2) 应是石头(16)；脚下放置被拒不生效
    time.sleep(0.3)
    persist = sample_chunk_column(host, port, 0, 0,
                                  [(0, 3, 0), (1, 3, 1), (0, 0, 0), (2, 4, 2)])

    # 验证脚下放置被拒：(0,4,0) 应仍是空气(state 0)
    foot_check = sample_chunk_column(host, port, 0, 0, [(0, 4, 0)])

    print(json.dumps({"block_changes": changes, "persistence": persist,
                      "foot_placement_rejected": foot_check},
                     indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
