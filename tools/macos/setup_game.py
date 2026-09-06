#!/usr/bin/env python3
"""One-command game setup for the macOS port: your disc image in, a ready
`game/` folder out.

Xbox 360 discs are XDVDFS, not ISO9660/UDF, so Finder, hdiutil and 7-Zip cannot
open them. This wraps the XDVDFS reader in xdvdfs_extract.py with the checks and
progress reporting you actually want when unpacking ~6 GB.

    tools/macos/setup_game.py --iso ~/Downloads/"NHL Legacy Edition.iso"

Also accepts a .7z straight from a dump (extracted with 7zz/bsdtar if present),
or an already-extracted folder via --from. Re-running is cheap: files that are
already present with the right size are skipped, so an interrupted run resumes.

This tool copies data out of YOUR OWN disc image. It ships no game content.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xdvdfs_extract as xdvdfs  # noqa: E402

# Files the port needs to boot. default.xex is the recompiled executable's
# source image; the .big archives hold everything else.
REQUIRED = ["default.xex"]
# Present on a good dump - used only to warn, not to fail, since regional
# releases differ.
EXPECTED = ["boot.big", "cache.big", "data0.big"]
SKIP_DIRS = ["$SystemUpdate"]


def human(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0


def find_repo_root():
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(4):
        d = os.path.dirname(d)
        if os.path.exists(os.path.join(d, "nhllegacy_manifest.toml")):
            return d
    return os.getcwd()


def extract_7z(archive, workdir):
    """Pull the first .iso out of a .7z using whatever archiver is installed."""
    tool = next((t for t in ("7zz", "7z", "7za") if shutil.which(t)), None)
    if tool:
        cmd = [tool, "x", "-y", f"-o{workdir}", archive]
    elif shutil.which("bsdtar"):
        cmd = ["bsdtar", "-x", "-f", archive, "-C", workdir]
    else:
        sys.exit(
            f"error: {archive} is a .7z and no extractor was found.\n"
            "       Install one (brew install sevenzip) or extract it yourself\n"
            "       and pass the .iso with --iso."
        )
    print(f"==> Extracting {os.path.basename(archive)} with {cmd[0]} (this takes a while)...")
    if subprocess.run(cmd, stdout=subprocess.DEVNULL).returncode != 0:
        sys.exit(f"error: {cmd[0]} failed to extract {archive}")
    for root, _dirs, files in os.walk(workdir):
        for name in files:
            if name.lower().endswith(".iso"):
                return os.path.join(root, name)
    sys.exit(f"error: no .iso found inside {archive}")


def copy_tree(src, out):
    """--from path: mirror an already-extracted game folder."""
    if not os.path.isfile(os.path.join(src, "default.xex")):
        # Tolerate being pointed one level high (e.g. at the mount point).
        nested = [
            os.path.join(src, d)
            for d in os.listdir(src)
            if os.path.isfile(os.path.join(src, d, "default.xex"))
        ]
        if len(nested) == 1:
            src = nested[0]
            print(f"==> Using {src} (that is where default.xex lives)")
        else:
            sys.exit(f"error: no default.xex in {src} - is that the game folder?")
    print(f"==> Copying from {src}")
    copied = 0
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        dest_dir = out if rel == "." else os.path.join(out, rel)
        os.makedirs(dest_dir, exist_ok=True)
        for name in files:
            s, d = os.path.join(root, name), os.path.join(dest_dir, name)
            if os.path.exists(d) and os.path.getsize(d) == os.path.getsize(s):
                continue
            shutil.copy2(s, d)
            copied += 1
    print(f"==> Copied {copied} file(s)")


def extract_iso(iso, out, force, include_system_update):
    with open(iso, "rb") as f:
        try:
            base = xdvdfs.find_base(f)
        except SystemExit:
            sys.exit(
                f"error: {iso} is not an Xbox/Xbox 360 disc image.\n"
                "       Xbox 360 discs are XDVDFS - a normal ISO9660 rip of the disc\n"
                "       made on a PC drive will NOT work; you need a real 360 dump."
            )
        root_sector, root_size = xdvdfs.read_volume(f, base)
        entries = xdvdfs.walk_dir(f, base, root_sector, root_size)

        skip = [] if include_system_update else SKIP_DIRS
        files = [
            e
            for e in entries
            if not e[3] and not any(e[0].split("/")[0].lower() == s.lower() for s in skip)
        ]
        names = {e[0].split("/")[-1].lower() for e in files}
        missing = [r for r in REQUIRED if r.lower() not in names]
        if missing:
            sys.exit(
                f"error: {', '.join(missing)} not found in the image.\n"
                "       This does not look like an NHL Legacy Edition disc."
            )
        absent = [e for e in EXPECTED if e.lower() not in names]
        if absent:
            print(f"warning: expected file(s) not on this disc: {', '.join(absent)}")

        total = sum(e[2] for e in files)
        print(f"==> Disc image OK ({len(files)} files, {human(total)})")

        free = shutil.disk_usage(os.path.dirname(os.path.abspath(out)) or ".").free
        if free < total * 1.05:
            sys.exit(
                f"error: not enough free space. Need about {human(total * 1.05)}, "
                f"have {human(free)}."
            )

        os.makedirs(out, exist_ok=True)
        done = skipped = 0
        started = time.time()
        # Only animate progress on a terminal; piped/redirected output gets a
        # handful of milestone lines instead of thousands of \r updates.
        tty = sys.stdout.isatty()
        next_milestone = 10.0
        for path, start, size, _is_dir in sorted(files, key=lambda e: e[0].lower()):
            dest = os.path.join(out, *path.split("/"))
            os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
            if not force and os.path.exists(dest) and os.path.getsize(dest) == size:
                done += size
                skipped += 1
                continue
            f.seek(base + start * xdvdfs.SECTOR)
            remaining = size
            with open(dest, "wb") as o:
                while remaining:
                    chunk = f.read(min(8 << 20, remaining))
                    if not chunk:
                        sys.exit(f"\nerror: unexpected end of image while reading /{path}")
                    o.write(chunk)
                    remaining -= len(chunk)
                    done += len(chunk)
                    pct = 100.0 * done / total
                    elapsed = time.time() - started
                    rate = done / elapsed if elapsed > 0.5 else 0
                    eta = (total - done) / rate if rate else 0
                    if tty:
                        sys.stdout.write(
                            f"\r    {pct:5.1f}%  {human(done)} / {human(total)}"
                            f"  {human(rate)}/s  ETA {int(eta) // 60:d}m{int(eta) % 60:02d}s   "
                        )
                        sys.stdout.flush()
                    elif pct >= next_milestone:
                        print(f"    {pct:5.1f}%  {human(done)} / {human(total)}", flush=True)
                        next_milestone += 10.0
        if tty:
            sys.stdout.write("\r" + " " * 78 + "\r")
        if skipped:
            print(f"==> Reused {skipped} file(s) already present")
        print(f"==> Extracted to {out} in {int(time.time() - started)}s")


def verify(out):
    xex = os.path.join(out, "default.xex")
    if not os.path.isfile(xex) or os.path.getsize(xex) == 0:
        sys.exit(f"error: {xex} is missing or empty - the setup did not complete.")
    total = sum(
        os.path.getsize(os.path.join(r, n)) for r, _d, fs in os.walk(out) for n in fs
    )
    count = sum(len(fs) for _r, _d, fs in os.walk(out))
    print(f"==> Verified: default.xex present, {count} files, {human(total)} total")


def main():
    ap = argparse.ArgumentParser(
        description="Unpack your NHL Legacy disc image into a ready-to-run game folder.",
        epilog="This tool reads YOUR OWN disc image and ships no game content.",
    )
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--iso", help="path to your .iso (or .7z containing one)")
    src.add_argument("--from", dest="from_dir", help="path to an already-extracted game folder")
    ap.add_argument("--out", help="destination folder (default: <repo>/game)")
    ap.add_argument("--force", action="store_true", help="re-extract files that already exist")
    ap.add_argument(
        "--include-system-update",
        action="store_true",
        help="also extract $SystemUpdate (not needed by the port)",
    )
    a = ap.parse_args()

    if not a.iso and not a.from_dir:
        ap.print_usage()
        sys.exit(
            "\nerror: point me at your dump, e.g.\n"
            '  tools/macos/setup_game.py --iso ~/Downloads/"NHL Legacy Edition.iso"'
        )

    out = os.path.abspath(a.out or os.path.join(find_repo_root(), "game"))

    if a.from_dir:
        os.makedirs(out, exist_ok=True)
        copy_tree(os.path.abspath(a.from_dir), out)
    else:
        iso = os.path.abspath(a.iso)
        if not os.path.exists(iso):
            sys.exit(f"error: {iso} does not exist")
        tmp = None
        try:
            if iso.lower().endswith(".7z"):
                tmp = tempfile.mkdtemp(prefix="nhl-iso-")
                iso = extract_7z(iso, tmp)
            extract_iso(iso, out, a.force, a.include_system_update)
        finally:
            if tmp:
                shutil.rmtree(tmp, ignore_errors=True)

    verify(out)
    print(
        "\nReady. Run the game with:\n"
        f"  cd {os.path.join(find_repo_root(), 'build', 'mac-arm64')}\n"
        f"  ./nhllegacy --game_data_root={out}\n"
    )


if __name__ == "__main__":
    main()
