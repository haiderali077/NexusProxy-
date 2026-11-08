#!/usr/bin/env python3

import socket
import subprocess
import sys
import time

from security_test_support import (
    TestCertificate,
    client_tls_environment,
    edge_tls_environment,
)


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def require_client_rejection(
    edge_path, client_path, edge_certificate, trusted_certificate, server_name
):
    tunnel_port = unused_port()
    public_port = unused_port()
    edge = None
    client = None
    try:
        edge = subprocess.Popen(
            [edge_path, str(tunnel_port), str(public_port)],
            env=edge_tls_environment(edge_certificate),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.25)
        if edge.poll() is not None:
            raise RuntimeError("edge exited before certificate test")

        client = subprocess.Popen(
            [client_path, "127.0.0.1", "1", "127.0.0.1", str(tunnel_port)],
            env=client_tls_environment(trusted_certificate, server_name),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        time.sleep(1.0)
        if client.poll() is not None:
            raise RuntimeError("client exited instead of applying reconnect policy")

        client.terminate()
        output, _ = client.communicate(timeout=3)
        if "TLS connection to the edge failed" not in output:
            raise RuntimeError("client did not report a TLS verification failure")
        if "[client tunnel=" in output:
            raise RuntimeError("client activated a tunnel with invalid identity")
    finally:
        stop_process(client)
        stop_process(edge)


def main():
    if len(sys.argv) != 3:
        print("usage: tls_verification_test.py <edge> <client>", file=sys.stderr)
        return 2

    valid = TestCertificate()
    untrusted = TestCertificate(common_name="untrusted")
    try:
        require_client_rejection(
            sys.argv[1], sys.argv[2], valid, untrusted, "localhost"
        )
        require_client_rejection(
            sys.argv[1], sys.argv[2], valid, valid, "wrong.example"
        )
        print("Untrusted certificates and incorrect hostnames were rejected")
        return 0
    except Exception as error:
        print(f"TLS verification test failed: {error}", file=sys.stderr)
        return 1
    finally:
        valid.cleanup()
        untrusted.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
