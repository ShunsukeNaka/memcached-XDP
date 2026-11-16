#!/usr/bin/env python3
"""Memcached の TCP / UDP 疎通確認 (標準ライブラリのみ)。

TCP で set した値を TCP と UDP の両方で get できるかを確かめる。
UDP の GET は XDP で先回りして返す対象なので、ここが通ることが前提条件になる。

UDP リクエストには 8 バイトのフレームヘッダが付く:
  request id (2) | sequence number (2) | total datagrams (2) | reserved (2)
"""
import argparse
import os
import socket
import struct
import sys


def tcp_cmd(host: str, port: int, payload: bytes) -> bytes:
    with socket.create_connection((host, port), timeout=2) as s:
        s.sendall(payload)
        buf = b""
        while not (buf.endswith(b"END\r\n") or buf.endswith(b"STORED\r\n")
                   or buf.endswith(b"ERROR\r\n")):
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf


def udp_get(host: str, port: int, key: str, req_id: int = 0x1234) -> bytes:
    header = struct.pack("!HHHH", req_id, 0, 1, 0)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(2)
        s.sendto(header + f"get {key}\r\n".encode(), (host, port))
        data, _ = s.recvfrom(65535)
    rid, seq, total, _ = struct.unpack("!HHHH", data[:8])
    if rid != req_id or seq != 0 or total != 1:
        raise RuntimeError(f"unexpected UDP header: id={rid} seq={seq} total={total}")
    return data[8:]


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--host", default=os.environ.get("MEMCACHED_HOST", "127.0.0.1"))
    p.add_argument("--port", type=int, default=int(os.environ.get("MEMCACHED_PORT", "11211")))
    args = p.parse_args()

    key, val = "mcxdp:check", b"hello-xdp"
    expected = f"VALUE {key} 0 {len(val)}\r\n".encode() + val + b"\r\nEND\r\n"

    ok = True

    r = tcp_cmd(args.host, args.port, f"set {key} 0 60 {len(val)}\r\n".encode() + val + b"\r\n")
    print(f"[tcp set] {r!r}")
    ok &= r == b"STORED\r\n"

    r = tcp_cmd(args.host, args.port, f"get {key}\r\n".encode())
    print(f"[tcp get] {r!r}")
    ok &= r == expected

    try:
        r = udp_get(args.host, args.port, key)
        print(f"[udp get] {r!r}")
        ok &= r == expected
    except (socket.timeout, RuntimeError) as e:
        print(f"[udp get] FAILED: {e}  (memcached を -U 付きで起動しているか確認)")
        ok = False

    print("OK" if ok else "NG")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
