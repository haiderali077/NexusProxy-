#!/usr/bin/env python3

import re
import signal
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time

from security_test_support import tls_client_when_ready


HEADER_FORMAT = "!BBHII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
OPEN = 3
MAX_STREAMS = 64


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def connect_when_ready(port):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"listener on port {port} did not start")


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
        raise RuntimeError("tunnel closed before a complete frame header")
    version, message_type, flags, stream_id, length = struct.unpack(
        HEADER_FORMAT, header
    )
    payload = receive_exact(connection, length)
    if len(payload) != length:
        raise RuntimeError("tunnel closed before a complete frame payload")
    return version, message_type, flags, stream_id, payload


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.kill()
    process.wait(timeout=3)


def test_stream_limit(edge_path):
    tunnel_port = unused_port()
    public_port = unused_port()
    edge = subprocess.Popen(
        [edge_path, str(tunnel_port), str(public_port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )
    tunnel = None
    public_connections = []
    try:
        tunnel = tls_client_when_ready("127.0.0.1", tunnel_port)
        tunnel.settimeout(3)
        stream_ids = set()
        for _ in range(MAX_STREAMS):
            public_connections.append(connect_when_ready(public_port))
            version, message_type, flags, stream_id, payload = receive_frame(tunnel)
            if (version, message_type, flags, payload) != (1, OPEN, 0, b""):
                raise RuntimeError("edge sent the wrong frame for a public stream")
            stream_ids.add(stream_id)

        if len(stream_ids) != MAX_STREAMS:
            raise RuntimeError("edge reused an active stream ID")

        rejected = connect_when_ready(public_port)
        rejected.settimeout(3)
        if rejected.recv(1) != b"":
            raise RuntimeError("connection beyond the stream limit stayed open")
        rejected.close()
    finally:
        for connection in public_connections:
            connection.close()
        if tunnel is not None:
            tunnel.close()
        stop_process(edge)


class EchoHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data = self.request.recv(32)
        if data:
            self.request.sendall(b"reply:" + data)


class EchoServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def test_correlated_logs(edge_path, client_path):
    tunnel_port = unused_port()
    public_port = unused_port()
    local = EchoServer(("127.0.0.1", 0), EchoHandler)
    local_thread = threading.Thread(target=local.serve_forever, daemon=True)
    local_thread.start()
    edge = None
    client = None
    try:
        edge = subprocess.Popen(
            [edge_path, str(tunnel_port), str(public_port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        client = subprocess.Popen(
            [client_path, "127.0.0.1", str(local.server_address[1]),
             "127.0.0.1", str(tunnel_port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

        deadline = time.monotonic() + 5
        response = b""
        while time.monotonic() < deadline and response != b"reply:hello":
            try:
                with socket.create_connection(
                    ("127.0.0.1", public_port), timeout=0.5
                ) as public:
                    public.settimeout(1)
                    public.sendall(b"hello")
                    response = receive_exact(public, len(b"reply:hello"))
            except OSError:
                time.sleep(0.05)
        if response != b"reply:hello":
            raise RuntimeError("request did not complete for the logging test")

        time.sleep(0.2)
        edge.send_signal(signal.SIGTERM)
        client.send_signal(signal.SIGTERM)
        edge_output, _ = edge.communicate(timeout=3)
        client_output, _ = client.communicate(timeout=3)

        edge_match = re.search(
            r"\[edge tunnel=(\d+) stream=(\d+)\] opened public stream",
            edge_output,
        )
        client_match = re.search(
            r"\[client tunnel=(\d+) stream=(\d+)\] opened local stream",
            client_output,
        )
        if edge_match is None or client_match is None:
            raise RuntimeError("stream lifecycle logs lack tunnel/stream context")
        if edge_match.group(2) != client_match.group(2):
            raise RuntimeError("edge and client logged different stream IDs")
    finally:
        stop_process(client)
        stop_process(edge)
        local.shutdown()
        local.server_close()
        local_thread.join(timeout=3)


def main():
    if len(sys.argv) != 3:
        print("usage: logging_and_limits_test.py <edge> <client>", file=sys.stderr)
        return 2
    try:
        test_stream_limit(sys.argv[1])
        test_correlated_logs(sys.argv[1], sys.argv[2])
        print("Stream limits and correlated lifecycle logs passed")
        return 0
    except Exception as error:
        print(f"logging/limits test failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
