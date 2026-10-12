"""Encrypted transport tests: chacha20-poly1305@openssh.com + kex-strict.

 1. asyncssh (independent third-party SSH client) runs the full KEX, encrypts,
    sends SERVICE_REQUEST + USERAUTH_REQUEST and must receive our DISCONNECT 14.
 2. A raw client written here from PROTOCOL.chacha20poly1305 (cryptography's ChaCha20
    + Poly1305 as the reference) in strict and non-strict mode: exact sequence-number
    handling, tamper detection, strict-kex violations.
"""
import os, sys, time, socket, struct, subprocess, hashlib, asyncio
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
from cryptography.hazmat.primitives import poly1305, serialization

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("SSHD_BIN", os.path.join(HERE, ".build", "sshd_host"))
PORT = 2298
LOG = "/tmp/sshd_session.log"
results = []

def check(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + (("  -- " + detail) if detail else ""))

class Server:
    def __init__(self):
        if os.path.exists(LOG): os.remove(LOG)
        # SSHD_HOSTKEY_PATH is baked in at build time; this server shares the build's key file, which is fine.
        env = dict(os.environ, SSHD_TEST_PORT=str(PORT), ASAN_OPTIONS="detect_leaks=0")
        self.log = open(LOG, "ab+")
        self.p = subprocess.Popen([BIN], stdout=self.log, stderr=subprocess.STDOUT, env=env)
        for _ in range(50):
            try: socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close(); break
            except OSError: time.sleep(0.1)
        time.sleep(0.2)
    def text(self): self.log.flush(); return open(LOG, "rb").read().decode(errors="replace")
    def stop(self): self.p.kill(); self.p.wait()
    def alive(self): return self.p.poll() is None




#  wire helpers (independent of the C code) 
def u32(n): return struct.pack(">I", n)
def sstr(b): return u32(len(b)) + b
def mpint(raw):
    n = int.from_bytes(raw, "big")
    if n == 0: return u32(0)
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return sstr((b"\x00" + b) if b[0] & 0x80 else b)
def frame(payload):              # cleartext framing: 4+packet_length is a multiple of 8
    pad = 8 - ((5 + len(payload)) % 8)
    if pad < 4: pad += 8
    return u32(1 + len(payload) + pad) + bytes([pad]) + payload + os.urandom(pad)
def read_exact(s, n):
    buf = b""
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c: raise EOFError("closed")
        buf += c
    return buf
def read_packet(s):
    ln = struct.unpack(">I", read_exact(s, 4))[0]
    body = read_exact(s, ln)
    return body[1:ln - body[0]]
def read_line(s):
    out = b""
    while not out.endswith(b"\n"): out += read_exact(s, 1)
    return out.rstrip(b"\r\n")
def client_kexinit(strict):
    nl = lambda x: sstr(x.encode())
    kex = "curve25519-sha256" + (",kex-strict-c-v00@openssh.com" if strict else "")
    return (b"\x14" + os.urandom(16) + nl(kex) + nl("ssh-ed25519") + nl("chacha20-poly1305@openssh.com") * 2 +
            nl("") * 2 + nl("none") * 2 + nl("") * 2 + b"\x00" + u32(0))


#  OpenSSH chacha20-poly1305 reference (PROTOCOL.chacha20poly1305) 
def _chacha(key, counter, seq, data):
    nonce16 = struct.pack("<I", counter) + b"\x00" * 4 + struct.pack(">Q", seq)   # djb layout, high ctr word 0
    return Cipher(algorithms.ChaCha20(key, nonce16), mode=None).encryptor().update(data)


def enc_packet(key64, seq, payload):
    k2, k1 = key64[:32], key64[32:]
    pad = 8 - ((1 + len(payload)) % 8)
    if pad < 4: pad += 8
    body = bytes([pad]) + payload + os.urandom(pad)
    polykey = _chacha(k2, 0, seq, b"\x00" * 32)
    enc_len = _chacha(k1, 0, seq, struct.pack(">I", len(body)))
    enc_body = _chacha(k2, 1, seq, body)
    tag = poly1305.Poly1305.generate_tag(polykey, enc_len + enc_body)
    return enc_len + enc_body + tag
def dec_packet(s, key64, seq):
    k2, k1 = key64[:32], key64[32:]
    enc_len = read_exact(s, 4)
    ln = struct.unpack(">I", _chacha(k1, 0, seq, enc_len))[0]
    assert ln % 8 == 0 and 8 <= ln <= 35000, "bad decrypted length %d" % ln
    rest = read_exact(s, ln + 16)
    polykey = _chacha(k2, 0, seq, b"\x00" * 32)
    poly1305.Poly1305.verify_tag(polykey, enc_len + rest[:ln], rest[ln:])      # raises InvalidSignature
    body = _chacha(k2, 1, seq, rest[:ln])
    return body[1:ln - body[0]]


