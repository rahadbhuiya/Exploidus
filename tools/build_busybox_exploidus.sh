#!/bin/bash
# build_busybox_exploidus.sh
SYSROOT="$(pwd)/exploidus-sysroot"

wget -q "https://busybox.net/downloads/busybox-1.36.1.tar.bz2"
tar -xjf busybox-1.36.1.tar.bz2
cd busybox-1.36.1

make defconfig

# Static build with our musl
cat >> .config << EOF
CONFIG_STATIC=y
CONFIG_CROSS_COMPILER_PREFIX="x86_64-elf-"
CONFIG_SYSROOT="${SYSROOT}"
CONFIG_EXTRA_CFLAGS="-ffreestanding -mno-red-zone -mno-sse -mno-sse2"
CONFIG_EXTRA_LDFLAGS="-static -nostdlib -L${SYSROOT}/lib"
EOF

make -j$(nproc)
# Output: busybox — single binary, symlink  ls/grep/sed/cat/find/...