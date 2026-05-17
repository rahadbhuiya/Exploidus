# Exploidus — Reactive Capability Kernel

A custom x86-64 operating system kernel built from scratch.

**Features:**
- Multiboot2 boot via GRUB2
- 4-level x86-64 paging with NX enforcement
- Colored zone physical memory manager (GREEN / YELLOW / RED)
- BLAKE3 capability token system with RDRAND seeding
- Intent-based preemptive scheduler (5 priority classes)
- Blocking waitpid (no busy-spin)
- VFS + ExFS filesystem with provenance records
- TCP/IP network stack (e1000, ARP, IP fragment reassembly, TCP, UDP, ICMP)
- 29 syscalls fully implemented (open/close/mmap/munmap/ps/audit/net)
- exploish interactive shell with real ps and audit commands

---

## Requirements

- Kali Linux (WSL2 on Windows, or native)
- x86_64-elf cross-compiler — built automatically by setup.sh
- NASM assembler
- QEMU x86_64 emulator
- xorriso + GRUB (for ISO builds)
- ~4 GB free disk space
- ~45 minutes first-time build (cross-compiler takes ~30 min)

---

## Quick Start (Kali Linux / WSL2)

    # Step 1: Install system packages
    sudo apt update
    sudo apt install -y build-essential nasm qemu-system-x86 xorriso \
                       grub-pc-bin grub-common mtools curl

    # Step 2: Build cross-compiler (once, takes ~30 min)
    bash setup.sh

    # Step 3: Reload PATH
    source ~/.bashrc

    # Step 4: Build kernel
    make

    # Step 5: Run
    make qemu

You will see the kernel boot and the exploish shell appear.

---

## Run Options

### Serial output in terminal (recommended)
    make qemu
All output goes to your terminal. Type commands here.

### VGA window (needs WSLg or X server)
    make qemu-vga
Opens a QEMU window with VGA text display.

### Bootable ISO
    make iso
    qemu-system-x86_64 -cdrom build/exploidus.iso -m 256M -serial stdio -display none

### GDB debugging
Terminal 1:
    make debug

Terminal 2:
    gdb build/exploidus.elf
    (gdb) target remote :1234
    (gdb) break kernel_main
    (gdb) continue

---

## Shell Commands

    help            List all commands
    ps              Show running processes (real kernel data)
    audit           Show audit log entries
    pid             Show current PID
    uname           Kernel version
    echo <text>     Print text
    clear           Clear screen
    cap             Show capability info
    rahu install    Install package (stub)
    rahu list       List packages
    rahu search     Search repository
    nafu remove     Remove package
    exit [code]     Exit

---

## Project Structure

    exploidus/
    kernel/arch/x86_64/    GDT, IDT, IRQ, ISR stubs
    kernel/boot/           Multiboot2 entry, long mode
    kernel/mm/             PMM, VMM, kmalloc
    kernel/cap/            BLAKE3 capability tokens
    kernel/audit/          Ring-buffer audit log
    kernel/proc/           Process table, scheduler, fork/exec
    kernel/syscall/        29 syscalls
    kernel/drivers/        VGA, serial, keyboard, ATA
    kernel/fs/vfs/         Virtual filesystem
    kernel/fs/exfs/        ExFS + provenance
    kernel/elf/            ELF64 loader
    kernel/net/            TCP/IP stack
    userspace/libc/        syscall wrappers, crt0
    userspace/shell/       exploish shell
    linker.ld              Kernel linker script
    Makefile               Build system
    setup.sh               Cross-compiler installer

---

## Build Targets

    make           Build kernel + shell
    make clean     Remove build artifacts
    make iso       Build bootable ISO
    make qemu      Run (serial, no window)
    make qemu-vga  Run with VGA window
    make debug     Run with GDB on :1234

---

## Troubleshooting

x86_64-elf-gcc not found:
    source ~/.bashrc
    # or:
    export PATH="$HOME/opt/cross/bin:$PATH"

QEMU blank screen:
    Use make qemu (serial mode), not make qemu-vga unless WSLg is available.

Build fails:
    make clean && make

WSL2 no GUI window:
    Use make qemu — no window needed for serial output.


## Author Rahad Bhuiya

## License MIT