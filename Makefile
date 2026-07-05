# Exploidus Kernel Build System
CC  := x86_64-elf-gcc
AS  := nasm
LD  := x86_64-elf-ld

#  FLAGS 
CFLAGS := -std=c11 -ffreestanding -fno-stack-protector \
          -fno-pic -fno-pie -mno-red-zone              \
          -mno-mmx -mno-sse -mno-sse2                  \
          -Wall -Wextra -Werror -Wno-unused-parameter  \
          -O2 -Ikernel/include -Ikernel

DEBUG_CFLAGS := -g -O2

ASFLAGS := -f elf64 -g

LDFLAGS := -T linker.ld -nostdlib \
           -z max-page-size=0x1000 --no-dynamic-linker

SHELL_LDFLAGS := -T userspace/shell/shell.ld -nostdlib \
                 -z max-page-size=0x1000 --no-dynamic-linker

# Shell-specific C flags (freestanding, no stdlib, debug info)
SHELL_CFLAGS := -std=c11 -ffreestanding -fno-stack-protector \
                -fPIC -mno-red-zone                            \
                -mno-mmx -mno-sse -mno-sse2                   \
                -Wall -Wextra -Werror -Wno-unused-parameter   \
                -D__EXPLOIDUS_USERSPACE__                      \
                -Iuserspace/libc -O2

USER_LDFLAGS := -nostdlib -z max-page-size=0x1000 --no-dynamic-linker

#  SOURCES 
KERNEL_C_SRCS := \
    kernel/main.c kernel/string.c \
    kernel/arch/x86_64/gdt.c kernel/arch/x86_64/idt.c \
    kernel/arch/x86_64/irq.c kernel/arch/x86_64/apic.c \
    kernel/mm/pmm.c kernel/mm/vmm.c kernel/mm/kmalloc.c \
    kernel/cap/capability.c kernel/cap/broker.c \
    kernel/crypto/blake3.c kernel/audit/audit.c \
    kernel/proc/process.c kernel/proc/scheduler.c \
    kernel/proc/fork_exec.c kernel/syscall/table.c \
    kernel/drivers/vga.c kernel/drivers/serial.c \
    kernel/drivers/keyboard.c kernel/drivers/fb.c kernel/drivers/font.c kernel/drivers/mouse.c kernel/drivers/fb_console.c kernel/drivers/ata.c kernel/drivers/driver.c \
    kernel/fs/vfs/vfs.c kernel/fs/exfs/exfs.c \
    kernel/elf/elf.c \
    kernel/net/net.c kernel/net/netstack.c \
    kernel/net/arp/arp.c kernel/net/ip/ip.c \
    kernel/net/icmp/icmp.c kernel/net/udp/udp.c \
    kernel/net/dns/dns.c \
    kernel/net/tcp/tcp.c kernel/net/socket/socket.c \
    kernel/net/drivers/e1000.c \
    kernel/cnsl/cnsl.c \
    kernel/cnsl/fim.c \
	kernel/huddlecluster/huddlecluster.c \
    kernel/ipc/ipc.c \
    kernel/shm/shm.c \
    kernel/sync/sync.c \

KERNEL_ASM_SRCS := \
    kernel/boot/start.asm \
    kernel/arch/x86_64/isr.asm \
    kernel/arch/x86_64/gdt_flush.asm \
    kernel/proc/context_switch.asm \
    kernel/proc/jump_userspace.asm \
    kernel/syscall/entry.asm

SHELL_C_SRCS   := userspace/shell/exploish.c userspace/shell/exploish_cmds.c
HELLO_C_SRCS   := userspace/bin/hello.c
AUDITD_C_SRCS  := userspace/bin/auditd.c
INIT_C_SRCS    := userspace/bin/init.c
HTTPD_C_SRCS   := userspace/bin/httpd.c
HTTPT_C_SRCS   := userspace/bin/httptest.c
RAHU_C_SRCS    := userspace/bin/rahu.c
COMP_C_SRCS    := userspace/compositor/compositor.c
GUI_DEMO_C_SRCS := userspace/bin/gui_demo.c
TERMINAL_C_SRCS := userspace/bin/terminal.c
SHELL_ASM_SRCS := userspace/libc/crt0.asm

LIBC_C_SRCS := \
    userspace/libc/stdio.c  \
    userspace/libc/string.c \
    userspace/libc/stdlib.c \
    userspace/libc/malloc.c

LIBC_OBJS := $(patsubst %.c, build/%.o, $(LIBC_C_SRCS))

