# Exploidus — Reactive Capability Kernel

A custom x86-64 operating system kernel built from scratch.

## About

Exploidus is a personal operating system project developed in Bangladesh for operating system research and education.
The project is built from scratch and continues to evolve with new kernel subsystems, userspace applications, networking, and graphics support.
To the best of the author's knowledge, Exploidus is among the earliest publicly documented from-scratch operating system projects developed in Bangladesh. If you are aware of an earlier publicly documented project, please open an issue with supporting evidence.


## Demo

![Exploidus Boot Demo](docs/exploidus_demo.gif)




**Features:**
- Multiboot2 boot via GRUB2
- 4-level x86-64 paging with NX enforcement
- Colored zone physical memory manager (GREEN / YELLOW / RED)
- BLAKE3 capability token system with RDRAND seeding
- Intent-based preemptive scheduler (5 priority classes)
- Blocking waitpid (no busy-spin)
- Crash isolation — a fault in a userspace process kills only that
  process; the kernel keeps running (kernel-mode faults still halt)
- Kernel synchronization primitives: IRQ-safe spinlocks, blocking
  mutexes/semaphores (kernel/sync/)
- Additive driver registry (kernel/drivers/driver.c) tracking every
  hardware driver initialized at boot
- VFS + ExFS filesystem with provenance records, a crash-consistent
  metadata journal, and atomic rename()
- Generic block-device abstraction (kernel/drivers/blockdev.c) — ATA
  and USB mass-storage both register as named block devices (`ata0`,
  `usb0`); filesystem code goes through this interface instead of
  calling a specific driver directly
- USB stack: UHCI host controller driver (kernel/usb/uhci.c) — PCI
  detection, control/interrupt/bulk transfers, full device
  enumeration, HID reports, and USB Mass Storage (Bulk-Only
  Transport + SCSI READ/WRITE) with a `mount` shell command to attach
  a USB stick's ExFS volume into the VFS
- TCP/IP network stack (e1000, ARP, IP fragment reassembly, TCP, UDP, ICMP)
- 82 syscalls fully implemented (open/close/mmap/munmap/ps/audit/net/fb_blit/sigaction/chmod/rmdir/rename/rtc/futex/tls/mount)
- exploish interactive shell with real ps and audit commands
- Userspace compositor (`alien` command) with double-buffered
  rendering, dirty-region-aware redraws, single-syscall window
  blitting (SYS_FB_BLIT), and real-time-paced frame pacing

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

    # Step 5: Run (disk + network attached, so /bin/rahu works)
    make qemu-disk

You will see the kernel boot and the exploish shell appear. Plain
`make qemu` also works and boots faster, but it has no disk attached —
/bin only contains what's embedded in the kernel image, so rahu and
hello won't be there. Use `qemu-disk`/`qemu-gui`/`qemu-run` whenever you
need a working /bin or networking.

---

## Run Options

`qemu`, `qemu-vga` and `qemu-iso` boot the kernel with no disk attached —
fine for testing the kernel/shell core, but /bin/rahu and /bin/hello
won't exist. The `-disk`/`-gui`/`-run` targets mount build/disk.img and
enable the e1000 NIC, so the full system (including rahu) is available.

### Serial output in terminal, no disk
    make qemu
All output goes to your terminal. Type commands here.

### VGA window, no disk (needs WSLg or X server)
    make qemu-vga
Opens a QEMU window with VGA text display.

### Full system: disk + network, terminal output (recommended for rahu)
    make qemu-disk
Mounts build/disk.img and enables networking, so /bin/rahu, /bin/hello
etc. are present and `rahu install` can reach the registry at
10.0.2.2:9090. Serial output goes to your terminal.

### Full system with a VGA window
    make qemu-gui
Same as qemu-disk, but also opens a VGA window alongside the serial output.

### Full system, GUI + serial log to file
    make qemu-run
Same as qemu-gui, but serial output is written to /tmp/serial.log instead
of your terminal — use `tail -f /tmp/serial.log` to follow it.

### Bootable ISO (no disk)
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
    rahu install    Install package (downloads from registry)
    rahu remove     Remove package (stub — not yet implemented)
    rahu list       List installed packages (stub — use 'ls /bin')
    rahu search     Search local package index
    rahu update     Refresh local package index
    mount <dev> <mountpoint>   Mount a block device (e.g. mount usb0 /media/usb0)
    exit [code]     Exit

    ls, cd, pwd     Navigate the filesystem
    cat, touch      Read / create a file
    write <f> <t>   Write text to a file
    mkdir, rmdir    Create / remove a directory
    rm <file>       Remove a file
    cp <src> <dst>  Copy a file
    mv <src> <dst>  Move/rename a file (atomic — see ExFS section below)
    cmp <f1> <f2>   Compare two files byte-for-byte
    find <path>     List files recursively
    chmod <mode> <file>   Change permission bits
    wc, grep, head, tail, xxd, sed, tee   Standard text-file tools
    env             Show environment

---

