#!/usr/bin/env bash
# Host-side crypto unit tests: reference vectors from OpenSSL/hashlib vs MiniWin's
# actual kernel headers. Needs: gcc, python3, `pip install cryptography`.
set -euo pipefail
cd "$(dirname "$0")"
python3 gen_crypto_vectors.py /tmp/crypto_vectors.txt
gcc -O1 -Wall -Wextra -Wno-unused-function -DHAVE_ECC_TESTS -o /tmp/host_crypto_test host_crypto_test.c
/tmp/host_crypto_test /tmp/crypto_vectors.txt