#  OBJECTS 
KC_OBJS  := $(patsubst %.c,   build/%.o, $(KERNEL_C_SRCS))
KA_OBJS  := $(patsubst %.asm, build/%.o, $(KERNEL_ASM_SRCS))
SC_OBJS  := $(patsubst %.c,   build/%.o, $(SHELL_C_SRCS))
HC_OBJS  := $(patsubst %.c,   build/%.o, $(HELLO_C_SRCS))
AD_OBJS  := $(patsubst %.c,   build/%.o, $(AUDITD_C_SRCS))
IN_OBJS  := $(patsubst %.c,   build/%.o, $(INIT_C_SRCS))
HT_OBJS  := $(patsubst %.c,   build/%.o, $(HTTPD_C_SRCS))
HTT_OBJS := $(patsubst %.c,   build/%.o, $(HTTPT_C_SRCS))
RH_OBJS  := $(patsubst %.c,   build/%.o, $(RAHU_C_SRCS))
COMP_OBJS     := $(patsubst %.c, build/%.o, $(COMP_C_SRCS))
GUI_DEMO_OBJS := $(patsubst %.c, build/%.o, $(GUI_DEMO_C_SRCS))
TERMINAL_OBJS := $(patsubst %.c, build/%.o, $(TERMINAL_C_SRCS))
SA_OBJS  := $(patsubst %.asm, build/%.o, $(SHELL_ASM_SRCS))
# CRT + libc for bin/* programs
BIN_OBJS := $(SA_OBJS) $(LIBC_OBJS)

ALL_KOBJS := $(KC_OBJS) $(KA_OBJS)

#  PHONY 
.PHONY: all clean iso qemu qemu-vga debug qemu-iso qemu-disk qemu-run


# PRIMARY TARGETS


all: build/exploidus.elf build/userspace/shell/exploish.elf build/userspace/bin/hello.elf build/userspace/bin/auditd.elf build/userspace/bin/init.elf build/userspace/bin/httpd.elf build/userspace/bin/httptest.elf build/userspace/yolish/ys.elf build/userspace/bin/rahu.elf build/userspace/compositor/compositor.elf build/userspace/bin/gui_demo.elf build/userspace/bin/terminal.elf

build/userspace/bin/httptest.elf: $(BIN_OBJS) $(HTT_OBJS) userspace/bin/auditd.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  httptest -> $@"
	$(LD) -T userspace/bin/auditd.ld $(USER_LDFLAGS) -o $@ $(BIN_OBJS) $(HTT_OBJS)
	x86_64-elf-strip --strip-debug $@

build/userspace/bin/httpd.elf: $(BIN_OBJS) $(HT_OBJS) userspace/bin/auditd.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  httpd  -> $@"
	$(LD) -T userspace/bin/auditd.ld $(USER_LDFLAGS) -o $@ $(BIN_OBJS) $(HT_OBJS)
	x86_64-elf-strip --strip-debug $@

build/userspace/bin/init.elf: $(BIN_OBJS) $(IN_OBJS) userspace/bin/init.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  init   -> $@"
	$(LD) -T userspace/bin/init.ld $(USER_LDFLAGS) -o $@ $(BIN_OBJS) $(IN_OBJS)
	x86_64-elf-strip --strip-debug $@

build/userspace/bin/auditd.elf: $(BIN_OBJS) $(AD_OBJS) userspace/bin/auditd.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  auditd -> $@"
	$(LD) -T userspace/bin/auditd.ld $(USER_LDFLAGS) -o $@ $(BIN_OBJS) $(AD_OBJS)
	x86_64-elf-strip --strip-debug $@

#  KERNEL ELF
build/userspace/bin/hello.elf: $(BIN_OBJS) $(HC_OBJS) userspace/bin/hello.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  hello  -> $@"
	$(LD) -T userspace/bin/hello.ld $(USER_LDFLAGS) -o $@ $(BIN_OBJS) $(HC_OBJS)
	x86_64-elf-strip --strip-debug $@

build/userspace/bin/rahu.elf: $(BIN_OBJS) $(RH_OBJS) userspace/bin/auditd.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  rahu   -> $@"
	$(LD) -T userspace/bin/auditd.ld $(USER_LDFLAGS) -o $@ $(BIN_OBJS) $(RH_OBJS)
	x86_64-elf-strip --strip-debug $@

build/userspace/compositor/compositor.elf: $(BIN_OBJS) $(COMP_OBJS) userspace/compositor/compositor.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  compositor -> $@"
	$(LD) -T userspace/compositor/compositor.ld $(USER_LDFLAGS) -o $@ $(BIN_OBJS) $(COMP_OBJS)
	x86_64-elf-strip --strip-debug $@

