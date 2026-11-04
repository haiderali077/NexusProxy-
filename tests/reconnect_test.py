#!/usr/bin/env python3

import socket
import socketserver
import subprocess
import sys
import threading
import time


MESSAGE_SIZE = 8


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
        request = receive_exact(self.request, MESSAGE_SIZE)
        with self.server.received_lock:
            self.server.received.append(request)
        if len(request) == MESSAGE_SIZE:
            self.request.sendall(b"R" + request)


class LocalServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), LocalHandler)
        self.received = []
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


def start_client(client_path, local_port, tunnel_port):
    return subprocess.Popen(
        [client_path, "127.0.0.1", str(local_port),
         "127.0.0.1", str(tunnel_port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )


def complete_request(public_port, request):
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
                    return
                last_error = RuntimeError("received an incomplete response")
        except OSError as error:
            last_error = error
        time.sleep(0.05)

    raise RuntimeError(f"request did not complete: {last_error}")


def main():
    if len(sys.argv) != 3:
        print("usage: reconnect_test.py <edge> <client>", file=sys.stderr)
        return 2

    edge_path, client_path = sys.argv[1:]
    tunnel_port = unused_port()
    public_port = unused_port()
    local_server = LocalServer()
    local_port = local_server.server_address[1]
    local_thread = threading.Thread(target=local_server.serve_forever, daemon=True)
    local_thread.start()

    edge = None
    first_client = None
    second_client = None
    try:
        edge = subprocess.Popen(
            [edge_path, str(tunnel_port), str(public_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.2)
        if edge.poll() is not None:
            raise RuntimeError("edge exited before opening its listeners")

        first_client = start_client(client_path, local_port, tunnel_port)
        time.sleep(0.2)

        # End one public stream before its fixed-size request is complete.
        with socket.create_connection(("127.0.0.1", public_port), timeout=2) as public:
            public.sendall(b"partial")
        time.sleep(0.2)

        # The tunnel must remain useful after that individual stream closes.
        first_request = b"first!\x00\x01"
        complete_request(public_port, first_request)

        # Stop the entire developer client, then connect a fresh process.
        stop_process(first_client)
        first_client = None
        time.sleep(0.3)

        second_client = start_client(client_path, local_port, tunnel_port)
        second_request = b"second\x00\x02"
        complete_request(public_port, second_request)

        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            with local_server.received_lock:
                received = list(local_server.received)
            if first_request in received and second_request in received:
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("local service did not observe both complete requests")

        print("Stream disconnect and tunnel reconnection passed")
        return 0
    except Exception as error:
        print(f"reconnect test failed: {error}", file=sys.stderr)
        return 1
    finally:
        stop_process(second_client)
        stop_process(first_client)
        stop_process(edge)
        local_server.shutdown()
        local_server.server_close()
        local_thread.join(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
