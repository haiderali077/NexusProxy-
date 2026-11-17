#!/usr/bin/env python3

import socket
import subprocess
import sys
import threading
import time


REQUEST_BYTES = 4 * 1024 * 1024 + 123
RESPONSE_BYTES = 5 * 1024 * 1024 + 321


def make_payload(size, marker):
    pattern = bytes([0, marker, 255, 127]) + b"NexusProxy-flow-control"
    repeats = (size + len(pattern) - 1) // len(pattern)
    return (pattern * repeats)[:size]


REQUEST = make_payload(REQUEST_BYTES, 0x31)
RESPONSE = make_payload(RESPONSE_BYTES, 0x52)


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def receive_exact(connection, size):
    received = bytearray()
    while len(received) < size:
        chunk = connection.recv(min(64 * 1024, size - len(received)))
        if not chunk:
            break
        received.extend(chunk)
    return bytes(received)


def run_local_service(port, ready, result):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", port))
            listener.listen(1)
            listener.settimeout(15)
            ready.set()

            connection, _ = listener.accept()
            with connection:
                connection.settimeout(20)
                result["request"] = receive_exact(connection, len(REQUEST))
                connection.sendall(RESPONSE)
    except Exception as error:
        result["error"] = repr(error)
        ready.set()


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def connect_when_ready(port):
    deadline = time.monotonic() + 10
    last_error = None
    while time.monotonic() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError as error:
            last_error = error
            time.sleep(0.05)
    raise RuntimeError(f"public listener did not become ready: {last_error}")


def main():
    if len(sys.argv) != 3:
        print("usage: large_transfer_test.py <edge> <client>", file=sys.stderr)
        return 2

    edge_path, client_path = sys.argv[1:]
    tunnel_port = unused_port()
    public_port = unused_port()
    local_port = unused_port()

    ready = threading.Event()
    local_result = {}
    local_thread = threading.Thread(
        target=run_local_service,
        args=(local_port, ready, local_result),
        daemon=True,
    )
    local_thread.start()
    if not ready.wait(timeout=3):
        print("local service did not start", file=sys.stderr)
        return 1

    edge = None
    client = None
    try:
        edge = subprocess.Popen(
            [edge_path, str(tunnel_port), str(public_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        client = subprocess.Popen(
            [client_path, "127.0.0.1", str(local_port),
             "127.0.0.1", str(tunnel_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )

        with connect_when_ready(public_port) as public:
            public.settimeout(20)
            public.sendall(REQUEST)
            response = receive_exact(public, len(RESPONSE))

        local_thread.join(timeout=5)
        if local_result.get("request") != REQUEST:
            raise RuntimeError("large request was truncated or corrupted")
        if response != RESPONSE:
            raise RuntimeError("large response was truncated or corrupted")
        if "error" in local_result:
            raise RuntimeError(f"local service failed: {local_result['error']}")

        print("Multi-megabyte request and response passed")
        return 0
    except Exception as error:
        print(f"large transfer test failed: {error}", file=sys.stderr)
        return 1
    finally:
        stop_process(client)
        stop_process(edge)


if __name__ == "__main__":
    raise SystemExit(main())
