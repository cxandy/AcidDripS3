"""Exercise tools/merge-image.py, the step that builds the flashable image.

The merge step produces the single 4 MiB image that gets flashed at 0x0, so a
bug in it ships a corrupt image rather than an error. CI re-reads the image and
compares every byte, which catches a broken merge -- but only after a full
arduino-cli build. This harness runs the same script against synthetic inputs
in a second, including the failure paths.

    python tools/test-merge-image.py

Two bugs this caught, both of which would have shipped:

  * Filtering the "# Name, Type, ..." header by dropping comment *cells*
    rather than comment *rows* turned the header into a partition called Type
    at offset Size. Only visible because the nominal case asserts on content,
    not just on exit code.
  * The app binary was assumed to be firmware.bin. arduino-cli names it after
    the sketch, so it is AcidBox.bin. Hence find_app_bin().
"""
import os
import pathlib
import subprocess
import sys
import tempfile

SCRIPT = pathlib.Path(__file__).resolve().parent / "merge-image.py"

CSV = """# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x100000,
spiffs,   data, spiffs,  0x110000,0x2E0000,
coredump, data, coredump,0x3F0000,0x10000,
"""

# What arduino-cli + the core actually leave in the build path.
FILL = {
    "bootloader.bin": 0x8000,
    "partitions.bin": 0x1000,
    "boot_app0.bin": 0x2000,
    "AcidBox.bin": 0x96000,       # 616724, the real app size
}
LFS_SIZE = 0x2E0000              # full partition, as mklittlefs pads it
FQBN = "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=noota_3g,FlashSize=16M"
OFFSETS = {"bootloader.bin": 0x0, "partitions.bin": 0x8000, "boot_app0.bin": 0xE000,
           "AcidBox.bin": 0x10000, "littlefs.bin": 0x110000}


def build_case(tmp, sizes=None, label="", expect_fail_sub=None, fqbn=FQBN,
               csv_text=None, extra_bins=()):
    sizes = FILL if sizes is None else sizes
    d = pathlib.Path(tmp)
    build = d / "build"
    # The script globs <core_root>/*/tools/partitions/<scheme>.csv, so mirror
    # the installed core's layout rather than handing it a flat path.
    pdir = d / "core" / "3.3.12" / "tools" / "partitions"
    pdir.mkdir(parents=True, exist_ok=True)
    build.mkdir(parents=True, exist_ok=True)
    (pdir / "noota_3g.csv").write_text(csv_text or CSV, encoding="utf-8")
    for name, size in sizes.items():
        (build / name).write_bytes(bytes((i * 7 + 3) & 0xFF for i in range(size)))
    for name in extra_bins:
        (build / name).write_bytes(b"\x00" * 16)
    (d / "littlefs.bin").write_bytes(
        bytes(range(256)) * (LFS_SIZE // 256) + b"\x00" * (LFS_SIZE % 256))

    r = subprocess.run(
        [sys.executable, str(SCRIPT), fqbn, str(d / "core"), str(build),
         str(d / "littlefs.bin"), str(d / "merged.bin")],
        capture_output=True, text=True,
        env=dict(os.environ, FS_PARTITION_LABEL="spiffs"),
    )

    if expect_fail_sub is None:
        ok = r.returncode == 0
        why = ""
    else:
        # Must fail, and must fail for THIS reason, with the reason on stdout
        # so GitHub renders it as an annotation rather than a bare exit code.
        ok = (r.returncode != 0 and expect_fail_sub in r.stdout
              and "::error::" in r.stdout)
        why = f" (wanted ::error:: {expect_fail_sub!r} on stdout)"
    print(f"\n=== {label} -> rc={r.returncode} ({'PASS' if ok else 'FAIL'}){why} ===")
    print((r.stdout + r.stderr).strip()[-1200:] or "(no output)")

    if expect_fail_sub is None and r.returncode == 0:
        m = (d / "merged.bin").read_bytes()
        print(f"merged size = {len(m)} (expect {0x400000})")
        ok = ok and len(m) == 0x400000
        for name, o in OFFSETS.items():
            src = (d / "littlefs.bin") if name == "littlefs.bin" else (build / name)
            src = src.read_bytes()
            same = m[o:o + len(src)] == src
            print(f"  {name:18} @0x{o:06x} {'ok' if same else 'MISMATCH'}")
            ok = ok and same
        # Padding between partitions must be erased flash, or the image is
        # carrying junk into gaps nobody asked us to write.
        gap_ok = m[0x9000:0xE000] == b"\xff" * 0x5000
        tail_ok = m[0x110000 + LFS_SIZE:] == b"\xff" * 0x10000
        print(f"  nvs gap is 0xff    : {gap_ok}")
        print(f"  coredump is 0xff   : {tail_ok}")
        ok = ok and gap_ok and tail_ok
    return ok


results = [
    build_case(tempfile.mkdtemp(), label="nominal (real sizes)"),
    build_case(tempfile.mkdtemp(),
               sizes=dict(FILL, **{"AcidBox.bin": 0x100001}),
               label="app overruns app0 by 1 byte",
               expect_fail_sub="only 1048576 available"),
]

miss = dict(FILL)
del miss["boot_app0.bin"]
results.append(build_case(tempfile.mkdtemp(), sizes=miss,
                          label="boot_app0.bin missing",
                          expect_fail_sub="boot_app0.bin missing"))

results.append(build_case(tempfile.mkdtemp(),
                          label="FQBN with no PartitionScheme",
                          fqbn="esp32:esp32:esp32s3:PSRAM=opi",
                          expect_fail_sub="no PartitionScheme= in FQBN"))

results.append(build_case(tempfile.mkdtemp(),
                          label="scheme CSV absent from the core",
                          fqbn="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=nosuch",
                          expect_fail_sub="nosuch.csv not found"))

results.append(build_case(tempfile.mkdtemp(),
                          label="FS_PARTITION_LABEL not in the CSV",
                          csv_text=CSV.replace("spiffs", "littlefs"),
                          expect_fail_sub="no partition labelled"))

results.append(build_case(tempfile.mkdtemp(),
                          label="no application binary in the build path",
                          sizes={k: v for k, v in FILL.items() if k != "AcidBox.bin"},
                          expect_fail_sub="found none"))

results.append(build_case(tempfile.mkdtemp(),
                          label="two candidates for the app binary",
                          extra_bins=("SomethingElse.bin",),
                          expect_fail_sub="found ['AcidBox.bin', 'SomethingElse.bin']"))

print("\n" + ("ALL PASS" if all(results) else "SOME FAILED"))
sys.exit(0 if all(results) else 1)