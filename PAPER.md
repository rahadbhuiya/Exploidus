# Exploidus: A Security-First Intent-Based Kernel for Server Environments

**Rahad Bhuiya**

---

## Abstract

Modern operating systems treat security as an optional layer added on top of an existing design. Exploidus challenges this assumption by embedding security directly into the kernel's core scheduling and resource management decisions. We present an intent-based preemptive scheduler that classifies every process by its operational purpose — audit, interactive, I/O, network, or compute — and enforces strict priority ordering based on security criticality. The kernel implements a BLAKE3-based capability token system seeded with hardware entropy via RDRAND, a custom TCP/IP network stack, a virtual filesystem with provenance tracking, and a working graphical environment — all written from scratch in approximately 10,000 lines of C and x86-64 Assembly. Exploidus demonstrates that security-first design is achievable at the kernel level without sacrificing functionality.

---

## 1. Introduction

Operating systems such as Linux and Windows were designed for general-purpose computing. Security was not a foundational concern in their original architecture — it was added incrementally over decades through patches, modules, and external tools. This approach has produced systems where a compromised userspace process can often escalate privileges, evade logging, or interfere with critical services before detection.

Exploidus takes a different approach. Every design decision — from the scheduler to the filesystem — is made with security as the primary constraint, not an afterthought.

The central contribution of Exploidus is an **intent-based preemptive scheduler**. Rather than treating all processes as equal candidates for CPU time, the kernel requires each process to declare its operational intent at creation. The scheduler uses this intent to enforce a fixed priority hierarchy: audit processes always run first, ensuring that security logs are never starved; interactive processes run second; followed by I/O, network, and compute workloads. This ordering guarantees that an attacker cannot exhaust CPU resources in a way that silences the audit subsystem.

A second contribution is a **BLAKE3 capability token system**. Every resource access in Exploidus requires a cryptographically valid capability token, seeded with hardware entropy from Intel RDRAND. Tokens can be revoked at runtime via a kernel-maintained revocation table. This design is inspired by the seL4 microkernel's capability model, adapted for a monolithic architecture.

Exploidus also includes a custom TCP/IP network stack, an ELF64 loader, a virtual filesystem with a custom filesystem (ExFS) that maintains provenance records, and a framebuffer-based graphical environment — all implemented from scratch without borrowing from existing kernel codebases.

The remainder of this paper is organized as follows. Section 2 describes the intent-based scheduler in detail. Section 3 covers the capability system. Section 4 presents the network stack. Section 5 compares Exploidus with related work. Section 6 discusses limitations and future work. Section 7 concludes.

---

## 2. Intent-Based Preemptive Scheduler

### 2.1 Motivation

Traditional schedulers such as the Linux Completely Fair Scheduler (CFS) optimize for throughput and fairness across all processes. They have no concept of what a process is trying to accomplish. A compute-intensive background job and a security audit process compete for CPU time on equal terms. In a threat scenario where an attacker floods the system with CPU-intensive processes, audit logging can be delayed or starved entirely — creating a window where attacks go unrecorded.

Exploidus addresses this by making process intent a first-class kernel concept.

### 2.2 Intent Classes

Every process in Exploidus is assigned one of five intent classes at creation time:

| Intent | Purpose | Priority |
|---|---|---|
| INTENT_AUDIT | Security logging | 1 (highest) |
| INTENT_INTERACTIVE | User-facing processes | 2 |
| INTENT_IO | Disk and file operations | 3 |
| INTENT_NETWORK | Network communication | 4 |
| INTENT_COMPUTE | Background computation | 5 (lowest) |

This ordering is fixed in the kernel and cannot be overridden by userspace. A process cannot declare a higher intent than its actual function.

### 2.3 Implementation

Each intent class maintains a separate FIFO queue implemented as a linked list. The scheduler always selects from the highest non-empty queue. Within a queue, processes are served in round-robin order with fixed time slices assigned per intent class — audit processes receive shorter slices to minimize latency, compute processes receive longer slices to reduce context switch overhead.

