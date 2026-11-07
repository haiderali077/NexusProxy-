import os
from pathlib import Path
import subprocess
import tempfile


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
