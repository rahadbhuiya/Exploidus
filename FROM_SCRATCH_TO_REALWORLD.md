# Exploidus: From-Scratch to Real-World GUI OS (Kali / Parrot Style)
> **Master Engineering Blueprint & Execution Roadmap**  
> *Target: From bare-metal / QEMU to bootable Live ISO with hardware drivers, modern memory management, POSIX layer, custom GUI desktop, and native cybersecurity tools.*

---

## 🧭 ১. আর্কিটেকচার টার্গেট ও লক্ষ্য (Vision)

Exploidus কোনো Linux ডিস্ট্রিবিউশন নয় (যেমনটা Kali বা Parrot — যারা মূলত Debian লিনাক্স কার্নেলের ওপর টুলস বসায়)। Exploidus হচ্ছে **একটি সম্পূর্ণ নিজস্ব x86_64 Reactive Capability Kernel**। 

বাস্তব হার্ডওয়্যারে (Real Laptop/PC) বুটযোগ্য এবং Kali/Parrot-এর মতো ডার্ক সাইবারসিকিউরিটি GUI সমৃদ্ধ ওএস তৈরি করতে পুরো আর্কিটেকচারটি এভাবে দাঁড় করাতে হবে:

```
┌──────────────────────────────────────────────────────────────────┐
│                   EXPLOIDUS USERSPACE (DESKTOP)                  │
│  ┌────────────────────┐ ┌────────────────────┐ ┌──────────────┐  │
│  │ ExWM Window Mgr    │ │ ExUI Toolkit       │ │ Terminal Em. │  │
│  └────────────────────┘ └────────────────────┘ └──────────────┘  │
│  ┌────────────────────┐ ┌────────────────────┐ ┌──────────────┐  │
│  │ CNSL Live Monitor  │ │ FIM Security Guard │ │ Port Scanner │  │
│  └────────────────────┘ └────────────────────┘ └──────────────┘  │
├──────────────────────────────────────────────────────────────────┤
│                  DISPLAY SERVER & COMPOSITOR                     │
│               `alien` v2.0 Compositor (Shm / GPU)               │
├──────────────────────────────────────────────────────────────────┤
│                     SYSTEM LIBC & POSIX ABI                      │
│             musl libc port + Exploidus Native Syscalls           │
├──────────────────────────────────────────────────────────────────┤
│                         EXPLOIDUS KERNEL                         │
│  ┌─────────────────┐ ┌──────────────────┐ ┌───────────────────┐  │
│  │ Intent Sched    │ │ BLAKE3 Security  │ │ Demand Paging VMM │  │
│  ├─────────────────┤ ├──────────────────┤ ├───────────────────┤  │
│  │ AHCI / NVMe     │ │ xHCI USB 3.0     │ │ e1000 / Realtek   │  │
│  ├─────────────────┤ ├──────────────────┤ ├───────────────────┤  │
│  │ ExFS (Journld)  │ │ TCP/IPv4/IPv6    │ │ GOP Framebuffer   │  │
│  └─────────────────┘ └──────────────────┘ └───────────────────┘  │
├──────────────────────────────────────────────────────────────────┤
│                     HARDWARE / BARE METAL                        │
│             UEFI x86_64, APIC, SATA SSD/NVMe, GPU/Display        │
└──────────────────────────────────────────────────────────────────┘
```

---

## 📋 ফেজ-বাই-ফেজ বাস্তবায়ন চেকলিস্ট

---

### ফেজ ১: আধুনিক বুট ও হার্ডওয়্যার ইনিশিয়ালাইজেশন (Boot & Base HW)
*বর্তমান অবস্থা: শুধু Legacy BIOS (Multiboot2)। আধুনিক ল্যাপটপে Legacy BIOS সাপোর্ট বন্ধ। তাই UEFI আবশ্যক।*