```c
static const int QUEUE_ORDER[INTENT_COUNT] = {
    INTENT_AUDIT,
    INTENT_INTERACTIVE,
    INTENT_IO,
    INTENT_NETWORK,
    INTENT_COMPUTE,
};
```

Context switching is performed in x86-64 Assembly. On every switch, the outgoing process's general-purpose registers, stack pointer, and instruction pointer are saved to its kernel stack. The incoming process's previously saved state is restored. The TSS RSP0 field is updated so that the CPU knows where to find the kernel stack on the next ring-3 interrupt.

### 2.4 Blocking and Unblocking

When a process calls `waitpid()`, it transitions to `PROC_BLOCKED` state and is removed from its intent queue. On every scheduler tick, the `unblock_waiters()` function scans the process table for blocked processes whose waited-on child has become a zombie. Qualifying processes are returned to `PROC_READY` and re-enqueued. This design eliminates busy-spin waiting entirely.

### 2.5 Security Guarantee

Because INTENT_AUDIT always runs before any other class, an attacker who controls an INTENT_COMPUTE or INTENT_NETWORK process cannot prevent audit records from being written. The audit subsystem is structurally protected by the scheduler itself — not by access control lists or runtime checks that can be bypassed.

---

## 3. Capability-Based Security System

### 3.1 Motivation

In traditional operating systems, access control is enforced through user IDs, file permissions, and access control lists. These mechanisms are checked at the point of access but do not prevent a compromised process from attempting unauthorized operations. A process that gains elevated privileges can silently access resources it was never intended to reach.

Exploidus adopts a capability-based security model. Every resource access requires a cryptographically valid token. Possession of the token is the only proof of authorization — there are no permission tables to bypass, no user ID checks to spoof.

### 3.2 Token Generation

Capability tokens are 128-bit values generated using the BLAKE3 cryptographic hash function. At kernel initialization, a 32-byte secret is seeded using Intel RDRAND — a hardware random number generator that draws entropy from a physical on-chip source. On processors without RDRAND support, the kernel falls back to the CPU timestamp counter (RDTSC) combined with a fixed XOR mask, ensuring initialization always succeeds.

```c
void cap_subsystem_init(void) {
    for (int i = 0; i < 4; i++) {
        uint64_t r = rdrand64();
        memcpy(g_kernel_secret + i * 8, &r, 8);
    }
}
```

Each token is derived by hashing the kernel secret together with the requesting process's PID, the target resource identifier, and a permission bitmask. This means tokens are unique per process, per resource, and per permission set — a token for read access cannot be used for write access, and a token issued to PID 3 cannot be used by PID 7.

### 3.3 Token Revocation

The kernel maintains a revocation table of 256 entries. When a process exits or a resource is destroyed, its associated tokens are added to the revocation table. Any subsequent attempt to use a revoked token is rejected by the capability broker, regardless of cryptographic validity. This prevents use-after-free style attacks at the capability level.

```c
#define REVOKE_TABLE_SIZE 256
typedef struct { uint64_t upper; uint64_t lower; } revoke_entry_t;
static revoke_entry_t g_revoke_table[REVOKE_TABLE_SIZE];
```

### 3.4 Comparison with seL4

The seL4 microkernel introduced formal capability-based access control with mathematical proof of correctness. Exploidus draws inspiration from this model but applies it within a monolithic kernel architecture. Unlike seL4, Exploidus does not provide formal verification — however, the core principle is the same: no resource access is permitted without an unforgeable token issued by the kernel.

The key difference is that Exploidus combines capability tokens with the intent-based scheduler. Even if an attacker obtains a valid capability token, they cannot use it from an INTENT_COMPUTE process to interfere with INTENT_AUDIT operations — the scheduler prevents it structurally.

---

## 4. Custom TCP/IP Network Stack

### 4.1 Motivation

Most hobby operating systems either skip networking entirely or port an existing TCP/IP implementation such as lwIP. Exploidus implements its own network stack from scratch. This decision was made for two reasons: first, to maintain full visibility into every packet that enters and leaves the system; second, to allow future integration with CNSL — a correlated network security layer that detects coordinated attacks across multiple log sources.

### 4.2 Architecture

The network stack follows a layered architecture, mirroring the standard TCP/IP model:

