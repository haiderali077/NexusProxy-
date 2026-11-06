#!/usr/bin/env python3

import signal
import socket
import subprocess
import sys
import time


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for_listener(port):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"listener on port {port} did not start")


def require_clean_exit(process, sent_signal, name):
    process.send_signal(sent_signal)
    try:
        status = process.wait(timeout=3)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"{name} did not stop within its shutdown deadline") from error
    if status != 0:
        raise RuntimeError(f"{name} exited with status {status}")


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.kill()
    process.wait(timeout=3)


def main():
    if len(sys.argv) != 3:
        print("usage: shutdown_test.py <edge> <client>", file=sys.stderr)
        return 2

    edge_path, client_path = sys.argv[1:]
    tunnel_port = unused_port()
    public_port = unused_port()
    edge = None
    client = None
    try:
        edge = subprocess.Popen(
            [edge_path, str(tunnel_port), str(public_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        wait_for_listener(tunnel_port)
        client = subprocess.Popen(
            [client_path, "127.0.0.1", "1", "127.0.0.1", str(tunnel_port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.3)

        require_clean_exit(edge, signal.SIGTERM, "edge")
        require_clean_exit(client, signal.SIGINT, "client")

        # Rebinding proves the edge released both listening descriptors.
        for port in (tunnel_port, public_port):
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(("127.0.0.1", port))

        print("Graceful SIGINT and SIGTERM shutdown passed")
        return 0
    except Exception as error:
        print(f"shutdown test failed: {error}", file=sys.stderr)
        return 1
    finally:
        stop_process(client)
        stop_process(edge)


if __name__ == "__main__":
    raise SystemExit(main())
