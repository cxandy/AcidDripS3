"""Check PIN_PLAN.md's arithmetic against the constraints it cites.

A plan whose sums are wrong is worse than no plan, because it looks authoritative. So the
pool is derived from the module's pin list, the assignment is read back out of the doc's
table, and the matrix layout is checked for coverage AND for the no-ghosting property.

The no-ghosting check is the one that matters: it re-derives which of V5's chords are genuine
simultaneous presses (from && vs || in V5's own source) and asserts every one of them lands
inside a single matrix row line. If someone reorders the layout, this fails loudly instead of
producing a board that ghosts.
"""

import io
import os
import re
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
V5 = (r"D:\coder\MIDI\ACID-DRIP-ESP32\vendor\AcidDrip\src"
      r"\Acid_Drip_Drum_Acid_Drift_V5\Acid_Drip_Drum_Acid_Drift_V5.ino")

fails = []
total = 0


def check(ok, label, detail=""):
    global total
    total += 1
    print("  %s  %s%s" % ("PASS" if ok else "FAIL", label,
                          ("  -- " + detail) if detail else ""))
    if not ok:
        fails.append(label)


# Read once, up front: section 2.2 (the reserve table) is checked before section 6 (the
# assignment table), so the document cannot be loaded halfway down. A file that is not where
# this expects is reported, not crashed on -- same convention as tools/test-sequencer.py:
# "解析不了" must say so, because a crash and a clean run look identical otherwise.
DOC_PATH = os.path.join(ROOT, "PIN_PLAN.md")
CFG_PATH = os.path.join(ROOT, "firmware", "AcidBox", "config.h")


def read(path):
    with io.open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


missing = [p for p in (DOC_PATH, CFG_PATH, V5) if not os.path.exists(p)]
if missing:
    print("  CANNOT RUN: missing %s" % ", ".join(missing))
    print()
    print("  PIN_PLAN.md and config.h live in this repository and must be found; the V5")
    print("  reference is outside it and its path is hard-coded, the same way")
    print("  tools/test-sequencer.py hard-codes it.")
    print("SUITE COMPLETE")
    sys.exit(0)

doc = read(DOC_PATH)
check(True, "PIN_PLAN.md found", DOC_PATH)


# ---------------------------------------------------------------- the pool
# WROOM-1 datasheet Table 3-1: these are the GPIOs the module actually breaks out.
# GPIO33/34 and GPIO46 are absent from that table -- they are NOT exposed.
EXPOSED = set(range(0, 22)) | {35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 47, 48}

RESERVED = {
    19: "USB-OTG D- (only USB connector on this board)",
    20: "USB-OTG D+ (only USB connector on this board)",
    0: "BOOT button / strapping",
    3: "strapping: JTAG signal source",
    45: "strapping: VDD_SPI 1.8V vs 3.3V",
    35: "octal PSRAM (SPIIO6)",
    36: "octal PSRAM (SPIIO7)",
    37: "octal PSRAM (SPIDQS / FSPID)",
}
POOL = sorted(EXPOSED - set(RESERVED))

print("=== pool ===")
check(len(EXPOSED) == 35, "WROOM-1 exposes 35 GPIOs", "got %d" % len(EXPOSED))
check(33 not in EXPOSED and 34 not in EXPOSED, "GPIO33/34 not exposed on WROOM-1")
check(46 not in EXPOSED, "GPIO46 not exposed on WROOM-1")
check(26 not in EXPOSED and 32 not in EXPOSED, "GPIO26-32 not exposed on WROOM-1")
check(len(RESERVED) == 8, "8 pins reserved", "got %d" % len(RESERVED))
check(len(POOL) == 27, "pool is 27 pins", "got %d" % len(POOL))

# no reserved pin may leak into the pool
leaked = sorted(set(POOL) & set(RESERVED))
check(not leaked, "no reserved pin is in the pool", "leaked: %s" % leaked)

