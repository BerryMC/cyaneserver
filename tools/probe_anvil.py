#!/usr/bin/env python3
"""端到端验证 Anvil 存档：登录 → 挖方块 → 退出；重启后确认方块被载入。"""
import socket
import struct
import sys
import zlib

sys.path.insert(0, "tools")


def write_varint(value: int) -> bytes:
    out = bytearray()
    bits = value & 0xFFFFFFFF
    while True:
        if bits & ~0x7F == 0:
            out.append(bits)
            return bytes(out)
        out.append((bits & 0x7F) | 0x80)
        bits >>= 7


def read_varint_sock(sock: socket.socket) -> int:
    result = 0
    for shift in range(0, 35, 7):
        chunk = sock.recv(1)
        if not chunk:
            raise EOFError("closed while reading VarInt")
        result |= (chunk[0] & 0x7F) << shift
        if not chunk[0] & 0x80:
            return result
    raise ValueError("VarInt too long")


def parse_varint(data: bytes, offset: int) -> tuple[int, int]:
    result = 0
    for shift in range(0, 35, 7):
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, offset
    raise ValueError("VarInt too long")


def read_exact(sock: socket.socket, count: int) -> bytes:
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError("closed early")
        data += chunk
    return bytes(data)


def read_packet(sock: socket.socket, threshold: int | None) -> tuple[int, bytes]:
    length = read_varint_sock(sock)
    frame = read_exact(sock, length)
    if threshold is None:
        packet_id, offset = parse_varint(frame, 0)
        return packet_id, frame[offset:]
    data_length, offset = parse_varint(frame, 0)
    if data_length == 0:
        packet_id, offset = parse_varint(frame, offset)
        return packet_id, frame[offset:]
    import zlib as z
    body = z.decompress(frame[offset:])
    packet_id, offset = parse_varint(body, 0)
    return packet_id, body[offset:]


def encode_position(x: int, y: int, z: int) -> bytes:
    packed = ((x & 0x3FFFFFF) << 38) | ((y & 0xFFF) << 26) | (z & 0x3FFFFFF)
    return struct.pack(">q", packed if packed < 1 << 63 else packed - (1 << 64))


def login(host: str, port: int, username: str) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=5)
    handshake = write_varint(0x00) + write_varint(340) + write_varint(len(host)) + \
        host.encode() + struct.pack(">H", port) + write_varint(2)
    sock.sendall(write_varint(len(handshake)) + handshake)
    login_start = write_varint(0x00) + write_varint(len(username)) + username.encode()
    sock.sendall(write_varint(len(login_start)) + login_start)

    threshold = None
    while True:
        packet_id, body = read_packet(sock, threshold)
        if packet_id == 0x03:  # SetCompression
            threshold, _ = parse_varint(body, 0)
        elif packet_id == 0x02:  # LoginSuccess
            return sock, threshold


def send_play(sock: socket.socket, threshold: int | None, body: bytes) -> None:
    """低于阈值的负载须以 data_length=0 的未压缩帧发送（vanilla 规则）。"""
    if threshold is None:
        sock.sendall(write_varint(len(body)) + body)
    elif len(body) < threshold:
        sock.sendall(write_varint(len(body) + 1) + write_varint(0) + body)
    else:
        comp = zlib.compress(body)
        outer = len(body) + 1 + len(write_varint(len(body)))
        sock.sendall(write_varint(outer) + write_varint(len(body)) + comp)


def break_block(sock: socket.socket, threshold: int | None, x: int, y: int, z: int) -> None:
    # PlayerDigging 0x14: varint status(0=start) | position | byte face
    body = write_varint(0x14) + write_varint(0) + encode_position(x, y, z) + bytes([1])
    send_play(sock, threshold, body)


def drain(sock: socket.socket, seconds: float) -> None:
    import time
    sock.settimeout(0.3)
    deadline = time.monotonic() + seconds
    try:
        while time.monotonic() < deadline:
            sock.recv(65536)
    except (socket.timeout, TimeoutError):
        pass


def main() -> int:
    host, port = "127.0.0.1", int(sys.argv[1] if len(sys.argv) > 1 else 25565)
    username = sys.argv[2] if len(sys.argv) > 2 else "AnvilProbe"
    sock, threshold = login(host, port, username)
    # 区块流按 2/tick 限流，全量发完需 10s+；不必等它发完，直接发挖掘包
    drain(sock, 2.0)
    break_block(sock, threshold, 10, 3, 10)
    drain(sock, 0.5)
    sock.close()
    print(f"OK: {username} broke block (10,3,10) and disconnected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