build/userspace/bin/gui_demo.elf: $(BIN_OBJS) $(GUI_DEMO_OBJS) userspace/bin/auditd.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  gui_demo -> $@"
	$(LD) -T userspace/bin/auditd.ld $(USER_LDFLAGS) -o $@ $(BIN_OBJS) $(GUI_DEMO_OBJS)
	x86_64-elf-strip --strip-debug $@

build/userspace/bin/terminal.elf: $(BIN_OBJS) $(TERMINAL_OBJS) userspace/bin/auditd.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  terminal -> $@"
	$(LD) -T userspace/bin/auditd.ld $(USER_LDFLAGS) -o $@ $(BIN_OBJS) $(TERMINAL_OBJS)
	x86_64-elf-strip --strip-debug $@

build/exploidus.elf: $(ALL_KOBJS) build/shell_blob.o build/hello_blob.o build/init_blob.o linker.ld
	@echo "[LD]  kernel -> $@"
	$(LD) $(LDFLAGS) -o $@ $(ALL_KOBJS) build/shell_blob.o build/hello_blob.o build/init_blob.o

#  SHELL ELF 
build/userspace/shell/exploish.elf: $(SA_OBJS) $(SC_OBJS) userspace/shell/shell.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  shell  -> $@"
	$(LD) -T userspace/shell/shell.ld $(USER_LDFLAGS) -o $@ $(SA_OBJS) $(SC_OBJS)
	x86_64-elf-strip --strip-debug $@
	@echo "[STRIP] exploish done"

#  SHELL BLOB 
build/shell_blob.o: build/userspace/shell/exploish.elf build/userspace/bin/rahu.elf
	@echo "[BIN] embedding shell blob"
	x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@


# COMPILE RULES


# Userspace libc objects — use SHELL_CFLAGS
build/userspace/libc/%.o: userspace/libc/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  libc: $<"
	$(CC) $(SHELL_CFLAGS) -D__EXPLOIDUS_LIBC__ -c $< -o $@

# Shell C objects — use SHELL_CFLAGS (debug, libc include)
build/userspace/shell/%.o: userspace/shell/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  shell: $<"
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

# Userspace bin objects — use SHELL_CFLAGS (fPIC, libc include)
build/userspace/bin/%.o: userspace/bin/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  bin: $<"
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

# Compositor objects
build/userspace/compositor/%.o: userspace/compositor/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  compositor: $<"
	$(CC) $(SHELL_CFLAGS) -Iuserspace/compositor -c $< -o $@

# Kernel C objects
build/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC]  $<"
	$(CC) $(CFLAGS) -c $< -o $@

# ASM objects (kernel + libc crt0)
build/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "[AS]  $<"
	$(AS) $(ASFLAGS) $< -o $@


# DEBUG BUILD


# Recompile kernel with -g -O2 on top of normal flags
debug: CFLAGS += $(DEBUG_CFLAGS)
debug: all
	@echo ""
	@echo "  Debug build ready: build/exploidus.elf"
	@echo "  Run:  make qemu-dbg"

qemu-dbg: build/exploidus.elf
	@echo "[QEMU] waiting for GDB on :1234 ..."
	qemu-system-x86_64 \
	    -kernel build/exploidus.elf \
	    -m 256M \
	     \
	    -s -S &
	gdb build/exploidus.elf \
	    -ex "set architecture i386:x86-64:intel" \
	    -ex "target remote :1234" \
	    -ex "b kernel_main" \
	    -ex "continue"


# ISO


iso: build/exploidus.iso

build/exploidus.iso: build/exploidus.elf
	@mkdir -p iso/boot/grub
	@cp build/exploidus.elf iso/boot/exploidus.elf
	@echo 'set gfxmode=800x600x32'                >  iso/boot/grub/grub.cfg
	@echo 'set gfxpayload=keep'                   >> iso/boot/grub/grub.cfg
	@echo 'set timeout=0'                              >> iso/boot/grub/grub.cfg
	@echo 'menuentry "Exploidus" {'             >> iso/boot/grub/grub.cfg
	@echo '    multiboot2 /boot/exploidus.elf'  >> iso/boot/grub/grub.cfg
	@echo '    boot'                            >> iso/boot/grub/grub.cfg
	@echo '}'                                   >> iso/boot/grub/grub.cfg
	grub-mkrescue -o build/exploidus.iso iso
	@echo "[ISO] build/exploidus.iso ready"


# QEMU TARGETS


qemu: build/exploidus.elf
	qemu-system-x86_64 \
	    -kernel build/exploidus.elf \
	    -m 256M \
	    -device usb-ehci -device usb-tablet \
	    -serial stdio