## Project Structure

    exploidus/
    kernel/arch/x86_64/    GDT, IDT, IRQ, ISR stubs, crash-isolating fault handler
    kernel/boot/           Multiboot2 entry, long mode
    kernel/mm/             PMM, VMM, kmalloc (spinlock-protected heap)
    kernel/cap/            BLAKE3 capability tokens
    kernel/audit/          Ring-buffer audit log
    kernel/proc/           Process table, scheduler, fork/exec
    kernel/sync/           Spinlock, mutex, semaphore primitives
    kernel/syscall/        82 syscalls
    kernel/drivers/        VGA, serial, keyboard, mouse, ATA, framebuffer,
                           driver.c (hardware driver registry),
                           blockdev.c (generic block-device abstraction)
    kernel/usb/            UHCI host controller driver — control/interrupt/
                           bulk transfers, enumeration, HID, mass storage
    kernel/fs/vfs/         Virtual filesystem
    kernel/fs/exfs/        ExFS + provenance
    kernel/elf/            ELF64 loader
    kernel/net/            TCP/IP stack
    userspace/libc/        syscall wrappers, crt0
    userspace/shell/       exploish shell
    userspace/compositor/  Windowing compositor (`alien` GUI mode)
    tools/mkexfs.py        Offline ExFS image formatter (builds build/disk.img)
    tools/fsck.py          Offline ExFS consistency checker/repairer
                           (python3 tools/fsck.py build/disk.img [--fix])
    linker.ld              Kernel linker script
    Makefile               Build system
    setup.sh               Cross-compiler installer

---

## Build Targets

    make           Build kernel + shell
    make clean     Remove build artifacts
    make iso       Build bootable ISO
    make qemu      Run (serial, no window, no disk)
    make qemu-vga  Run with VGA window (no disk)
    make qemu-disk Run with disk + network attached (rahu works)
    make qemu-usb-storage-test  Run with a USB mass-storage device attached
                   instead of usb-tablet, to exercise the UHCI mass-storage path
    make qemu-gui  Same as qemu-disk, with a VGA window
    make qemu-run  Same as qemu-gui, serial output logged to a file
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

