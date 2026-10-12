import os, sys, time, socket, struct, subprocess, hashlib, base64, logging, re, threading
import paramiko
from paramiko import Transport
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
from cryptography.hazmat.primitives import serialization

PORT = 2299
HK = "/tmp/exo_test_hostkey"
HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, ".build", "sshd_host")
results = []
if os.path.exists("/tmp/sshd_host.log"): os.remove("/tmp/sshd_host.log")   # the E1 check parses this log; start clean every run

def check(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + (("  -- " + detail) if detail else ""))

class Server:
    def __init__(self, fresh=True):
        if fresh and os.path.exists(HK): os.remove(HK)
        env = dict(os.environ, SSHD_TEST_PORT=str(PORT),
                   ASAN_OPTIONS="detect_leaks=0")
        self.log = open("/tmp/sshd_host.log", "ab+")
        self.p = subprocess.Popen([BIN], stdout=self.log, stderr=subprocess.STDOUT, env=env)
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close(); break
            except OSError: time.sleep(0.1)
        time.sleep(0.2)
    def text(self):
        self.log.flush(); return open("/tmp/sshd_host.log", "rb").read().decode(errors="replace")
    def stop(self):
        self.p.kill(); self.p.wait()
    def alive(self): return self.p.poll() is None

def fp_of(blob): return "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("=")

# ---------- raw framing helpers (independent of the C code) ----------
def u32(n): return struct.pack(">I", n)
def sstr(b): return u32(len(b)) + b
def mpint(raw):  # raw = 32 bytes, big-endian integer, as OpenSSH/paramiko treat the X25519 output
    n = int.from_bytes(raw, "big")
    if n == 0: return u32(0)
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    if b[0] & 0x80: b = b"\x00" + b
    return sstr(b)
def frame(payload):
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
def client_kexinit():
    nl = lambda x: sstr(x.encode())
    return (b"\x14" + os.urandom(16) + nl("curve25519-sha256") + nl("ssh-ed25519") +
            nl("chacha20-poly1305@openssh.com") * 2 + nl("") * 2 + nl("none") * 2 + nl("") * 2 + b"\x00" + u32(0))

def raw_session(coalesce, init_q=None, init_override=None):
    """Returns (reply_payload or None, ctx). Runs ID + KEXINIT + KEX_ECDH_INIT.
    The client sends everything WITHOUT waiting for the server's KEXINIT (as OpenSSH does)."""
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    v_s = read_line(s)
    v_c = b"SSH-2.0-rawtest_1.0"
    ki_c = client_kexinit()
    priv = X25519PrivateKey.generate()
    q_c = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw) if init_q is None else init_q
    init = init_override if init_override is not None else (b"\x1e" + sstr(q_c))
    wire = [v_c + b"\r\n", frame(ki_c), frame(init)]
    if coalesce: s.sendall(b"".join(wire))          # ID + KEXINIT + KEX_ECDH_INIT in ONE segment
    else:
        for w in wire: s.sendall(w); time.sleep(0.05)
    try:
        ki_s = read_packet(s)
        reply = read_packet(s)
    except (EOFError, ConnectionError, socket.timeout):
        return None, None
    return reply, dict(s=s, v_c=v_c, v_s=v_s, ki_c=ki_c, ki_s=ki_s, priv=priv, q_c=q_c)

def verify_reply(reply, c):
    assert reply[0] == 31, "not KEX_ECDH_REPLY"
    p = 1
    def get():
        nonlocal p
        n = struct.unpack(">I", reply[p:p+4])[0]; v = reply[p+4:p+4+n]; p += 4 + n; return v
    ks, q_s, sigblob = get(), get(), get()
    assert p == len(reply), "trailing bytes in reply"
    alg_len = struct.unpack(">I", ks[:4])[0]; assert ks[4:4+alg_len] == b"ssh-ed25519"
    pub = ks[4+alg_len+4:]; assert len(pub) == 32
    sb = sigblob; n = struct.unpack(">I", sb[:4])[0]; assert sb[4:4+n] == b"ssh-ed25519"
    sig = sb[4+n+4:]; assert len(sig) == 64
    K = c["priv"].exchange(X25519PublicKey.from_public_bytes(q_s))
    H = hashlib.sha256(sstr(c["v_c"]) + sstr(c["v_s"]) + sstr(c["ki_c"]) + sstr(c["ki_s"]) +
                       sstr(ks) + sstr(c["q_c"]) + sstr(q_s) + mpint(K)).digest()
    Ed25519PublicKey.from_public_bytes(pub).verify(sig, H)   # raises on bad signature
    return ks, H

