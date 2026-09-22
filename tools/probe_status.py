#!/usr/bin/env python3
"""对 Server List Ping 做端到端探测，打印原始字节与解析后的 JSON。

用作协议 340 的行为预言机：同一个脚本分别打原版服务端与 cyane，逐字段对比。
"""
import json
import socket
import struct
import sys


def write_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def read_varint(sock: socket.socket) -> int:
    result = 0
    for shift in range(0, 35, 7):
        byte = sock.recv(1)
        if not byte:
            raise EOFError("connection closed while reading VarInt")
        result |= (byte[0] & 0x7F) << shift
        if not byte[0] & 0x80:
            return result
    raise ValueError("VarInt too long")


def read_exact(sock: socket.socket, count: int) -> bytes:
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError(f"connection closed after {len(data)}/{count} bytes")
        data += chunk
    return bytes(data)


def read_packet(sock: socket.socket) -> bytes:
    length = read_varint(sock)
    return read_exact(sock, length)


def write_packet(sock: socket.socket, payload: bytes) -> None:
    sock.sendall(write_varint(len(payload)) + payload)


def write_string(text: str) -> bytes:
    encoded = text.encode("utf-8")
    return write_varint(len(encoded)) + encoded


def probe(host: str, port: int, protocol: int = 340) -> dict:
    result: dict = {"target": f"{host}:{port}"}
    with socket.create_connection((host, port), timeout=10) as sock:
        handshake = (
            write_varint(0x00)
            + write_varint(protocol)
            + write_string(host)
            + struct.pack(">H", port)
            + write_varint(1)
        )
        write_packet(sock, handshake)
        write_packet(sock, write_varint(0x00))
        status = read_packet(sock)
        result["status_raw"] = status.hex()
        result["status_packet_id"] = status[0]
        body = status[1:]
        length = 0
        for index, byte in enumerate(body):
            length |= (byte & 0x7F) << (7 * index)
            if not byte & 0x80:
                body = body[index + 1 :]
                break
        text = body[:length].decode("utf-8")
        result["status_json"] = json.loads(text)
        result["status_text"] = text

        payload = 0x0123456789ABCDEF
        write_packet(sock, write_varint(0x01) + struct.pack(">q", payload))
        pong = read_packet(sock)
        result["pong_packet_id"] = pong[0]
        result["pong_payload"] = struct.unpack(">q", pong[1:9])[0]
        result["pong_echoes"] = result["pong_payload"] == payload
    return result


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <host> <port> [protocol]", file=sys.stderr)
        return 2
    host, port = sys.argv[1], int(sys.argv[2])
    protocol = int(sys.argv[3]) if len(sys.argv) > 3 else 340
    result = probe(host, port, protocol)
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
