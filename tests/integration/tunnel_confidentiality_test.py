#!/usr/bin/env python3

import select
import socket
import socketserver
import subprocess
import sys
import threading
import time


PAYLOAD = b"NEXUS-CONFIDENTIAL-PAYLOAD-4f8d2b1a"
RESPONSE = b"reply:" + PAYLOAD


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


class LocalHandler(socketserver.BaseRequestHandler):
    def handle(self):
        received = b""
        while len(received) < len(PAYLOAD):
            chunk = self.request.recv(len(PAYLOAD) - len(received))
            if not chunk:
                return
            received += chunk
        if received == PAYLOAD:
            self.request.sendall(RESPONSE)


class LocalServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def recording_proxy(listen_port, edge_port, ready, stop, captured, errors):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", listen_port))
            listener.listen(1)
            listener.settimeout(0.2)
            ready.set()

            client = None
            while client is None and not stop.is_set():
                try:
                    client, _ = listener.accept()
                except socket.timeout:
                    continue
            if client is None:
                return

            with client, socket.create_connection(
                ("127.0.0.1", edge_port), timeout=3
            ) as edge:
                client.setblocking(False)
                edge.setblocking(False)
                peers = {client: edge, edge: client}
                while not stop.is_set():
                    readable, _, exceptional = select.select(
                        [client, edge], [], [client, edge], 0.1
                    )
                    if exceptional:
                        break
                    for source in readable:
                        try:
                            data = source.recv(64 * 1024)
                        except BlockingIOError:
                            continue
                        if not data:
                            return
                        captured.extend(data)
                        peers[source].sendall(data)
    except Exception as error:
        errors.append(error)
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


def request_through_edge(public_port):
    deadline = time.monotonic() + 8
    last_error = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(
                ("127.0.0.1", public_port), timeout=1
            ) as public:
                public.settimeout(2)
                public.sendall(PAYLOAD)
                received = b""
                while len(received) < len(RESPONSE):
                    chunk = public.recv(len(RESPONSE) - len(received))
                    if not chunk:
                        break
                    received += chunk
                if received == RESPONSE:
                    return
                last_error = RuntimeError("incomplete response")
        except OSError as error:
            last_error = error
        time.sleep(0.05)
    raise RuntimeError(f"request did not complete: {last_error}")


def main():
    if len(sys.argv) != 3:
        print("usage: tunnel_confidentiality_test.py <edge> <client>", file=sys.stderr)
        return 2

    edge_path, client_path = sys.argv[1:]
    edge_tunnel_port = unused_port()
    proxy_port = unused_port()
    public_port = unused_port()
    local = LocalServer(("127.0.0.1", 0), LocalHandler)
    local_thread = threading.Thread(target=local.serve_forever, daemon=True)
    local_thread.start()

    ready = threading.Event()
    stop = threading.Event()
    captured = bytearray()
    proxy_errors = []
    proxy_thread = threading.Thread(
        target=recording_proxy,
        args=(proxy_port, edge_tunnel_port, ready, stop, captured, proxy_errors),
        daemon=True,
    )
    edge = None
    client = None
    try:
        edge = subprocess.Popen(
            [edge_path, str(edge_tunnel_port), str(public_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.2)
        proxy_thread.start()
        if not ready.wait(timeout=3):
            raise RuntimeError("recording proxy did not start")

        client = subprocess.Popen(
            [client_path, "127.0.0.1", str(local.server_address[1]),
             "127.0.0.1", str(proxy_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        request_through_edge(public_port)
        if proxy_errors:
            raise RuntimeError(f"recording proxy failed: {proxy_errors[0]}")
        if PAYLOAD in captured or RESPONSE in captured:
            raise RuntimeError("forwarded application plaintext appeared on tunnel")
        if not captured or captured[0] != 0x16:
            raise RuntimeError("recording did not observe a TLS handshake")

        print("Recorded tunnel traffic contained TLS records, not payload plaintext")
        return 0
    except Exception as error:
        print(f"tunnel confidentiality test failed: {error}", file=sys.stderr)
        return 1
    finally:
        stop.set()
        stop_process(client)
        stop_process(edge)
        if proxy_thread.is_alive():
            proxy_thread.join(timeout=3)
        local.shutdown()
        local.server_close()
        local_thread.join(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
