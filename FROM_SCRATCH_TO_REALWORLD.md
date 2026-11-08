# Exploidus: From Scratch to Production-Grade Operating System

> Master Engineering Blueprint & Architectural Roadmap
> Target: From bare-metal / QEMU to bootable Live ISO with hardware drivers, modern memory management, native POSIX layer, custom GUI desktop, and native cybersecurity tools.

---

## 1. Architectural Vision

Exploidus is a 100% from-scratch x86_64 Reactive Capability Operating System. It does not use any third-party kernel code or pre-existing distributions. It implements an independent monolithic capability kernel with an intent-based scheduler, built-in network security, crash-consistent ExFS filesystem, and native Yolish runtime support.

The target system architecture is structured as follows:

```
+------------------------------------------------------------------+
|                   EXPLOIDUS USERSPACE (DESKTOP)                  |
|  +--------------------+ +--------------------+ +--------------+  |
|  | ExWM Window Mgr    | | ExUI Toolkit       | | Terminal Em. |  |
|  +--------------------+ +--------------------+ +--------------+  |
|  +--------------------+ +--------------------+ +--------------+  |
|  | CNSL Live Monitor  | | FIM Security Guard | | Port Scanner |  |
|  +--------------------+ +--------------------+ +--------------+  |
+------------------------------------------------------------------+
|                  DISPLAY SERVER & COMPOSITOR                     |
|               Alien v2.0 Compositor (Shm / GPU)                  |
+------------------------------------------------------------------+
|                     SYSTEM LIBC & POSIX ABI                      |
|             Native C Libc + Exploidus Native Syscalls            |
+------------------------------------------------------------------+
|                         EXPLOIDUS KERNEL                         |
|  +-----------------+ +------------------+ +-------------------+  |
|  | Intent Sched    | | BLAKE3 Security  | | Demand Paging VMM |  |
|  +-----------------+ +------------------+ +-------------------+  |
|  | AHCI / NVMe     | | xHCI USB 3.0     | | e1000 / Realtek   |  |
|  +-----------------+ +------------------+ +-------------------+  |
|  | ExFS (Journld)  | | TCP/IPv4/IPv6    | | GOP Framebuffer   |  |
|  +-----------------+ +------------------+ +-------------------+  |
+------------------------------------------------------------------+
|                     HARDWARE / BARE METAL                        |
|             UEFI x86_64, APIC, SATA SSD/NVMe, GPU/Display        |
+------------------------------------------------------------------+
```

---

## 2. Phase-by-Phase Implementation Checklist

---

### Phase 1: Modern Boot & Hardware Initialization (Boot & Base HW)
*Target: Support both modern UEFI and legacy Multiboot2 boot paths across bare-metal hardware.*

- [x] **1.1 UEFI Bootloader Support (GRUB-EFI / Limine / EFI Stub)**
  - Path: `kernel/boot/efi_main.c` or GRUB EFI configuration
  - Objective: Pass linear framebuffer base address and resolution (1920x1080x32bpp) from UEFI Graphics Output Protocol (GOP) into the kernel.
  - Verification: Bootable directly from modern PC and laptop UEFI boot menus without legacy CSM.
- [x] **1.2 ACPI Table Parsing (RSDP, XSDT, MADT)**
  - Path: `kernel/arch/x86_64/acpi.c`, `kernel/arch/x86_64/acpi.h`
  - Objective: Enumerate multi-core CPU topology and hardware IRQ routing tables.
- [x] **1.3 Local APIC & I/O APIC Drivers (Retire Legacy PIC)**
  - Path: `kernel/arch/x86_64/apic.c`, `kernel/arch/x86_64/ioapic.c`
  - Objective: Handle multi-core hardware interrupts and high-resolution timer events.
- [x] **1.4 Symmetric Multiprocessing (SMP / Multi-Core)**
  - Path: `kernel/arch/x86_64/smp.c`
  - Objective: Wake secondary CPU cores (Application Processors) via INIT-SIPI-SIPI sequence and integrate with intent scheduler.

---

### Phase 2: Memory Management & Kernel Core Expansion (VMM & Core)
*Target: Replace static allocation limits with scalable virtual memory management.*

- [x] **2.1 Demand Paging & Page Fault Handler**
  - Path: `kernel/mm/vmm.c`, `kernel/arch/x86_64/idt.c` (ISR 14)
  - Objective: Allocate physical frames on-demand upon page fault instead of eager contiguous mapping.