```
Application (socket API)
        ↓
    TCP / UDP
        ↓
       IP
        ↓
      ARP
        ↓
  e1000 NIC driver
```

Each layer is implemented as a separate module with a clean interface. Packets flow downward on transmit and upward on receive through explicit function calls.

### 4.3 Layer Implementations

**ARP** — The Address Resolution Protocol layer maintains a cache of IP-to-MAC mappings. On startup, the kernel sends a gratuitous ARP to announce its presence on the network.

**IP** — The IPv4 layer handles packet fragmentation and reassembly. Incoming fragments are held in a reassembly buffer until all fragments of a datagram arrive, then passed up to TCP or UDP as a complete packet.

**TCP** — The TCP implementation supports the full three-way handshake, connection teardown, sliding window flow control, and retransmission. Initial sequence numbers are generated using RDRAND to prevent sequence number prediction attacks.

```c
static uint32_t tcp_rand32(void) {
    uint64_t val = 0;
    uint8_t ok = 0;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile ("rdrand %0\n setc %1\n"
                         : "=r"(val), "=qm"(ok));
        if (ok) return (uint32_t)val;
    }
    /* Fallback: Knuth multiplicative hash */
    static uint32_t s_ctr = 0xDEADBEEF;
    s_ctr += 2654435761u;
    return s_ctr;
}
```

**UDP** — A lightweight UDP implementation supports connectionless datagram transmission and reception.

**ICMP** — Basic ICMP echo request and reply are supported, allowing the kernel to respond to ping.

### 4.4 Socket API

Userspace programs access the network through a socket API exposed via system calls. The API supports `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`, and `close()` — sufficient for implementing both client and server applications in userspace.

### 4.5 Security Considerations

Because Exploidus controls the entire network stack, future integration with CNSL will allow the kernel to inspect every packet at the IP layer before it reaches userspace — enabling detection and blocking of attacks that userspace security tools cannot catch.

---

## 5. Related Work

### 5.1 Linux

Linux is the dominant open-source kernel and the most widely deployed server operating system. Its Completely Fair Scheduler provides excellent throughput and fairness but has no concept of process intent. Security in Linux is layered on top of the base kernel through mechanisms such as SELinux, AppArmor, seccomp, and eBPF — each added years or decades after the original design. This architectural debt means that a newly discovered privilege escalation vulnerability can bypass multiple security layers simultaneously, as demonstrated by vulnerabilities such as Dirty COW (CVE-2016-5195) and Dirty Pipe (CVE-2022-0847).

### 5.2 Windows

Windows NT introduced a priority-based scheduler with 32 priority levels. Like Linux, it has no intent concept — priority is a numeric value with no semantic meaning attached. Security is enforced through the Windows Security Reference Monitor, a separate subsystem that checks access tokens on every object access.

### 5.3 seL4

seL4 is a formally verified microkernel developed by CSIRO in Australia. It provides mathematical proof that its capability-based access control cannot be bypassed. Exploidus adopts seL4's capability concept within a monolithic architecture, accepting a weaker isolation guarantee in exchange for lower complexity and better performance on single-server deployments.

### 5.4 Barrelfish

Barrelfish was a research operating system developed jointly by ETH Zurich and Microsoft Research, targeting multi-core scalability. It explored capability-based resource management and heterogeneous hardware. The project is no longer active, with its final release in 2020. Barrelfish targeted many-core scalability while Exploidus targets security-first server deployments.

### 5.5 SerenityOS

SerenityOS is an independently developed operating system built from scratch by Andreas Kling, begun in 2018. It demonstrates that a single developer can build a complete OS with GUI, networking, and a browser engine over several years. The key difference is focus — SerenityOS targets a nostalgic desktop experience while Exploidus targets security-critical server environments.

### 5.6 QNX

QNX is a commercial real-time operating system used in automotive, medical, and industrial systems. It uses a microkernel with priority-based scheduling where process priorities reflect real-time criticality. Exploidus can be seen as applying RTOS scheduling philosophy — critical tasks always run first — to general-purpose server security.

---

## 6. Limitations and Future Work

### 6.1 Current Limitations

