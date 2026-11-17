#!/usr/bin/env python3

import socket
import struct
import subprocess
import sys
import time

from security_test_support import tls_client_when_ready, tls_server_connection


HEADER_FORMAT = "!BBHII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
PING = 6
PONG = 7


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def receive_exact(connection, size):
    received = b""
    while len(received) < size:
        chunk = connection.recv(size - len(received))
        if not chunk:
            break
        received += chunk
    return received


def receive_frame(connection):
    header = receive_exact(connection, HEADER_SIZE)
    if len(header) != HEADER_SIZE:
        raise RuntimeError("connection closed before a complete frame header")
    version, message_type, flags, stream_id, length = struct.unpack(
        HEADER_FORMAT, header
    )
    payload = receive_exact(connection, length)
    if len(payload) != length:
        raise RuntimeError("connection closed before a complete frame payload")
    return version, message_type, flags, stream_id, payload


def encode_frame(message_type, payload):
    return struct.pack(HEADER_FORMAT, 1, message_type, 0, 0, len(payload)) + payload


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def test_edge_answers_ping(edge_path):
    tunnel_port = unused_port()
    public_port = unused_port()
    edge = subprocess.Popen(
        [edge_path, str(tunnel_port), str(public_port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )
    try:
        time.sleep(0.2)
        token = b"12345678"
        with tls_client_when_ready("127.0.0.1", tunnel_port) as tunnel:
            tunnel.settimeout(3)
            tunnel.sendall(encode_frame(PING, token))
            version, message_type, flags, stream_id, payload = receive_frame(tunnel)
            if (version, message_type, flags, stream_id, payload) != (
                1, PONG, 0, 0, token
            ):
                raise RuntimeError("edge returned the wrong PONG frame")
    finally:
        stop_process(edge)


def test_client_sends_ping(client_path):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        tunnel_port = listener.getsockname()[1]

        client = subprocess.Popen(
            [client_path, "127.0.0.1", "1", "127.0.0.1", str(tunnel_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        try:
            listener.settimeout(3)
            raw_tunnel, _ = listener.accept()
            with tls_server_connection(raw_tunnel) as tunnel:
                tunnel.settimeout(7)
                version, message_type, flags, stream_id, payload = receive_frame(tunnel)
                if (version, message_type, flags, stream_id, len(payload)) != (
                    1, PING, 0, 0, 8
                ):
                    raise RuntimeError("client sent the wrong PING frame")
                tunnel.sendall(encode_frame(PONG, payload))
                time.sleep(0.1)
                if client.poll() is not None:
                    raise RuntimeError("client rejected a valid PONG")
        finally:
            stop_process(client)


def main():
    if len(sys.argv) != 3:
        print("usage: heartbeat_test.py <edge> <client>", file=sys.stderr)
        return 2
    try:
        test_edge_answers_ping(sys.argv[1])
        test_client_sends_ping(sys.argv[2])
        print("PING and PONG behavior passed")
        return 0
    except Exception as error:
        print(f"heartbeat test failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
