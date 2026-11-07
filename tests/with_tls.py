#!/usr/bin/env python3

import os
import subprocess
import sys

from security_test_support import TestCertificate


def main():
    if len(sys.argv) < 2:
        print("usage: with_tls.py <test-script> [arguments...]", file=sys.stderr)
        return 2

    certificate = TestCertificate()
    try:
        environment = os.environ.copy()
        environment["NEXUS_TLS_CERT_FILE"] = str(certificate.certificate)
        environment["NEXUS_TLS_KEY_FILE"] = str(certificate.private_key)
        environment["NEXUS_TLS_CA_FILE"] = str(certificate.certificate)
        environment["NEXUS_TLS_SERVER_NAME"] = "localhost"
        result = subprocess.run(
            [sys.executable, *sys.argv[1:]],
            env=environment,
            check=False,
        )
        return result.returncode
    finally:
        certificate.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
