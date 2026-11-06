#!/usr/bin/env python3

import socket
import struct
import subprocess
import sys
import time


HEADER_FORMAT = "!BBHII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
PING = 6


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
        raise RuntimeError("client closed before sending a frame")
    version, message_type, flags, stream_id, length = struct.unpack(
        HEADER_FORMAT, header
    )
    payload = receive_exact(connection, length)
    if len(payload) != length:
        raise RuntimeError("client sent an incomplete frame")
    return version, message_type, flags, stream_id, payload


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def main():
    if len(sys.argv) != 3:
        print("usage: timeout_test.py <edge> <client>", file=sys.stderr)
        return 2

    edge_path, client_path = sys.argv[1:]
    edge = None
    client = None
    first_tunnel = None
    second_tunnel = None
    idle_tunnel = None
    try:
        # The real edge must remove a tunnel that never sends any frames.
        edge_tunnel_port = unused_port()
        public_port = unused_port()
        edge = subprocess.Popen(
            [edge_path, str(edge_tunnel_port), str(public_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.2)
        idle_tunnel = socket.create_connection(
            ("127.0.0.1", edge_tunnel_port), timeout=2
        )

        # This fake edge accepts PING but never answers. The same client process
        # must abandon the dead session and establish a second connection.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", 0))
            listener.listen(2)
            tunnel_port = listener.getsockname()[1]
            client = subprocess.Popen(
                [client_path, "127.0.0.1", "1", "127.0.0.1", str(tunnel_port)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.STDOUT,
            )

            listener.settimeout(4)
            first_tunnel, _ = listener.accept()
            first_tunnel.settimeout(4)
            version, message_type, flags, stream_id, payload = receive_frame(
                first_tunnel
            )
            if (version, message_type, flags, stream_id, len(payload)) != (
                1,
                PING,
                0,
                0,
                8,
            ):
                raise RuntimeError("client did not begin its liveness handshake")

            listener.settimeout(20)
            second_tunnel, _ = listener.accept()
            if client.poll() is not None:
                raise RuntimeError("client exited instead of reconnecting")

        idle_tunnel.settimeout(24)
        if idle_tunnel.recv(1) != b"":
            raise RuntimeError("edge sent unexpected data to an idle tunnel")

        print("Connection and tunnel timeouts passed")
        return 0
    except Exception as error:
        print(f"timeout test failed: {error}", file=sys.stderr)
        return 1
    finally:
        for connection in (first_tunnel, second_tunnel, idle_tunnel):
            if connection is not None:
                connection.close()
        stop_process(client)
        stop_process(edge)


if __name__ == "__main__":
    raise SystemExit(main())