qemu-vga: build/exploidus.elf
	qemu-system-x86_64 \
	    -kernel build/exploidus.elf \
	    -m 256M \
	    -device usb-ehci -device usb-tablet \
	    -vga virtio -serial stdio

qemu-iso: build/exploidus.iso
	qemu-system-x86_64 \
	    -cdrom build/exploidus.iso \
            -netdev user,id=n0 \
            -device e1000,netdev=n0 \
	    -m 256M \
	    -serial stdio \
            


# CLEAN


clean:
	rm -rf build iso
	@echo "[CLN] workspace clean"

# DISK IMAGE


build/disk.img: build/userspace/bin/hello.elf build/userspace/bin/auditd.elf build/userspace/bin/init.elf build/userspace/bin/httpd.elf build/userspace/bin/httptest.elf build/userspace/shell/exploish.elf build/userspace/bin/rahu.elf build/userspace/compositor/compositor.elf build/userspace/bin/gui_demo.elf build/userspace/bin/terminal.elf
	@mkdir -p $(dir $@)
	@echo "[DISK] Creating 64M ExFS disk image..."
	@qemu-img create -f raw build/disk.img 64M
	@python3 tools/mkexfs.py build/disk.img build/userspace/bin/hello.elf build/userspace/bin/auditd.elf build/userspace/bin/init.elf build/userspace/bin/httpd.elf build/userspace/bin/httptest.elf build/userspace/shell/exploish.elf build/userspace/bin/rahu.elf build/userspace/compositor/compositor.elf build/userspace/bin/gui_demo.elf build/userspace/bin/terminal.elf
	@echo "[DISK] build/disk.img ready"

qemu-disk: build/exploidus.iso build/disk.img
	qemu-system-x86_64 \
	    -cdrom build/exploidus.iso \
	    -netdev user,id=n0,hostfwd=tcp::8080-:80 \
	    -device e1000,netdev=n0 \
	    -drive file=build/disk.img,format=raw,if=ide,index=0 \
	    -m 256M \
	    -device usb-ehci -device usb-tablet \
	    -serial stdio \
	    -cpu qemu64,+rdrand \
	    -object filter-dump,id=f1,netdev=n0,file=/tmp/qemu-net.pcap

qemu-gui: build/exploidus.iso build/disk.img
	qemu-system-x86_64 \
	    -cdrom build/exploidus.iso \
	    -netdev user,id=n0 \
	    -device e1000,netdev=n0 \
	    -drive file=build/disk.img,format=raw,if=ide,index=0 \
	    -m 256M \
	    -device usb-ehci -device usb-tablet \
	    -vga virtio \
	    -serial stdio \
	    -cpu qemu64,+rdrand \
	    -object filter-dump,id=f1,netdev=n0,file=/tmp/qemu-net.pcap

qemu-run: build/exploidus.iso build/disk.img
	qemu-system-x86_64 \
	    -cdrom build/exploidus.iso \
            -netdev user,id=n0 \
            -device e1000,netdev=n0 \
	    -drive file=build/disk.img,format=raw,if=ide,index=0 \
	    -m 256M \
	    -net nic,model=e1000 -net user \
	     -cpu qemu64,+rdrand \
	    -object filter-dump,id=f1,netdev=n0,file=/tmp/qemu-net.pcap \
	    -device usb-ehci -device usb-tablet \
	    -serial file:/tmp/serial.log \
	    -vga virtio -display gtk,grab-on-hover=off 

# Hello binary blob
build/hello_blob.o: build/userspace/bin/hello.elf
	@echo "[BIN] embedding hello blob"
	x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@

build/init_blob.o: build/userspace/bin/init.elf
	@echo "[BIN] embedding init blob"
	x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@
#  Yolish interpreter 
YOLISH_SRCS := userspace/yolish/lexer.c \
               userspace/yolish/parser.c \
               userspace/yolish/eval.c   \
               userspace/yolish/main.c
YOLISH_OBJS := $(patsubst %.c, build/%.o, $(YOLISH_SRCS))

build/userspace/yolish/%.o: userspace/yolish/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  yolish: $<"
	$(CC) $(SHELL_CFLAGS) -Iuserspace/yolish -Iuserspace/libc -c $< -o $@

build/userspace/yolish/ys.elf: $(SA_OBJS) $(YOLISH_OBJS) userspace/shell/shell.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  ys -> $@"
	$(LD) -T userspace/shell/shell.ld $(USER_LDFLAGS) -o $@ $(SA_OBJS) $(YOLISH_OBJS)