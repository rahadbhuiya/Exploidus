#!/bin/sh
# Builds the host-side sshd test binaries from the CURRENT repo sources.
# Output goes to tests/sshd-host/.build/ (safe to delete; add to .gitignore).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
B="$HERE/.build"
HK="${HK:-/tmp/exo_test_hostkey}"

rm -rf "$B"; mkdir -p "$B/userspace/bin" "$B/userspace/libc"
ln -s "$ROOT/kernel" "$B/kernel"
cp "$HERE/host_shim_syscall.h" "$B/userspace/libc/syscall.h"   # shim replaces the real syscall.h
for f in sshd.c sshd_packet.c sshd_packet.h sshd_kex.c sshd_kex.h; do
    cp "$ROOT/userspace/bin/$f" "$B/userspace/bin/$f"
done
cp "$HERE/test_kdf.c" "$B/userspace/bin/"

# TEST-ONLY: paramiko insists on a MAC match even for AEAD ciphers, so the test copy advertises a
# (never-used) MAC name. Only the advertised KEXINIT list changes; the KEX code under test is untouched.
sed -i 's/#define MAC_ALGORITHMS      ""/#define MAC_ALGORITHMS      "hmac-sha2-256"/' "$B/userspace/bin/sshd_packet.c"
grep -q 'hmac-sha2-256' "$B/userspace/bin/sshd_packet.c" || { echo "MAC sed failed (MAC_ALGORITHMS define changed?)"; exit 1; }

cd "$B"
C=kernel/crypto
# shift-base off: ed25519.c (TweetNaCl-derived) left-shifts negative values; gcc defines that, UBSan flags it.
FLAGS="-std=gnu11 -g -O1 -fno-builtin -U_FORTIFY_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-sanitize=shift-base -Wall -Wextra -Wno-unused-parameter"
CRYPTO="$C/x25519.c $C/sha256.c $C/sha512.c $C/ed25519.c"
gcc $FLAGS -DSSHD_HOSTKEY_PATH=\"$HK\" -o sshd_host userspace/bin/sshd.c userspace/bin/sshd_packet.c userspace/bin/sshd_kex.c $CRYPTO
gcc $FLAGS -o test_kdf userspace/bin/test_kdf.c userspace/bin/sshd_packet.c $CRYPTO
echo "built: $B/sshd_host $B/test_kdf"