# The document's own reserve table has to name every pin that must be reserved. Checked
# against the doc rather than only against RESERVED above, because a table that quietly
# loses its octal-PSRAM row is exactly the edit that would go unnoticed.
_sec22 = doc.split("### 2.2")[1].split("### 2.3")[0]
# Parse the TABLE ROWS only, and only the ones whose count column is a number. The table uses
# the count to distinguish the two very different statements it makes:
#     | **GPIO35 / GPIO36 / GPIO37** | 3 | occupied by octal PSRAM |   <- reserved
#     | **GPIO26-GPIO32**           | - | not broken out          |   <- does not exist
# And prose is not a table: GPIO43/44 are named in the paragraph below as "only if you bring
# them out yourself", which is neither. Two earlier versions of this check parsed the whole
# section as a flat list and were wrong in exactly those two ways.
DOC_RESERVED, DOC_NOT_EXPOSED = set(), set()
for line in _sec22.splitlines():
    m = re.match(r"\|\s*\*\*(GPIO[^|]*?)\*\*\s*\|\s*([^|]+?)\s*\|", line)
    if not m:
        continue
    pins = {int(n) for n in re.findall(r"GPIO(\d+)", m.group(1))}
    if not pins:
        continue
    (DOC_RESERVED if m.group(2).strip().isdigit() else DOC_NOT_EXPOSED).update(pins)

MUST_RESERVE = {0, 3, 19, 20, 45, 35, 36, 37}
check(MUST_RESERVE <= DOC_RESERVED,
      "the doc's section 2.2 names every pin that must be reserved",
      "missing: %s" % sorted(MUST_RESERVE - DOC_RESERVED))
# Restricted to pins that are actually exposed, section 2.2 must name exactly the reserved
# set. GPIO46 has a count of 1 but is not broken out, so it drops out here.
check(DOC_RESERVED & EXPOSED == set(RESERVED),
      "section 2.2's exposed pins are exactly the reserved set",
      "doc-only: %s / script-only: %s"
      % (sorted((DOC_RESERVED & EXPOSED) - set(RESERVED)),
         sorted(set(RESERVED) - (DOC_RESERVED & EXPOSED))))
# and the ones the table marks "not broken out" really are not
check(bool(DOC_NOT_EXPOSED) and not (DOC_NOT_EXPOSED & EXPOSED),
      "the pins section 2.2 calls unexposed really are unexposed",
      "claims unexposed: %s, of which are actually exposed: %s"
      % (sorted(DOC_NOT_EXPOSED), sorted(DOC_NOT_EXPOSED & EXPOSED)))
check(not (DOC_RESERVED & DOC_NOT_EXPOSED),
      "no pin is both reserved and unexposed")


# ---------------------------------------------------------------- the assignment
# Read the recommended table back out of the doc, so the doc cannot drift from the plan.
#
# Only section 6 is parsed. The doc has two tables with the same column names: section 3
# counts what V5 needs, section 6 assigns what we use. Parsing both merged them into one
# nonsense set the first time -- which is why this slices the doc first.
sec = doc.split("## 6. 推荐分配表")[1].split("## 7.")[0]

ASSIGNED = {}
# Role names start with Chinese ("矩阵行 R0"), so the leading-char class has to allow CJK.
for line in sec.splitlines():
    m = re.match(r"\|\s*(?![*\-|])([^\d|][^|]*?)\s*\|\s*\*{0,2}([\d,\s]+)\*{0,2}\s*\|\s*\d+\s*\|", line)
    if not m:
        continue
    role = m.group(1).strip()
    pins = [int(x) for x in m.group(2).replace(" ", "").split(",") if x]
    if pins and role not in ("合计", "余量", "保留（救砖）"):
        ASSIGNED[role] = pins

assigned_pins = [p for pins in ASSIGNED.values() for p in pins]
SPARE = [8, 9, 10, 21]
UART_RSV = [43, 44]

print("\n=== assignment ===")
check(len(assigned_pins) == len(set(assigned_pins)), "no pin assigned twice",
      "dupes: %s" % sorted({p for p in assigned_pins if assigned_pins.count(p) > 1}))
