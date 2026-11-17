#!/usr/bin/env python3

import socket
import subprocess
import sys
import threading
import time


REQUEST = b"nexus-request\x00\xff"
RESPONSE = b"nexus-response\x00\x80\xff"


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def run_local_service(port, ready, result):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", port))
            listener.listen(1)
            listener.settimeout(10)
            ready.set()

            connection, _ = listener.accept()
            with connection:
                connection.settimeout(10)
                received = b""
                while len(received) < len(REQUEST):
                    chunk = connection.recv(len(REQUEST) - len(received))
                    if not chunk:
                        break
                    received += chunk

                result["request"] = received
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


def request_through_edge(public_port):
    deadline = time.monotonic() + 10
    last_error = None

    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", public_port), timeout=1) as public:
                public.settimeout(3)
                public.sendall(REQUEST)
                received = b""
                while len(received) < len(RESPONSE):
                    chunk = public.recv(len(RESPONSE) - len(received))
                    if not chunk:
                        break
                    received += chunk
                if received == RESPONSE:
                    return received
        except OSError as error:
            last_error = error
        time.sleep(0.05)

    raise RuntimeError(f"public request did not complete: {last_error}")


def main():
    if len(sys.argv) != 3:
        print("usage: integration_test.py <edge> <client>", file=sys.stderr)
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
        time.sleep(0.2)
        if edge.poll() is not None:
            raise RuntimeError("edge exited before opening its listeners")
        client = subprocess.Popen(
            [client_path, "127.0.0.1", str(local_port),
             "127.0.0.1", str(tunnel_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )

        response = request_through_edge(public_port)
        local_thread.join(timeout=3)

        if response != RESPONSE:
            raise RuntimeError("public caller received the wrong response bytes")
        if local_result.get("request") != REQUEST:
            raise RuntimeError("local service received the wrong request bytes")
        if "error" in local_result:
            raise RuntimeError(f"local service failed: {local_result['error']}")

        print("End-to-end tunnel request passed")
        return 0
    except Exception as error:
        print(f"integration test failed: {error}", file=sys.stderr)
        return 1
    finally:
        stop_process(client)
        stop_process(edge)


if __name__ == "__main__":
    raise SystemExit(main())
