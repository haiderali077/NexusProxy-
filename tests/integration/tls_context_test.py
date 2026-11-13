#!/usr/bin/env python3

import subprocess
import sys

from security_test_support import TestCertificate


def main():
    if len(sys.argv) != 2:
        print("usage: tls_context_test.py <tls-context-test>", file=sys.stderr)
        return 2

    certificate = TestCertificate()
    try:
        result = subprocess.run(
            [
                sys.argv[1],
                str(certificate.certificate),
                str(certificate.private_key),
            ],
            check=False,
            text=True,
            capture_output=True,
        )
        if result.returncode != 0:
            print(result.stdout, end="", file=sys.stderr)
            print(result.stderr, end="", file=sys.stderr)
        return result.returncode
    finally:
        certificate.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