def kdf(k_raw, H, letter, n=64):
    kmp = mpint(k_raw)
    out = hashlib.sha256(kmp + H + letter + H).digest()          # session_id == H on the first KEX
    while len(out) < n: out += hashlib.sha256(kmp + H + out).digest()
    return out[:n]


def handshake(strict, ignore_before_init=False):
    """Runs ID + KEX + NEWKEYS. Returns ctx with the socket, both keys and the seq numbers to use next."""
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    v_s = read_line(s); v_c = b"SSH-2.0-rawtest_1.0"
    s.sendall(v_c + b"\r\n")
    ki_c = client_kexinit(strict)
    s.sendall(frame(ki_c)); seq_s = 1
    ki_s = read_packet(s); seq_r = 1
    server_marker = b"kex-strict-s-v00@openssh.com" in ki_s
    if ignore_before_init:
        s.sendall(frame(b"\x02" + sstr(b"hello"))); seq_s += 1
    priv = X25519PrivateKey.generate()
    q_c = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    s.sendall(frame(b"\x1e" + sstr(q_c))); seq_s += 1
    reply = read_packet(s); seq_r += 1
    assert reply[0] == 31
    p = 1
    def get():
        nonlocal p
        n = struct.unpack(">I", reply[p:p+4])[0]; v = reply[p+4:p+4+n]; p += 4 + n; return v
    ks, q_s, sigblob = get(), get(), get()
    K = priv.exchange(X25519PublicKey.from_public_bytes(q_s))
    H = hashlib.sha256(sstr(v_c) + sstr(v_s) + sstr(ki_c) + sstr(ki_s) + sstr(ks) + sstr(q_c) + sstr(q_s) + mpint(K)).digest()
    pub = ks[4 + 11 + 4:]; sig = sigblob[4 + 11 + 4:]
    Ed25519PublicKey.from_public_bytes(pub).verify(sig, H)
    assert read_packet(s) == b"\x15"; seq_r += 1
    s.sendall(frame(b"\x15")); seq_s += 1
    if strict: seq_s = seq_r = 0
    return dict(s=s, k_c2s=kdf(K, H, b"C"), k_s2c=kdf(K, H, b"D"), seq_s=seq_s, seq_r=seq_r, server_marker=server_marker)

SVC_REQ = b"\x05" + sstr(b"ssh-userauth")
SVC_ACC = b"\x06" + sstr(b"ssh-userauth")
AUTH_NONE = b"\x32" + sstr(b"tester") + sstr(b"ssh-connection") + sstr(b"none")

def full_session(strict):
    c = handshake(strict)
    s = c["s"]
    s.sendall(enc_packet(c["k_c2s"], c["seq_s"], SVC_REQ)); c["seq_s"] += 1
    acc = dec_packet(s, c["k_s2c"], c["seq_r"]); c["seq_r"] += 1
    s.sendall(enc_packet(c["k_c2s"], c["seq_s"], AUTH_NONE)); c["seq_s"] += 1
    disc = dec_packet(s, c["k_s2c"], c["seq_r"])
    s.close()
    return c, acc, disc

def dropped(fn):
    """True if the server closes without sending a decryptable reply."""
    try: fn(); return False
    except (EOFError, ConnectionError, socket.timeout, AssertionError, Exception) as e:
        return not isinstance(e, KeyboardInterrupt)