- [ ] **1.1 UEFI Bootloader সাপোর্ট (GRUB-EFI / Limine / Custom EFI Stub)**
  - ফাইল: `kernel/boot/efi_main.c` অথবা GRUB EFI config
  - কাজ: UEFI Graphics Output Protocol (GOP) থেকে লিনিয়ার ফ্রেমবাফারের ঠিকানা ও রেজোলিউশন (1920x1080x32bpp) কার্নেলে পাস করা।
  - টেস্ট: আধুনিক ল্যাপটপের UEFI বুট মেনু থেকে কার্নেল লোড হওয়া।
- [ ] **1.2 ACPI টেবিল পার্সিং (RSDP, XSDT, MADT)**
  - ফাইল: `kernel/arch/x86_64/acpi.c`, `kernel/arch/x86_64/acpi.h`
  - কাজ: হার্ডওয়্যারের কোয়াড-কোর/মাল্টি-কোর সিপিইউ ও IRQ রাউটিং খুঁজে বের করা।
- [ ] **1.3 Local APIC & I/O APIC ড্রাইভ (Legacy PIC রিটায়ার করা)**
  - ফাইল: `kernel/arch/x86_64/apic.c`, `kernel/arch/x86_64/ioapic.c`
  - কাজ: মাল্টি-কোর প্রসেসর ও আধুনিক হার্ডওয়্যার ইন্টারাপ্ট হ্যান্ডলিং।
- [ ] **1.4 SMP (Symmetric Multiprocessing) / Multi-Core সাপোর্ট**
  - ফাইল: `kernel/arch/x86_64/smp.c`
  - কাজ: সেকেন্ডারি সিপিইউ কোরগুলোকে ওয়েক-আপ (SIPI/INIT IPI) করানো এবং লোড ব্যালেন্সিং করা।

---

### ফেজ ২: মেমোরি ম্যানেজমেন্ট ও কোর কার্নেল সংস্কার (VMM & Core)
*বর্তমান অবস্থা: 4MB স্ট্যাটিক হিপ ও বেসিক পেজিং। GUI অ্যাপস ও ব্রাউজার চালাতে কয়েকশো মেগাবাইট ডায়নামিক মেমোরি লাগে।*

- [ ] **2.1 Demand Paging ও Page Fault Handler**
  - ফাইল: `kernel/mm/vmm.c`, `kernel/arch/x86_64/idt.c` (ISR 14)
  - কাজ: প্রোগ্রাম মেমোরি চাইলে সাথে সাথে ফিজিক্যাল পেজ না দিয়ে অন-ডিমান্ড অ্যালোকেশন করা।
- [ ] **2.2 Slab / SLUB Allocator (kmalloc সংস্কার)**
  - ফাইল: `kernel/mm/slab.c`
  - কাজ: ফিক্সড সাইজের ক্যাশ পুল (32B, 64B, 128B, ... 4KB) তৈরি যাতে মেমোরি ফ্র্যাগমেন্টেশন না হয়।
- [ ] **2.3 File-backed `mmap()` ও Anonymous `mmap()`**
  - ফাইল: `kernel/syscall/mmap.c`, `kernel/fs/vfs/vfs.c`
  - কাজ: শেয়ার্ড লাইব্রেরি ও GUI ইমেজ বাফার ডাইরেক্ট মেমোরিতে ম্যাপ করা।
- [ ] **2.4 রিয়েল থ্রেডিং সাপোর্ট (POSIX `clone()` ও kernel thread stack)**
  - ফাইল: `kernel/proc/thread.c`, `kernel/proc/scheduler.c`
  - কাজ: একাধিক থ্রেড একই অ্যাড্রেস স্পেস (`CR3`) শেয়ার করবে। GUI ও ব্রাউজার রানটাইমের জন্য অত্যন্ত জরুরি।
- [ ] **2.5 পাইপ ও ফাইল ডেসক্রিপ্টর রিডাইরেকশন (`pipe()`, `dup2()`)**
  - ফাইল: `kernel/fs/pipe.c`, `kernel/syscall/table.c`
  - কাজ: শেল কমান্ড চেইনিং (`cat file | grep text > out.txt`) সাপোর্ট।
