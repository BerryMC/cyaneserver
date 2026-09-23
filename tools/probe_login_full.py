#!/usr/bin/env python3
"""Full login probe: reads all packets until server disconnects."""
import json
import socket
import struct
import sys
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


def parse_varint(data: bytes, offset: int) -> tuple[int, int]:
    result = 0
    for shift in range(0, 35, 7):
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return result, offset
    raise ValueError("VarInt too long")


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


def describe(packet_id: int, body: bytes) -> dict:
    info: dict = {"packet_id": hex(packet_id), "total_bytes": len(body)}
    if packet_id == 0x02:  # Login Success
        uuid_len, after = parse_varint(body, 0)
        info["uuid"] = body[after : after + uuid_len].decode("utf-8", "replace")
        after += uuid_len
        name_len, after2 = parse_varint(body, after)
        info["username"] = body[after2 : after2 + name_len].decode("utf-8", "replace")
    elif packet_id == 0x03:  # Set Compression
        threshold, _ = parse_varint(body, 0)
        info["compression_threshold"] = threshold
    elif packet_id == 0x23:  # JoinGame
        eid, after = parse_varint(body, 0)
        info["entity_id"] = eid
        info["gamemode"] = body[after]
        after += 1
        dim, after = parse_varint(body, after)
        info["dimension"] = dim
        info["difficulty"] = body[after]
        after += 1
        info["max_players"] = body[after]
        after += 1
        tl, after = parse_varint(body, after)
        info["level_type"] = body[after : after + tl].decode("utf-8", "replace")
        after += tl
        info["reduced_debug"] = body[after]
    elif packet_id == 0x2E:  # PlayerInfo
        action, after = parse_varint(body, 0)
        count, after = parse_varint(body, after)
        info["action"] = action
        info["count"] = count
        if count > 0 and action == 0:
            uuid_bytes = body[after : after + 16]
            info["uuid_hex"] = uuid_bytes.hex()
            after += 16
            nl, after = parse_varint(body, after)
            info["name"] = body[after : after + nl].decode("utf-8", "replace")
            after += nl
            pc, after = parse_varint(body, after)
            info["properties"] = pc
            for _ in range(pc):
                pl, after = parse_varint(body, after)
                after += pl
                vl, after = parse_varint(body, after)
                after += vl
                if body[after]:
                    after += 1
                    sl, after = parse_varint(body, after)
                    after += sl
            gm, after = parse_varint(body, after)
            info["gamemode"] = gm
            ping, after = parse_varint(body, after)
            info["ping"] = ping
            if after < len(body):
                has_display = body[after]
                info["has_display_name"] = has_display
                after += 1
                if has_display != 0 and after < len(body):
                    nl, after = parse_varint(body, after)
                    info["display_name"] = body[after : after + nl].decode("utf-8", "replace")
    elif packet_id == 0x41:  # UpdateHealth
        import struct as s
        import struct as s2
        info["health"] = s2.unpack(">f", body[:4])[0]
        info["saturation"] = s2.unpack(">f", body[5:9])[0]
        food, _ = parse_varint(body, 4)
        info["food"] = food
    elif packet_id == 0x1A:  # Disconnect
        length, after = parse_varint(body, 0)
        info["reason"] = body[after : after + length].decode("utf-8", "replace")
    elif packet_id == 0x46:  # SpawnPosition
        info["x"] = int.from_bytes(body[:4], "big", signed=True)
        info["y"] = int.from_bytes(body[4:8], "big", signed=True)
        info["z"] = int.from_bytes(body[8:12], "big", signed=True)
    elif packet_id == 0x47:  # TimeUpdate
        info["world_time"] = int.from_bytes(body[:8], "big", signed=True)
        info["day_time"] = int.from_bytes(body[8:16], "big", signed=True)
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
            while True:
                packet_id, body = read_packet(sock, threshold)
                info = describe(packet_id, body)
                if packet_id == 0x03:
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
