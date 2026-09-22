#!/usr/bin/env python3
"""探测登录流程：观测服务端在 LoginStart 之后实际发出的包 ID 与字段布局。

offline 模式原版会跳过加密，直接 SetCompression + LoginSuccess；
online 模式会先发 EncryptionRequest，可据此确认包 ID 与字段结构。
"""
import json
import socket
import struct
import sys


def write_varint(value: int) -> bytes:
    out = bytearray()
    bits = value & 0xFFFFFFFF
    while True:
        if bits & ~0x7F == 0:
            out.append(bits)
            return bytes(out)
        out.append((bits & 0x7F) | 0x80)
        bits >>= 7


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


def read_packet(sock: socket.socket, threshold: int | None) -> bytes:
    """threshold 为 None 表示压缩未启用，否则按 长度|未压缩长度|数据 解码。"""
    length = read_varint(sock)
    frame = read_exact(sock, length)
    if threshold is None:
        return frame
    data_length, offset = parse_varint(frame, 0)
    if data_length == 0:
        return frame[offset:]
    import zlib

    return zlib.decompress(frame[offset:])


def parse_varint(data: bytes, offset: int) -> tuple[int, int]:
    result = 0
    for shift in range(0, 35, 7):
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, offset
    raise ValueError("VarInt too long")


def describe(packet: bytes) -> dict:
    packet_id, offset = parse_varint(packet, 0)
    info: dict = {"packet_id": hex(packet_id), "total_bytes": len(packet)}
    body = packet[offset:]
    if packet_id == 0x00:
        length, after = parse_varint(body, 0)
        info["disconnect_reason"] = body[after : after + length].decode("utf-8", "replace")
    elif packet_id == 0x01:
        sid_len, after = parse_varint(body, 0)
        info["server_id"] = body[after : after + sid_len].decode("utf-8", "replace")
        after += sid_len
        key_len, after = parse_varint(body, after)
        info["public_key_bytes"] = key_len
        info["public_key_prefix"] = body[after : after + 8].hex()
        after += key_len
        token_len, after = parse_varint(body, after)
        info["verify_token_bytes"] = token_len
        info["trailing_bytes"] = len(body) - after
    elif packet_id == 0x02:
        length, after = parse_varint(body, 0)
        info["uuid"] = body[after : after + length].decode("utf-8", "replace")
        info["username"] = body[after + length :].decode("utf-8", "replace")
    elif packet_id == 0x03:
        threshold, _ = parse_varint(body, 0)
        info["compression_threshold"] = threshold
    return info


def probe(host: str, port: int, username: str, protocol: int = 340) -> dict:
    result: dict = {"target": f"{host}:{port}", "username": username}
    with socket.create_connection((host, port), timeout=10) as sock:
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

        packets = []
        sock.settimeout(6)
        threshold = None
        try:
            for _ in range(3):
                packet = read_packet(sock, threshold)
                info = describe(packet)
                if info["packet_id"] == "0x3":
                    threshold = info["compression_threshold"]
                packets.append(info)
        except (TimeoutError, EOFError) as error:
            result["stopped_after"] = str(error)
        result["packets"] = packets
    return result


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <host> <port> [username]", file=sys.stderr)
        return 2
    username = sys.argv[3] if len(sys.argv) > 3 else "CyaneProbe"
    print(json.dumps(probe(sys.argv[1], int(sys.argv[2]), username), indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
