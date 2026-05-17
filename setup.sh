#!/bin/bash
# setup.sh — Exploidus cross-compiler installer
# Builds x86_64-elf-gcc and x86_64-elf-binutils from source.
# Run once on Kali Linux (WSL2 or native).
# Takes ~30-45 minutes depending on your CPU.

set -e

PREFIX="$HOME/opt/cross"
TARGET="x86_64-elf"
BINUTILS_VER="2.41"
GCC_VER="13.2.0"
BINUTILS_URL="https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
GCC_URL="https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"

echo "========================================"
echo " Exploidus Cross-Compiler Setup"
echo " Target : $TARGET"
echo " Prefix : $PREFIX"
echo "========================================"

#  Check for required host tools 
MISSING=""
for tool in gcc g++ make bison flex libgmp-dev libmpfr-dev libmpc-dev \
            libisl-dev texinfo curl; do
    if ! dpkg -l "$tool" 2>/dev/null | grep -q "^ii"; then
        MISSING="$MISSING $tool"
    fi
done

if [ -n "$MISSING" ]; then
    echo "[setup] Installing missing packages:$MISSING"
    sudo apt-get update -qq
    sudo apt-get install -y $MISSING
fi

#  Already built? 
if [ -f "$PREFIX/bin/$TARGET-gcc" ]; then
    echo "[setup] Cross-compiler already present at $PREFIX"
    echo "[setup] Delete $PREFIX to rebuild."
    # Make sure PATH is set
    if ! grep -q "opt/cross" "$HOME/.bashrc"; then
        echo "export PATH=\"\$HOME/opt/cross/bin:\$PATH\"" >> "$HOME/.bashrc"
    fi
    echo "[setup] Run: source ~/.bashrc"
    exit 0
fi

mkdir -p "$PREFIX"
WORKDIR="$(mktemp -d)"
echo "[setup] Working directory: $WORKDIR"

#  Binutils 
echo ""
echo "[1/4] Downloading binutils-${BINUTILS_VER}..."
curl -# -L "$BINUTILS_URL" -o "$WORKDIR/binutils.tar.xz"

echo "[2/4] Building binutils..."
cd "$WORKDIR"
tar -xf binutils.tar.xz
mkdir build-binutils
cd build-binutils
../binutils-${BINUTILS_VER}/configure \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --with-sysroot \
    --disable-nls \
    --disable-werror \
    --quiet
make -j"$(nproc)" --quiet
make install --quiet
echo "[setup] Binutils installed."

#  GCC 
echo ""
echo "[3/4] Downloading gcc-${GCC_VER} (this is large, ~80 MB)..."
cd "$WORKDIR"
curl -# -L "$GCC_URL" -o gcc.tar.xz

echo "[4/4] Building GCC (this takes ~25 minutes)..."
tar -xf gcc.tar.xz
mkdir build-gcc
cd build-gcc
../gcc-${GCC_VER}/configure \
    --target="$TARGET" \
    --prefix="$PREFIX" \
    --disable-nls \
    --enable-languages=c \
    --without-headers \
    --quiet
make -j"$(nproc)" all-gcc all-target-libgcc --quiet
make install-gcc install-target-libgcc --quiet
echo "[setup] GCC installed."

#  Cleanup 
rm -rf "$WORKDIR"

#  Update PATH 
if ! grep -q "opt/cross" "$HOME/.bashrc"; then
    echo "" >> "$HOME/.bashrc"
    echo "# Exploidus cross-compiler" >> "$HOME/.bashrc"
    echo "export PATH=\"\$HOME/opt/cross/bin:\$PATH\"" >> "$HOME/.bashrc"
    echo "[setup] Added $PREFIX/bin to ~/.bashrc"
fi

export PATH="$PREFIX/bin:$PATH"

echo ""
echo "========================================"
echo " Cross-compiler build complete!"
echo " Location: $PREFIX/bin"
echo "========================================"
echo ""
echo " Now run:"
echo "   source ~/.bashrc"
echo "   make"
echo "   make qemu"
echo ""
