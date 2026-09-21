# Exploidus — OS Vision

Exploidus is a security-first server operating system built
from scratch, designed as an alternative to Windows and Linux
for server environments.

## Why Exploidus?

Most operating systems treat security as an add-on.
Exploidus treats it as a foundation.

## Current State (v0.1.0)

### Kernel
- Intent-based scheduler with 5 priority classes (COMPUTE, IO, NETWORK, INTERACTIVE, AUDIT)
- Custom TCP/IP network stack (ARP, IPv4, TCP, UDP, socket layer)
- BLAKE3 capability token security system
- 55 syscalls implemented
- VFS + ExFS filesystem with lseek, stat, fstat, dup, dup2, rename,
  and a crash-consistent metadata journal
- ASLR — hardware RNG (RDRAND) based, all userspace binaries randomised on every boot

### Security
- CNSL (Correlated Network Security Layer) — kernel-level attack detection, IP blocking, tick-based auto-expire, connected to scheduler
- FIM (File Integrity Monitor) — watches 13 critical paths
- HuddleCluster — built-in load balancer with inner/outer ring promotion, rotation connected to scheduler tick

### Userspace
- exploish — interactive shell with capability-aware builtins
- yolish — intent-aware scripting language
  - `@intent("network")` annotation makes actual SYS_SET_INTENT syscall
  - `spawn("path", "intent")` builtin for intent-aware process creation
- init (PID 1) — spawns auditd, httpd, exploish in order, monitors shell
- auditd — kernel audit ring poller, logs to /var/log/audit.log
- httpd — HTTP server on port 80


## Roadmap

- [x] CNSL integration — built-in kernel level attack detection
- [x] HuddleCluster integration — built-in auto load balancing
- [x] ASLR — all userspace binaries randomised on boot
- [x] yolish intent syscall — @intent annotation wired to kernel scheduler
- [x] lseek, stat, fstat, dup, dup2 syscalls
- [x] argv/envp passing (System V ABI)
- [x] ExFS: crash-consistent journal (verified with deterministic
      crash-injection testing), indirect/double-indirect block
      addressing, atomic rename()
- [x] exec with argv passing (full execve) — SYS_EXECVE(path, argv,
      envp): replaces the calling process's own image in place (same
      PID, old address space freed), unlike SYS_EXECV/spawn which
      start a new child process
- [ ] yolish — add more builtins (file I/O, networking)
- [ ] Optional GUI — enable/disable on demand
- [ ] Smooth horizontal scaling via HuddleCluster
- [ ] Advanced cybersecurity platform at OS level
- [~] sshd — remote access daemon. TCP listener (port 22) + RFC 4253
      §4.2 identification-string exchange done and real (accepts a
      real SSH client's connection, negotiates protocol version,
      logs its software version). X25519 (Curve25519, key exchange)
      and ChaCha20-Poly1305 (AEAD cipher) ported and verified against
      RFC 7748 / RFC 8439 test vectors — both in kernel/crypto/,
      meant to compile as sshd's own userspace objects (not kernel
      objects — X25519 briefly was, corrected). Not yet integration-
      tested as userspace objects with the real cross-toolchain, and
      not yet wired into sshd.c. Still needed before real SSH key
      exchange works: SHA-256 (KDF, RFC 8731's curve25519-sha256),
      host-key signing, and the actual SSH_MSG_KEXINIT/KEX_ECDH_INIT/
      REPLY state machine in sshd.c — each to be verified against its
      own test vectors before wiring in, same as x25519.c/chacha20.c/
      poly1305.c were.

## Architecture

```
  exploish / yolish / userspace programs
            ↓
    Exploidus syscall layer (55 syscalls)
            ↓
  ┌─────────────────────────────────────┐
  │         Exploidus Kernel            │
  │  intent scheduler │ CNSL │ HC       │
  │  VFS+ExFS │ TCP/IP │ BLAKE3 caps    │
  └─────────────────────────────────────┘
            ↓
       x86-64 hardware
```

## Author

Rahad Bhuiya — [GitHub](https://github.com/rahadbhuiya)