- [ ] **2.6 রিয়েল সিগন্যাল হ্যান্ডলিং (`sigaction`, `sigreturn`, POSIX signals)**
  - ফাইল: `kernel/arch/x86_64/signal.c`, `kernel/arch/x86_64/sigreturn.asm`
  - কাজ: প্রসেস ইন্টারাপ্ট, রিস্টোর স্ট্যাক ফ্রেম, `SIGINT` (Ctrl+C), `SIGCHLD` পরিচালনা।

---

### ফেজ ৩: স্টোরেজ ও ইনপুট ড্রাইভারস (Real World Hardware)
*বর্তমান অবস্থা: শুধু Legacy ATA (IDE) এবং UHCI (USB 1.1)। রিয়েল পিসিতে SATA SSD ও USB 3.0 থাকে।*

- [ ] **3.1 PCI / PCIe বাস এনিউমারেশন (Generic Scanner)**
  - ফাইল: `kernel/drivers/pci.c`, `kernel/drivers/pci.h`
  - কাজ: মাদারবোর্ডের সব কানেক্টেড কার্ড (GPU, NIC, NVMe, USB) স্ক্যান ও BAR অ্যাড্রেস পড়া।
- [ ] **3.2 AHCI / SATA কন্ট্রোলার ড্রাইভার (SSD / HDD)**
  - ফাইল: `kernel/drivers/ahci.c`, `kernel/drivers/ahci.h`
  - কাজ: আধুনিক SATA হার্ডড্রাইভ/এসএসডি থেকে এক্সএফএস (ExFS) পার্টিশন মাউন্ট করা।
- [ ] **3.3 xHCI (USB 3.0) ড্রাইভার**
  - ফাইল: `kernel/usb/xhci.c`
  - কাজ: রিয়েল পিসির ইউএসবি কিবোর্ড, ইউএসবি মাউস এবং পেনড্রাইভ (Mass Storage) ডিটেক্ট করা।
- [ ] **3.4 VESA / GOP লিনিয়ার ফ্রেমবাফার কনসোল ড্রাইভার**
  - ফাইল: `kernel/drivers/gop.c`, `kernel/drivers/fb.c`
  - কাজ: হাই-রেজোলিউশন (1080p) স্ক্রিনে 32-বিট কালার সরাসরি ড্র করার ক্ষমতা।

---

### ফেজ ৪: স্ট্যান্ডার্ড লাইব্রেরি ও পিওএসআইএক্স (musl libc Port)
*সবচেয়ে বড় টার্নিং পয়েন্ট: নিজস্ব ছোট libc-এর বদলে musl libc সাপোর্ট আনা।*

- [ ] **4.1 Exploidus Syscall ABI-কে Linux Syscall Table-এর সাথে অ্যালাইন করা**
  - ফাইল: `kernel/syscall/table.c`
  - কাজ: কমন লিনাক্স সাইসকল নম্বরগুলো ম্যাপ করা (যেমন `sys_read=0`, `sys_write=1`, `sys_open=2`, `sys_close=3`, `sys_stat=4`, `sys_mmap=9`, `sys_clone=56`, `sys_execve=59` ইত্যাদি)।
- [ ] **4.2 musl libc পোর্টিং ও কম্পাইলেশন**
  - ডিরেক্টরি: `userspace/libs/musl/`
  - কাজ: musl libc কম্পাইল করে Exploidus-এর কার্নেলের ওপর রান করানো।
- [ ] **4.3 ডায়নামিক লিঙ্কার (`ld-exploidus.so`)**
  - কাজ: `.so` (Shared Objects) ফাইল লোড ও ডায়নামিক রিলোকেশন সমর্থন।
- [ ] **4.4 PTY / TTY সিস্টেম (`/dev/ptmx`, `/dev/pts/*`)**
  - কাজ: GUI টার্মিনাল এমুলেটরগুলোর ব্যাকএন্ড হ্যান্ডলিং।

---

### ফেজ ৫: Kali / Parrot-স্টাইল GUI ও ডেস্কটপ আর্কিটেকচার (GUI Engine)

