#!/usr/bin/env python3
"""
mkfozu.py -- builds a .fozu package archive for the Exploidus rahu
package manager.

.fozu format (all integers little-endian, native x86-64 byte order):

    magic         4 bytes   "FOZU"
    format_ver    u32       (=1)
    name_len      u32
    name          name_len bytes
    version_len   u32
    version       version_len bytes
    file_count    u32
    for each file:
        path_len  u32
        path      path_len bytes   (absolute install path, e.g. "/bin/hello")
        mode      u32              (permission bits, e.g. 0755)
        size      u64
        data      size bytes

This is a plain sequential container, not an index+offset table format --
rahu on the device parses it top to bottom with open()/read(), which is
all this libc has (no mmap, no fseek-heavy random access needed).

Usage:
    python3 mkfozu.py <name> <version> <output.fozu> <local_path>:<install_path>[:<mode>] [...]

Example:
    python3 mkfozu.py hello 1.0 hello.fozu build/userspace/bin/hello.elf:/bin/hello:755

Multiple files can be listed to bundle a package with several installed
files (a binary plus a config file, a couple of libs, etc):
    python3 mkfozu.py myapp 2.1 myapp.fozu \\
        build/myapp.elf:/bin/myapp:755 \\
        assets/myapp.conf:/etc/myapp.conf:644
"""
import struct
import sys


def u32(n):
    return struct.pack("<I", n)


def u64(n):
    return struct.pack("<Q", n)


def build(name: str, version: str, entries):
    """entries: list of (local_path, install_path, mode_int)"""
    out = bytearray()
    out += b"FOZU"
    out += u32(1)  # format version

    name_b = name.encode("utf-8")
    out += u32(len(name_b))
    out += name_b

    ver_b = version.encode("utf-8")
    out += u32(len(ver_b))
    out += ver_b

    out += u32(len(entries))

    for local_path, install_path, mode in entries:
        with open(local_path, "rb") as f:
            data = f.read()
        path_b = install_path.encode("utf-8")
        out += u32(len(path_b))
        out += path_b
        out += u32(mode)
        out += u64(len(data))
        out += data
        print(f"  + {install_path}  ({len(data)} bytes, mode {oct(mode)})")

    return bytes(out)


def parse_entry(spec: str):
    """Parse "local_path:install_path[:mode]" -> (local_path, install_path, mode)."""
    parts = spec.split(":")
    if len(parts) == 2:
        local_path, install_path = parts
        mode = 0o755
    elif len(parts) == 3:
        local_path, install_path, mode_s = parts
        mode = int(mode_s, 8) if not mode_s.startswith("0") else int(mode_s, 8)
    else:
        raise ValueError(
            f"bad entry '{spec}', expected local_path:install_path[:mode]"
        )
    return local_path, install_path, mode


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        sys.exit(1)

    name = sys.argv[1]
    version = sys.argv[2]
    output_path = sys.argv[3]
    entry_specs = sys.argv[4:]

    entries = [parse_entry(s) for s in entry_specs]

    print(f"Building {output_path} (package '{name}' v{version}):")
    data = build(name, version, entries)

    with open(output_path, "wb") as f:
        f.write(data)

    print(f"\nWrote {output_path}: {len(data)} bytes, {len(entries)} file(s).")


if __name__ == "__main__":
    main()