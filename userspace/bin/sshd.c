/*
 * sshd.c — Exploidus SSH daemon (identification exchange, KEXINIT
 * negotiation, and curve25519-sha256 key exchange; no encrypted
 * transport yet)
 *
 * Listens on port 22, accepts TCP connections, then runs the
 * plaintext part of an SSH 2.0 handshake:
 *
 *   1. identification strings (RFC 4253 §4.2)
 *   2. SSH_MSG_KEXINIT algorithm negotiation (§7.1) against the one
 *      algorithm this daemon supports per category
 *      (curve25519-sha256 / ssh-ed25519 / chacha20-poly1305@openssh.com
 *      — see sshd_packet.c)
 *   3. SSH_MSG_KEX_ECDH_INIT / KEX_ECDH_REPLY / NEWKEYS (RFC 8731):
 *      ephemeral X25519, exchange hash, ed25519 host-key signature,
 *      and RFC 4253 §7.2 derivation of the two cipher keys
 *      (see sshd_kex.c)
 *
 * What this deliberately does NOT do yet: switch to the encrypted
 * packet layer after NEWKEYS. The keys are derived and held in
 * ssh_keys_t, but the OpenSSH chacha20-poly1305@openssh.com packet
 * format (two keys, sequence-number nonce, encrypted length field —
 * NOT the RFC 8439 AEAD in kernel/crypto/chacha20poly1305.c) isn't
 * implemented, so after the handshake completes sshd closes honestly
 * instead of speaking cleartext a real client would never send.
 *
 * Host key: a 32-byte ed25519 seed persisted at SSHD_HOSTKEY_PATH
 * (generated from getrandom() on first start). If the file can't be
 * created the key is ephemeral for this run, and sshd says so — a
 * client will then see a different host key after every restart.
 */

#include "../libc/syscall.h"
#include "sshd_packet.h"
#include "sshd_kex.h"
#include "../../kernel/crypto/sha256.h"
#include "../../kernel/crypto/ed25519.h"

#define SSHD_PORT   22
#define SSH_ID_STR  "SSH-2.0-Exploidus_0.1"
#define SSH_ID_LINE SSH_ID_STR "\r\n"
#ifndef SSHD_HOSTKEY_PATH
#define SSHD_HOSTKEY_PATH "/ssh_host_ed25519_key"
#endif
/* RFC 4253 §4.2: an identification string is at most 255 bytes
 * including the trailing CR LF. Give ourselves a little slack for a
 * client that sends extra "extension" lines before its real one --
 * we still only look at the last CRLF-terminated line we captured. */
#define ID_BUF_SIZE 512

static void print(const char *s)   { puts(s); }
static void println(const char *s) { puts(s); putc('\n'); }

static void print_int(int64_t v)
{
    if (v < 0) { putc('-'); v = -v; }
    if (v == 0) { putc('0'); return; }
    char tmp[21]; int i = 0;
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) putc(tmp[i]);
}

