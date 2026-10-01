#!/usr/bin/env python3
"""Merge the Arduino build output into one image flashable at 0x0.

With PartitionScheme=noota_3g the whole 4 MiB of flash is one unbroken span:
nothing is imported between the end of the bootloader at 0x0 and the end of
coredump at 0x400000. So five files at five addresses -- one of which every
tutorial gets wrong -- collapse into a single download flashed at 0x0.

Every offset comes from the core's own partition CSV, looked up via the
PartitionScheme= in the FQBN that was actually compiled. The 0xe000 vs
0xe0000 trap is exactly the kind of thing that rots when it lives in a
comment, so it is derived rather than written down.

Usage:
    merge-image.py <FQBN> <core_root> <build_dir> <fs_path> <out_path>

Needs FS_PARTITION_LABEL in the environment (the label of the filesystem
partition in the CSV). Diagnostics go to stdout on purpose: GitHub only turns
::error:: lines on stdout into annotations on the commit page.
"""
import csv as _csv
import glob
import hashlib
import os
import sys

# Everything arduino-cli and the core put in the build path under a name we
# know. Whatever .bin is left over is the application.
KNOWN_BINS = ("bootloader.bin", "partitions.bin", "boot_app0.bin")


def die(msg):
    print(f"::error::{msg}")
    sys.exit(1)


def num(s):
    return int(s.strip(), 0)


def read_parts(csv_path):
    parts = []
    with open(csv_path, newline="") as fh:
        for row in _csv.reader(fh):
            # Skip the "# Name, Type, ..." header and blank lines. Filter
            # whole rows, never individual cells: dropping the '#' cell
            # silently leaves 'Type' as the name and 'Size' as the offset.
            if not row or not row[0].strip() or row[0].lstrip().startswith("#"):
                continue
            if len(row) < 5:
                continue
            name, _type, _sub, off, size = (row + [""] * 5)[:5]
            try:
                parts.append((name.strip(), num(off), num(size)))
            except ValueError:
                die(f"cannot parse partition row {row!r} in {csv_path}")
    if not parts:
        die(f"no partitions parsed from {csv_path}")
    return parts