**Single-core only** — The current Exploidus scheduler assumes a single CPU core. Adding SMP support requires per-core run queues, inter-processor interrupts, and fine-grained spinlocks — a significant architectural change.

**No formal verification** — Unlike seL4, Exploidus makes no mathematical guarantees about its security properties. The capability system and scheduler have been designed carefully but not formally proven correct.

**Revocation table size** — The capability revocation table is currently fixed at 256 entries. A long-running server with many short-lived processes could exhaust this table.

**No ASLR** — Address Space Layout Randomization is not yet implemented. Userspace processes are loaded at fixed virtual addresses, making memory corruption exploits more predictable.

**Package manager incomplete** — The `rahu` package manager exists as a shell command but has no download or install logic implemented.

**No multi-user support** — Exploidus currently runs as a single-user system with no concept of unprivileged users or per-user capability sets.

### 6.2 Future Work

**CNSL integration** — The most significant planned addition is building CNSL directly into the kernel network stack. CNSL is a correlated attack detection system that correlates SSH, web, and database log sources to detect coordinated attacks. At the kernel level, CNSL would have access to raw packet data before it reaches userspace.

**HuddleCluster integration** — HuddleCluster is a penguin-inspired self-organizing load balancer that rotates servers between active and resting states based on EMA-smoothed temperature scores. Integrating HuddleCluster into Exploidus would allow the OS itself to manage server load distribution without external orchestration tools.

**SMP support** — Each core could maintain its own per-intent queues, with a global rebalancer ensuring audit processes are always represented on at least one core.

**Formal verification** — A long-term goal is to formally verify the capability token system using a proof assistant such as Coq or Isabelle.

**ASLR and stack canaries** — Standard exploit mitigations should be added to the ELF loader and process creation path.

**Adaptive intent** — A future extension would allow the kernel to observe process behavior over time and adjust its intent classification dynamically.

---

## 7. Conclusion

Operating system security has historically been treated as a problem to be solved after the fact — through patches, modules, and external tools layered on top of kernels designed without security as a primary concern. This approach has produced systems that are difficult to harden and expensive to audit.

Exploidus demonstrates an alternative. By embedding security into the scheduler itself, the kernel provides a structural guarantee that audit processes cannot be starved, that capability tokens cannot be forged, and that resource access cannot occur without explicit kernel authorization. These guarantees do not depend on userspace cooperation or administrator configuration — they are enforced by the kernel at every scheduling decision.

The system is implemented in approximately 10,000 lines of C and x86-64 Assembly, boots on real hardware via QEMU, runs a working graphical environment, and supports a custom TCP/IP network stack, filesystem, and interactive shell. It was built entirely from scratch without borrowing from existing kernel codebases.

Exploidus is not a finished product. It lacks SMP support, formal verification, and several features required for production deployment. But it establishes a foundation — a proof of concept that security-first kernel design is achievable by a single developer, and that intent-based scheduling is a viable alternative to the numeric priority systems that have dominated operating system design for fifty years.

Future work will focus on integrating CNSL for kernel-level attack correlation and HuddleCluster for built-in load balancing — completing the vision of an operating system where security, scheduling, and infrastructure management are not separate concerns but a single unified design.

---

## References

1. Liedtke, J. (1995). On micro-kernel construction. *ACM SIGOPS Operating Systems Review*, 29(5), 237-250.
2. Klein, G., et al. (2009). seL4: Formal verification of an OS kernel. *ACM SIGOPS*, 43(2), 207-220.
3. Baumann, A., et al. (2009). The multikernel: A new OS architecture for scalable multicore systems. *ACM SIGOPS*, 43(2), 29-44.
4. O'Connor, J., et al. (2020). BLAKE3: One function, fast everywhere. *IACR Cryptology ePrint Archive*.
5. Zitterbart, D., et al. (2011). Coordinated movements prevent jamming in an emperor penguin huddle. *PLOS ONE*, 6(6).
6. McKusick, M., et al. (1996). *The Design and Implementation of the 4.4BSD Operating System*. Addison-Wesley.
7. Intel Corporation. (2023). *Intel 64 and IA-32 Architectures Software Developer's Manual*.