static int str_starts(const char *s, const char *prefix)
{
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

/* Extracts the protoversion field ("2.0", "1.99", ...) out of a
 * "SSH-protoversion-softwareversion ..." line into out (bounded,
 * NUL-terminated). Returns 0 on success, -1 if the line is malformed
 * (no second '-' found within the scanned prefix). */
static int parse_protoversion(const char *line, char *out, int outsz)
{
    const char *p = line + 4; /* skip "SSH-" */
    int i = 0;
    while (*p && *p != '-' && *p != '\r' && *p != '\n' && i < outsz - 1)
        out[i++] = *p++;
    out[i] = 0;
    if (*p != '-') return -1;
    return 0;
}

/* ---- host key: load or generate+persist, print fingerprint ---- */

static ssh_hostkey_t g_hostkey;

static void wipe_bytes(void *p, uint32_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* Unpadded base64, the form `ssh` prints fingerprints in. out needs
 * 4*ceil(n/3)+1 bytes. */
static void b64_nopad(char *out, const uint8_t *in, uint32_t n)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = T[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = T[v & 63];
    }
    out[o] = 0;
}

static void hostkey_init(void)
{
    uint8_t seed[ED25519_SEED_LEN];
    int have = 0;

    int fd = open(SSHD_HOSTKEY_PATH, O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, seed, sizeof(seed));
        close(fd);
        have = (r == (ssize_t)sizeof(seed));
        if (!have)
            println("[SSHD] host key file is unreadable or truncated, generating a new one");
    }

    if (!have) {
        if (getrandom(seed, sizeof(seed)) < 0) {
            println("[SSHD] Error: no randomness for host key generation");
            exit(1);
        }
        fd = open(SSHD_HOSTKEY_PATH, O_WRONLY | O_CREAT | O_TRUNC);
        ssize_t w = -1;
        if (fd >= 0) {
            w = write(fd, seed, sizeof(seed));
            close(fd);
        }
        if (w == (ssize_t)sizeof(seed)) {
            chmod(SSHD_HOSTKEY_PATH, 0600);
            println("[SSHD] generated a new ed25519 host key and saved it to " SSHD_HOSTKEY_PATH);
        } else {
            println("[SSHD] WARNING: could not persist the host key; it is EPHEMERAL "
                    "(clients will see a new host key after every restart)");
        }
    } else {
        println("[SSHD] loaded ed25519 host key from " SSHD_HOSTKEY_PATH);
    }

    ed25519_keypair_from_seed(g_hostkey.public_key, g_hostkey.secret_key, seed);
    wipe_bytes(seed, sizeof(seed));

    uint8_t blob[64], digest[SHA256_DIGEST_LEN];
    char fp[48];
    uint32_t blob_len = ssh_hostkey_blob(blob, g_hostkey.public_key);
    sha256(blob, blob_len, digest);
    b64_nopad(fp, digest, sizeof(digest));
    print("[SSHD] host key fingerprint: SHA256:");
    println(fp);
}

/* ---- identification line: read exactly one line, nothing past it ---- */

/* Reads one LF-terminated line a byte at a time. Reading in bulk
 * would swallow the start of the client's KEXINIT when it arrives
 * in the same TCP segment as the identification string, and
 * ssh_packet_recv() would then see a stream that starts mid-packet.
 * Strips CR LF, NUL-terminates, returns the length, or -1 on
 * close / error / a line over RFC 4253 §4.2's 255-byte limit. */
static int read_id_line(cap_token_t cap, int conn, char *buf, int bufsz)
{
    int n = 0;
    for (int total = 0; total < 255; total++) {
        char c;
        int r = xrecv(cap, conn, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') {
            if (n > 0 && buf[n - 1] == '\r') n--;
            buf[n] = 0;
            return n;
        }
        if (n < bufsz - 1) buf[n++] = c;
    }
    return -1;
}