def main():
    if len(sys.argv) != 6:
        die("usage: merge-image.py <FQBN> <core_root> <build_dir> <fs_path> <out_path>")
    fqbn, core_root, build_dir, fs_path, out_path = sys.argv[1:6]

    scheme = ""
    for field in fqbn.split(","):
        if field.startswith("PartitionScheme="):
            scheme = field.split("=", 1)[1]
    if not scheme:
        die(f"no PartitionScheme= in FQBN: {fqbn}")

    hits = glob.glob(
        os.path.join(core_root, "*", "tools", "partitions", f"{scheme}.csv")
    )
    if not hits:
        die(
            f"{scheme}.csv not found under {core_root}; looked in "
            f"{os.path.join(core_root, '*', 'tools', 'partitions')}"
        )
    csv_path = hits[0]
    print(f"partition scheme: {scheme}")
    print(f"partition CSV: {csv_path}")

    parts = read_parts(csv_path)
    by_name = {p[0]: p for p in parts}
    for n in ("otadata", "app0"):
        if n not in by_name:
            die(f"partition {n!r} missing from {csv_path}")

    app_off, app_size = by_name["app0"][1], by_name["app0"][2]
    fs_name = os.environ.get("FS_PARTITION_LABEL")
    if not fs_name:
        die("FS_PARTITION_LABEL not set")
    if fs_name not in by_name:
        die(f"no partition labelled {fs_name!r} in {csv_path}")
    fs_off, fs_size = by_name[fs_name][1], by_name[fs_name][2]
    ota_off = by_name["otadata"][1]
    total = max(o + s for _n, o, s in parts)

    # The bundle step has already normalised arduino-cli's sketch-prefixed
    # names to these four, and it is the only place that has looked at the
    # build directory. So the contract is these names, not a guess here.
    app_bin = "firmware.bin"
    missing = [n for n in ("bootloader.bin", "partitions.bin", "boot_app0.bin",
                           app_bin) if not os.path.exists(os.path.join(build_dir, n))]
    if missing:
        die(f"missing from {build_dir}: {', '.join(missing)}; the bundle step "
            f"should have left bootloader.bin, partitions.bin, boot_app0.bin "
            f"and {app_bin} there")

    def path_of(name):
        return fs_path if name == "littlefs.bin" else os.path.join(build_dir, name)

    pieces = [
        ("bootloader.bin", 0x0),
        ("partitions.bin", 0x8000),
        ("boot_app0.bin", ota_off),
        (app_bin, app_off),
        ("littlefs.bin", fs_off),
    ]

    buf = bytearray(b"\xff" * total)
    print("| file | address | bytes | sha256 (first 16) |")
    print("|---|---|---|---|")
    for name, off in pieces:
        p = path_of(name)
        if not os.path.exists(p):
            die(f"{name} missing at {p}")
        with open(p, "rb") as fh:
            data = fh.read()
        if off + len(data) > total:
            die(f"{name} at 0x{off:x} + {len(data)} runs past 0x{total:x}")
        buf[off:off + len(data)] = data
        print(
            f"| {name} | 0x{off:x} | {len(data)} | "
            f"{hashlib.sha256(data).hexdigest()[:16]} |"
        )

    # The app and the filesystem have hard limits. Overrunning either is silent
    # corruption in a merged image -- the neighbouring partition just gets
    # eaten -- so refuse instead of writing it.
    for name, limit in ((app_bin, app_size), ("littlefs.bin", fs_size)):
        used = os.path.getsize(path_of(name))
        if used > limit:
            die(f"{name} is {used} bytes, only {limit} available")

    with open(out_path, "wb") as fh:
        fh.write(buf)

    # Read it back and prove every byte landed where the CSV says it should.
    # A merge bug that ships is far worse than a merge bug that fails here.
    with open(out_path, "rb") as fh:
        check = fh.read()
    if len(check) != total:
        die(f"merged image is {len(check)} bytes, expected {total}")
    for name, off in pieces:
        with open(path_of(name), "rb") as fh:
            data = fh.read()
        end = off + len(data)
        if check[off:end] != data:
            die(f"verify failed: {name} != merged[0x{off:x}:]")

    # arduino-cli 1.5.1 emits its own merged image alongside the pieces, sized
    # to the whole flash. Comparing the regions we care about is a free,
    # independent check that the offsets parsed out of the CSV are the ones
    # the toolchain itself used -- the 0xe000 vs 0xe0000 trap would show up
    # here rather than as a silently unbootable board on the bench.
    ac = os.path.join(build_dir, "arduino-cli-merged.bin")
    if os.path.exists(ac):
        size = os.path.getsize(ac)
        print(f"cross-check against arduino-cli's own {size}-byte merged image:")
        with open(ac, "rb") as fh:
            ref = fh.read()
        for name, off in pieces:
            if name == "littlefs.bin":
                continue  # ours alone; the core has no filesystem image
            with open(path_of(name), "rb") as fh:
                data = fh.read()
            if off + len(data) > size:
                die(f"arduino-cli merged image is only {size} bytes; "
                    f"{name} at 0x{off:x} does not fit inside it")
            if ref[off:off + len(data)] != data:
                die(
                    f"offset cross-check failed: {name} at 0x{off:x} differs "
                    f"from arduino-cli's merged image. The CSV offsets and the "
                    f"offsets the build actually used are not the same thing."
                )
            print(f"  {name:18} @0x{off:06x} agrees")
    else:
        print("cross-check skipped: no arduino-cli-merged.bin to compare against")

    print(
        f"| **merged.bin** | **0x0** | **{total}** | "
        f"**{hashlib.sha256(check).hexdigest()[:16]}** |"
    )
    print()
    print(f"Flash this ONE file at 0x0. It covers 0x0-0x{total:x}, which is")
    print(f"every partition in {os.path.basename(csv_path)}. Padding between")
    print("partitions is 0xFF, i.e. erased flash.")


if __name__ == "__main__":
    main()