```
┌────────────────────────────────────────────────────────┐
│ [★ ExploidOS Top Panel: 14:30 | CPU: 12% | RAM: 1.2G]  │
├────────────────────────────────────────────────────────┤
│                                                        │
│   ┌─[ ExTerminal ]──────────────┐                      │
│   │ root@exploidus:~# nmap-ex   │                      │
│   │ [CNSL] Real-time guard active                      │
│   │ Scanning subnet 192.168.1.0 │                      │
│   │ > 192.168.1.1 [HTTP, SSH]   │                      │
│   │ > 192.168.1.45 [EXPLOID_NODE│                      │
│   └─────────────────────────────┘                      │
│                                                        │
│   ┌─[ CNSL Security Dashboard ]─┐                      │
│   │ Active Attacks Blocked: 0   │                      │
│   │ FIM Status: 13 paths clean  │                      │
│   └─────────────────────────────┘                      │
│                                                        │
├────────────────────────────────────────────────────────┤
│ [Applications] [Terminal] [ExFS Browser] [Tools] [Exit]│
└────────────────────────────────────────────────────────┘
```

- [ ] **5.1 `alien` v2.0 Compositor (Display Server)**
  - ফাইল: `userspace/compositor/alien.c`, `userspace/compositor/shm_surface.c`
  - কাজ: শেয়ার্ড মেমোরি বাফার (SHM) দিয়ে প্রতিটি উইন্ডোকে আলাদা অফ-স্ক্রিন ক্যানভাসে রেন্ডার এবং স্ক্রিনে ব্লেন্ডিং করা।
- [ ] **5.2 FreeType 2 ফন্ট ইঞ্জিন ইন্টিগ্রেশন**
  - ডিরেক্টরি: `userspace/libs/freetype/`
  - কাজ: ক্রিস্প ট্রুটাইপ (TTF) ফন্ট রেন্ডার করা (যেমন: Hack, JetBrains Mono, Fira Code)।
- [ ] **5.3 ExUI লাইটওয়েট উইজেট টুলকিট (C/Lua)**
  - ডিরেক্টরি: `userspace/gui/exui/`
  - উইজেটস: `Window`, `Button`, `TextBox`, `ListView`, `Canvas`, `Panel`, `Menu`
- [ ] **5.4 ExWM (Exploidus Window Manager)**
  - ফাইল: `userspace/gui/exwm/wm.c`
  - ফিচারস:
    - ডার্ক সায়ান/নিয়ন গ্রিন সাইবার হ্যাকার থিম (Kali/Parrot অ্যাসথেটিক)
    - উইন্ডো ড্র্যাগ, মিনিমাইজ, ম্যাক্সিমাইজ, টাইলিং স্ন্যাপ
    - টপ/বটম স্ট্যাটাস প্যানেল (ক্লক, রিসোর্স মনিটর, ব্যাটারি/পাওয়ার)
- [ ] **5.5 কোর ডেস্কটপ অ্যাপস**
  - **ExTerminal**: ANSI কালার, ট্যাব, কপি-পেস্ট সহ ফুল টার্মিনাল এমুলেটর
  - **ExFiles**: ফোল্ডার/ফাইল নেভিগেশন ও ExFS প্রোভেন্যান্স হিস্ট্রি ভিউয়ার
  - **ExEditor**: সিনট্যাক্স হাইলাইটিং সহ লাইটওয়েট টেক্সট কোড এডিটর

---

### ফেজ ৬: নেটিভ সাইবারসিকিউরিটি ও পেন্টেস্টিং টুলকিট (Kali-Style)

Exploidus-এর সবচেয়ে শক্তিশালী দিক এর নিজস্ব নিরাপত্তা আর্কিটেকচার। এই টুলগুলো নেটিভলি তৈরি করতে হবে:

- [ ] **6.1 পূর্ণাঙ্গ SSHD ডেমন (`/bin/sshd`)**
  - ফাইল: `userspace/bin/sshd.c`
  - কাজ: বিদ্যমান X25519 + ChaCha20-Poly1305 + Ed25519 ক্রিপ্টো স্ট্যাকের সাথে SSH Handshake স্টেট মেশিন যুক্ত করা।
