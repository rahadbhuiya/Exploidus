/*
 * sshd.c — Exploidus SSH daemon (protocol version exchange only)
 *
 * Listens on port 22, accepts TCP connections, and performs the
 * plaintext identification-string exchange from RFC 4253 §4.2 --
 * this is the ONE part of the real SSH protocol that is defined to
 * happen before any encryption, so it's genuine, spec-correct SSH
 * protocol behavior, not a simulation of one.
 *
 * What this deliberately does NOT do: key exchange (Diffie-Hellman /
 * Curve25519), any symmetric cipher (AES/ChaCha20), any MAC (HMAC),
 * host-key signatures (Ed25519/RSA), or authentication. None of
 * those primitives exist anywhere in this codebase yet
 * (kernel/crypto/ only has BLAKE3, a hash) -- and hand-rolling
 * cipher/KEX/signature code from scratch, here, without independent
 * review or test-vector verification, is exactly the kind of thing
 * that produces a "secure shell" that isn't actually secure. So
 * sshd currently completes the identification exchange (real clients
 * — OpenSSH etc. — will get as far as printing our banner and
 * negotiating protocol version), logs what it learned about the
 * connecting client, and then closes the connection with an honest
 * message instead of pretending to continue into a key exchange it
 * can't really do.
 *
 * Next real step (separate, deliberately not started here): port a
 * small, well-reviewed reference crypto implementation (Curve25519 +
 * ChaCha20-Poly1305 is the modern SSH baseline) and verify it against
 * known test vectors BEFORE wiring it into any protocol code.
 */

#include "../libc/syscall.h"

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
     * Identification exchange complete -- this is as far as sshd
     * goes right now. A real server would proceed into
     * SSH_MSG_KEXINIT / Diffie-Hellman key exchange here. Close
     * honestly instead of accepting cleartext input that a real SSH
     * client would never send unencrypted, or pretending to
     * negotiate a cipher suite this daemon can't actually run.
     */
    println("[SSHD] identification exchange OK -- key exchange/encryption "
             "not implemented yet, closing connection");
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