check(len(assigned_pins) == 21, "21 pins assigned", "got %d" % len(assigned_pins))
check(sorted(SPARE + UART_RSV + assigned_pins) == POOL,
      "assigned + spare + UART reserve == pool exactly",
      "assigned %d + spare %d + uart %d = %d, pool %d"
      % (len(assigned_pins), len(SPARE), len(UART_RSV),
         len(assigned_pins) + len(SPARE) + len(UART_RSV), len(POOL)))
check(not (set(SPARE) & set(assigned_pins)), "spare pins are unassigned")

# every assignment role the doc promises must be present with the right pin count
EXPECT = {"I2S": 3, "POT": 3, "TFT": 6, "MIDI": 1, "R": 4, "C": 4}
got = {
    "I2S": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("I2S")),
    "POT": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("POT")),
    "TFT": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("TFT")),
    "MIDI": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("MIDI")),
    "R": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("矩阵行")),
    "C": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("矩阵列")),
}
check(got == EXPECT, "block sizes match the doc's table", "%s vs %s" % (got, EXPECT))
check(got["I2S"] + got["POT"] + got["TFT"] + got["MIDI"] + got["R"] + got["C"] == 21,
      "blocks sum to 21")

# ADC1 is GPIO1-10 on the S3 -- pots MUST be inside it
pots = [p for k, v in ASSIGNED.items() if k.startswith("POT") for p in v]
check(all(1 <= p <= 10 for p in pots), "all pots are on ADC1 (GPIO1-10)", str(pots))
check(not (set(pots) & set(range(11, 21))), "no pot is on ADC2")
check(sorted(SPARE[:3]) == [8, 9, 10], "spare 8/9/10 are ADC1 channels for future pots")

# I2S must stay put -- it is the verified audio path
i2s = sorted(p for k, v in ASSIGNED.items() if k.startswith("I2S") for p in v)
check(i2s == [5, 6, 7], "I2S stays on 5/6/7", str(i2s))

# matrix columns must not collide with the audio path or the pots
cols = [p for k, v in ASSIGNED.items() if k.startswith("矩阵列") for p in v]
check(not (set(cols) & (set(pots) | set(i2s))), "matrix columns clear of pots and I2S")
check(not (set(cols) & set(SPARE)), "matrix columns are not listed as spare")


# ---------------------------------------------------------------- V5 chords
# Which of V5's pad combinations are genuine simultaneous presses? The difference between
# || and && is the whole no-ghosting argument, so it is re-read from V5's source rather
# than from the doc that quotes it.
v5 = read(V5)


def v5_index(name):
    m = re.search(r"#define\s+%s\s+(\d+)" % name, v5)
    return int(m.group(1)) if m else None


# (chord, pad indices, source line, is_genuine_simultaneous)
CHORDS = [
    ("PLAY",   [0, 1],                 ":482-483", True),
    ("FUNC",   [6, 7],                 ":484-485", True),
    ("BM_SW",  [8, 9],                 ":242-243", True),
    ("MIX",    [14, 15],               ":373-374", True),
    ("CHAIN",  [v5_index("WALK_PAD_A"), v5_index("WALK_PAD_D")], ":5213", True),
    ("WALK",   [v5_index("WALK_PAD_A"), v5_index("WALK_PAD_B"),
                v5_index("WALK_PAD_C"), v5_index("WALK_PAD_D")], ":5233-5234", True),
    ("SET",    [v5_index("SET_PAD_A"), v5_index("SET_PAD_B")], ":5277", False),
]

print("\n=== V5 chords (re-derived from source) ===")
for name, idx, where, genuine in CHORDS:
    check(all(i is not None for i in idx), "%s: pad indices resolve in V5" % name, str(idx))
    pads = [i + 1 for i in idx if i is not None]
    check(pads == sorted(pads), "%s: pads %s in ascending order" % (name, pads))

