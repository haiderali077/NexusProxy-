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
        if len(request) == MESSAGE_SIZE:
            self.request.sendall(b"R" + request)


class LocalServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def start_edge(edge_path, tunnel_port, public_port):
    edge = subprocess.Popen(
        [edge_path, str(tunnel_port), str(public_port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )
    time.sleep(0.2)
    if edge.poll() is not None:
        raise RuntimeError("edge exited before opening its listeners")
    return edge


def complete_request(public_port, request):
    expected = b"R" + request
    deadline = time.monotonic() + 10
    last_error = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", public_port), timeout=1) as public:
                public.settimeout(3)
                public.sendall(request)
                if receive_exact(public, len(expected)) == expected:
                    return
                last_error = RuntimeError("received an incomplete response")
        except OSError as error:
            last_error = error
        time.sleep(0.05)
    raise RuntimeError(f"request did not complete: {last_error}")


def main():
    if len(sys.argv) != 3:
        print("usage: automatic_reconnect_test.py <edge> <client>", file=sys.stderr)
        return 2

    edge_path, client_path = sys.argv[1:]
    tunnel_port = unused_port()
    public_port = unused_port()
    local_server = LocalServer(("127.0.0.1", 0), LocalHandler)
    local_port = local_server.server_address[1]
    local_thread = threading.Thread(target=local_server.serve_forever, daemon=True)
    local_thread.start()

    edge = None
    client = None
    try:
        # Start the client first to prove connection refusal is retried.
        client = subprocess.Popen(
            [client_path, "127.0.0.1", str(local_port),
             "127.0.0.1", str(tunnel_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.4)
        if client.poll() is not None:
            raise RuntimeError("client exited while the edge was unavailable")

        edge = start_edge(edge_path, tunnel_port, public_port)
        complete_request(public_port, b"first!\x00\x01")

        stop_process(edge)
        edge = None
        time.sleep(0.4)
        if client.poll() is not None:
            raise RuntimeError("client exited after the tunnel disconnected")

        edge = start_edge(edge_path, tunnel_port, public_port)
        complete_request(public_port, b"second\x00\x02")

        if client.poll() is not None:
            raise RuntimeError("client process was replaced instead of reconnecting")

        print("Automatic client reconnection passed")
        return 0
    except Exception as error:
        print(f"automatic reconnect test failed: {error}", file=sys.stderr)
        return 1
    finally:
        stop_process(client)
        stop_process(edge)
        local_server.shutdown()
        local_server.server_close()
        local_thread.join(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