- [x] **2.2 Slab / SLUB Allocator**
  - Path: `kernel/mm/slab.c`
  - Objective: Fixed-size cache allocation pools (32B, 64B, 128B, up to 4KB) to eliminate heap fragmentation.
- [ ] **2.3 File-Backed and Anonymous mmap()**
  - Path: `kernel/syscall/mmap.c`, `kernel/fs/vfs/vfs.c`
  - Objective: Map executable binaries, shared regions, and GUI image buffers directly into virtual address space.
- [ ] **2.4 Threading Support (Kernel Thread Contexts)**
  - Path: `kernel/proc/thread.c`, `kernel/proc/scheduler.c`
  - Objective: Support multiple threads sharing a single address space (CR3) with distinct user stacks.
- [x] **2.5 Anonymous Pipes & Stream Redirection (pipe, dup2)**
  - Path: `kernel/fs/pipe.c`, `kernel/syscall/table.c`
  - Objective: Shell pipeline support for command chaining (`cmd1 | cmd2 > out.txt`).
- [ ] **2.6 Complete Signal Delivery & Handling (sigaction, sigreturn)**
  - Path: `kernel/arch/x86_64/signal.c`, `kernel/arch/x86_64/sigreturn.asm`
  - Objective: Interrupt userspace contexts, push signal frames, and restore execution state on SIGINT, SIGCHLD, and SIGTERM.

---

### Phase 3: Real-World Storage & Input Drivers
*Target: Direct driver support for modern SATA SSD, NVMe, and USB 3.0 controllers.*

- [ ] **3.1 PCI / PCIe Bus Enumeration Scanner**
  - Path: `kernel/drivers/pci.c`, `kernel/drivers/pci.h`
  - Objective: Scan PCI configuration space (Busses 0-255), discover vendor/device IDs, and parse Base Address Registers (BARs).
- [ ] **3.2 AHCI / SATA Controller Driver (SSD / HDD)**
  - Path: `kernel/drivers/ahci.c`, `kernel/drivers/ahci.h`
  - Objective: Issue DMA read/write commands to SATA SSDs and mount native ExFS partitions directly.
- [ ] **3.3 xHCI (USB 3.0) Host Controller Driver**
  - Path: `kernel/usb/xhci.c`
  - Objective: Detect and drive modern USB keyboards, mice, and USB mass storage flash drives.
- [ ] **3.4 VESA / GOP Linear Framebuffer Console Driver**
  - Path: `kernel/drivers/gop.c`, `kernel/drivers/fb.c`
  - Objective: Render high-resolution (1080p) 32-bpp graphics directly to display hardware.

---

### Phase 4: Standard Library & System Environment
*Target: Full POSIX-compliant userspace compatibility layer.*

- [ ] **4.1 Exploidus Syscall ABI Expansion**
  - Path: `kernel/syscall/table.c`, `userspace/libc/syscall.h`
  - Objective: Provide full coverage for standard system calls including `read`, `write`, `open`, `close`, `stat`, `mmap`, `fork`, `execve`.
- [ ] **4.2 Complete Native Libc Implementation**
  - Path: `userspace/libc/`
  - Objective: Provide full string, memory, math, formatting, and file I/O operations without third-party dependencies.
- [ ] **4.3 Dynamic Relocating Loader**
  - Path: `kernel/elf/elf.c`
  - Objective: Support shared object relocations, symbol resolution, and runtime library loading.
- [ ] **4.4 Pseudo-Terminal Subsystem (PTY / TTY)**
  - Path: `kernel/fs/pty.c`
  - Objective: Provide master/slave pseudo-terminals (`/dev/ptmx`, `/dev/pts/*`) for GUI terminal emulators.

---

### Phase 5: Custom Dark Cybersecurity GUI Desktop Engine

```
+--------------------------------------------------------+
| [Exploidus Top Panel: 14:30 | CPU: 12% | RAM: 1.2G]    |
+--------------------------------------------------------+
|                                                        |
|   +-[ ExTerminal ]--------------+                      |
|   | star@exploidus:~$ ex-scan   |                      |
|   | [CNSL] Real-time guard active                      |
|   | Scanning subnet 192.168.1.0 |                      |
|   | > 192.168.1.1 [HTTP, SSH]   |                      |
|   | > 192.168.1.45 [EXPLOID_NODE|                      |
|   +-----------------------------+                      |
|                                                        |
|   +-[ CNSL Security Dashboard ]-+                      |
|   | Active Attacks Blocked: 0   |                      |
|   | FIM Status: 13 paths clean  |                      |
|   +-----------------------------+                      |
|                                                        |
+--------------------------------------------------------+
| [Applications] [Terminal] [ExFS Browser] [Tools] [Exit]|
+--------------------------------------------------------+
```

