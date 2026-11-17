#!/usr/bin/env python3

import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time

from security_test_support import tls_client_when_ready


LARGE_RESPONSE_BYTES = 8 * 1024 * 1024


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class LocalHandler(socketserver.BaseRequestHandler):
    def handle(self):
        command = self.request.recv(1)
        if command == b"S":
            self.server.slow_started.set()
            chunk = b"x" * (64 * 1024)
            remaining = LARGE_RESPONSE_BYTES
            try:
                while remaining:
                    current = chunk[:min(len(chunk), remaining)]
                    self.request.sendall(current)
                    remaining -= len(current)
            except OSError:
                # The public test socket closes after the fairness assertion.
                pass
        elif command == b"N":
            self.request.sendall(b"OK")


class LocalServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, address, handler):
        super().__init__(address, handler)
        self.slow_started = threading.Event()


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def reject_malformed_tunnel(tunnel_port):
    # version=99, type=OPEN, flags=0, stream=1, payload length=0
    malformed = struct.pack("!BBHII", 99, 3, 0, 1, 0)
    with tls_client_when_ready("127.0.0.1", tunnel_port) as raw:
        raw.settimeout(3)
        raw.sendall(malformed)
        try:
            response = raw.recv(1)
            if response:
                raise RuntimeError("edge replied on a malformed tunnel")
        except ConnectionResetError:
            return


def complete_normal_request(public_port):
    deadline = time.monotonic() + 10
    last_error = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", public_port), timeout=1) as public:
                public.settimeout(3)
                public.sendall(b"N")
                if public.recv(2) == b"OK":
                    return
                last_error = RuntimeError("normal stream returned wrong bytes")
        except OSError as error:
            last_error = error
        time.sleep(0.05)
    raise RuntimeError(f"normal stream did not recover: {last_error}")


def main():
    if len(sys.argv) != 3:
        print("usage: protection_test.py <edge> <client>", file=sys.stderr)
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
    slow_public = None
    try:
        edge = subprocess.Popen(
            [edge_path, str(tunnel_port), str(public_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.2)
        if edge.poll() is not None:
            raise RuntimeError("edge exited before opening its listeners")

        reject_malformed_tunnel(tunnel_port)
        time.sleep(0.2)

        client = subprocess.Popen(
            [client_path, "127.0.0.1", str(local_port),
             "127.0.0.1", str(tunnel_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.2)

        slow_public = socket.create_connection(
            ("127.0.0.1", public_port), timeout=2
        )
        slow_public.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        slow_public.sendall(b"S")
        if not local_server.slow_started.wait(timeout=3):
            raise RuntimeError("slow local response did not start")

        # Never read the large response. Its stream should run out of credit,
        # while a different stream continues through the same tunnel.
        time.sleep(0.5)
        complete_normal_request(public_port)

        slow_public.close()
        slow_public = None

        print("Malformed tunnel and slow-stream backpressure passed")
        return 0
    except Exception as error:
        print(f"protection test failed: {error}", file=sys.stderr)
        return 1
    finally:
        if slow_public is not None:
            slow_public.close()
        stop_process(client)
        stop_process(edge)
        local_server.shutdown()
        local_server.server_close()
        local_thread.join(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
