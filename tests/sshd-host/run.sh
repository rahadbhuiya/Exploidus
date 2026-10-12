#!/bin/sh
# Build, then run both host tests. Needs: gcc, python3, `pip install paramiko asyncssh cryptography`.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
"$HERE/build.sh"
cd "$HERE"
python3 test_kdf.py
python3 test_kex.py
python3 test_session.py