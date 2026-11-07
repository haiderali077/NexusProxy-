import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import time


class TestCertificate:
    def __init__(self, common_name="localhost", include_localhost=True):
        self._temporary_directory = tempfile.TemporaryDirectory(
            prefix="nexus-security-"
        )
        self.directory = Path(self._temporary_directory.name)
        self.certificate = self.directory / "certificate.pem"
        self.private_key = self.directory / "private-key.pem"

        command = [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-sha256",
            "-nodes",
            "-days",
            "1",
            "-subj",
            f"/CN={common_name}",
            "-keyout",
            str(self.private_key),
            "-out",
            str(self.certificate),
        ]
        if include_localhost:
            command.extend(
                ["-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"]
            )
        subprocess.run(
            command,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        # The generated key is test-only, but restrictive permissions model how
        # a mounted production key should be exposed to the edge process.
        os.chmod(self.private_key, 0o600)

    def cleanup(self):
        self._temporary_directory.cleanup()


def edge_tls_environment(certificate):
    environment = os.environ.copy()
    environment["NEXUS_TLS_CERT_FILE"] = str(certificate.certificate)
    environment["NEXUS_TLS_KEY_FILE"] = str(certificate.private_key)
    return environment


def client_tls_environment(certificate, expected_name="localhost"):
    environment = os.environ.copy()
    environment["NEXUS_TLS_CA_FILE"] = str(certificate.certificate)
    environment["NEXUS_TLS_SERVER_NAME"] = expected_name
    return environment


def tls_client_connection(host, port, timeout=3):
    context = ssl.create_default_context(cafile=os.environ["NEXUS_TLS_CA_FILE"])
    raw_socket = socket.create_connection((host, port), timeout=timeout)
    try:
        return context.wrap_socket(
            raw_socket,
            server_hostname=os.environ.get("NEXUS_TLS_SERVER_NAME", host),
        )
    except Exception:
        raw_socket.close()
        raise


def tls_client_when_ready(host, port, timeout=3):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            return tls_client_connection(host, port, timeout=0.5)
        except OSError as error:
            last_error = error
            time.sleep(0.05)
    raise RuntimeError(f"TLS listener on port {port} did not start: {last_error}")


def tls_server_connection(raw_socket):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.load_cert_chain(
        certfile=os.environ["NEXUS_TLS_CERT_FILE"],
        keyfile=os.environ["NEXUS_TLS_KEY_FILE"],
    )
    return context.wrap_socket(raw_socket, server_side=True)