# the SET row really does use || -- verify the doc's most load-bearing claim
set_line = [l for l in v5.splitlines() if "SET_PAD_A" in l and "digitalRead" in l]
check(len(set_line) == 1, "SET's condition is one line", "found %d" % len(set_line))
if set_line:
    check("||" in set_line[0], "SET really is || (a disjunction, not a chord)",
          set_line[0].strip()[:80])

walk_line = [l for l in v5.splitlines() if "WALK_PAD_B" in l and "digitalRead" in l]
check(any("||" in l for l in walk_line), "WALK's disarm condition uses ||")
check("Disarm if any of the four pads released" in v5,
      "V5 says all four WALK pads must be held -- so it IS a 4-key chord")


# ---------------------------------------------------------------- matrix layout
# PARSED OUT OF THE DOC, not written here. The first version of this file hard-coded the
# layout, which meant the ghosting checks validated the checker's own copy rather than the
# plan: mutation "PLAY straddles two row lines" -- which edits the doc -- passed straight
# through. Same shape as §6.8.9's "the assertion was about a slice that did not contain the
# thing asserted", one layer down.
#
# The doc draws it as:
#         C0    C1    C2    C3
#   R0:   pad1  pad2  pad7  pad8     <- PLAY(1+2), FUNC(7+8)
LAYOUT = {}
for line in doc.splitlines():
    m = re.match(r"\s*R(\d):((?:\s+pad\d+)+)", line)
    if m:
        LAYOUT[int(m.group(1))] = [int(x) for x in re.findall(r"pad(\d+)", m.group(2))]
check(bool(LAYOUT), "matrix layout parsed out of the doc", str(LAYOUT))

print("\n=== matrix layout ===")
covered = [p for row in LAYOUT.values() for p in row]
check(sorted(covered) == list(range(1, 17)), "all 16 pads placed exactly once",
      "got %d cells" % len(covered))
check(all(len(row) == 4 for row in LAYOUT.values()), "every row line has exactly 4 pads")
check(len(LAYOUT) == 4, "4 row lines (4x4 matrix uses 8 pins)")

row_of = {p: r for r, row in LAYOUT.items() for p in row}

for name, idx, where, genuine in CHORDS:
    if not genuine:
        continue
    pads = [i + 1 for i in idx]
    rows = {row_of[p] for p in pads}
    check(len(rows) == 1,
          "%s (%s) sits in ONE row line -> no ghosting" % (name, "+".join(map(str, pads))),
          "spans rows %s" % sorted(rows))

genuine_rows = {row_of[p] for _, idx, _, g in CHORDS if g for p in [i + 1 for i in idx]}
free_rows = sorted(set(LAYOUT) - genuine_rows)
check(len(free_rows) == 1, "exactly one row line carries no chord at all",
      "free: %s" % free_rows)
if free_rows:
    free_pads = sorted(LAYOUT[free_rows[0]])
    check(free_pads == [3, 4, 5, 6], "that row is pads 3-6, as the doc claims",
          str(free_pads))


# ---------------------------------------------------------------- current-tree defects
print("\n=== defects claimed in section 9 ===")
cfg = read(CFG_PATH)


def strip_comments(text):
    """Drop // comments before reading a C initialiser list.

    PAD_PINS reads {1,2,3,...} with a trailing `// pads 1-8` comment. Without this, the
    comment's digits are parsed as pins and the array comes out with 24 entries instead of
    16 -- which is exactly what happened on the first run.
    """
    out = []
    for line in text.splitlines():
        i = line.find("//")
        out.append(line if i < 0 else line[:i])
    return "\n".join(out)


cfg_code = strip_comments(cfg)