GUI (`alien`)/mouse feels slow or stuttery:
    All qemu-disk/qemu-gui targets now pass -accel kvm:tcg, which uses
    KVM hardware acceleration when available and falls back to plain
    software emulation (TCG) otherwise. Check whether KVM is actually
    available on your host:
        ls -la /dev/kvm
    If that file doesn't exist, QEMU is running the CPU in pure
    software emulation, which can be dramatically (10-50x) slower than
    real hardware — enough on its own to make interrupt-heavy code
    (mouse polling, the compositor) feel laggy no matter how optimized
    the OS code is. If you're inside a VM (VMware/VirtualBox/cloud),
    enable nested virtualization for the guest (e.g. VMware: VM
    Settings -> Processor -> "Virtualize Intel VT-x/EPT or AMD-V/RVI";
    VirtualBox: Settings -> System -> Processor -> "Enable Nested
    VT-x/AMD-V") and make sure the kvm kernel module is loaded
    (`sudo modprobe kvm_intel` or `kvm_amd`).


## Author Rahad Bhuiya

## License MIT

---

## Recent Stability Fixes

A round of fixes to the GUI/compositor path and kernel core:

- **Mouse cursor race**: the kernel's PS/2 IRQ handler no longer draws
  its own cursor directly on the framebuffer while the compositor owns
  the screen (`fb_console_enabled()` gates it) — it was racing with
  the compositor's double-buffered draw and flip.
- **fb_blend_pixel double-buffer bug**: shadows/blends were being
  written straight to the front buffer, bypassing double buffering,
  which caused visible flicker on every composite.
- **SYS_FB_BLIT**: a new syscall blits a whole window's ARGB buffer in
  one call instead of one syscall per pixel (previously ~120,000
  syscalls per window per frame for a typical window size).
- **composite_frame_light()**: plain cursor movement no longer
  triggers a full wallpaper gradient repaint; only the affected
  windows/dock are redrawn.
- **Software VSync**: the compositor paces repaints against real
  elapsed time (`uptime()`), not a loop-iteration counter, so the
  frame rate stays consistent regardless of other work happening in
  the event loop.
- **Crash isolation**: a fault (page fault, GPF, divide-by-zero, ...)
  in userspace code now kills only that process, the same way
  `sys_exit()` does, instead of halting the entire kernel. Faults in
  kernel-mode code still halt, since kernel state may be corrupted.
- **kmalloc/kfree race**: the heap free-list is now protected by an
  IRQ-safe spinlock — previously an interrupt firing mid-allocation
  could corrupt it.
- **kernel/sync/**: new spinlock/mutex/semaphore primitives for future
  use protecting shared kernel/driver state.
- **Driver registry**: `kernel/drivers/driver.c` tracks every hardware
  driver initialized at boot (additive — boot order is unchanged).
- **Removed hot-path debug logging**: `unblock_sleepers()` (runs from
  the timer IRQ at 100Hz) and `ipc_send()` (runs on every message)
  used to `serial_print()` on every event. Serial writes busy-wait on
  real UART timing, so this was stalling keyboard/mouse/scheduling
  system-wide on every process wake-up or IPC message — likely the
  single biggest contributor to perceived input lag.
- **QEMU acceleration**: `qemu-disk`/`qemu-gui` now pass
  `-accel kvm:tcg`, using KVM hardware acceleration when the host
  supports it (see Troubleshooting if `/dev/kvm` isn't available).

## Native Language-Porting Foundation + Lua 5.5.0

Exploidus now has a real, working **Lua 5.5.0** port (`userspace/lua/`,
run with `lua` from the shell) — the first third-party language
runtime running on the OS. Built entirely on native Exploidus
syscalls, not a Linux compatibility layer (see the syscall ABI
discussion in project history for why). Getting there required a
real libc foundation that mostly didn't exist before:

- `setjmp`/`longjmp` (x86-64 asm), a real `<math.h>` (bit-trick +
  Taylor-series based, no hardware libm dependency), `<errno.h>`,
  `<ctype.h>`, `<assert.h>`, `<locale.h>`, real `<time.h>` (backed by
  a new CMOS RTC driver, `kernel/drivers/rtc.c` — Exploidus had no
  real calendar-time source before, only ticks-since-boot)
- **FPU/SSE support**: userspace couldn't return a `double` at all
  before (`-mno-sse` was set everywhere) — `kernel/arch/x86_64/fpu.c`
  enables SSE and gives every process its own FXSAVE/FXRSTOR state,
  saved/restored on every context switch
- **Native `SYS_SET_TLS`** and **`SYS_FUTEX_WAIT`/`WAKE`** (intra-
  process scope) for anything needing thread-local storage or a
  wait/wake primitive to build synchronization on top of
- A real PMM bug this surfaced: `parse_memory_map()` used a hardcoded
  2MB "safe to allocate" cutoff that didn't account for the kernel's
  own `.bss` (including its 4MB static heap array) extending well
  past that — small test binaries never needed enough pages to hit
  it, but Lua's ~60-page binary did, corrupting live kernel memory.
  Fixed via a real `_kernel_end` linker symbol.
- A second, general bug the Lua port surfaced: **any function pointer
  in an ASLR-relocated binary holds its link-time address, not the
  real runtime one** (the loader shifts binaries without processing
  relocations) — see the dedicated section below.
- **Real signal delivery**: `signal()` now actually works — a
  hardware fault (SIGSEGV/SIGFPE/SIGILL) in a process with a
  registered handler redirects execution there instead of always
  killing the process. One-shot, and the handler must call `exit()`
  itself (no sigreturn/resume support yet).
- **Terminal echo + backspace-editable input**: `kernel_read()` had no
  cooked-mode line editing at all — invisible while typing, and a
  backspace became a literal embedded byte instead of deleting a
  character. Fine for exploish (which does its own full editor) but
  broke any ordinary program reading stdin normally (Lua's REPL,
  `yolish`). New `SYS_TTY_SET_RAW` lets exploish opt out; everyone
  else gets real echo/backspace by default now.

## VFS / Filesystem

- **Permission enforcement**: file mode bits were stored at creation
  but never checked anywhere — any process could read/write any file
  regardless of permissions. `vfs_open()` now actually enforces
  owner read/write bits, and a new `chmod()` syscall lets you set
  them.
- **`rmdir()`**: was entirely missing (only `mkdir` existed). Refuses
  non-empty directories.
- **Write-through block cache**: ExFS did a full 8-sector PIO ATA read
  for every single block access, even ones read moments earlier
  (directory traversal, inode lookups). New 256 KiB write-through
  cache in `exfs_volume_t`.
- Fixed `fopen(path, "w")` never actually creating a new file — it
  never passed `O_CREAT`, so it silently failed on any path that
  didn't already exist. Not specific to any one program; anything
  writing a new file via `fopen()` hit this.

## ExFS: crash consistency, extended addressing, atomic rename

A focused pass closed the biggest gaps in ExFS: `write()` could never
address past 12 direct blocks (a hard 48 KiB file-size cap), there was
no crash recovery for metadata operations, no `rename()` existed, and
an on-disk integrity field had been declared but never populated. Full
design rationale for each item lives in
[`CHANGELOG-exfs-completion.md`](CHANGELOG-exfs-completion.md); this
is the summary.

- **Extended block addressing**: `write()` now allocates through
  single- and double-indirect pointer blocks, not just the 12 direct
  ones — max file size went from 48 KiB to ~1.03 GiB.
  `triple_indirect` exists in the on-disk inode but stays
  unimplemented (documented, not silently broken — nothing in this
  project needs a file anywhere near that size yet).
- **Crash-consistent metadata journal**: `create`/`unlink`/`rmdir`/
  `rename`/`write`'s metadata writes (inodes, dirents, indirect
  pointer blocks) are staged into a single-transaction write-ahead
  journal and applied atomically on commit; a committed-but-not-
  applied transaction is replayed on the next mount after an unclean
  shutdown. File *data* blocks and the block bitmap are deliberately
  **not** journaled (ordered mode, same as ext3's default) — the
  worst case on a crash there is a leaked block, never directory-tree
  corruption. Backward compatible: the journal region is carved out
  of previously-unused superblock bytes, so a volume formatted before
  this change simply runs without journaling instead of failing to
  mount.
- **Verified with deterministic crash injection, not just code
  review**: a `crashtest <1|2|3>` shell command (backed by
  `SYS_DEBUG_EXFS_CRASH`, test-only) halts the VM at each of the three
  points in a journal commit that matter for its correctness argument
  — before the commit point, right after it (before the real blocks
  are applied), and right after the real blocks are applied (before
  the journal is cleared). All three reboot to the expected state: no
  replay + operation absent, replay + operation present, and a safe
  (idempotent) replay respectively.
- **`rename()`**: new `exfs_op_rename()` + `vfs_rename()` +
  `SYS_RENAME` syscall + libc `rename()`. Same-directory and
  cross-directory moves, POSIX file-overwrite semantics, refuses
  overwriting a directory. Pure directory-entry move — no file data is
  ever copied.
- **`block_hash` (BLAKE3) now actually gets written**: this on-disk
  inode field existed since early ExFS but nothing ever populated it.
  `write()` now stores a BLAKE3 digest of the bytes it just wrote.
- **Fixed: `unlink()` leaked `double_indirect` blocks** — it freed
  `direct[]` and `indirect` but never touched `double_indirect`
  (previously unreachable anyway, since write couldn't allocate that
  far).
- **Fixed: duplicate directory entries** — `exfs_op_create()` had no
  check for an existing name, so two callers creating the same path
  (e.g. a caller falling back to create() after an unrelated failed
  `open()`) could silently leave two dirents with the same name
  pointing at two different inodes.
- **Fixed: a same-block journal write could silently discard an
  earlier one in the same transaction** — found via live testing, not
  code review. Two different examples of the same root cause: (1)
  `rename()` staging the source-removal and destination-insertion
  edits as two separate writes when they landed in the same physical
  block (always true for a same-directory rename) — the journal's
  same-block dedup ("staged again → overwrite") kept only the second
  edit. (2) `exfs_write_inode()` always read fresh from disk rather
  than checking the transaction's own staged-but-uncommitted content
  first — since 16 inodes share one on-disk block
  (`EXFS_INODE_SIZE=256`, `EXFS_BLOCK_SIZE=4096`), creating the first
  file in a freshly made subdirectory (which writes both the new
  file's inode *and* the parent directory's updated inode in one
  transaction) could have the second write silently erase the first,
  leaving the new file's own inode blank (`mode=0`) and every
  subsequent `open()` on it failing permission checks. Fixed
  generally with `exfs_txn_read_block()` — any read-modify-write
  inside an active transaction now sees its own transaction's staged
  changes first, not just what's already on disk.
- Shell commands `cp`, `mv`, `cmp`, `tail`, `find`, `sed`, `chmod`,
  `env`, `tee` had working implementations in `exploish_cmds.c` that
  were never wired into the shell's command dispatcher — invisible,
  unreachable dead code. Wired up, plus a `mv <src> <dir>/` trailing-
  slash convenience (keeps the source's own name). `rm` was a stub
  that printed "not yet implemented" without ever calling `unlink()`
  — now does.
- **Directory growth past the 12-block cap**: directories now walk
  through indirect/double-indirect blocks the same way files do —
  verified with 250 files in one directory (well past the old
  ~180-entry limit), all individually resolvable.
- **`tools/fsck.py`**: an offline consistency checker/repairer (same
  host-side-tool pattern as `tools/mkexfs.py`) — cross-checks every
  inode's blocks against the bitmap (leaked / corrupted / cross-
  linked blocks), walks the directory tree for dangling dirents and
  orphaned inodes. `--fix` only clears leaked-block bitmap bits, the
  one unambiguously safe repair; everything else is reported for a
  human to look at. Validated against five hand-built corruption
  cases (see `CHANGELOG-exfs-completion.md`).

## Networking

- Fixed busy-spin polling (no yield, up to 500 back-to-back retries)
  and a silent stack-buffer-overflow risk in `http_get()`/
  `http_download()`.
- **UDP sockets could never receive any data, ever**: the
  `net_socket(SOCK_UDP)` API's receive ring buffer existed but nothing
  in the whole codebase ever wrote to it — went unnoticed because DNS
  resolution bypasses the socket API via its own direct callback.
  Bridged the two (`socket_udp_recv_cb` in `kernel/net/socket/socket.c`).
- **No loopback interface existed**: `ip4_output()` had no special
  case for `127.0.0.1` (or the interface's own IP) — such packets fell
  through to real ARP+Ethernet+NIC transmission, which can never
  succeed, so they were silently dropped. This broke all localhost
  communication even though the *receiving* side already had
  loopback-accept logic. Now delivers straight to `ip4_input()`
  instead of transmitting, like a real OS's `lo` interface.
- `net_connect()`/`net_recv()`'s TCP "timeout" was a loop-iteration
  count, not real elapsed time (so it didn't reliably mean what it
  claimed to). Converted to a real wall-clock deadline.

## Process / Scheduling

- Per-process CPU time accounting (`ticks_used`) was declared and
  exposed via `ps`'s TICKS column but never actually incremented
  anywhere — `ps` always showed 0. Now wired up in `sched_tick()`,
  plus a CPU% column. Honest caveat: a process idly halted waiting
  for keyboard input (`kernel_read()`'s wait loop) still counts as
  "current" during that halt, so CPU% for an interactive shell reads
  higher than its *actual* work would suggest — this is a real
  scheduler-level idle-vs-running distinction Exploidus doesn't make
  yet, not just a display bug.

## Resolved: zombie reaping leaked every process's address space

Found while using the new `free` command (below) to sanity-check
`SYS_EXECVE` for leaks: running one `exectest` (spawn + 4 execve
hops + exit + shell's `waitpid()`) left `used_kb` permanently ~320KB
higher than before, every single time.

Traced it to `sys_waitpid_impl()` (`kernel/proc/fork_exec.c`) — reaping
a zombie just set its process-table slot to `PROC_UNUSED` and cleared
`pid`. `proc_exit()` (`kernel/proc/process.c`) releases IPC state, FPU
state, and file descriptors, but neither it nor the reap path ever
called `free_user_address_space()` on the process's own `cr3`. This
had nothing to do with `SYS_EXECVE` specifically — `SYS_EXECVE`
already frees each *intermediate* image correctly (that's the whole
point of the fix above) — it's that **no code path anywhere freed a
process's *final* address space** once it exited. Every process that
ever ran and got `waitpid()`'d on leaked its last image's memory,
permanently, forever — this bug predates this session's changes
entirely and would affect plain `spawn()`+`waitpid()` with zero
`execve()` calls involved.

Fix: `sys_waitpid_impl()` now frees `child->cr3` right before marking
the slot `UNUSED`, under the kernel's own PML4 (same
identity-map reasoning as `SYS_EXECVE`'s teardown — the *waiting
parent's* PML4 is what's live at that point, not the child's, and
`free_user_address_space()` needs the full map), switching back to
the caller's own `cr3` before returning since the syscall return path
goes to the caller's user-mode code.

**Not fixed by this**: a process that exits with no parent ever
calling `waitpid()` on it (an orphaned background daemon, or a parent
that dies first) still leaks — nothing currently reaps zombies except
an explicit `waitpid()` call. That's a separate gap (no init-style
re-parenting / no periodic zombie sweep) from the one fixed here.

## sshd — protocol version exchange (no encryption yet, on purpose)

New `userspace/bin/sshd.c`, listening on port 22, auto-started by
`init` alongside `auditd`/`httpd`. Implements RFC 4253 §4.2's
identification-string exchange: sends `SSH-2.0-Exploidus_0.1\r\n`,
reads the connecting client's own identification line, validates it
starts with `SSH-` and carries a recognized protoversion (`2.0` or
the legacy `1.99` compat marker), and logs the client's reported
software version. This is genuine SSH protocol behavior, not a
simulation of one — the identification exchange is specifically the
one part of the real protocol defined to happen in plaintext, before
any key exchange.

**Deliberately stops there.** A real SSH server continues into
`SSH_MSG_KEXINIT` / Diffie-Hellman (or Curve25519) key exchange, then
a symmetric cipher (AES/ChaCha20), a MAC (HMAC), and host-key
signatures (Ed25519/RSA) for the rest of the session. None of those
primitives exist anywhere in this codebase — `kernel/crypto/` is
BLAKE3 (a hash) only. Hand-writing cipher/KEX/signature code from
scratch inline in protocol code, with no independent review and no
verification against known test vectors, is exactly how a "secure
shell" ends up not actually being secure — so `sshd` completes the
identification exchange, logs what it learned, and closes the
connection with an honest message instead of accepting cleartext
input a real SSH client would never send unencrypted after this
point.

Next real step (separate, not started): port a small, well-reviewed
reference crypto implementation (Curve25519 + ChaCha20-Poly1305 is
the modern SSH baseline) and verify each primitive against its own
published test vectors *before* wiring any of it into protocol code.

## Resolved: idle shell prompt starved every background daemon

Found chasing the SSH hang above: `curl` against `httpd` (port 8080)
came back with nothing either — so this had nothing to do with
`sshd`'s own logic, it was systemic. Traced to `kernel_read()`
(`kernel/syscall/table.c`, backs `SYS_READ` on fd 0): when no key was
waiting, both its raw-mode and cooked-mode blocking loops did a bare

```
__asm__ volatile ("sti; hlt; cli" ::: "memory");
```

and looped back to check the keyboard again — **never calling
`sched_yield()`**. `hlt` returns control to the same instruction
stream on the next interrupt; it does not consult the scheduler.  So
a process blocked reading stdin (the shell, sitting idle at its
prompt) stayed `g_current_proc` continuously, silently re-grabbing
execution after every timer/keyboard/network IRQ returned.
`sched_next()` never got called on its behalf, so no other `READY`
process — `sshd`/`httpd` sitting in their own `accept()` loop, which
*does* call `sched_yield()` — ever got a turn: with nothing to switch
away *from* (the shell), their own yields had nowhere to hand control
to. Net effect: a connection could sit fully `ESTABLISHED` with data
waiting, and the daemon that owned it would simply never get
scheduled to notice, for as long as a human was sitting idle at the
shell prompt — which in an interactive session is effectively always.

Fixed by replacing both `hlt` spins in `kernel_read()` with
`sched_yield()` (which still halts when there's truly nothing else
`READY` — see its own comment in `kernel/proc/scheduler.c` — so idle
CPU usage doesn't regress). Also fixed the identical pattern in
`sys_http_get`/`sys_http_download`'s own receive loops
(`kernel/syscall/table.c`), which had copied the same bare-`hlt` idiom
for the same reason and would have starved everything else for the
duration of any `wget`-style download.

## Resolved: `netbuf` pool race between IRQ-context `net_poll()` and process-context sends

Found right after the scheduler-starvation fix above: `curl` against
`httpd` crashed the kernel with a General Protection Fault in
`tcp_send_segment()`, writing `hdr->src_port` right after
`hdr = netbuf_push(buf, TCP_HDR_LEN)` — `hdr` was non-`NULL` (so the
existing null-check didn't catch it) but pointed at invalid/reused
memory.

`net_poll()` (`kernel/net/netstack.c`) runs from inside the 100Hz
timer IRQ handler itself (`sched_tick()` → `net_poll()`,
`kernel/proc/scheduler.c`), meaning it — and anything it calls into,
including `tcp_send_segment()` for ACKs/retransmits — can interrupt
*any* kernel code, including another in-progress
`tcp_send_segment()` call mid-flight in process/syscall context.
`netbuf_alloc()`'s free-slot scan was a classic check-then-set race
(`if (!g_pool_used[i]) { g_pool_used[i] = true; ...`) with no
protection: if a timer tick landed between the check and the mark,
and its own call chain needed a netbuf too, both the interrupted call
and the nested IRQ-context call could claim the *same* "free" slot
before either marked it used — handing one physical `netbuf_t` to two
live callers at once. One side's `memset()` stomped the other's
in-progress header writes, and whichever side finished first freed
the buffer out from under the side still using it.

This had no way to manifest before the scheduler-starvation fix
above: with only one process ever really getting CPU time, there was
effectively only ever one call path active in this pool at once. Two
daemons genuinely running concurrently for the first time is what
finally hit the window — crashed on `httpd`'s first response send
(port 80), never on `sshd` (port 22, whose whole connection had
already completed and closed by the time `httpd` was hit) —
consistent with a timing race, not a per-port bug.

**First attempt at this fix was itself wrong** and worth recording:
a bare `cli`/`sti` pair around the scan+mark stopped the crash but
produced a *hang* instead. Reason: `netbuf_alloc()` can be reached
from `net_poll()` while *already inside* the timer IRQ handler, where
IF is already 0 (the IDT interrupt gate cleared it on entry) — an
unconditional `sti` there re-enables interrupts prematurely, mid-ISR,
letting another IRQ nest on top of it. The actual fix uses
`spin_lock_irqsave()`/`spin_unlock_irqrestore()`
(`kernel/sync/sync.c`), which already existed in this codebase for
exactly this — they save the real `RFLAGS.IF` on entry and only
restore that same value afterward, so the same call is safe from both
process context and IRQ context.

**Known related risk, not fixed here**: `tcp_conn_alloc()`
(`kernel/net/tcp/tcp.c`) scans `g_conns[]` with the identical
check-then-set pattern, also reachable from both `net_poll()`
(IRQ context) and process/syscall context — likely the same bug
class, unaddressed.

## Resolved: `httpd` overflowed a `netbuf`'s own header fields on its first real response

The two `netbuf`-pool-race fixes above (both attempts) were real bugs
worth having fixed, but **neither was the actual cause** of the
crash they were chasing — that took a third pass with direct
instrumentation (`serial_print`ing every `netbuf_alloc()`/
`netbuf_push()`/`tcp_send_segment()` call) to actually catch red-
handed: `buf->data`, read back on the *next* header-prepend call,
decoded as literal ASCII — `a href='`, a fragment straight out of
`httpd`'s own dashboard HTML.

The real bug: a `netbuf_t`'s usable payload space, from `buf->data`
(which starts at `_storage + NETBUF_HEADROOM`) to the end of
`_storage`, is `NETBUF_CAP - NETBUF_HEADROOM = 1536 - 256 = 1280`
bytes (`kernel/net/net.h`) — and nothing in `tcp_send_segment()`
(`kernel/net/tcp/tcp.c`) ever checked a payload against that before
`memcpy`-ing it into `buf->data`. `httpd.c`'s response-sending loop
chunked at up to **1400 bytes** per `xsend()` call — 120 bytes past
the buffer's real capacity — so the `memcpy` ran straight off the end
of `_storage[]` and into `netbuf_t`'s own trailing fields (`data`,
`len`, `next`, which sit immediately after `_storage` in the struct),
overwriting `data` itself with raw response bytes. The very next
`netbuf_push()` call on that same buffer (prepending the TCP header)
then dereferenced that corrupted `data` pointer, producing the
General Protection Fault. `sshd` never came close to triggering this
— its entire identification line is ~23 bytes.

Fixed in two places: `tcp_send_segment()` now refuses (`serial_print`s
why and returns `false`) any payload larger than the buffer can
actually hold, protecting every current and future caller of that
function, not just this one. `httpd.c`'s chunk size was also lowered
to 1200 (with margin) as defense in depth, so a refusal here shows up
as a marginally slower response rather than depending on the kernel-
side check alone.

The two earlier `netbuf`-pool-race fixes are left in place — the
check-then-set race in `netbuf_alloc()`'s free-slot scan against
IRQ-context `net_poll()` is real and worth having closed with
`spin_lock_irqsave()`/`spin_unlock_irqrestore()`, it just wasn't
*this* crash.

## `free` shell command now backed by real numbers (`SYS_MEMINFO`)

Was a hardcoded placeholder (`cmd_ext_free()` in
`userspace/shell/exploish_cmds.c` printed a fixed `262144 / -- / --`
with no syscall behind it at all). New `SYS_MEMINFO` syscall fills a
`meminfo_t` (`kernel/mm/pmm.h`) with real total/used/free physical
memory, computed from `pmm_total_pages()` (new accessor for the
previously-private `g_frame_count`) and `pmm_free_pages()` summed
across all three zones. Kernel-heap (`kmalloc`) usage was deliberately
left out — `kmalloc_total_used()` only exists under a `DEBUG_HEAP`
build flag this tree doesn't set by default, and turning that on
kernel-wide as a side effect of a `free` command fix felt like a
separate decision.

## Full execve (SYS_EXECVE)

The original `exec()` syscall (`SYS_EXEC`) took an already-in-memory
ELF buffer, ignored any caller-supplied argv/envp (always ran with a
default `argv[0] = "exploidus"`), and — while auditing it for this —
turned out to leak the old address space entirely: it swapped
`cr3` to the new image and never freed the pages the old one was
using. `spawn()`/`SYS_EXECV` covered argv passing, but only by
starting a brand-new child process, which isn't real `exec()`
semantics (the caller keeps running under its own PID).

New `SYS_EXECVE(path, argv, envp)` (`sys_execve_impl()` in
`kernel/proc/fork_exec.c`) is real POSIX-style exec:

- Loads the new image from a **path** (through the VFS), not a
  pre-loaded buffer.
- Passes caller-supplied **argv/envp** through to `elf_load()`.
- Replaces the **calling process's own** address space in place —
  same PID, not a new child.
- **Frees the old address space** before switching over, so a
  process that calls `execve()` repeatedly no longer leaks memory
  every time.
- Resets signal handlers and any in-flight signal-resume state to
  defaults, matching POSIX `execve()` — the old handler table points
  at addresses in memory that no longer exists.

**A real bug this surfaced**: `elf_load()` writes segment data and
the new stack into freshly allocated `ZONE_RED` physical pages via a
raw kernel pointer cast (`memset((void*)phys, ...)`) with no virtual
translation — safe only with a full identity-mapped view of physical
memory active. A process's own restricted PML4
(`make_isolated_pml4()`) only identity-maps the low 32MB plus its own
already-mapped pages, so this would fault the instant a `ZONE_RED`
page landed above that — the exact same class of bug `fork()`'s
page-table clone already had to work around (see the `sys_fork_impl`
comment above). `SYS_EXECVE` runs the whole load, and the old-image
teardown, under the kernel's own PML4, switching to the new image's
PML4 only at the very last moment inside `jump_to_userspace()`. The
original `SYS_EXEC` path calls `elf_load()` without this guard and
most likely has the same latent fault under memory pressure; not
touched in this pass since fixing it means changing `SYS_EXEC`'s
existing (already-in-memory-buffer) calling convention, not just
adding a new syscall.

Userspace: `execve(path, argv, envp)` in `userspace/libc/syscall.h`.
`argv`/`envp` may be `NULL`; the kernel falls back to `argv = [path]`
when `argv` is `NULL`, matching `SYS_EXECV`'s existing convention.

## Resolved: function pointers now survive ASLR (real relocation processing + static-PIE)

The loader used to give ET_EXEC binaries ASLR (random load base) by
shifting the whole image without processing ELF relocations — RIP-
relative code self-corrected under a pure shift, but an explicit
function pointer **value** (a signal handler passed to `signal()`, a
static callback table, anything stored as data rather than computed
at the call site) kept its **link-time** (base-0) address instead of
the real runtime one, jumping to the wrong place if called. The old
workaround was linking anything that took a function pointer against
a fixed, non-ASLR'd base (`fixed.ld`) instead.

This is now fixed properly:

- `kernel/elf/elf.c`'s `apply_relocations()` correctly processes
  `R_X86_64_RELATIVE` entries at load time.
- **Every userspace binary migrated from ET_EXEC to static-PIE**
  (`pie.ld` + `PIE_LDFLAGS` in the Makefile) — `shell`, `rahu`, `lua`,
  `compositor`, and everything else now actually exercises the
  relocation path instead of opting out via `fixed.ld`. The old
  fixed-base `.ld` scripts are left in-tree for reference but are no
  longer used.
- ASLR entropy widened (7→8 bits of real 2MB-granularity randomness)
  and the **stack** is now randomized too (previously fixed at
  `USER_STACK_TOP`) — both within the existing safe pd0 user address
  range, no page-table changes needed.
- A real bug this surfaced and fixed: `syscall1()`/`syscall2()`
  (`userspace/libc/syscall.h`) left `rsi`/`rdx` unset for syscalls
  that only logically need one argument (e.g. `spawn(path)`). Under
  the old fixed-address build this happened to be harmless; under PIE
  (different code addresses → different leftover register values)
  `spawn("/bin/lua")`'s stale `rdx` sometimes looked like a valid
  pointer, and `sys_spawn()` reads `rdx` unconditionally to detect an
  optional args string — so garbage leaked into the child's `argv[1]`
  and corrupted Lua's `collectargs()`/`handle_script()`, crashing on
  a bad `strcmp()` pointer. Fixed by explicitly zeroing the unused
  registers in the syscall wrappers.

Remaining honest limitation: 8 bits of ASLR entropy is still weak
compared to a desktop OS (Linux gives 28+) — going further needs
`make_isolated_pml4()` to clear more than one PD table's worth of
user address space, a bigger architectural change not done yet.

## USB stack (UHCI)

`kernel/usb/uhci.c` — PCI class-code detection (not vendor-specific,
finds any UHCI controller), controller reset, and a real transfer
layer built from Transfer Descriptors / Queue Heads:

- **Control transfers**: full device enumeration — partial (8-byte)
  device descriptor, `SET_ADDRESS`, full 18-byte descriptor via
  multi-packet chaining, configuration descriptor parsing (interface
  class/endpoints), `SET_CONFIGURATION`.
- **Interrupt transfers**: HID report reads (verified against a QEMU
  `usb-tablet`), correctly distinguishing "NAK, nothing new to report"
  from a real transfer error.
- **Bulk transfers + USB Mass Storage (Bulk-Only Transport)**: CBW/CSW
  command wrapping, SCSI `INQUIRY`, `READ CAPACITY (10)`, `READ (10)`,
  `WRITE (10)` — verified end-to-end against QEMU's `usb-storage`
  (vendor/product strings decoded correctly, capacity matched the
  test image's real size exactly, and a `WRITE(10)` + `READ(10)`
  round-trip byte-matched).
- Registers a detected mass-storage device as block device `usb0`
  (see the block-device section above) — `mount usb0 /media/usb0`
  from the shell mounts its ExFS volume into the VFS.

Honest limitations: single-device only (hardcoded to USB address 1 —
a second simultaneously-connected device would collide), no
interrupt-endpoint periodic background polling (HID reads are
one-shot, not scheduled), no multi-packet reads/writes beyond what
fits in one TD chain (~512 bytes at typical bulk packet sizes), and
no `WRITE`-based filesystem operations tested yet (only raw sector
read/write, and the `mount` path so far).

To test the mass-storage path in QEMU: `make qemu-usb-storage-test`
(separate from `qemu-disk`, which attaches a `usb-tablet` for
mouse/GUI testing instead).

## Kernel hardening

- **`kmalloc()` integer-overflow guard**: a request size near
  `UINT64_MAX` used to wrap `(size + 15) & ~15` around to a tiny
  value, silently handing back a 16-byte buffer while the caller
  believed it got the huge size it asked for — a heap-overflow
  primitive. Now rejected before any arithmetic on the size happens.
  Verified with a host-compiled standalone stress-test harness
  (`tests/test_kmalloc.c`) covering OOM exhaustion + recovery,
  double-free safety, coalescing, and a documented (not yet fixed)
  fragmentation limitation: first-fit + adjacent-only coalescing can
  fail a request even when total free memory is sufficient, if free
  blocks are scattered between still-used ones.
- **IRQ-driven ATA driver**: `ata_wait_not_busy()`/`ata_wait_drq()`
  now use `hlt` instead of busy-spinning when interrupts are enabled,
  falling back to the original busy-poll automatically when they
  aren't (early boot, before `sti` — `exfs_mount()` for the root
  filesystem runs in exactly that window, so an IRQ-only wait would
  hang boot).