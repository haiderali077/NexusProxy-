#!/usr/bin/env python3

import os
import socket
import ssl
import struct
import subprocess
import sys
import time

from security_test_support import TestCertificate, edge_tls_environment


AUTH = 8
AUTH_OK = 9
AUTH_FAILED = 10


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def encode_frame(message_type, payload=b""):
    return struct.pack("!BBHII", 1, message_type, 0, 0, len(payload)) + payload


def receive_exact(connection, size):
    received = b""
    while len(received) < size:
        chunk = connection.recv(size - len(received))
        if not chunk:
            break
        received += chunk
    return received


def receive_frame(connection):
    header = receive_exact(connection, 12)
    if len(header) != 12:
        raise RuntimeError("connection closed before authentication response")
    version, message_type, flags, stream_id, length = struct.unpack(
        "!BBHII", header
    )
    payload = receive_exact(connection, length)
    return version, message_type, flags, stream_id, payload


def tls_connection(port, certificate):
    context = ssl.create_default_context(cafile=str(certificate.certificate))
    raw = socket.create_connection(("127.0.0.1", port), timeout=2)
    try:
        return context.wrap_socket(raw, server_hostname="localhost")
    except Exception:
        raw.close()
        raise


def authenticate(port, certificate, token, expected_type):
    with tls_connection(port, certificate) as connection:
        connection.settimeout(3)
        connection.sendall(encode_frame(AUTH, token.encode("ascii")))
        frame = receive_frame(connection)
        if frame != (1, expected_type, 0, 0, b""):
            raise RuntimeError(f"unexpected authentication response: {frame}")


def stop_process(process):
    if process is None or process.poll() is not None:
        return ""
    process.terminate()
    try:
        return process.communicate(timeout=3)[0]
    except subprocess.TimeoutExpired:
        process.kill()
        return process.communicate(timeout=3)[0]


def main():
    if len(sys.argv) != 2:
        print("usage: authentication_security_test.py <edge>", file=sys.stderr)
        return 2

    certificate = TestCertificate()
    correct_token = "correct-token-material-0123456789abcdef"
    wrong_token = "incorrect-token-value-0123456789abcdef"
    tunnel_port = unused_port()
    public_port = unused_port()
    edge = None
    output = ""
    try:
        environment = edge_tls_environment(certificate)
        environment["NEXUS_AUTH_TOKEN"] = correct_token
        edge = subprocess.Popen(
            [sys.argv[1], str(tunnel_port), str(public_port)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        time.sleep(0.3)
        if edge.poll() is not None:
            raise RuntimeError("edge exited before authentication test")

        for _ in range(3):
            authenticate(tunnel_port, certificate, wrong_token, AUTH_FAILED)

        try:
            authenticate(tunnel_port, certificate, correct_token, AUTH_OK)
            raise RuntimeError("rate limiter accepted a connection during cooldown")
        except (OSError, ssl.SSLError, RuntimeError) as error:
            if "during cooldown" in str(error):
                raise

        time.sleep(2.1)
        authenticate(tunnel_port, certificate, correct_token, AUTH_OK)

        output = stop_process(edge)
        edge = None
        private_key_text = certificate.private_key.read_text()
        if correct_token in output or wrong_token in output:
            raise RuntimeError("authentication token appeared in process logs")
        if private_key_text in output or "BEGIN PRIVATE KEY" in output:
            raise RuntimeError("private key material appeared in process logs")
        if "authentication rate limit is active" not in output:
            raise RuntimeError("edge did not report the generic rate-limit event")

        print("Authentication rejection, cooldown, and secret-safe logs passed")
        return 0
    except Exception as error:
        print(f"authentication security test failed: {error}", file=sys.stderr)
        return 1
    finally:
        if edge is not None:
            output += stop_process(edge)
        certificate.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