static void handle_connection(cap_token_t cap, int conn)
{
    /* Send our identification line first -- most real SSH servers
     * do this immediately on accept rather than waiting for the
     * client, and RFC 4253 permits either order. */
    println("[SSHD] sending our identification line...");
    int sent = xsend(cap, conn, SSH_ID_LINE, (uint16_t)strlen(SSH_ID_LINE));
    print("[SSHD] xsend() returned ");
    print_int(sent);
    println("");
    if (sent <= 0) {
        println("[SSHD] send failed, closing");
        return;
    }

    static char client_id[ID_BUF_SIZE];
    println("[SSHD] waiting for client identification line...");
    int id_len = read_id_line(cap, conn, client_id, ID_BUF_SIZE);
    if (id_len < 0) {
        println("[SSHD] connection closed (or bad line) before a valid identification string");
        return;
    }

    if (!str_starts(client_id, "SSH-")) {
        println("[SSHD] rejecting: client's first line is not an SSH identification string");
        print("[SSHD]   got: ");
        println(client_id);
        return;
    }

    char proto[16];
    if (parse_protoversion(client_id, proto, sizeof(proto)) < 0) {
        println("[SSHD] rejecting: malformed identification string (no protoversion)");
        return;
    }

    int proto_ok = (strcmp_u(proto, "2.0") == 0) || (strcmp_u(proto, "1.99") == 0);
    print("[SSHD] client identification: ");
    println(client_id);
    print("[SSHD] protocol version: ");
    print(proto);
    println(proto_ok ? " (supported)" : " (unsupported)");

    if (!proto_ok) {
        println("[SSHD] closing: only SSH protocol 2.0 (or 1.99 compat) is recognized");
        return;
    }

    /* ---- KEXINIT exchange (RFC 4253 §7.1) ---- */
    uint8_t cookie[16];
    getrandom(cookie, 16);

    static uint8_t kexinit_out[SSH_PACKET_MAX];
    uint32_t kexinit_len = ssh_kexinit_build(kexinit_out, cookie);

    println("[SSHD] sending our SSH_MSG_KEXINIT...");
    if (!ssh_packet_send(cap, conn, kexinit_out, kexinit_len)) {
        println("[SSHD] failed to send KEXINIT, closing");
        return;
    }

    println("[SSHD] waiting for peer's SSH_MSG_KEXINIT...");
    static uint8_t kexinit_in[SSH_PACKET_MAX];
    int32_t kin_len = ssh_packet_recv(cap, conn, kexinit_in);
    if (kin_len < 0) {
        println("[SSHD] failed to receive a valid KEXINIT packet, closing");
        return;
    }

    if (!ssh_kexinit_check(kexinit_in, (uint32_t)kin_len)) {
        println("[SSHD] negotiation FAILED -- peer does not offer all of "
                "curve25519-sha256 / ssh-ed25519 / chacha20-poly1305@openssh.com");
        return;
    }
    println("[SSHD] negotiation OK -- peer supports curve25519-sha256 / "
            "ssh-ed25519 / chacha20-poly1305@openssh.com");

    /* A peer that guessed a different algorithm and sent its KEX
     * packet speculatively: that packet must be dropped, and the
     * real one follows (RFC 4253 §7.1). */
    if (ssh_kexinit_wrong_guess(kexinit_in, (uint32_t)kin_len)) {
        println("[SSHD] peer guessed a different kex/host-key algorithm; discarding its first KEX packet");
        static uint8_t discard[SSH_PACKET_MAX];
        if (ssh_packet_recv(cap, conn, discard) < 0) {
            println("[SSHD] connection lost while discarding the guessed packet");
            return;
        }
    }

    /* ---- curve25519-sha256 key exchange (RFC 8731) ---- */
    ssh_keys_t keys;
    int kex_ok = ssh_kex_ecdh_server(cap, conn,
        (const uint8_t *)client_id, (uint32_t)id_len,
        (const uint8_t *)SSH_ID_STR, (uint32_t)strlen(SSH_ID_STR),
        kexinit_in, (uint32_t)kin_len,
        kexinit_out, kexinit_len,
        &g_hostkey, &keys);

    if (!kex_ok) {
        println("[SSHD] key exchange FAILED, closing");
        return;
    }

    println("[SSHD] key exchange complete: host key signed H, NEWKEYS exchanged, "
            "cipher keys derived");
    wipe_bytes(&keys, sizeof(keys));
    println("[SSHD] closing: encrypted packet layer (chacha20-poly1305@openssh.com) "
            "not implemented yet");
}

int main(void)
{
    println("=== Exploidus SSH Daemon (sshd) ===");
    println("Handshake only (ID + KEXINIT + curve25519-sha256 KEX) -- see comment at top of sshd.c");

    hostkey_init();

    println("Listening on port 22...");

    cap_token_t null_cap = {0, 0};

    int srv = xsocket(null_cap, SOCK_TCP);
    if (srv < 0) {
        println("[SSHD] Error: socket failed");
        exit(1);
    }

    if (xbind(null_cap, srv, SSHD_PORT) < 0) {
        println("[SSHD] Error: bind failed");
        exit(1);
    }

    if (xlisten(null_cap, srv) < 0) {
        println("[SSHD] Error: listen failed");
        exit(1);
    }

    println("[SSHD] Ready.");

    while (1) {
        int conn = xaccept(null_cap, srv);
        if (conn < 0) {
            sleep_ticks(5);
            continue;
        }
        print("[SSHD] accepted connection, fd=");
        print_int(conn);
        println("");

        handle_connection(null_cap, conn);
        xclose(conn);
        println("[SSHD] connection closed, back to accept loop");
    }

    return 0;
}