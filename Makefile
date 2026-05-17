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
                -fno-pic -fno-pie -mno-red-zone               \
                -mno-mmx -mno-sse -mno-sse2                   \
                -Wall -Wextra -Werror -Wno-unused-parameter   \
                -Iuserspace/libc -g -O2

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
    kernel/drivers/keyboard.c kernel/drivers/fb.c kernel/drivers/font.c kernel/drivers/mouse.c kernel/drivers/fb_console.c kernel/drivers/ata.c \
    kernel/fs/vfs/vfs.c kernel/fs/exfs/exfs.c \
    kernel/elf/elf.c \
    kernel/net/net.c kernel/net/netstack.c \
    kernel/net/arp/arp.c kernel/net/ip/ip.c \
    kernel/net/icmp/icmp.c kernel/net/udp/udp.c \
    kernel/net/tcp/tcp.c kernel/net/socket/socket.c \
    kernel/net/drivers/e1000.c

KERNEL_ASM_SRCS := \
    kernel/boot/start.asm \
    kernel/arch/x86_64/isr.asm \
    kernel/arch/x86_64/gdt_flush.asm \
    kernel/proc/context_switch.asm \
    kernel/proc/jump_userspace.asm \
    kernel/syscall/entry.asm

SHELL_C_SRCS   := userspace/shell/exploish.c userspace/shell/exploish_cmds.c
HELLO_C_SRCS   := userspace/bin/hello.c
SHELL_ASM_SRCS := userspace/libc/crt0.asm

#  OBJECTS 
KC_OBJS  := $(patsubst %.c,   build/%.o, $(KERNEL_C_SRCS))
KA_OBJS  := $(patsubst %.asm, build/%.o, $(KERNEL_ASM_SRCS))
SC_OBJS  := $(patsubst %.c,   build/%.o, $(SHELL_C_SRCS))
HC_OBJS  := $(patsubst %.c,   build/%.o, $(HELLO_C_SRCS))
SA_OBJS  := $(patsubst %.asm, build/%.o, $(SHELL_ASM_SRCS))

ALL_KOBJS := $(KC_OBJS) $(KA_OBJS)

#  PHONY 
.PHONY: all clean iso qemu qemu-vga debug qemu-iso qemu-disk qemu-run


# PRIMARY TARGETS


all: build/exploidus.elf build/userspace/bin/hello.elf
#  KERNEL ELF 
build/userspace/bin/hello.elf: $(SA_OBJS) $(HC_OBJS) userspace/bin/hello.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  hello  -> $@"
	$(LD) -T userspace/bin/hello.ld -nostdlib -z max-page-size=0x1000 --no-dynamic-linker -o $@ $(SA_OBJS) $(HC_OBJS)

build/exploidus.elf: $(ALL_KOBJS) build/shell_blob.o build/hello_blob.o linker.ld
	@echo "[LD]  kernel -> $@"
	$(LD) $(LDFLAGS) -o $@ $(ALL_KOBJS) build/shell_blob.o build/hello_blob.o

#  SHELL ELF 
build/userspace/shell/exploish.elf: $(SA_OBJS) $(SC_OBJS) userspace/shell/shell.ld
	@mkdir -p $(dir $@)
	@echo "[LD]  shell  -> $@"
	$(LD) $(SHELL_LDFLAGS) -o $@ $(SA_OBJS) $(SC_OBJS)

#  SHELL BLOB 
build/shell_blob.o: build/userspace/shell/exploish.elf
	@echo "[BIN] embedding shell blob"
	x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@


# COMPILE RULES


# Shell C objects — use SHELL_CFLAGS (debug, libc include)
build/userspace/shell/%.o: userspace/shell/%.c
	@mkdir -p $(dir $@)
	@echo "[CC]  shell: $<"
	$(CC) $(SHELL_CFLAGS) -c $< -o $@

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


build/disk.img: build/userspace/bin/hello.elf
	@mkdir -p $(dir $@)
	@echo "[DISK] Creating 64M ExFS disk image..."
	@qemu-img create -f raw build/disk.img 64M
	@python3 tools/mkexfs.py build/disk.img build/userspace/bin/hello.elf
	@echo "[DISK] build/disk.img ready"

qemu-disk: build/exploidus.iso build/disk.img
	qemu-system-x86_64 \
	    -cdrom build/exploidus.iso \
	    -netdev user,id=n0 \
	    -device e1000,netdev=n0 \
	    -drive file=build/disk.img,format=raw,if=ide,index=0 \
	    -m 256M \
	    -device usb-ehci -device usb-tablet \
	    -serial stdio \
	    -cpu qemu64,+rdrand

qemu-run: build/exploidus.iso build/disk.img
	qemu-system-x86_64 \
	    -cdrom build/exploidus.iso \
            -netdev user,id=n0 \
            -device e1000,netdev=n0 \
	    -drive file=build/disk.img,format=raw,if=ide,index=0 \
	    -m 256M \
	    -net nic,model=e1000 -net user \
	     -cpu qemu64,+rdrand \
	    -device usb-ehci -device usb-tablet \
	    -serial file:/tmp/serial.log \
	    -vga virtio -display gtk,grab-on-hover=off 

# Hello binary blob
build/hello_blob.o: build/userspace/bin/hello.elf
	@echo "[BIN] embedding hello blob"
	x86_64-elf-objcopy -I binary -O elf64-x86-64 -B i386:x86-64 $< $@
