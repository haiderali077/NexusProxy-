#!/usr/bin/env python3

import concurrent.futures
import socket
import socketserver
import subprocess
import sys
import threading
import time


CONNECTION_COUNT = 20
REQUEST_SIZE = 8


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


class LocalHandler(socketserver.BaseRequestHandler):
    def handle(self):
        request = receive_exact(self.request, REQUEST_SIZE)
        if len(request) != REQUEST_SIZE:
            return
        with self.server.received_lock:
            self.server.received.add(request)
        self.request.sendall(b"R" + request)


class LocalServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), LocalHandler)
        self.received = set()
        self.received_lock = threading.Lock()


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def send_public_request(public_port, index):
    request = index.to_bytes(4, "big") + b"\x00NPX"
    expected = b"R" + request
    deadline = time.monotonic() + 10
    last_error = None

    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", public_port), timeout=1) as public:
                public.settimeout(3)
                public.sendall(request)
                response = receive_exact(public, len(expected))
                if response == expected:
                    return request
                last_error = RuntimeError(f"wrong response for request {index}")
        except OSError as error:
            last_error = error
        time.sleep(0.05)

    raise RuntimeError(f"request {index} did not complete: {last_error}")


def main():
    if len(sys.argv) != 3:
        print("usage: concurrent_test.py <edge> <client>", file=sys.stderr)
        return 2

    edge_path, client_path = sys.argv[1:]
    tunnel_port = unused_port()
    public_port = unused_port()
    local_server = LocalServer()
    local_port = local_server.server_address[1]
    local_thread = threading.Thread(target=local_server.serve_forever, daemon=True)
    local_thread.start()

    edge = None
    client = None
    try:
        edge = subprocess.Popen(
            [edge_path, str(tunnel_port), str(public_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.2)
        if edge.poll() is not None:
            raise RuntimeError("edge exited before opening its listeners")

        client = subprocess.Popen(
            [client_path, "127.0.0.1", str(local_port),
             "127.0.0.1", str(tunnel_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )

        with concurrent.futures.ThreadPoolExecutor(
            max_workers=CONNECTION_COUNT
        ) as executor:
            futures = [
                executor.submit(send_public_request, public_port, index)
                for index in range(CONNECTION_COUNT)
            ]
            completed = {future.result() for future in futures}

        expected = {
            index.to_bytes(4, "big") + b"\x00NPX"
            for index in range(CONNECTION_COUNT)
        }
        if completed != expected:
            raise RuntimeError("public requests were lost or duplicated")
        if local_server.received != expected:
            raise RuntimeError("local service received crossed or missing streams")

        print("20 concurrent tunnel requests passed")
        return 0
    except Exception as error:
        print(f"concurrent test failed: {error}", file=sys.stderr)
        return 1
    finally:
        stop_process(client)
        stop_process(edge)
        local_server.shutdown()
        local_server.server_close()
        local_thread.join(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
