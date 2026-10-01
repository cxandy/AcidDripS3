"""Exercise the merged-image step from .github/workflows/build.yml.

The merge step builds the single 4 MiB image that gets flashed at 0x0, so a
bug in it ships a corrupt image rather than an error. CI re-reads the image
and compares every byte, which catches a broken merge -- but only after a
full arduino-cli build. This harness pulls the same script straight out of
the YAML (so the two cannot drift) and runs it against synthetic inputs in
a second, including the failure paths.

    python tools/test-merge-image.py

The first version of this script filtered the "# Name, Type, ..." header by
dropping comment *cells* rather than comment *rows*, which silently turned
the header into a partition called Type at offset Size. That only showed up
because the nominal case is asserted on content, not just on exit code.
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

y = pathlib.Path(".github/workflows/build.yml").read_text(encoding="utf-8")
m = re.search(r"<<'PY'\n(.*?)\n[ ]*PY", y, re.S)
if not m:
    sys.exit("heredoc not found in build.yml")
script = m.group(1)

# Bash strips the common leading indentation of a <<'PY' heredoc. Reproduce that
# so what we test is what the runner executes.
lines = script.split("\n")
nonblank = [l for l in lines if l.strip()]
indent = min(len(l) - len(l.lstrip()) for l in nonblank)
script = "\n".join(l[indent:] if l.strip() else "" for l in lines)

work = pathlib.Path(tempfile.mkdtemp())
script_path = work / "_merge_script.py"
script_path.write_text(script, encoding="utf-8")
print(f"extracted {len(script)} chars, de-indented by {indent}")

CSV = """# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x100000,
spiffs,   data, spiffs,  0x110000,0x2E0000,
coredump, data, coredump,0x3F0000,0x10000,
"""

FILL = {
    "bootloader.bin": 0x8000,
    "partitions.bin": 0x1000,
    "boot_app0.bin": 0x2000,
    "firmware.bin": 0x96000,      # 616724, the real size
    "littlefs.bin": 0x2E0000,    # full partition, as mklittlefs pads it
}

FQBN = "esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=noota_3g,FlashSize=16M"


def build_case(tmp, sizes=None, label="", expect_fail_sub=None, fqbn=FQBN,
               csv_text=None):
    sizes = sizes or FILL
    d = pathlib.Path(tmp)
    build = d / "build"
    # The script globs <core_root>/*/tools/partitions/<scheme>.csv, so mirror
    # the installed core's layout rather than handing it a flat path.
    pdir = d / "core" / "3.3.12" / "tools" / "partitions"
    pdir.mkdir(parents=True, exist_ok=True)
    build.mkdir(parents=True, exist_ok=True)
    (pdir / "noota_3g.csv").write_text(csv_text or CSV, encoding="utf-8")
    for name, size in sizes.items():
        if name == "littlefs.bin":
            (d / name).write_bytes(bytes(range(256)) * (size // 256) + b"\x00" * (size % 256))
        else:
            (build / name).write_bytes(bytes((i * 7 + 3) & 0xFF for i in range(size)))
    env = dict(os.environ, FS_PARTITION_LABEL="spiffs")
    r = subprocess.run(
        [sys.executable, str(script_path),
         fqbn, str(d / "core"), str(build), str(d / "littlefs.bin"),
         str(d / "merged.bin")],
        capture_output=True, text=True, env=env,
    )
    out = (r.stdout + r.stderr).strip()

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
    print(out[-1400:] if out else "(no output)")

    if expect_fail_sub is None and r.returncode == 0:
        m = pathlib.Path(d / "merged.bin").read_bytes()
        print(f"merged size = {len(m)} (expect {0x400000})")
        if len(m) != 0x400000:
            ok = False
        off = {"bootloader.bin": 0x0, "partitions.bin": 0x8000,
               "boot_app0.bin": 0xE000, "firmware.bin": 0x10000,
               "littlefs.bin": 0x110000}
        for name, o in off.items():
            src = (d / name).read_bytes() if name == "littlefs.bin" else (build / name).read_bytes()
            same = m[o:o + len(src)] == src
            print(f"  {name:18} @0x{o:06x} {'ok' if same else 'MISMATCH'}")
            ok = ok and same
        # the 0x5000 gap between partitions must be erased flash
        gap_ok = m[0x9000:0xE000] == b"\xff" * 0x5000
        print(f"  nvs gap is 0xff    : {gap_ok}")
        ok = ok and gap_ok
        # tail past littlefs (coredump) must be untouched too
        tail_ok = m[0x110000 + 0x2E0000:] == b"\xff" * 0x10000
        print(f"  coredump is 0xff   : {tail_ok}")
        ok = ok and tail_ok
    return ok


results = []
results.append(build_case(tempfile.mkdtemp(), label="nominal (real sizes)"))
results.append(build_case(tempfile.mkdtemp(),
                          sizes=dict(FILL, **{"firmware.bin": 0x100001}),
                          label="firmware overruns app0 by 1 byte",
                          expect_fail_sub="only 1048576 available"))
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
                          fqbn="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=nosuchscheme",
                          expect_fail_sub="nosuchscheme.csv not found"))
results.append(build_case(tempfile.mkdtemp(),
                          label="FS_PARTITION_LABEL not in the CSV",
                          csv_text=CSV.replace("spiffs", "littlefs"),
                          expect_fail_sub="no partition labelled"))

print("\n" + ("ALL PASS" if all(results) else "SOME FAILED"))
sys.exit(0 if all(results) else 1)