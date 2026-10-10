import os, subprocess, socket, sys
from paramiko import Transport
from paramiko.message import Message
HERE = os.path.dirname(os.path.abspath(__file__))
cases = {
 "random":        os.urandom(32),
 "leading zeros": b"\x00\x00\x00" + os.urandom(29),
 "high bit set":  b"\xff" * 32,
 "top bit + lz":  b"\x00\x80" + os.urandom(30),
 "tiny":          b"\x00" * 31 + b"\x01",
 "0x7f.. (no pad)": b"\x7f" + os.urandom(31),
}
a, b = socket.socketpair()
ok_all = True
for name, k in cases.items():
    H = os.urandom(32)
    out = subprocess.run([os.path.join(HERE, ".build", "test_kdf"), k.hex(), H.hex()], capture_output=True, text=True, check=True).stdout.split()
    t = Transport(a)
    K = int.from_bytes(k, "big")
    t.kex_engine = Transport._kex_info['curve25519-sha256@libssh.org'](t)   # => hash_algo = SHA-256, not paramiko's SHA-1 fallback
    assert t.kex_engine.hash_algo.__name__ == 'openssl_sha256' or 'sha256' in repr(t.kex_engine.hash_algo)
    t._set_K_H(K, H); t.session_id = H
    m = Message(); m.add_mpint(K)
    exp = (m.asbytes().hex(), t._compute_key('C', 64).hex(), t._compute_key('D', 64).hex())
    ok = tuple(out) == exp
    ok_all &= ok
    print(("PASS " if ok else "FAIL ") + name + "  (mpint / key C->S / key S->C vs paramiko)")
    if not ok: print(" C   :", out); print(" py  :", exp)
sys.exit(0 if ok_all else 1)