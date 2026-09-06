#!/usr/bin/env python3
"""Minimal XDVDFS (Xbox / Xbox 360 XGD) reader: list or extract files from an ISO.

Xbox 360 discs are not ISO9660/UDF, so 7-Zip and hdiutil cannot read them. The
filesystem is XDVDFS: a volume descriptor at sector 32 of the game partition,
then directory tables holding a binary search tree of entries.

Usage:
  xdvdfs_extract.py <iso> --list
  xdvdfs_extract.py <iso> --extract default.xex --out <dir>
"""
import argparse, os, struct, sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game-partition byte offsets by disc generation: original Xbox, XGD3, XGD2, XGD1.
# Probed in order; the volume magic confirms which one is right.
BASES = [0x00000000, 0x02080000, 0x0FD90000, 0x18300000]
ATTR_DIR = 0x10


def find_base(f):
    """Locate the game partition by probing known offsets for the volume magic."""
    for base in dict.fromkeys(BASES):
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise SystemExit("error: no XDVDFS volume descriptor found (not an Xbox/Xbox 360 disc image?)")


def read_volume(f, base):
    f.seek(base + 32 * SECTOR + 20)
    root_sector, root_size = struct.unpack("<II", f.read(8))
    return root_sector, root_size


def walk_dir(f, base, sector, size, prefix=""):
    """Yield (path, start_sector, size, is_dir) for every entry in one directory table."""
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    out, stack, seen = [], [0], set()
    while stack:
        off = stack.pop()
        # 0xFFFF terminates a branch; also guard against malformed/looping tables.
        if off == 0xFFFF or off * 4 >= len(table) or off in seen:
            continue
        seen.add(off)
        p = off * 4
        if p + 14 > len(table):
            continue
        left, right, start, fsize, attrs, nlen = struct.unpack_from("<HHIIBB", table, p)
        name = table[p + 14 : p + 14 + nlen].decode("latin-1")
        if not name:
            continue
        stack.append(left)
        stack.append(right)
        path = f"{prefix}{name}"
        is_dir = bool(attrs & ATTR_DIR)
        out.append((path, start, fsize, is_dir))
        if is_dir:
            out.extend(walk_dir(f, base, start, fsize, prefix=path + "/"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", help="filename to extract (case-insensitive, matches basename)")
    ap.add_argument("--extract-all", action="store_true", help="extract the whole tree to --out")
    ap.add_argument("--skip", action="append", default=[],
                    help="top-level directory to skip when extracting all (repeatable)")
    ap.add_argument("--out", default=".")
    a = ap.parse_args()

    with open(a.iso, "rb") as f:
        base = find_base(f)
        root_sector, root_size = read_volume(f, base)
        print(f"[xdvdfs] partition base 0x{base:X}, root sector {root_sector}, root size {root_size}", file=sys.stderr)
        entries = sorted(walk_dir(f, base, root_sector, root_size), key=lambda e: e[0].lower())

        if a.list:
            for path, _s, sz, is_dir in entries:
                print(f"{'<DIR>' if is_dir else sz:>12}  {path}")
            print(f"\n{len(entries)} entries", file=sys.stderr)

        if a.extract_all:
            written = 0
            for path, start, sz, is_dir in entries:
                top = path.split("/")[0]
                if any(top.lower() == s.lower() for s in a.skip):
                    continue
                dest = os.path.join(a.out, *path.split("/"))
                if is_dir:
                    os.makedirs(dest, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
                f.seek(base + start * SECTOR)
                remaining = sz
                with open(dest, "wb") as o:
                    while remaining:
                        chunk = f.read(min(8 << 20, remaining))
                        if not chunk:
                            raise SystemExit(f"error: unexpected EOF reading /{path}")
                        o.write(chunk)
                        remaining -= len(chunk)
                written += 1
                print(f"[xdvdfs] {path} ({sz})", file=sys.stderr)
            print(f"[xdvdfs] extracted {written} files to {a.out}", file=sys.stderr)

        if a.extract:
            want = a.extract.lower()
            hits = [e for e in entries if not e[3] and e[0].lower().split("/")[-1] == want]
            if not hits:
                raise SystemExit(f"error: {a.extract!r} not found in image")
            for path, start, sz, _ in hits:
                os.makedirs(a.out, exist_ok=True)
                dest = os.path.join(a.out, path.split("/")[-1])
                f.seek(base + start * SECTOR)
                remaining = sz
                with open(dest, "wb") as o:
                    while remaining:
                        chunk = f.read(min(1 << 20, remaining))
                        if not chunk:
                            raise SystemExit("error: unexpected EOF while reading image")
                        o.write(chunk)
                        remaining -= len(chunk)
                print(f"[xdvdfs] wrote {dest} ({sz} bytes) from /{path}", file=sys.stderr)


if __name__ == "__main__":
    main()