- [ ] **6.2 CNSL লাইভ ডিফেন্স ড্যাশবোর্ড (GUI)**
  - ফাইল: `userspace/gui/apps/cnsl_monitor.c`
  - কাজ: কার্নেল লেভেল অ্যাটাক ডিটেকশন, অটো-ব্লকড আইপি গ্রাফ ও নোটিফিকেশন প্রদর্শন।
- [ ] **6.3 FIM (File Integrity Monitor) ভিউয়ার**
  - কাজ: এক্সএফএস (ExFS) ফাইলে অনাকাঙ্ক্ষিত পরিবর্তন হলে ইনস্ট্যান্ট ভিজ্যুয়াল অ্যালার্ট।
- [ ] **6.4 নেটিভ নেটওয়ার্ক টুলস (`ex-nmap`, `ex-sniffer`, `ex-netcat`)**
  - কাজ: Exploidus কার্নেল TCP/IP সকেট ব্যবহার করে পোর্ট স্ক্যানিং, প্যাকেট ইনস্পেকশন ও র সিগন্যাল ট্র্যাকিং।
- [ ] **6.5 রাহাদ প্যাকেজ ম্যানেজার (`rahu`) সম্পূর্ণ করা**
  - কাজ: রিমোট এক্সপ্লয়ডাস রিপোজিটরি থেকে বাইনারি প্যাকেজ ডাউনলোড, ডিপেনডেন্সি চেক এবং `/bin`-এ ইন্সটল।

---

### ফেজ ৭: রিয়েল-ওয়ার্ল্ড লাইভ বুটেবল ISO ও ইনস্টলার (Live ISO & Installer)

- [ ] **7.1 হাইব্রিড UEFI/BIOS বুটেবল ISO জেনারেটর**
  - ফাইল: `tools/mkiso.sh`, `iso/boot/grub/grub.cfg`
  - টুলস: `xorriso`, `grub-mkrescue`, `mtools`
  - ফিচার: সিডিরম এবং ইউএসবি পেনড্রাইভ (Rufus/Ventoy/dd) উভয় মাধ্যমে বুট সাপোর্ট।
- [ ] **7.2 লাইভ র‍্যাম বুট সাপোর্ট (Initramfs / Ramdisk)**
  - হার্ডডিস্ক স্পর্শ না করেই পেনড্রাইভ থেকে পুরো ওএস র‍্যামে লোড হয়ে সরাসরি ডেসকটপে ঢুকবে।
- [ ] **7.3 গ্রাফিক্যাল ড্রাইভ ইনস্টলার (`ex-installer`)**
  - হার্ডডিস্ক পার্টিশনিং (GPT)
  - ExFS ফাইলসিস্টেমে ফরম্যাট
  - রিয়েল ড্রাইভে সিস্টেম কপি ও GRUB বুটলোডার স্থাপন

---

## 🚀 আজকের জন্য তাৎক্ষণিক ৩টি পরবর্তী পদক্ষেপ (Next Immediate Steps)

যদি আজ থেকেই স্ক্র্যাচ থেকে রিয়েল-ওয়ার্ল্ডের যাত্রা শুরু করতে চাও, এই ক্রমানুসারে হাত দাও:

1. **UEFI GOP ফ্রেমবাফার কনসোল সেটআপ**:
   - `iso/boot/grub/grub.cfg`-এ লিনিয়ার ফ্রেমবাফার রেজোলিউশন কনফিগার করে কার্নেল এন্ট্রিতে GOP হ্যান্ডলার চেক করা।
2. **Dynamic Paging & Heap বৃদ্ধি**:
   - `kernel/mm/pmm.c` ও `kernel/mm/vmm.c`-তে ফিক্সড 4MB লিমিট সরিয়ে পুরো সিস্টেম র‍্যাম ব্যবহারযোগ্য করা।
3. **কিবোর্ড/মাউস ইন্টারাপ্ট ফিক্স**:
   - `alien` কম্পোজিটর যাতে লিনিয়ার ফ্রেমবাফারে মাউস ও কিবোর্ড দিয়ে স্মুথ 60 FPS উইন্ডো মুভ করতে পারে।
