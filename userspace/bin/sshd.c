/*
 * sshd.c — Exploidus SSH daemon (identification exchange +
 * SSH_MSG_KEXINIT negotiation; no key exchange/encryption yet)
 *
 * Listens on port 22, accepts TCP connections, performs the
 * plaintext identification-string exchange from RFC 4253 §4.2, then
 * exchanges and checks SSH_MSG_KEXINIT (§7.1) against the one
 * algorithm this daemon supports per category (curve25519-sha256 /
 * ssh-ed25519 / chacha20-poly1305@openssh.com — see sshd_packet.c).
 * Both are genuine, spec-correct SSH protocol behavior, not a
 * simulation of it — KEXINIT itself is defined to be sent in
 * plaintext, before any key exchange happens.
 *
 * What this deliberately does NOT do yet: the actual Diffie-Hellman
 * exchange (SSH_MSG_KEX_ECDH_INIT/REPLY) or anything encrypted after
 * it. Every crypto primitive that exchange needs is now ported and
 * verified against its own published test vectors
 * (kernel/crypto/x25519.c, chacha20poly1305.c, sha256.c, sha512.c,
 * ed25519.c — see the project README for each one's verification
 * transcript), but wiring them into an actual KEX_ECDH_INIT/REPLY
 * state machine, host-key persistence, and the switch to encrypted
 * packet framing is separate work, not done here. sshd closes
 * honestly after negotiation instead of accepting cleartext input a
 * real SSH client would never send unencrypted past this point.
 */

#include "../libc/syscall.h"
#include "sshd_packet.h"

#define SSHD_PORT   22
#define SSH_ID_LINE "SSH-2.0-Exploidus_0.1\r\n"
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

    static char buf[ID_BUF_SIZE];
    println("[SSHD] waiting for client identification line...");
    int n = xrecv(cap, conn, buf, ID_BUF_SIZE - 1);
    print("[SSHD] xrecv() returned ");
    print_int(n);
    println("");
    if (n <= 0) {
        println("[SSHD] connection closed before sending an identification string");
        return;
    }
    buf[n] = 0;

    /* Trim at the first CR or LF -- don't trust the client to have
     * sent exactly one clean line, and never echo/log anything past
     * that back out or into a fixed-size buffer without a bound. */
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') { buf[i] = 0; break; }
    }

    if (!str_starts(buf, "SSH-")) {
        println("[SSHD] rejecting: client's first line is not an SSH identification string");
        print("[SSHD]   got: ");
        println(buf);
        return;
    }

    char proto[16];
    if (parse_protoversion(buf, proto, sizeof(proto)) < 0) {
        println("[SSHD] rejecting: malformed identification string (no protoversion)");
        return;
    }

    int proto_ok = (strcmp_u(proto, "2.0") == 0) || (strcmp_u(proto, "1.99") == 0);
    print("[SSHD] client identification: ");
    println(buf);
    print("[SSHD] protocol version: ");
    print(proto);
    println(proto_ok ? " (supported)" : " (unsupported)");

    if (!proto_ok) {
        println("[SSHD] closing: only SSH protocol 2.0 (or 1.99 compat) is recognized");
        return;
    }

    /*
     * Identification exchange complete. Continue into SSH_MSG_KEXINIT
     * algorithm negotiation (RFC 4253 §7.1) -- real protocol
     * behavior, still no cryptography running yet (KEXINIT itself is
     * plaintext, negotiated before any key exchange happens). This is
     * as far as sshd goes right now: the actual Diffie-Hellman
     * exchange (SSH_MSG_KEX_ECDH_INIT/REPLY) that would follow a
     * successful negotiation isn't wired in yet, even though every
     * crypto primitive it needs (kernel/crypto/x25519.c,
     * chacha20poly1305.c, sha256.c, ed25519.c) is now ported and
     * verified against its own test vectors -- see the project
     * README. Close honestly after negotiation rather than pretend to
     * continue into key exchange this daemon can't yet actually do.
     */
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

    int negotiated = ssh_kexinit_check(kexinit_in, (uint32_t)kin_len);
    println(negotiated
        ? "[SSHD] negotiation OK -- peer supports curve25519-sha256 / "
          "ssh-ed25519 / chacha20-poly1305@openssh.com"
        : "[SSHD] negotiation FAILED -- peer does not offer all of "
          "curve25519-sha256 / ssh-ed25519 / chacha20-poly1305@openssh.com");

    println("[SSHD] closing: key exchange (SSH_MSG_KEX_ECDH_INIT/REPLY) "
             "not implemented yet");
}

int main(void)
{
    println("=== Exploidus SSH Daemon (sshd) ===");
    println("Protocol version exchange only -- see comment at top of sshd.c");
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