pad_m = re.search(r"PAD_PINS\[NUM_PADS\]\s*=\s*\{(.*?)\};", cfg_code, re.S)
check(pad_m is not None, "PAD_PINS[] found")
if pad_m:
    pads = [int(x) for x in re.findall(r"\d+", pad_m.group(1))]
    check(len(pads) == 16, "PAD_PINS has 16 entries (comments stripped)", "got %d" % len(pads))
    check(pads[2] == 3, "pad 3 (index 2) really is GPIO3 -- the strapping-pin defect")
    check(len(pads) == len(set(pads)), "PAD_PINS has no internal duplicate")
    # The next three assert DEFECTS, not health. They are written to fail if the defect is
    # ever fixed by accident -- which is what makes them worth keeping afterwards.
    check(set(pads) & {3}, "GPIO3 really is in PAD_PINS (section 9.1's claim)")
    check(not (set(pads) & set([35, 36, 37, 19, 20, 0, 45])),
          "PAD_PINS avoids the PSRAM / USB / BOOT pins it must",
          "collides: %s" % sorted(set(pads) & {35, 36, 37, 19, 20, 0, 45}))
    check(not (set(pads) & set([5, 6, 7])), "current PAD_PINS avoids the I2S pins")

pot_m = re.search(r"POT_PINS\[POT_NUM\]\s*=\s*\{([^}]*)\};", cfg_code)
check(pot_m is not None, "POT_PINS[] found")
pots_now = [int(x) for x in re.findall(r"\d+", pot_m.group(1))]
check(pots_now == [15, 16, 17], "current POT_PINS is {15,16,17}", str(pots_now))
check(all(p > 10 for p in pots_now), "every current pot is on ADC2, not ADC1 -- must move")

mt = re.search(r"#define\s+MIDITX_PIN\s+(\d+)", cfg_code)
check(mt is not None, "MIDITX_PIN found")
if mt and pot_m:
    tx = int(mt.group(1))
    check(tx in pots_now, "MIDITX_PIN really does collide with POT_PINS (section 9.2)",
          "MIDITX=%d, pots=%s" % (tx, pots_now))

# Four DEBUG_PORT definitions appear in the text: Serial0, plus one inside each arm of the
# ESP_ARDUINO_VERSION_MAJOR #if/#else, plus the HWCDCSerial override. Only the LAST one is
# what compiles under FQBN USBMode=hwcdc,CDCOnBoot=cdc -- so that is what gets asserted.
dbg = re.findall(r"#\s*define\s+DEBUG_PORT\s+(\w+)", cfg_code)
check(bool(dbg), "DEBUG_PORT is defined somewhere")
if dbg:
    check(dbg[-1] == "HWCDCSerial",
          "the LAST DEBUG_PORT -- the one that actually compiles -- is HWCDCSerial",
          "all: %s, last: %s" % (dbg, dbg[-1]))
check(re.search(r"#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT\s*\n\s*#undef DEBUG_PORT",
                cfg_code) is not None,
      "that last DEBUG_PORT is the one guarded by USBMode + CDCOnBoot, both set in the FQBN")
check(len(dbg) >= 2, "config.h really does contain the contradiction section 9.4 describes",
      "%d textual definitions" % len(dbg))

# pads and pots are separate physical controls, so they must not share a pin. Section 9.2 is
# about a DIFFERENT collision (POT_PINS vs MIDITX_PIN); conflating the two was the first
# version of this check, and it asserted something the document never claimed.
if pad_m:
    check(not (set(pads) & set(pots_now)), "no pad shares a pin with a pot",
          "intersection: %s" % sorted(set(pads) & set(pots_now)))

# every pin the doc's matrix implies must be one the pool actually has
matrix_pins = sorted([p for k, v in ASSIGNED.items()
                      if k.startswith("矩阵") for p in v])
check(len(matrix_pins) == 8, "8 matrix pins parsed out of the doc's table",
      "got %d" % len(matrix_pins))
check(all(p in POOL for p in matrix_pins), "all 8 matrix pins are in the pool",
      str([p for p in matrix_pins if p not in POOL]))
check(not (set(matrix_pins) & set(assigned_pins) - set(matrix_pins)),
      "matrix pins are not double-booked against other roles")

print("\n" + "=" * 60)
print("%d checks, %d failed" % (total, len(fails)))
if fails:
    for f in fails:
        print("  FAILED: %s" % f)
print("SUITE COMPLETE")
sys.exit(0)