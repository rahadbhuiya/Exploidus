#!/usr/bin/env python3
"""
gen_index.py -- builds index.json for the rahu package registry.

IMPORTANT: Exploidus's kernel/crypto/blake3.c is explicitly a
"single-chunk" implementation (see its own comment: "Supports inputs
up to one chunk (1024 bytes)"). For inputs over 1024 bytes it does NOT
implement real multi-chunk BLAKE3 (no chunk/tree structure) -- it just
keeps chaining 64-byte blocks. This was verified against the actual
kernel source: hashes match the standard `blake3` library for inputs
<=1024 bytes, and diverge completely above that.

Since virtually every real package is bigger than 1024 bytes (even a
"hello world" ELF is ~19KB), this script reimplements that exact
kernel behavior in Python rather than using the standard blake3
library, so the hashes it writes will actually match what
`rahu install` computes on the device. It will NOT match `b3sum` or
any standard BLAKE3 tool for inputs over 1024 bytes -- that's expected,
not a bug in this script.

Usage:
    python3 gen_index.py [packages_dir] [output_path]

Defaults: packages_dir="packages", output_path="<packages_dir>/../index.json"

Scans packages_dir for *.elf and *.fozu files and writes one line per
package:
    <name> <64-char-hex-hash> - <size> bytes

A .fozu file is a multi-file package archive (see tools/mkfozu.py); a
bare .elf is still supported as a single-binary package installed
straight to /bin/<name>, for backward compatibility.

Serve the parent directory with: python3 -m http.server 9090
(so /index.json and /packages/<name>.fozu both resolve correctly)
"""
import sys
import os

IV = [0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
      0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19]
MSG_PERMUTATION = [2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8]
MASK32 = 0xFFFFFFFF
BLOCK_LEN = 64
CHUNK_START = 1 << 0
CHUNK_END = 1 << 1
ROOT = 1 << 3


def _rotr32(x, n):
    x &= MASK32
    return ((x >> n) | (x << (32 - n))) & MASK32


def _g(state, a, b, c, d, mx, my):
    state[a] = (state[a] + state[b] + mx) & MASK32
    state[d] = _rotr32(state[d] ^ state[a], 16)
    state[c] = (state[c] + state[d]) & MASK32
    state[b] = _rotr32(state[b] ^ state[c], 12)
    state[a] = (state[a] + state[b] + my) & MASK32
    state[d] = _rotr32(state[d] ^ state[a], 8)
    state[c] = (state[c] + state[d]) & MASK32
    state[b] = _rotr32(state[b] ^ state[c], 7)


def _round(state, m):
    _g(state, 0, 4, 8, 12, m[0], m[1])
    _g(state, 1, 5, 9, 13, m[2], m[3])
    _g(state, 2, 6, 10, 14, m[4], m[5])
    _g(state, 3, 7, 11, 15, m[6], m[7])
    _g(state, 0, 5, 10, 15, m[8], m[9])
    _g(state, 1, 6, 11, 12, m[10], m[11])
    _g(state, 2, 7, 8, 13, m[12], m[13])
    _g(state, 3, 4, 9, 14, m[14], m[15])


def _permute(m):
    tmp = [m[MSG_PERMUTATION[i]] for i in range(16)]
    for i in range(16):
        m[i] = tmp[i]


def _compress(chaining_value, block_words, counter, block_len, flags):
    state = list(chaining_value) + list(IV[:4]) + [
        counter & MASK32, (counter >> 32) & MASK32, block_len, flags
    ]
    block = list(block_words)
    for _ in range(7):
        _round(state, block)
        _permute(block)
    out = [0] * 16
    for i in range(8):
        out[i] = (state[i] ^ state[i + 8]) & MASK32
        out[i + 8] = (state[i + 8] ^ chaining_value[i]) & MASK32
    return out


def kernel_blake3(data: bytes) -> bytes:
    """Bit-for-bit match of Exploidus's kernel/crypto/blake3.c
    blake3_hash(). Verified against the real C source for inputs of
    0, 1, 10, 63, 64, 65, 100, 1023, 1024, 1025, 2000, 4096 and 19344
    bytes -- all matched exactly."""
    chaining = list(IV)
    input_len = len(data)
    offset = 0
    first = True
    out16 = None

    while offset < input_len or first:
        first = False
        remaining = input_len - offset
        this_block = min(remaining, BLOCK_LEN)
        is_last = (offset + this_block >= input_len)

        padded = bytearray(BLOCK_LEN)
        chunk = data[offset:offset + this_block]
        padded[:len(chunk)] = chunk

        block_words = []
        for i in range(16):
            w = (padded[i * 4] | (padded[i * 4 + 1] << 8) |
                 (padded[i * 4 + 2] << 16) | (padded[i * 4 + 3] << 24))
            block_words.append(w)

        flags = 0
        if offset == 0:
            flags |= CHUNK_START
        if is_last:
            flags |= CHUNK_END | ROOT

        out16 = _compress(chaining, block_words, 0, this_block, flags)

        if is_last:
            break
        chaining = out16[:8]
        offset += this_block

    out = bytearray(32)
    for i in range(8):
        v = out16[i]
        out[i * 4 + 0] = v & 0xFF
        out[i * 4 + 1] = (v >> 8) & 0xFF
        out[i * 4 + 2] = (v >> 16) & 0xFF
        out[i * 4 + 3] = (v >> 24) & 0xFF
    return bytes(out)


def main():
    pkg_dir = sys.argv[1] if len(sys.argv) > 1 else "packages"
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.normpath(pkg_dir)) or ".", "index.json")

    if not os.path.isdir(pkg_dir):
        print(f"Not a directory: {pkg_dir}")
        print("Usage: python3 gen_index.py [packages_dir] [output_path]")
        sys.exit(1)

    lines = []
    for fname in sorted(os.listdir(pkg_dir)):
        if not (fname.endswith(".elf") or fname.endswith(".fozu")):
            continue
        path = os.path.join(pkg_dir, fname)
        with open(path, "rb") as f:
            data = f.read()
        digest = kernel_blake3(data).hex()
        if fname.endswith(".fozu"):
            name = fname[:-len(".fozu")]
        else:
            name = fname[:-len(".elf")]
        lines.append(f"{name} {digest} - {len(data)} bytes")
        flag = "  (over 1024B, kernel-quirk hash)" if len(data) > 1024 else ""
        print(f"  {name}: {len(data)} bytes -> {digest}{flag}")

    if not lines:
        print(f"No .elf files found in {pkg_dir}")
        sys.exit(1)

    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")

    print(f"\nWrote {len(lines)} entries to {out_path}")
    print("Serve the parent directory with: python3 -m http.server 9090")


if __name__ == "__main__":
    main()