srv = Server()
try:
    # ===== 1. asyncssh (third party) =====
    try:
        import asyncssh
        async def go():
            try:
                await asyncssh.connect("127.0.0.1", PORT, known_hosts=None, username="tester",
                                       client_keys=None, agent_path=None, password=None,
                                       kex_algs=["curve25519-sha256"], encryption_algs=["chacha20-poly1305@openssh.com"],
                                       server_host_key_algs=["ssh-ed25519"], login_timeout=15)
            except Exception as e:
                return e
            return None
        err = asyncio.run(go())
        time.sleep(0.4); txt = srv.text()
        code = getattr(err, "code", None); reason = str(getattr(err, "reason", err))
        check("S1 asyncssh: full KEX + encrypted transport + service request, ends with our DISCONNECT 14",
              code == 14 and "not implemented" in reason, "err=%r" % (err,))
        check("S2 server logged the decrypted service request and userauth request",
              "service request: ssh-userauth" in txt and "userauth request: user=tester method=none" in txt)
        check("S3 asyncssh negotiated strict kex (server saw the marker)", "kex-strict: ON" in txt)
    except ImportError:
        check("S1 asyncssh available (pip install asyncssh)", False)

    # ===== 2. raw client, strict =====
    c, acc, disc = full_session(strict=True)
    check("R1 strict: server advertised kex-strict-s", c["server_marker"])
    check("R2 strict: SERVICE_ACCEPT decrypts/authenticates with the reference chacha (seq reset to 0 both ways)", acc == SVC_ACC)
    check("R3 strict: DISCONNECT(14) 'not implemented' arrives encrypted",
          disc[0] == 1 and struct.unpack(">I", disc[1:5])[0] == 14 and b"not implemented" in disc)

    # ===== 3. raw client, NON-strict: sequence numbers continue across NEWKEYS =====
    c, acc, disc = full_session(strict=False)
    check("R4 non-strict: seq numbers continue across NEWKEYS (server sent 3 packets before, we used seq 3)",
          acc == SVC_ACC and c["seq_s"] == 5)

    # ===== 4. tamper / protocol-violation cases =====
    def tamper_tag():
        c = handshake(True); w = bytearray(enc_packet(c["k_c2s"], c["seq_s"], SVC_REQ)); w[-1] ^= 1
        c["s"].sendall(bytes(w)); dec_packet(c["s"], c["k_s2c"], c["seq_r"])
    check("T1 flipped Poly1305 tag bit: connection dropped, no reply", dropped(tamper_tag))
    def tamper_body():
        c = handshake(True); w = bytearray(enc_packet(c["k_c2s"], c["seq_s"], SVC_REQ)); w[6] ^= 0x80
        c["s"].sendall(bytes(w)); dec_packet(c["s"], c["k_s2c"], c["seq_r"])
    check("T2 flipped ciphertext bit: dropped (tag covers the body)", dropped(tamper_body))
    def tamper_len():
        c = handshake(True); w = bytearray(enc_packet(c["k_c2s"], c["seq_s"], SVC_REQ)); w[0] ^= 1
        c["s"].sendall(bytes(w)); dec_packet(c["s"], c["k_s2c"], c["seq_r"])
    check("T3 flipped encrypted-length bit: dropped", dropped(tamper_len))
    def wrong_seq():
        c = handshake(True); c["s"].sendall(enc_packet(c["k_c2s"], c["seq_s"] + 3, SVC_REQ))   # forgot the strict reset
        dec_packet(c["s"], c["k_s2c"], c["seq_r"])
    check("T4 strict mode: client that does NOT reset its seq at NEWKEYS is rejected", dropped(wrong_seq))
    def ignore_strict():
        c = handshake(True, ignore_before_init=True)
        c["s"].sendall(enc_packet(c["k_c2s"], c["seq_s"], SVC_REQ)); dec_packet(c["s"], c["k_s2c"], c["seq_r"])
    check("T5 strict mode: SSH_MSG_IGNORE injected before KEX_ECDH_INIT is rejected", dropped(ignore_strict))
    c = handshake(False, ignore_before_init=True)
    c["s"].sendall(enc_packet(c["k_c2s"], c["seq_s"], SVC_REQ))
    check("T6 non-strict: the same IGNORE is tolerated (RFC 4253 behaviour)",
          dec_packet(c["s"], c["k_s2c"], c["seq_r"]) == SVC_ACC); c["s"].close()
    def wrong_service():
        c = handshake(True); c["s"].sendall(enc_packet(c["k_c2s"], c["seq_s"], b"\x05" + sstr(b"ssh-connection")))
        d = dec_packet(c["s"], c["k_s2c"], c["seq_r"]); assert d[0] == 1 and struct.unpack(">I", d[1:5])[0] == 7
    check("T7 unknown service gets DISCONNECT 7 (service not available), still encrypted", not dropped(wrong_service))
    txt = srv.text()
    # T1 (tag) and T2 (body) fail the tag check. T3 (flipped length bit) and T4 (wrong seq => wrong
    # nonce => garbage length) are normally rejected at the length check, before any tag is looked at,
    # so they leave no "authentication FAILED" line; they are covered by the dropped() checks above.
    check("T1/T2 server logged the Poly1305 failure (not just any error)", txt.count("packet authentication FAILED") >= 2)
    check("T5 server logged the strict-kex violation", "strict-kex violation" in txt)
    check("T8 sshd survived everything (no crash / sanitizer abort)", srv.alive())
    c, acc, disc = full_session(strict=True)
    check("T9 sshd still serves a valid encrypted session afterwards", acc == SVC_ACC)
finally:
    srv.stop()

bad = [n for n, ok in results if not ok]
print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
sys.exit(1 if bad else 0)