- [ ] **5.1 Alien v2.0 Compositor & Display Server**
  - Path: `userspace/compositor/alien.c`, `userspace/compositor/shm_surface.c`
  - Objective: Render windows into dedicated off-screen shared memory buffers and compose them with alpha blending to the screen.
- [ ] **5.2 Custom Vector & Bitmap Font Engine**
  - Path: `kernel/drivers/font.c`, `userspace/compositor/`
  - Objective: Render anti-aliased monospace programmer fonts (Hack, JetBrains Mono, Fira Code).
- [ ] **5.3 ExUI Lightweight Widget Toolkit**
  - Path: `userspace/gui/exui/`
  - Widgets: `Window`, `Button`, `TextBox`, `ListView`, `Canvas`, `Panel`, `Menu`
- [ ] **5.4 ExWM (Exploidus Window Manager)**
  - Path: `userspace/gui/exwm/wm.c`
  - Features:
    - Dark cyan and neon green cybersecurity aesthetic
    - Window dragging, minimize, maximize, and tiling snap
    - Top status panel with clock, CPU/RAM meter, and active process count
- [ ] **5.5 Core Desktop Applications**
  - **ExTerminal**: Full terminal emulator with ANSI color and tab support
  - **ExFiles**: Visual ExFS directory navigator and provenance history inspector
  - **ExEditor**: Lightweight code and text editor with syntax highlighting

---

### Phase 6: Native Cybersecurity & Auditing Toolkit

- [ ] **6.1 Full SSHD Remote Daemon (`/bin/sshd`)**
  - Path: `userspace/bin/sshd.c`
  - Objective: Complete the X25519 + ChaCha20-Poly1305 + Ed25519 SSH handshake state machine for secure remote shell access.
- [ ] **6.2 CNSL Live Defense Monitor**
  - Path: `userspace/gui/apps/cnsl_monitor.c`
  - Objective: Visual interface for kernel-level attack detection, active IP blocklists, and security event alerts.
- [ ] **6.3 File Integrity Monitor (FIM) Dashboard**
  - Path: `userspace/bin/fim_check.c`
  - Objective: Monitor and display tamper verification status across 13 critical filesystem paths using BLAKE3 hashes.
- [ ] **6.4 Native Network Security Tools (`ex-scan`, `ys-sec`)**
  - Path: `userspace/bin/`
  - Objective: Fast port scanning, raw packet inspection, and protocol banner grabbing using Exploidus TCP/IP sockets.
- [ ] **6.5 Rahu Package Manager Complete Integration**
  - Path: `userspace/bin/rahu.c`
  - Objective: Package installation, dependency resolution, and signature verification for native `.fozu` packages.

---

### Phase 7: Production Live Bootable ISO & Installer

- [ ] **7.1 Hybrid UEFI/BIOS Bootable ISO Generator**
  - Path: `Makefile`, `iso/boot/grub/grub.cfg`
  - Objective: Dual-mode bootable ISO supporting both optical media and USB flash drives (Rufus / Ventoy / raw dd).
- [ ] **7.2 Live Ramdisk Execution**
  - Objective: Complete userspace execution in RAM without requiring hard disk modifications.
- [ ] **7.3 Graphical Drive Installer (`ex-installer`)**
  - GPT disk partitioning
  - ExFS filesystem creation
  - System deployment and GRUB bootloader configuration on target drive

---

## 3. Immediate Implementation Priorities

1. **UEFI GOP Linear Framebuffer Configuration**:
   - Ensure linear framebuffer resolution parameters in `iso/boot/grub/grub.cfg` pass cleanly into kernel initialization.
2. **Dynamic Paging & Physical Memory Expansion**:
   - Transition from static memory limits to full system RAM tracking in `kernel/mm/pmm.c` and `kernel/mm/vmm.c`.
3. **Interactive Terminal Text Editor (`/bin/kobi`)**:
   - Enable users to create, modify, and save configuration files and Yolish scripts directly inside the running Exploidus environment.