srv = Server()
try:
    # ===== A: paramiko (third-party client) =====
    # paramiko 5 only knows the "@libssh.org" alias of this KEX and has no chacha20-poly1305@openssh.com.
    # Alias both names to existing implementations so negotiation reaches the key exchange; the
    # KEX itself (H, mpint K, signature check) is paramiko's own code. Cipher use after NEWKEYS is moot.
    Transport._kex_info['curve25519-sha256'] = Transport._kex_info['curve25519-sha256@libssh.org']
    Transport._cipher_info['chacha20-poly1305@openssh.com'] = Transport._cipher_info['aes128-gcm@openssh.com']
    logbuf = []
    class H_(logging.Handler):
        def emit(self, r): logbuf.append(r.getMessage())
    lg = logging.getLogger("paramiko.transport"); lg.setLevel(logging.DEBUG); lg.addHandler(H_())
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    t = Transport(sock)
    t._preferred_kex = ('curve25519-sha256',)
    t._preferred_keys = ('ssh-ed25519',)
    t._preferred_ciphers = ('chacha20-poly1305@openssh.com',)
    t._preferred_macs = ('hmac-sha2-256',)
    err = None
    try: t.start_client(timeout=10)
    except Exception as e: err = e
    time.sleep(0.5)
    joined = "\n".join(logbuf)
    sig_failed = "Signature verification" in joined and "failed" in joined.lower()
    switched = "Switch to new keys" in joined
    check("A1 paramiko completed KEX incl. host-key signature verification (reached 'Switch to new keys')",
          switched and not sig_failed, "" if switched else ("error=%r | log tail: %s" % (err, joined[-400:])))
    hk = getattr(t, "host_key", None) if switched else None
    if hk:
        paramiko_fp = "SHA256:" + base64.b64encode(hashlib.sha256(hk.asbytes()).digest()).decode().rstrip("=")
        m = re.search(r"host key fingerprint: (SHA256:\S+)", srv.text())
        check("A2 host key fingerprint seen by paramiko == fingerprint sshd printed at startup",
              m and m.group(1) == paramiko_fp, "sshd=%s paramiko=%s" % (m and m.group(1), paramiko_fp))
    check("A3 sshd logged key exchange complete", "key exchange complete" in srv.text())
    try: t.close()
    except Exception: pass
    time.sleep(0.3)

    # ===== B: independent raw client, segments sent separately =====
    reply, c = raw_session(coalesce=False)
    try: ks, H = verify_reply(reply, c); ok, why = True, ""
    except Exception as e: ok, why = False, repr(e)
    check("B1 raw client: REPLY parses, signature verifies over independently computed H (separate segments)", ok, why)
    if ok:
        c["s"].sendall(frame(b"\x15"))   # client's NEWKEYS
        nk = read_packet(c["s"])
        check("B2 server sent SSH_MSG_NEWKEYS after the REPLY", nk == b"\x15")
        time.sleep(0.3)
        check("B3 sshd logged 'key exchange complete' after our NEWKEYS", srv.text().count("key exchange complete") >= 2)
        c["s"].close()

    # ===== C: ID + KEXINIT + KEX_ECDH_INIT coalesced into ONE TCP segment =====
    reply, c = raw_session(coalesce=True)
    try: verify_reply(reply, c); ok, why = True, ""
    except Exception as e: ok, why = False, repr(e)
    check("C1 coalesced ID+KEXINIT+KEX_ECDH_INIT in a single send still works (ID line read must not over-read)", ok, why)
    if c: c["s"].close()

    # ===== D: negative cases =====
    reply, c = raw_session(coalesce=False, init_q=b"\x00" * 32)
    time.sleep(0.3)
    check("D1 all-zero (low-order) client public key: no REPLY sent, connection dropped", reply is None)
    check("D1b sshd logged the low-order rejection", "low-order" in srv.text())
    reply, c = raw_session(coalesce=False, init_override=b"\x1e" + sstr(b"\x01" * 31))
    time.sleep(0.3)
    check("D2 malformed KEX_ECDH_INIT (31-byte Q_C): no REPLY, connection dropped", reply is None)
    reply, c = raw_session(coalesce=False, init_override=b"\x1f" + sstr(b"\x01" * 32))
    time.sleep(0.3)
    check("D3 wrong message type where KEX_ECDH_INIT expected: no REPLY, connection dropped", reply is None)
    check("D4 sshd survived all negative cases (no crash / sanitizer abort)", srv.alive())
    # server should still serve a good client afterwards
    reply, c = raw_session(coalesce=False)
    try: verify_reply(reply, c); ok = True
    except Exception: ok = False
    check("D5 sshd still serves a valid handshake after the bad ones", ok)
    if c: c["s"].close()

    first_fp = re.search(r"host key fingerprint: (SHA256:\S+)", srv.text()).group(1)
finally:
    srv.stop()

# ===== E: host key persistence across restart =====
srv2 = Server(fresh=False)
try:
    txt = srv2.text()
    fps = re.findall(r"host key fingerprint: (SHA256:\S+)", txt)
    check("E1 restart loads the persisted host key (same fingerprint)",
          len(fps) >= 2 and fps[-1] == first_fp and "loaded ed25519 host key" in txt,
          "fps=%s" % fps)
    check("E2 host key file is exactly 32 bytes, mode 0600",
          os.path.getsize(HK) == 32 and (os.stat(HK).st_mode & 0o777) == 0o600)
finally:
    srv2.stop()

bad = [n for n, ok in results if not ok]
print("\n%d/%d passed" % (len(results) - len(bad), len(results)))
sys.exit(1 if bad else 0)