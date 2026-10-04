"""Check PIN_PLAN.md's arithmetic against the constraints it cites.

A plan whose sums are wrong is worse than no plan, because it looks authoritative. So the
pool is derived from the module's pin list, the assignment is read back out of the doc's
table, and V5's chord table is re-derived from V5's own source rather than trusted.

Two things this file deliberately does NOT do any more:

  * It does not hard-code the pad-to-pin mapping. The mapping is read out of the doc's
    table, because an earlier revision hard-coded it and the ghosting checks then validated
    the checker's own copy instead of the plan -- a doc edit passed straight through.
  * It does not check for ghosting. The matrix was dropped in favour of one-pin-per-pad
    (see PIN_PLAN.md section 4.2), so there is no layout to check. What is left is the
    pigeonhole count that killed the "zero diodes" claim, plus a check that the doc still
    carries it as a retired finding rather than quietly dropping it.
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


def cells(line):
    """Split a markdown table row into stripped cells.

    Replaced a single large regex, which was wrong in a way that mattered: its count column
    only accepted bare digits, so the spare and total rows -- whose counts are **2** and
    **28** -- never parsed at all and silently stayed out of the numbers. A parser that drops
    rows is more dangerous than one that crashes, because the totals still look right.
    """
    line = line.strip()
    if not line.startswith("|"):
        return []
    return [c.strip() for c in line.strip("|").split("|")]


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

# Five pins are genuinely unavailable. An earlier revision reserved eight, counting GPIO0/3/45
# as unusable strapping pins. Section 2.5 works out that each of those three is usable as a
# switch-to-GND pad input, so they moved out of this set and into the pool -- which is where
# two of the three spare pins came from.
RESERVED = {
    19: "USB-OTG D- (only USB connector on this board)",
    20: "USB-OTG D+ (only USB connector on this board)",
    35: "octal PSRAM (SPIIO6)",
    36: "octal PSRAM (SPIIO7)",
    37: "octal PSRAM (SPIDQS / FSPID)",
}
POOL = sorted(EXPOSED - set(RESERVED))

# Strapping pins, and the default the datasheet gives them (Table 3-1). These are usable, so
# the assertions below are that they ARE in the pool -- an edit that quietly re-reserves one
# of them would shrink the pool by a pin and the 28+2==30 sum would fail, but the failure
# message would not say why.
STRAPPING = (0, 3, 45)

print("=== pool ===")
check(len(EXPOSED) == 35, "WROOM-1 exposes 35 GPIOs", "got %d" % len(EXPOSED))
check(33 not in EXPOSED and 34 not in EXPOSED, "GPIO33/34 not exposed on WROOM-1")
check(46 not in EXPOSED, "GPIO46 not exposed on WROOM-1")
check(26 not in EXPOSED and 32 not in EXPOSED, "GPIO26-32 not exposed on WROOM-1")
check(len(RESERVED) == 5, "5 pins reserved", "got %d" % len(RESERVED))
check(len(POOL) == 30, "pool is 30 pins", "got %d" % len(POOL))
check(all(p in POOL for p in STRAPPING),
      "GPIO0/3/45 are IN the pool -- section 2.5 found them usable as pad inputs")

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
# And prose is not a table. Two earlier versions of this check parsed the whole section as a
# flat list and were wrong in exactly those two ways.
DOC_RESERVED, DOC_NOT_EXPOSED = set(), set()
for line in _sec22.splitlines():
    m = re.match(r"\|\s*\*\*(GPIO[^|]*?)\*\*\s*\|\s*([^|]+?)\s*\|", line)
    if not m:
        continue
    pins = {int(n) for n in re.findall(r"GPIO(\d+)", m.group(1))}
    if not pins:
        continue
    (DOC_RESERVED if m.group(2).strip().isdigit() else DOC_NOT_EXPOSED).update(pins)

MUST_RESERVE = {19, 20, 35, 36, 37}
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
# The one that carries the plan's headroom: section 2.2 must NOT list the strapping pins as
# reserved any more. Reserving them again would silently cost a pin of margin.
check(not (DOC_RESERVED & set(STRAPPING)),
      "section 2.2 does not re-reserve GPIO0/3/45",
      "re-reserved: %s" % sorted(DOC_RESERVED & set(STRAPPING)))


# ---------------------------------------------------------------- the assignment
# Read the table back out of the doc, so the doc cannot drift from the plan.
#
# Only section 6 is parsed. The doc has two tables with the same column names: section 3
# counts what V5 needs, section 6 assigns what we use. Parsing both merged them into one
# nonsense set the first time -- which is why this slices the doc first.
sec = doc.split("## 6. 分配表")[1].split("## 7.")[0]

ASSIGNED = {}
SKIP = ("合计", "余量", "排针", "非功能脚")
skipped = []
doc_total = doc_spare_pins = doc_spare_count = None

for line in sec.splitlines():
    c = cells(line)
    if len(c) < 3:
        continue
    role, pincell, countcell = c[0], c[1], c[2]
    if not role or set(role) <= set("*-| :"):
        continue
    pins = [int(x) for x in re.findall(r"\d+", pincell)]
    if not re.search(r"\d", countcell):
        continue
    # SKIP is tested BEFORE the pin count, because the 合计 row has an EMPTY pin cell and so
    # never reaches the assignment branch. Checking the count first dropped it silently --
    # which is how "skipped has 3 entries" can fail while every assigned number still looks
    # correct.
    if any(k in role for k in SKIP):
        skipped.append(role)
        n = re.search(r"\d+", countcell)
        # The two non-functional rows carry numbers too, and they are read back here rather
        # than left unchecked: a doc saying "合计 26" above 28 rows of pins is exactly the
        # arithmetic error this file exists to catch, and none of the sums below can see it.
        if "合计" in role:
            doc_total = int(n.group(0)) if n else None
        if "余量" in role:
            doc_spare_pins, doc_spare_count = pins, (int(n.group(0)) if n else None)
    elif pins:
        ASSIGNED[role] = pins

# The skip list has to stay exhaustive. If a row lands in the wrong bucket -- a header being
# counted as a functional pin, say -- then "assigned == 28" would be satisfied by the wrong
# thing. Pinning the count means a future row cannot quietly join the skipped set.
check(len(skipped) == 2,
      "section 6's two non-functional rows (total / spare) are excluded",
      "skipped: %s" % skipped)
# The doc's own numbers, read back. "合计 28" is a claim about the rows above it, so it has to
# agree with them; likewise the spare row's count against how many pins it lists. Asserting
# assigned==28 alone would not notice a wrong 合计 -- the error would sit in the document
# forever, looking authoritative.
check(doc_total is not None,
      "section 6's total row is parseable")
check(doc_total == 28, "section 6's total row says 28", "says %s" % doc_total)

assigned_pins = [p for pins in ASSIGNED.values() for p in pins]
SPARE = [47, 48]

print("\n=== assignment ===")
check(len(assigned_pins) == len(set(assigned_pins)), "no pin assigned twice",
      "dupes: %s" % sorted({p for p in assigned_pins if assigned_pins.count(p) > 1}))
check(len(assigned_pins) == 28, "28 pins assigned", "got %d" % len(assigned_pins))
check(sorted(SPARE + assigned_pins) == POOL,
      "assigned + spare == pool exactly",
      "assigned %d + spare %d = %d, pool %d"
      % (len(assigned_pins), len(SPARE),
         len(assigned_pins) + len(SPARE), len(POOL)))
check(not (set(SPARE) & set(assigned_pins)), "spare pins are unassigned")
# SPARE above is this script's own copy. The doc has to say the same thing, or the
# assigned+spare==pool sum below is checking the script's assumption rather than the plan.
check(doc_spare_pins == SPARE,
      "the doc's spare row lists the pins this script assumes are spare",
      "doc: %s / script: %s" % (doc_spare_pins, SPARE))
check(doc_spare_count == len(doc_spare_pins or []),
      "the spare row's count matches how many pins it lists",
      "count %s, pins %s" % (doc_spare_count, doc_spare_pins))
check(doc_total is not None and doc_total == len(assigned_pins),
      "the total row agrees with the sum of the rows above it",
      "total says %s, rows sum to %d" % (doc_total, len(assigned_pins)))

# every assignment role the doc promises must be present with the right pin count
EXPECT = {"I2S": 3, "POT": 3, "TFT": 4, "MIDI": 2, "pad": 16}
got = {
    "I2S": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("I2S")),
    "POT": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("POT")),
    "TFT": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("TFT")),
    "MIDI": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("MIDI")),
    "pad": sum(len(v) for k, v in ASSIGNED.items() if k.startswith("pad")),
}
check(got == EXPECT, "block sizes match the doc's table", "%s vs %s" % (got, EXPECT))
check(sum(got.values()) == 28, "blocks sum to 28")

# ADC1 is GPIO1-10 on the S3 -- pots MUST be inside it
pots = sorted(p for k, v in ASSIGNED.items() if k.startswith("POT") for p in v)
check(all(1 <= p <= 10 for p in pots), "all pots are on ADC1 (GPIO1-10)", str(pots))
check(not (set(pots) & set(range(11, 21))), "no pot is on ADC2")

# I2S must stay put -- it is the verified audio path
i2s = sorted(p for k, v in ASSIGNED.items() if k.startswith("I2S") for p in v)
check(i2s == [5, 6, 7], "I2S stays on 5/6/7", str(i2s))

# TFT lost two pins: RST goes to EN and the backlight to 3V3, so neither may reappear as a
# role. If either does, the 28+2==30 sum would have to come from somewhere else.
check(not any("BL" in k or "背光" in k for k in ASSIGNED),
      "no backlight role in the table (backlight is tied to 3V3)",
      str([k for k in ASSIGNED if "BL" in k or "背光" in k]))
check(not any("RST" in k for k in ASSIGNED),
      "no TFT RST role in the table (RST is tied to EN)",
      str([k for k in ASSIGNED if "RST" in k]))

# MIDI must not sit on UART0's pins. The ROM console prints to U0TXD = GPIO43 before any
# firmware runs, so a MIDI output circuit on 43 would emit the boot log as MIDI data.
midi = sorted(p for k, v in ASSIGNED.items() if k.startswith("MIDI") for p in v)
check(43 not in midi and 44 not in midi,
      "MIDI does not sit on GPIO43/44 (the ROM console)", str(midi))
check("U0TXD" in doc and "GPIO43" in doc,
      "section 6 explains why: the ROM console prints to GPIO43")
check(midi == [18, 21], "MIDI is on 18/21 (UART1)", str(midi))

# The spare pair is held back for a stated reason, and the doc has to keep stating it -- if
# 47/48 ever get used, the reason has to be revisited rather than dropped.
check(SPARE == [47, 48], "the spare pair is GPIO47/48")
check("1.8V" in doc, "the doc keeps the 1.8V-domain reason for holding 47/48 back")

# pad -> GPIO, read out of the doc's own table
pad_gpio = {}
for k, v in ASSIGNED.items():
    m = re.match(r"pad (\d+)$", k)
    if m:
        pad_gpio[int(m.group(1))] = v[0]
check(sorted(pad_gpio) == list(range(1, 17)),
      "all 16 pads are placed exactly once", "got %s" % sorted(pad_gpio))
check(all(g in POOL for g in pad_gpio.values()),
      "every pad pin is in the pool",
      str([g for g in pad_gpio.values() if g not in POOL]))


# ---------------------------------------------------------------- V5 chords
# Re-derived from V5's source rather than trusted from the doc: an earlier revision of the
# plan had three of these wrong (CHAIN, SET, DRIFT) and the checker agreed with the doc.
v5 = read(V5)
V5_LINES = v5.splitlines()


def v5_define(name, depth=0):
    """Resolve a V5 #define to its integer, following one level of alias.

    CHAIN_PAD_A is `#define CHAIN_PAD_A WALK_PAD_B`, not a number, so a plain integer
    lookup returns None for it -- and None silently drops pad 12 out of the CHAIN chord.
    """
    m = re.search(r"#define\s+%s\s+(\w+)" % re.escape(name), v5)
    if not m:
        return None
    tok = m.group(1)
    return int(tok) if tok.isdigit() else (v5_define(tok, depth + 1) if depth < 4 else None)


def v5_pads(*names):
    return sorted({v5_define(n) + 1 for n in names if v5_define(n) is not None})


def src(label, span, *needles):
    """Assert a cited V5 line range really contains what the doc says it does."""
    a, b = span
    text = "\n".join(V5_LINES[a - 1:b])
    check(all(n in text for n in needles), label,
          ("lines %d-%d: " % (a, b)) + text.strip().replace("\n", " ")[:70])


EXPECT_CHORD = {
    "PLAY / STOP": v5_pads("PAD_PLAY_A", "PAD_PLAY_B"),
    "BM": v5_pads("BM_SW_A", "BM_SW_B"),
    "DRIFT": v5_pads("BM_SW_A", "BM_SW_B", "LAYER_PAD_C"),
    "ACID WALKS": v5_pads("WALK_PAD_A", "WALK_PAD_B", "WALK_PAD_C", "WALK_PAD_D"),
    "PATTERN CHAIN": v5_pads("CHAIN_PAD_A", "CHAIN_PAD_B"),
    "SETTINGS": v5_pads("SET_PAD_A", "SET_PAD_B"),
    "MIX EDIT": v5_pads("MIX_PAD_A", "MIX_PAD_B"),
    "FUNC": v5_pads("PAD_FUNC_A", "PAD_FUNC_B"),
}

print("\n=== V5 chords (re-derived from source) ===")
check(all(v for v in EXPECT_CHORD.values()), "every chord's pad indices resolve in V5",
      str({k: v for k, v in EXPECT_CHORD.items() if not v}))
# The three corrections the previous revision got wrong, asserted individually because they
# are the ones most likely to be "corrected" back.
check(EXPECT_CHORD["PATTERN CHAIN"] == [12, 13],
      "CHAIN is pads 12+13, not 11+14", str(EXPECT_CHORD["PATTERN CHAIN"]))
check(EXPECT_CHORD["SETTINGS"] == [1, 9], "SETTINGS is pads 1+9", str(EXPECT_CHORD["SETTINGS"]))
check(EXPECT_CHORD["DRIFT"] == [9, 10, 11], "DRIFT is 9+10+11", str(EXPECT_CHORD["DRIFT"]))

# Now read the doc's chord table back and compare. Scoped to section 5.3 and stopped once
# all eight rows are matched, because the correction table immediately below repeats some
# row labels ("DRIFT", "CHAIN", "SET") with different column meanings.
sec53 = doc.split("### 5.3")[1].split("\n## ")[0] if "### 5.3" in doc else ""
DOC_CHORD = {}
for line in sec53.splitlines():
    if len(DOC_CHORD) == len(EXPECT_CHORD):
        break
    c = cells(line)
    if len(c) < 4:
        continue
    label, padcell = c[0].strip(), c[1].replace("*", "")
    nums = [int(x) for x in re.findall(r"\d+", padcell)]
    if not nums:
        continue
    for key in EXPECT_CHORD:
        if label == key or label.startswith(key):
            DOC_CHORD[key] = sorted(set(nums))
            break

check(sorted(DOC_CHORD) == sorted(EXPECT_CHORD),
      "section 5.3's table lists all eight gestures",
      "missing: %s / extra: %s" % (sorted(set(EXPECT_CHORD) - set(DOC_CHORD)),
                                   sorted(set(DOC_CHORD) - set(EXPECT_CHORD))))
for key, expected in sorted(EXPECT_CHORD.items()):
    check(DOC_CHORD.get(key) == expected,
          "section 5.3: %s is pads %s" % (key, "+".join(map(str, expected))),
          "doc says %s" % DOC_CHORD.get(key))
check("**是**" in sec53, "section 5.3 marks all eight as genuine simultaneous presses")

# The "read && vs ||" rule, which is what turns SET from a disjunction into a hold chord.
src("V5:5277 is SET's DISARM -- || there means both pads must be held",
    (5277, 5277), "SET_PAD_A", "SET_PAD_B", "||")
src("V5:5705 SET arming requires the other pad down -- a real chord",
    (5705, 5705), "SET_PAD_A", "pState[SET_PAD_B]")

# Every citation the chord table leans on, verified against the file. A doc that cites a
# line number has to mean it.
src("V5:5651 PLAY arms on 1+2", (5651, 5651), "PAD_PLAY_A", "pState[PAD_PLAY_B]")
src("V5:5660 BM arms on 9+10", (5660, 5660), "BM_SW_A", "pState[BM_SW_B]")
src("V5:1474 the layer fire reads pad 11 -- that is what makes 9+10+11 DRIFT",
    (1474, 1474), "LAYER_PAD_C", "LOW")
src("V5:5685 pad 11 defers while 9/10 is held", (5685, 5685), "LAYER_PAD_C", "pState[BM_SW_A]")
src("V5:5668-5670 WALK requires all four pads down", (5668, 5670),
    "WALK_PAD_A", "WALK_PAD_B", "WALK_PAD_C", "WALK_PAD_D")
src("V5:5695-5696 CHAIN arms on 12+13", (5695, 5696), "CHAIN_PAD_A", "CHAIN_PAD_B")
src("V5:5714 MIX arms on 15+16", (5714, 5714), "MIX_PAD_A", "pState[MIX_PAD_B]")
src("V5:5721 FUNC arms on 7+8", (5721, 5721), "PAD_FUNC_A", "pState[PAD_FUNC_B]")

# Pads no chord touches -- derived here, not copied. This is what decides where the three
# strapping pins may sit.
CHORD_PADS = set()
for v in EXPECT_CHORD.values():
    CHORD_PADS.update(v)
FREE_PADS = sorted(set(range(1, 17)) - CHORD_PADS)
check(FREE_PADS == [3, 4, 5, 6],
      "pads 3/4/5/6 are in no chord at all (derived from V5)", str(FREE_PADS))
check("不参与任何手势的 pad 是 3、4、5、6" in doc,
      "section 5.3 states the chord-free pads the same way")

strapping_pads = {g: p for p, g in pad_gpio.items() if g in STRAPPING}
check(set(strapping_pads) == set(STRAPPING),
      "all three strapping pins carry a pad",
      "mapped: %s" % sorted(strapping_pads))
check(all(p in FREE_PADS for p in strapping_pads.values()),
      "every strapping pin lands on a chord-free pad",
      str(sorted(strapping_pads.items())))


# ---------------------------------------------------------------- retired matrix finding
# The matrix is gone, so there is no layout to check. What stays is the finding that killed
# it, so that "zero diodes" cannot quietly return as a live claim.
print("\n=== section 5: the retired matrix finding ===")
check("### 5" in doc, "section 5 exists")
check("已退役" in doc, "section 5 is labelled a retired finding, not a live requirement")
check("24" in doc, "the pigeonhole count of 24 destructive pairs is kept")
check("C(4,2)" in doc, "the 24 is derived, not asserted (C(4,2) per column)")
check("300 个随机布局" in doc, "the 24-pair minimum was measured over 300 random layouts")
check("与 pad 怎么排无关" in doc or "与布局无关" in doc,
      "the doc says the count is layout-independent")
check("二极管" in doc and "必需品" in doc,
      "diodes are stated as mandatory for any matrix, not optional")
check("不是本方案的必要条件" in doc,
      "section 5 says explicitly that it is not a requirement of this plan")


# ---------------------------------------------------------------- PCB hard rules
# These are the rules that, if dropped from the schematic, produce a board that does not
# boot (GPIO45 pulled high) or burns a pin (MIDI RX at 5V). Cheap to state, easy to lose.
print("\n=== section 2.5 / 6: PCB rules ===")
check("### 2.5" in doc, "section 2.5 exists -- the strapping pins are costed")
check("禁止任何上拉" in doc or "禁止上拉" in doc,
      "GPIO45's net is forbidden a pull-up (it would make the chip unbootable)")
check(re.search(r"GPIO3\s*加\s*10k\s*上拉", doc) is not None,
      "GPIO3 gets a 10k pull-up (its default is Floating)")
check(re.search(r"GPIO0\s*加\s*10k\s*上拉", doc) is not None,
      "GPIO0 gets a 10k pull-up (strapping needs a defined high)")
check("按下接地" in doc and "本文的前提" in doc,
      "the doc states the premise: pads are switch-to-GND inputs, so strapping is safe")
check("6.8k / 10k" in doc, "the MIDI RX divider value is stated")
check("不耐 5V" in doc, "the doc says why the MIDI RX divider is mandatory")
check(re.search(r"2\.475V", doc) is not None,
      "the doc gives the input-high threshold the divider has to clear")


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

    # Section 9.1's defect must actually be fixed BY the planned table -- i.e. pad 3 must not
    # still land on GPIO3 there. Under the new table pad 3 moves to GPIO0 and GPIO3 becomes
    # pad 4, which is a chord-free pad.
    check(pad_gpio.get(3) not in (None, 3),
          "the planned table moves pad 3 off GPIO3", "planned: %s" % pad_gpio.get(3))

pot_m = re.search(r"POT_PINS\[POT_NUM\]\s*=\s*\{([^}]*)\};", cfg_code)
check(pot_m is not None, "POT_PINS[] found")
pots_now = [int(x) for x in re.findall(r"\d+", pot_m.group(1))] if pot_m else []
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


# ---------------------------------------------------------------- custom PCB sections
# "自制 PCB" was the user's answer to the board question. The claims in section 2.4 that
# carry weight and are cheap to lose in an edit get asserted rather than assumed.
print("\n=== section 2.4 / 4.3: what a custom PCB does and does not change ===")
check("### 2.4" in doc, "section 2.4 exists")
check("自制 PCB" in doc, "the document records the custom-PCB answer")
# The claim most likely to be forgotten by whoever lays the board out: a PCB cannot reach
# past the module package. GPIO26-37 are inside the WROOM; 33/34/46 have no package pin.
check("模块封装是硬边界" in doc, "section 2.4 states that a PCB cannot override the module package")
check("连封装引脚都不存在" in doc,
      "section 2.4 names the pins that have no package pin at all")
check("模块是划算的" in doc,
      "section 2.4 records the bare-chip alternative and why it loses")
check("排针" not in doc.split("## 6. 分配表")[1].split("## 7.")[0],
      "section 6 no longer gives GPIO43/44 to a header -- they are pads 15 and 16")

# Regression guard on a claim that was WRONG and got written into an earlier revision: that
# when the firmware is broken, USB is the one thing that does not work, so you need a UART0
# header. HARDWARE_SETUP.md:353-354 refutes it -- the native USB port is a fixed-function
# USB-Serial-JTAG peripheral that does not depend on the firmware. Asserting the ABSENCE of
# the claim, next to its correction, is the only way it cannot quietly come back.
BAD_RESCUE = ["USB 恰恰是不工作的那个", "USB恰恰是不工作的那个"]
RETRACTION = ("错的", "已更正", "已删除", "早期版本", "错误")
stray = []
for b in BAD_RESCUE:
    start = 0
    while True:
        at = doc.find(b, start)
        if at < 0:
            break
        window = doc[max(0, at - 500):at + 500]
        if not any(r in window for r in RETRACTION):
            stray.append((b, doc.count("\n", 0, at) + 1))
        start = at + len(b)
check(not stray,
      "every mention of the broken-firmware claim sits next to its retraction",
      "stray: %s" % stray)
check("固定功能" in doc and "和固件无关" in doc,
      "section 2.4 gives the corrected rescue story: USB-Serial-JTAG is fixed-function")

# Regression guard on the older claim nested inside it: that flashing and the log compete
# for the single USB connector. They do not -- flashing means the firmware is not running,
# so there is nothing to log. The three "zero bytes" builds are not evidence of a conflict
# either; they were the log pointed at UART0, which this board does not route anywhere.
BAD_CONFLICT = ["不再争用", "两个口彻底分开", "抢同一个口", "烧录走这里"]
stray = []
for b in BAD_CONFLICT:
    start = 0
    while True:
        at = doc.find(b, start)
        if at < 0:
            break
        window = doc[max(0, at - 500):at + 500]
        if "伪问题" not in window:
            stray.append((b, doc.count("\n", 0, at) + 1))
        start = at + len(b)
check(not stray,
      "every mention of the conflict claim is next to its retraction, not asserted",
      "stray: %s" % stray)
check("伪问题" in doc and "从不同时发生" in doc,
      "section 2.4 explicitly retracts the conflict claim and says why it was wrong")

# The LED/TFT fork is the only thing that would reverse the recommendation, so its existence
# and its direction both get asserted. Sliced to section 4.3 on purpose: phrases about row
# and column multiplexing also appear in 2.4 (which forward-references 4.3), so a doc-wide
# search satisfies a "section 4.3 ..." assertion with text from the wrong section -- the same
# shape as the sequencer suite's #22.
check("### 4.3" in doc, "section 4.3 exists")
sec43 = doc.split("### 4.3", 1)[1].split("\n## ", 1)[0] if "### 4.3" in doc else ""
check("结论立刻反转" in sec43,
      "section 4.3 says the no-TFT case reverses the choice")
check("矩阵方案做不到" in sec43,
      "section 4.3 names the actual blocker: a matrix cannot drive pad LEDs")
check("行列线正在复用" in sec43 or "驱不了灯" in sec43,
      "section 4.3 explains WHY -- row/column lines are being multiplexed during the scan")
# The earlier revision said only the matrix could not drive LEDs, which would have left the
# recommended direct plan looking like it could. It cannot either: 16 pads already occupy all
# 16 GPIOs, and an LED needs an output.
check("16 个 pad 已经占满 16 个 GPIO" in sec43,
      "section 4.3 also rules LEDs out for the RECOMMENDED direct plan")
check("C 方案" in sec43 or "扩展芯片" in sec43,
      "section 4.3 names the option that does allow LEDs")

# Section 7 is what actually gets implemented; the one claim worth guarding there is that
# the direct plan leaves the pad-reading code alone, because that is the whole reason for
# preferring it.
sec7 = doc.split("## 7.")[1].split("## 8.")[0] if "## 7." in doc else ""
check("pads_m3.ino" in sec7 and "不改" in sec7,
      "section 7 states that pads_m3.ino is not modified")

print("\n" + "=" * 60)
print("%d checks, %d failed" % (total, len(fails)))
if fails:
    for f in fails:
        print("  FAILED: %s" % f)
print("SUITE COMPLETE")
sys.exit(0)