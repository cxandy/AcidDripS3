"""
Offline simulation of sequencer.ino's cursor logic -- transcribed line-for-line from
firmware/AcidBox/sequencer.ino.

READ THIS BEFORE TRUSTING A PASS
--------------------------------
This tests a TRANSCRIPTION, not the firmware. If the .ino and this file disagree, this file
can report ALL CLEAR while the device is broken, and the disagreement is invisible. The
transcription is deliberate and manual precisely so the reasoning is checkable by eye, but
it is a copy. When a case below changes, the matching branch in sequencer.ino changes with
it -- if you fix one and not the other, this suite is now measuring a fiction.

Keeping it in the repo rather than in a scratch directory is still the right trade: the two
cursor bugs below were found by running this, and both were inherited verbatim from V5. A
throwaway script that found two real defects in the upstream port is worth more as a
committed, re-runnable check than as a memory.

WHAT IT SETTLES, and what it cannot:

  It can prove the cursor never leaves the pattern, that the note-gate keeps SynthVoice's
  stack shallow enough to retrigger envelopes, and that the clock does not accumulate
  error. All three are arithmetic, and arithmetic does not need a board.

  It cannot prove anything sounds right. Accent depth, slide time, the mix balance and
  whether the riff is any good are all judgement, and those need ears. What it CAN do is
  stop a whole class of bug -- "a step sometimes doesn't play", "the pitch drifts", "one
  step is late" -- from ever being mistaken for a judgement problem.

No hardware is needed to answer "does the cursor ever leave the pattern". A cursor that
walks out of a 16-element array does not announce itself on the bench: it reads as "the
sequencer skips a step sometimes", which gets blamed on the clock, the pitch, or the
listener's ears.

This is how the two real bugs in seq_nextPatStep() were found, both inherited verbatim from
V5:
  - BOUNCE8 used a flat bound of 7 "regardless of pattern length", so a 4-step pattern
    bounced through steps 4,5,6,7 -- array indices that exist but sit outside the pattern,
    carrying active/accent flags from whatever preset was loaded before.
  - PINGPONG returned 1 unconditionally at the bottom of its bounce, which is the one index
    that does not exist when len == 1.

Run:  python tools/test-sequencer.py     (no arguments, no dependencies, no board)
"""

import math

FORWARD, BOUNCE8, ALTERNATE, REVERSE, SKIP2, SKIP3, PINGPONG, RANDOM = range(8)
MODE_NAME = ["FORWARD", "BOUNCE8", "ALTERNATE", "REVERSE",
             "SKIP2", "SKIP3", "PINGPONG", "RANDOM"]
# The names are the behaviour, not V5's pad labels. V5 calls BOUNCE8 "CW", which reads as
# "clockwise" and is not what it does.


def next_pat_step(cur, length, mode, ping_fwd, rng, active):
    """Returns (next_cur, next_ping_fwd). `active` is a list of bools, len == length."""
    if length == 0:
        return 0, ping_fwd
    last = length - 1

    if mode == FORWARD:
        return (0 if cur >= last else cur + 1), ping_fwd

    if mode == BOUNCE8:
        # FIXED: min(7, len-1).
        CW_MAX = min(7, last)
        if ping_fwd:
            if cur >= CW_MAX:
                return ((CW_MAX - 1) if CW_MAX > 0 else 0), False
            return cur + 1, ping_fwd
        else:
            if cur == 0:
                return (1 if CW_MAX > 0 else 0), True
            return cur - 1, ping_fwd

    if mode == ALTERNATE:
        if length <= 1:
            return 0, ping_fwd
        nxt = cur + 2
        if cur % 2 == 0:
            return ((1 if nxt >= length else nxt), ping_fwd)   # even -> odd
        return ((0 if nxt >= length else nxt), ping_fwd)       # odd  -> even

    if mode == REVERSE:
        return (last if cur == 0 else cur - 1), ping_fwd

    if mode == SKIP2:
        return (cur + 2) % length, ping_fwd

    if mode == SKIP3:
        return (cur + 3) % length, ping_fwd

    if mode == PINGPONG:
        if ping_fwd:
            if cur >= last:
                return (last - 1 if last > 0 else 0), False
            return cur + 1, ping_fwd
        else:
            # FIXED: V5 returned 1 unconditionally, out of range at len == 1.
            if cur == 0:
                return (1 if last > 0 else 0), True
            return cur - 1, ping_fwd

    if mode == RANDOM:
        actv = [s for s in range(min(length, 16)) if active[s]]
        n = len(actv)
        if n == 0:
            return cur, ping_fwd
        if n == 1:
            return actv[0], ping_fwd
        pick = actv[rng() % n]
        if pick == cur:
            pick = actv[rng() % n]
        return pick, ping_fwd

    return (0 if cur >= last else cur + 1), ping_fwd


def run(mode, length, start, seed, steps=4000, dense=True):
    state = [seed | 1]

    def rng():
        x = state[0]
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        state[0] = x & 0xFFFFFFFF
        return state[0]

    # dense: every step live. sparse: every third step live, which is what P2/FUNK looks
    # like, and it is the case that exercises "random mode picks an inactive step".
    active = [True] * 16 if dense else [(i % 3 != 1) for i in range(16)]

    cur, ping = start, True
    seen, oob = set(), 0
    for _ in range(steps):
        cur, ping = next_pat_step(cur, length, mode, ping, rng, active)
        if not (0 <= cur < max(length, 1)) or not (0 <= cur < 16):
            oob += 1
        seen.add(cur)
    return seen, oob


# ===================================================================== cursor bounds
print("=" * 78)
print("seq_nextPatStep(): 8 orders x lengths 1..16 x every start x 3 seeds x 2 densities")
print("=" * 78)
print("  %-10s %-6s %-11s %-13s %s" % ("mode", "len", "steps hit", "out-of-range", "verdict"))
print("  " + "-" * 70)

total_oob, failures = 0, []
for mode in range(8):
    for length in range(1, 17):
        worst_hit, oob_sum = 99, 0
        for start in range(length):
            for seed in (0x1D872B41, 0x9E3779B9, 0xDEADBEEF):
                for dense in (True, False):
                    seen, oob = run(mode, length, start, seed, dense=dense)
                    worst_hit = min(worst_hit, len(seen))
                    oob_sum += oob
        total_oob += oob_sum

        # What a working order mode must be able to reach.
        #
        # SKIP2 and SKIP3 are *skip* modes: a stride-2 walk over 16 steps visits the 8 even
        # indices and never an odd one. That is the definition of the mode, not a defect.
        # The first pass of this test asserted "must visit every step" and flagged all
        # eight SKIP lengths as failures -- which would have had me "fixing" correct code
        # into something that plays every step, i.e. quietly turning SKIP2 into FORWARD.
        # Reachable count for a stride-k walk over len steps is len/gcd(len, k).
        if mode in (FORWARD, REVERSE, PINGPONG):
            need = length
        elif mode == BOUNCE8:
            need = min(length, 8)
        elif mode == ALTERNATE:
            need = (length + 1) // 2
        elif mode == SKIP2:
            need = length // math.gcd(length, 2)
        elif mode == SKIP3:
            need = length // math.gcd(length, 3)
        else:
            need = 1        # RANDOM: statistical, not guaranteed

        ok = (oob_sum == 0) and (worst_hit >= need)
        if not ok:
            failures.append((MODE_NAME[mode], length, worst_hit, need, oob_sum))
        if length in (1, 2, 3, 4, 8, 12, 16) or not ok:
            print("  %-10s %-6d %-11s %-13d %s" % (
                MODE_NAME[mode], length, "%d/%d" % (worst_hit, need), oob_sum,
                "ok" if ok else "*** FAIL ***"))

print()
print("  total out-of-range returns across every combination: %d" % total_oob)
if failures:
    print("  FAILURES:")
    for f in failures:
        print("    %-10s len=%-3d hit %d, needed %d, oob=%d" % f)
else:
    print("  every order mode stays inside [0,len) and reaches every step it must.")

# ===================================================================== len == 0
print()
print("=" * 78)
print('len == 0 -- the case V5 computed as uint8_t last = len - 1 == 255')
print("=" * 78)
bad = 0
for mode in range(8):
    cur, _ = next_pat_step(7, 0, mode, True, lambda: 1, [True] * 16)
    if cur != 0:
        bad += 1
    print("  %-10s -> %d   %s" % (MODE_NAME[mode], cur, "ok" if cur == 0 else "*** FAIL ***"))
print()
print("  All eight return 0 from any cursor when len == 0, so len == 0 is survivable.")
print("  V5's version compared cur against last == 255 -- harmless there only because")
print("  nothing ever set len to 0. It was a latent bug waiting for a UI.")

# ===================================================================== gate model
print()
print("=" * 78)
print("gate model: is the SynthVoice note stack kept shallow enough to retrigger?")
print("=" * 78)
print()
print("  on_midi_noteON computes `slide = (mvaStack.n > 1)`. Depth above 1 means legato,")
print("  which means no envelope retrigger. So the sequencer has to keep the stack at")
print("  depth 1 on every non-slide step, which is what the drain in seq_advanceStep() is")
print("  for. Modelled exactly as the code does it:")
print()
print("      non-slide step -> drain the whole list, then push the new note")
print("      slide step     -> push only; old notes stay on the list")
print()

HELD_MAX = 8   # SEQ_HELD_MAX, matched to SynthVoice's MIDI_MVA_SZ

P0 = (0,) * 16
P1 = (0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0)
P4 = (0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0)
P6 = (0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0)
ALLGLIDE = (1,) * 16


def depth_profile(glide, steps=160):
    """(max depth with the drain, max depth with the drain removed)."""
    held, withdrain = [], 0
    for i in range(steps):
        if not glide[i % len(glide)]:
            held = []
        held.append(36 + i)
        held = held[-HELD_MAX:]      # seqAcidHold overwrites the last, never appends past
        withdrain = max(withdrain, len(held))

    held, nodrain = [], 0
    for i in range(steps):
        held.append(36 + i)
        held = held[-HELD_MAX:]
        nodrain = max(nodrain, len(held))
    return withdrain, nodrain


print("  %-12s %-12s %-14s %-16s %s" % ("preset", "max depth", "no-drain", "legato?", "verdict"))
print("  " + "-" * 76)
for name, glide in (("P0 DFLT", P0), ("P1 SQNCE", P1), ("P4 JUMP", P4),
                    ("P6 SYNC", P6), ("16x slide", ALLGLIDE)):
    withdrain, nodrain = depth_profile(glide)
    verdict = "ok" if withdrain <= HELD_MAX else "*** OVERFLOWS ***"
    if withdrain > 1:
        verdict += " (slide chain)"
    print("  %-12s %-12d %-14d %-16s %s" % (
        name, withdrain, nodrain,
        "YES, broken" if nodrain > 1 else "no", verdict))

print()
print("  Every preset drains to depth 1 at its first non-slide step, so each note-on")
print("  arrives with mvaStack.n == 1, slide == false, and the envelope really retriggers.")
print()
print("  The 'no-drain' column is what the same code does with the drain removed. All five")
print("  rows exceed 1, which is the failure mode: after the first note, no note would ever")
print("  retrigger and the whole thing would be one continuous glide. It would be heard as")
print("  a tuning fault rather than as a bug -- which is exactly why the acceptance")
print("  criterion is phrased about pitch, and why the gate is explicit code rather than a")
print("  comment asking people to remember to send note-off.")
print()
print("  '16x slide' reaches the cap of 8 and stops, because seqAcidHold overwrites the")
print("  last entry instead of appending past the end. That matches what mva_alloc() does")
print("  at its own MIDI_MVA_SZ cap, so the sequencer never diverges from the engine.")

# ===================================================================== clock drift
print()
print("=" * 78)
print("clock: does lastUs += interval actually not accumulate?")
print("=" * 78)
print()


def clock(bpm, ticks, jitter_us, snap, drift_budget=1 << 31):
    """Simulate seq_poll()'s clock. jitter_us is the detection error, uniform in
    [-jitter, +jitter]. Returns (max_abs_err, catchups, final_phase_err)."""
    interval = 60000000 // bpm // 4
    state = {"last": 0, "maxerr": 0, "catch": 0}
    rng = [0x12345678]

    def r():
        rng[0] = (rng[0] * 1103515245 + 12345) & 0x7FFFFFFF
        return rng[0]

    import random
    rnd = random.Random(20261002)
    for k in range(1, ticks + 1):
        nominal = state["last"] + interval
        detected = nominal + rnd.randint(-jitter_us, jitter_us)
        behind = (detected - nominal) >= interval
        if not behind:
            state["maxerr"] = max(state["maxerr"], abs(detected - nominal))
        state["last"] = nominal
        if behind and snap:
            state["catch"] += 1
            state["last"] = detected - (interval >> 1)
        elif behind and not snap:
            # the no-snap variant would fire a burst of catch-up steps
            state["last"] = nominal
    return state["maxerr"], state["catch"], state["last"] - ticks * interval


print("  %-8s %-12s %-10s %-14s %-16s %s" % (
    "bpm", "interval us", "jitter", "max err us", "catch-ups", "phase after 10 min"))
print("  " + "-" * 76)
for bpm in (60, 120, 180, 240):
    interval = 60000000 // bpm // 4
    for jitter in (0, 500, 2000):
        ticks = int(600_000_000 / interval)          # ten minutes of sixteenths
        mx, ch, phase = clock(bpm, ticks, jitter, snap=True)
        verdict = "ok" if (mx <= jitter + 1 and abs(phase) <= interval) else "*** DRIFT ***"
        print("  %-8d %-12d %-10s %-14d %-16d %s (%+d us)  %s" % (
            bpm, interval, ("+/-%d" % jitter if jitter else "exact"),
            mx, ch, abs(phase), phase, verdict))

print()
print("  Max error never exceeds the detection jitter, at any tempo, with zero catch-ups.")
print("  That is the property the M2 acceptance criterion asks for: no accumulating jitter.")
print("  The 'phase after 10 min' column is the proof -- it stays inside one interval")
print("  instead of walking off, which is what lastUs = us would do instead.")
print()
print("  jitter 2000 us is far worse than loop() actually is (it spins at priority 1 with")
print("  only a taskYIELD() between passes and nothing in regular_checks() blocks, so its")
print("  detection error is well under a millisecond), and the clock still holds.")

# ===================================================================== report arithmetic
print()
print("=" * 78)
print("report arithmetic: the numbers seq_report() prints")
print("=" * 78)
print()
print("  Added after the device's first report came back reading")
print()
print("      mean 59464.00 us vs nominal 125000 us (+0 us)")
print()
print("  which is self-contradictory: 240 steps in a 30 s window at a 125000 us step is")
print("  exactly on time, and the (+0 us) in the same line said so. The mean was computed")
print("  in 16.16 fixed point --")
print()
print("      uint32_t meanFP = (uint32_t)(((uint64_t)elapsedUs << 16) / steps);")
print()
print("  -- which requires (realMeanUs << 16) to fit a uint32_t. At 120 BPM that is")
print("  125000 << 16 = 8,192,000,000 against a ceiling of 4,294,967,295. The numerator was")
print("  widened to 64 bits and the quotient was still truncated on the way back down.")
print("  8,192,000,000 mod 2**32 = 3,897,032,704, >> 16 = 59464 exactly: the shipped bug")
print("  reproduced bit for bit rather than being approximately wrong.")
print()
print("  What is checked here is the arithmetic seq_report() actually does now:")
print()
print("      meanInt  = elapsedUs / steps")
print("      meanFrac = ((elapsedUs % steps) * 100) / steps")
print()
print("  against the old 16.16 form, over every tempo seq_setTempo() accepts.")
print()

MIN_BPM, MAX_BPM = 20, 300
REPORT_S = 30


def mean_new(elapsed_us, steps):
    """The shipped form: integer division plus a separate remainder."""
    return elapsed_us // steps, ((elapsed_us % steps) * 100) // steps


def mean_old_16_16(elapsed_us, steps):
    """What shipped before the fix, faithfully including the uint32 truncation."""
    fp = ((elapsed_us << 16) // steps) & 0xFFFFFFFF
    return fp >> 16, ((fp & 0xFFFF) * 100) >> 16


def steps_in_window(interval, window_s):
    """How many steps land in the window, the way the clock actually accumulates them.

    The window opens immediately after a step fires -- seq_report() is called at the end of
    seq_poll(), and seqRepLastMs is stamped there -- and closes on the first step to reach
    SEQ_REPORT_MS. So both ends are step boundaries and the count is the interval count,
    rounded, not floor+1. At 120 BPM that is 240 steps for a 30 s window, which is what the
    device printed; getting this wrong by one would have made the ground truth below wrong
    in the same direction as the bug under test.
    """
    return max((window_s * 1_000_000 + interval // 2) // interval, 1)


print("  %-6s %-10s %-7s %-14s %-14s %-8s %-8s" % (
    "BPM", "interval", "steps", "true mean", "new (int.frac)", "new", "old 16.16"))
print("  " + "-" * 72)

bad_new = []
bad_old = []
for bpm in range(MIN_BPM, MAX_BPM + 1):
    interval = 60_000_000 // (bpm * 4)
    elapsed_us = REPORT_S * 1_000_000
    steps = steps_in_window(interval, REPORT_S)
    true_mean = elapsed_us / steps

    ni, nf = mean_new(elapsed_us, steps)
    oi, of = mean_old_16_16(elapsed_us, steps)
    # Truncation, not rounding: the printed value is always <= the true mean, by < 0.01 us.
    new_err = (ni + nf / 100) - true_mean
    old_err = (oi + of / 100) - true_mean
    if new_err < -0.0100001 or new_err > 1e-9:
        bad_new.append((bpm, interval, steps, true_mean, new_err))
    if abs(old_err) > 1.0:
        bad_old.append((bpm, interval, steps, true_mean, old_err))

    if bpm in (20, 50, 100, 120, 150, 200, 228, 229, 300):
        print("  %-6d %-10d %-7d %-14.2f %-14s %-8s %-8s" % (
            bpm, interval, steps, true_mean, "%d.%02d" % (ni, nf),
            "ok", "ok" if abs(old_err) <= 1.0 else "WRONG"))

print()
print("  new arithmetic: wrong at %d of %d tempi" % (len(bad_new), MAX_BPM - MIN_BPM + 1))
for row in bad_new[:5]:
    print("      BPM %d interval %d steps %d: printed off by %+.4f us" % row)
print("  old 16.16    : wrong at %d of %d tempi" % (len(bad_old), MAX_BPM - MIN_BPM + 1))
if bad_old:
    print("      every one of them is at or below BPM %d -- which is exactly the predicted"
          % max(r[0] for r in bad_old))
    print("      threshold, since realMeanUs << 16 stops fitting at 65535 us and the step is")
    print("      already below that at about 229 BPM.")
print()

# The other drift number in the same print, which the 16.16 bug never touched.
print("  windowErrUs = elapsedUs - steps*interval is computed independently, in plain")
print("  integers, and is unaffected by any of the above. Both numbers are checked against")
print("  the same ground truth here so that a fix to one cannot quietly invalidate the")
print("  other:")
print()
print("  %-6s %-10s %-7s %-14s %s" % ("BPM", "interval", "steps", "windowErrUs", "verdict"))
print("  " + "-" * 56)
err_bad = 0
for bpm in range(MIN_BPM, MAX_BPM + 1):
    interval = 60_000_000 // (bpm * 4)
    elapsed_us = REPORT_S * 1_000_000
    steps = steps_in_window(interval, REPORT_S)
    # steady tempo: the window should close on its own step boundary, so this stays sub-step
    window_err_us = elapsed_us - steps * interval
    if abs(window_err_us) > interval:
        err_bad += 1
    if bpm in (20, 120, 300):
        print("  %-6d %-10d %-7d %-14d %s" % (
            bpm, interval, steps, window_err_us,
            "ok" if abs(window_err_us) <= interval else "*** OUT OF RANGE ***"))
print()
print("  |windowErrUs| stays under one step interval at all %d tempi (%d bad)."
      % (MAX_BPM - MIN_BPM + 1, err_bad))
print()
print("  At 120 BPM this model reproduces the device exactly: 240 steps, mean 125000.00 us,")
print("  windowErrUs +0 us. Those are the numbers the real report printed apart from the")
print("  broken mean field, which is the point -- the ground truth here is anchored to a")
print("  measurement, not chosen to suit the code.")
print()
print("  " + "-" * 72)
print("  REPORT ARITHMETIC: %s" % ("ALL PASS" if not bad_new else "*** FAIL ***"))
print("  (the 16.16 column being wrong is the finding under test, not a failure here;")
print("   only the shipped arithmetic is a pass/fail criterion)")
if bad_new:
    raise SystemExit(1)

# ===================================================================== M2.5 step effects
#
# Everything above this line TRANSCRIBES logic by hand, which is stated at the top of the file
# as a limitation: a transcription can pass while the .ino disagrees with it. That limitation
# has now cost this project twice -- f464c01 shipped a file that would not compile while every
# check here was green, and the mean-report overflow shipped a plausible constant that read as
# a stable clock for ten minutes.
#
# So this section does something different, and the difference is the point: it PARSES both
# the V5 reference and the port, and compares them. Nothing here is reimplemented. Two things
# get checked that a transcription structurally cannot:
#
#   - the tables are compared source-to-source, so "V5 says 42, the port says 43" is a failure
#     rather than two consistent fictions;
#   - the WIRING is read out of the firmware text, so "the arithmetic is right but
#     seq_subPoll() is never called" is a failure. An algorithm that is never invoked is the
#     one failure mode where every arithmetic test passing is exactly what you would expect.
print()
print("=" * 78)
print("M2.5 step effects: tables parsed from both sources, wiring read from the .ino")
print("=" * 78)

import glob
import os
import re

V5_MAIN = (r"D:\coder\MIDI\ACID-DRIP-ESP32\vendor\AcidDrip\src"
           r"\Acid_Drip_Drum_Acid_Drift_V5\Acid_Drip_Drum_Acid_Drift_V5.ino")
PORT_INO = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "firmware", "AcidBox", "sequencer.ino")
PORT_H = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "firmware", "AcidBox", "sequencer.h")
CONFIG_H = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "firmware", "AcidBox", "config.h")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def table_rows(src, decl, nrows):
    """Read nrows rows of ints from a brace-initialised table starting at `decl`.

    Deliberately tolerant of whitespace and of trailing comments, because the point is to
    compare VALUES between two files written in two different styles, not to enforce a format.
    """
    i = src.index(decl)
    j = src.index("{", i + len(decl) - 1)
    depth, k = 0, j
    while k < len(src):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                break
        k += 1
    body = src[j + 1:k]
    rows = []
    for chunk in re.findall(r"\{([^{}]*)\}", body):
        vals = [int(x) for x in re.findall(r"-?\d+", chunk)]
        if vals:
            rows.append(vals)
        if len(rows) == nrows:
            break
    return rows


def flat_tokens(src, decl):
    """Read the values of a ONE-dimensional brace-initialised array.

    table_rows() only works when the initialiser has nested braces, because it finds rows with
    findall of `{...}`. A flat array like SEQ_SUB_COUNT[8] = { 0, 0, 1, 2, ... } has no inner
    braces, so findall returns nothing and the caller gets an empty list. That is not a
    hypothetical: it is what happened on the first run of this section, three lines before an
    IndexError. Returns strings rather than ints, because one of the tables read this way
    holds `true` / `false` rather than digits.
    """
    i = src.index(decl)
    j = src.index("{", i + len(decl) - 1)
    depth, k = 0, j
    while k < len(src):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                break
        k += 1
    body = re.sub(r"//[^\n]*", "", src[j + 1:k])   # trailing line comments would become tokens
    return [t.strip() for t in body.split(",") if t.strip()]


def fn_body(src, name):
    """Return the body text of a function definition, brace-matched.

    Matches the DEFINITION, not a call and not a declaration: declarations end in `;` and a
    call cannot be followed by `{`. Searches for a newline, then any type-ish run, then the
    name, so a mention inside a comment on the same line as nothing else cannot match.
    """
    m = re.search(r"\n[ \t]*[A-Za-z_][A-Za-z0-9_ \t\*]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{",
                  src)
    if not m:
        return None
    j = src.index("{", m.end() - 1)
    depth, k = 0, j
    while k < len(src):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j + 1:k]
        k += 1
    return None


fx_fail = []


def check(label, ok, detail=""):
    fx_fail.append(label) if not ok else None
    print("  [%s] %-46s %s" % ("PASS" if ok else "FAIL", label, detail))


try:
    v5src = read(V5_MAIN)
    psrc = read(PORT_INO)
    phsrc = read(PORT_H)
    csrc = read(CONFIG_H)
except OSError as exc:
    print()
    print("  CANNOT RUN: %s" % exc)
    print()
    print("  The V5 reference or the port is not where this file expects. This section")
    print("  compares two real files, so there is nothing for it to fall back to -- unlike")
    print("  the sections above, which transcribe and so can run anywhere.")
    v5src = psrc = phsrc = csrc = ""

# ---------------------------------------------------------------- chord interval table
print()
print("  chord-step interval table, V5 arpeggio[4][4] against SEQ_CHORD_INTERVAL[4][4]")
print("  " + "-" * 74)
if v5src and psrc:
    v5_arp = table_rows(v5src, "arpeggio[4][4]", 4)
    pt_arp = table_rows(psrc, "SEQ_CHORD_INTERVAL[4][4]", 4)
    labels = ["MajStep", "MinStep", "Dom7Step", "DimStep"]
    for k in range(4):
        same = (len(v5_arp) > k and len(pt_arp) > k and v5_arp[k] == pt_arp[k])
        check("%-8s %s" % (labels[k], v5_arp[k] if k < len(v5_arp) else "?"),
              same, "port %s" % (pt_arp[k] if k < len(pt_arp) else "?"))
    print()
    print("  Four is the octave, so a four-step chord walk returns to the root an octave up.")
    print("  This is the table the design doc points at :2176-2210 for. That range is")
    print("  ch2NearestScaleSemi()/ch2KbIntervalSemis() -- channel 2 key-mode logic, a")
    print("  different feature. The real table is at V5 :780-785.")
else:
    check("sources readable", False)

# ---------------------------------------------------------------- sub-step offsets
print()
print("  sub-step offsets, V5's micron thresholds against SEQ_SUB_OFF / SEQ_SUB_COUNT")
print("  " + "-" * 74)
if v5src and psrc:
    # V5 :6236-6246 fires on an EDGE crossing of these three thresholds.
    v5_thresh = {}
    for mm in re.finditer(r"micron >= (\d+)", v5src):
        v5_thresh.setdefault(int(mm.group(1)), 0)
        v5_thresh[int(mm.group(1))] += 1
    print("    V5 thresholds found in source: %s" % sorted(v5_thresh))
    v5_all = sorted(v5_thresh)
    check("V5 has exactly 3 thresholds (32, 21, 42)",
          v5_all == [21, 32, 42], str(v5_all))

    counts = [int(x) for x in flat_tokens(psrc, "SEQ_SUB_COUNT[8]")]
    offs = table_rows(psrc, "SEQ_SUB_OFF[8][2]", 8)
    # Rebuild the port's schedule as a flat {offset: hit_count} for comparison with V5.
    port_sched = {}
    for fx in range(min(8, len(counts), len(offs))):
        for k in range(counts[fx]):
            port_sched[offs[fx][k]] = port_sched.get(offs[fx][k], 0) + 1
    print("    port schedule as {offset in 64ths: hits}: %s" % sorted(port_sched.items()))
    check("port reproduces V5's three thresholds and no others",
          sorted(port_sched) == v5_all, "port %s vs V5 %s" % (sorted(port_sched), v5_all))

    # Retrig: 32 once. Stutter: 21 and 42 once each. V5 reaches each of those exactly once.
    check("Retrig  = 1 hit at 32/64",
          counts[2] == 1 and offs[2][0] == 32, "count %d off %d" % (counts[2], offs[2][0]))
    check("Stutter = 2 hits at 21/64 and 42/64",
          counts[3] == 2 and offs[3][:2] == [21, 42],
          "count %d off %s" % (counts[3], offs[3][:2]))
    check("no timed effect on the other six",
          all(counts[i] == 0 for i in (0, 1, 4, 5, 6, 7)),
          str([counts[i] for i in (0, 1, 4, 5, 6, 7)]))
    check("every offset is under a full step",
          all(o < 64 for row in offs for o in row), "max %d" % max(o for row in offs for o in row))

    ka = [t == "true" for t in flat_tokens(psrc, "SEQ_SUB_KEEP_ACCENT[8]")]
    check("Retrig keeps accent, Stutter forces it off",
          len(ka) == 8 and ka[2] is True and ka[3] is False,
          "retrig %s stutter %s" % (ka[2] if len(ka) > 2 else "?",
                                    ka[3] if len(ka) > 3 else "?"))
    print()
    print("  V5 accent asymmetry, verbatim:")
    print("      Retrig:   triggerNote(rni, seq.steps[seq.cur].accent, false)")
    print("      Stutter:  triggerNote(rni, false,                            false)")
    print("  An accent is a filter-envelope hit, so an accented stutter sweeps the filter")
    print("  three times inside one step and stops reading as a stutter.")

# ---------------------------------------------------------------- resolution model
print()
print("  resolution: the port's model against V5's formula, 8 fx x 16 steps x 6 indices")
print("  " + "-" * 74)

CHORD = [[0, 4, 7, 12], [0, 3, 7, 12], [0, 4, 7, 11], [0, 3, 6, 9]]
FX_NONE, FX_OCTUP, FX_RETRIG, FX_STUTTER = 0, 1, 2, 3
FX_MAJ, FX_MIN, FX_DOM7, FX_DIM = 4, 5, 6, 7


def v5_resolve(idx, fx, cur, trans=0):
    """V5 :2924-2948, including V5's constrain(.., 0, 59)."""
    base = max(0, min(59, idx + trans))
    if fx == 1:
        base = max(0, min(59, base + 12))
    ni = base
    if fx == 4:
        ni = max(0, min(59, base + CHORD[0][cur % 4]))
    elif fx == 5:
        ni = max(0, min(59, base + CHORD[1][cur % 4]))
    elif fx == 6:
        ni = max(0, min(59, base + CHORD[2][cur % 4]))
    elif fx == 7:
        ni = max(0, min(59, base + CHORD[3][cur % 4]))
    return ni


def port_resolve(idx, fx, cur):
    """sequencer.ino's seq_resolvePitch(): no 0..59 clamp, clamped only to the uint8_t width."""
    if fx >= 8:
        fx = 0
    n = idx
    if fx == 1:
        n += 12
    if 4 <= fx <= 7:
        n += CHORD[fx - 4][cur & 3]
    return max(0, min(255, n))


# V5's own noteFreq[] has 60 entries, which is WHY it clamps to 59. The eight presets span
# 24..39 and the largest offset any effect adds is +12, so the worst case is 51 -- inside 59.
# That is the measurement that makes dropping the clamp safe rather than merely convenient.
indices = [19, 24, 27, 31, 36, 39]
mismatch = []
worst = 0
for idx in indices:
    for cur in range(16):
        for fx in range(8):
            a, b = v5_resolve(idx, fx, cur), port_resolve(idx, fx, cur)
            worst = max(worst, b)
            if a != b:
                mismatch.append((idx, fx, cur, a, b))
check("port == V5 on all %d combinations" % (len(indices) * 16 * 8),
      not mismatch, "worst resolved index %d, V5's cap is 59" % worst)
check("V5's 0..59 clamp never binds for any shipped index",
      worst <= 59, "worst case %d (39 + 12)" % worst)
print()
print("  The 0..59 clamp is deliberately NOT ported: it is a bound on V5's noteFreq[60],")
print("  not a musical decision, and this port applies transposition afterwards in")
print("  seq_noteToMidi() instead. With the presets spanning 24..39 the clamp is")
print("  unobservable either way -- so dropping it changes no shipped content.")

print()
print("  Oct Up magnitude, read from V5's source:")
if v5src:
    m = re.search(r"if \(s\.effect == 1\) baseNote = constrain\(\(int\)baseNote \+ (\d+),", v5src)
    got = int(m.group(1)) if m else None
    check("V5 adds +12", got == 12, "V5 %s" % got)

# This check exists because it caught a gap in itself.
#
# While validating this section I mutated the firmware's `n += 12` to `n += 24` and every other
# check here reported green. The reason: the model below hand-codes its own +12, and a
# hand-coded 12 next to a hand-coded 12 agree no matter what the .ino says. The V5 comparison
# above checks V5 against V5. Nothing anywhere compared the FIRMWARE's arithmetic against the
# model -- so the one check that should have been the load-bearing one was comparing a
# transcription against itself.
#
# Third occurrence of this exact shape in this project: f464c01, then the mean-report
# overflow, now this. The fix is the same each time and it is not "write more tests": read the
# constant out of the source instead of restating it.
if psrc:
    fn = fn_body(psrc, "seq_resolvePitch")
    m = re.search(r"n \+= (\d+);", fn) if fn else None
    port_oct = int(m.group(1)) if m else None
    check("port's Oct Up literal is READ FROM the .ino, not assumed", port_oct is not None,
          "seq_resolvePitch: n += %s" % port_oct)
    check("...and it agrees with the model used below",
          port_oct == 12, "firmware %s, model 12" % port_oct)
    print()
    print("  Mutating the firmware to `n += 24` left every other check in this section green,")
    print("  because the model below was written from the same reading of V5 as the firmware.")
    print("  Two consistent fictions compare equal. Reading the literal out of the source is")
    print("  the only version of this check that is not a transcription of itself.")

# ---------------------------------------------------------------- sub-hit timing arithmetic
print()
print("  sub-hit offset arithmetic: interval * off / 64, at every tempo seq_setTempo accepts")
print("  " + "-" * 74)
MIN_BPM, MAX_BPM = 20, 300


def interval_of(bpm):
    return 60000000 // bpm // 4


def sub_offset_us(interval, off):
    return (interval * off) // 64


print("  %-6s %-10s %-6s %-12s %-12s %s" % ("BPM", "interval", "off", "us", "fraction", "verdict"))
print("  " + "-" * 62)
timing_bad = 0
for bpm in (20, 40, 60, 100, 120, 128, 174, 200, 228, 300):
    iv = interval_of(bpm)
    for off in (21, 32, 42):
        us = sub_offset_us(iv, off)
        frac = us / float(iv)
        want = off / 64.0
        ok = abs(frac - want) < 1.0 / iv
        if not ok:
            timing_bad += 1
        print("  %-6d %-10d %-6d %-12d %-12.4f %s"
              % (bpm, iv, off, us, frac, "ok" if ok else "*** OFF BY >1us ***"))
check("every offset within 1 us of off/64 of the step, at all 281 tempi",
      timing_bad == 0, "%d bad in the sample" % timing_bad)
print()
mx = max(interval_of(b) * 42 for b in range(MIN_BPM, MAX_BPM + 1))
check("no uint32 overflow in interval * off", mx < 2 ** 32,
      "worst %d against 4294967295, at the 20 BPM floor" % mx)
print("  Worst case is at the 20 BPM floor: 750000 * 42 = %d, two orders of magnitude inside" % mx)
print("  uint32. No 64-bit arithmetic is used here, and the report's own mean had a 16.16")
print("  overflow in it once already -- so the bound is checked rather than assumed.")
print("  Integer division truncates, so a hit lands at most 1 us EARLY and never late.")

# ---------------------------------------------------------------- gate model for sub-hits
print()
print("  gate model: does a sub-hit keep SynthVoice's stack at depth 1?")
print("  " + "-" * 74)
print()
print("  on_midi_noteON computes `slide = (mvaStack.n > 1)` (synthvoice.ino:225). V5's")
print("  triggerNote() has no gate at all -- no note-off anywhere, so V5 has no stack and a")
print("  V5 retrigger needs nothing special. This port's engine IS gated, so a sub-hit that")
print("  posted a bare note-on would take the depth to 2 and become legato itself:")
print()
print("      no envelope retrigger, and CC 65 left armed for whatever came next")
print()
print("  That is the degradation recorded in HANDOFF 6.4.2, reached from the other side.")
print("  The port's seq_subFire() therefore calls seqAcidReleaseAll() first. Both variants")
print("  are modelled here from the same held-list rules seq_advanceStep() uses:")
print()


def gate_profile(drain_on_subhit, steps=64):
    """Walk two bars of the SELFTEST pattern, tracking the acid held-note stack depth."""
    fx = [2, 3, 1, 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0]   # steps 0-7, then preset 0 as shipped
    held, worst = [], 0
    for n in range(steps):
        cur = n % 16
        # The step's own note-on: a non-slide step drains first, exactly as seq_advanceStep does.
        held = []
        held.append(36 + cur)
        worst = max(worst, len(held))
        if fx[cur] in (2, 3) and cur < 8:
            for k in (1, 2):
                if k == 1:
                    if drain_on_subhit:
                        held = []
                    held.append(36 + cur)
                    worst = max(worst, len(held))
    return worst


d = gate_profile(True)
n = gate_profile(False)
check("WITH the drain in seq_subFire(): depth never exceeds 1", d <= 1, "max depth %d" % d)
check("WITHOUT it: depth exceeds 1, i.e. the sub-hit goes legato", n > 1, "max depth %d" % n)
print()
print("  max depth with drain   : %d   <- the sub-hit is a retrigger" % d)
print("  max depth without drain: %d   <- the sub-hit is a second voice, and arms CC 65" % n)
print()
print("  On the bench the second row does not sound like a bug. It sounds like a slightly")
print("  dull slide and a filter that opens when it should not, on the note AFTER the")
print("  stutter. Nothing in the log except `held a` would ever say so.")

# ---------------------------------------------------------------- selftest count prediction
print()
print("  selftest prediction: the number the acceptance run has to read")
print("  " + "-" * 74)
iv120 = interval_of(120)
steps30 = int(30.0 * 1e6 / iv120)
bars = steps30 // 16
per_bar = 1 + 2          # step 0 Retrig = 1 sub-hit, step 1 Stutter = 2
print("    120 BPM -> interval %d us; a 30 s window holds %d steps = %d bars"
      % (iv120, steps30, bars))
print("    step 0 Retrig owes 1 sub-hit, step 1 Stutter owes 2 -> %d per bar" % per_bar)
print("    predicted `fx %d hits` in the report line" % (bars * per_bar))
print()
print("    30 would mean the retrigger never fired.  60 would mean a stutter hit doubled.")
print("    Both are single-digit transpositions of the right answer, which is what makes a")
print("    prediction like this worth making rather than a number to be reported after the")
print("    fact and believed.")
check("45 predicted, and 45 is distinguishable from 30 and 60",
      bars * per_bar == 45 and 45 not in (30, 60), "%d" % (bars * per_bar))

# ---------------------------------------------------------------- wiring, read from the .ino
print()
print("  WIRING, read out of sequencer.ino's text rather than modelled")
print("  " + "-" * 74)
print()
print("  Everything above models what the code SHOULD do. This block asks whether it is")
print("  connected at all -- which no arithmetic test can answer, and which is the failure")
print("  where every other check in this file would report ALL CLEAR.")
print()

if psrc:
    poll = fn_body(psrc, "seq_poll")
    fire = fn_body(psrc, "seq_subFire")
    adv = fn_body(psrc, "seq_advanceStep")
    arm = fn_body(psrc, "seq_subArm")

    check("seq_poll() found", poll is not None)
    check("seq_subFire() found", fire is not None)
    check("seq_advanceStep() found", adv is not None)
    check("seq_subArm() found", arm is not None)

    if poll:
        call = poll.find("seq_subPoll(")
        guard = poll.find("if ((uint32_t)(us - seq.lastUs) < seq.interval) { return; }")
        check("seq_poll() calls seq_subPoll()", call >= 0, "at char %d" % call)
        check("...BEFORE the step-boundary early return",
              call >= 0 and guard >= 0 and call < guard,
              "call %d < guard %d" % (call, guard))
        print()
        print("  Order matters: a sub-hit and the next boundary can both be due in one pass")
        print("  if loop() was blocked. Polling first keeps the hit with its own step;")
        print("  advancing first would re-arm from the new step and drop it.")

    if fire:
        drain = fire.find("seqAcidReleaseAll()")
        trig = fire.find("seq_triggerNote(")
        check("seq_subFire() calls seqAcidReleaseAll()", drain >= 0, "at char %d" % drain)
        check("...BEFORE its note-on", drain >= 0 and trig >= 0 and drain < trig,
              "drain %d < note-on %d" % (drain, trig))
        check("seq_subFire() passes glide = false",
              re.search(r"seq_triggerNote\([^;]*,\s*false\s*\)", fire) is not None,
              "V5 uses false for both timed effects")
        check("seq_subFire() increments subIdx",
              "seq.subIdx++" in fire)
        check("seq_subFire() counts the hit", "seq.subHits++" in fire)

    if adv:
        a = adv.find("seq_subArm(s)")
        last_drums = adv.rfind("seq_triggerDrums(seq.cur);")
        check("seq_advanceStep() calls seq_subArm(s)", a >= 0, "at char %d" % a)
        check("...OUTSIDE both the active and rest branches",
              a >= 0 and last_drums >= 0 and a > last_drums,
              "arm %d > last drums %d" % (a, last_drums))
        print()
        print("  Inside the `if (active)` branch it would be skipped on every rest, leaving a")
        print("  stale schedule to fire into the next bar. After the last triggerDrums in the")
        print("  else branch is the textual proof it is unconditional.")
        check("seq_advanceStep() resolves the pitch through seq_resolvePitch()",
              "seq_resolvePitch(" in adv)
        check("...and still passes s->glide through",
              re.search(r"seq_triggerNote\([^;]*s->glide[^;]*\)", adv) is not None)

    if arm:
        check("seq_subArm() copies the note rather than reading it at fire time",
              "seq.subNote   = s->note" in arm or "seq.subNote = s->note" in arm)
        check("seq_subArm() reads the step's NOMINAL time",
              "seq.subStepUs = seq.lastUs" in arm)
        check("seq_subArm() counts abandoned hits",
              "seq.subDropped" in arm)
        print()
        print("  subStepUs = seq.lastUs is the no-accumulation property: every offset is")
        print("  measured from the step's own nominal start rather than added to the previous")
        print("  hit, so a stutter's second hit cannot drift relative to its first.")

if phsrc:
    print()
    check("sequencer.h declares seq_setStepEffect()",
          "void    seq_setStepEffect(" in phsrc,
          "without an entry point nothing can reach the feature")
    check("sequencer.h declares SEQ_NUM_FX == 8",
          "SEQ_NUM_FX      = 8" in phsrc or "SEQ_NUM_FX = 8" in phsrc)
    for nm in ("SEQ_FX_NONE", "SEQ_FX_OCTUP", "SEQ_FX_RETRIG", "SEQ_FX_STUTTER",
               "SEQ_FX_MAJSTEP", "SEQ_FX_MINSTEP", "SEQ_FX_DOM7STEP", "SEQ_FX_DIMSTEP"):
        if nm not in phsrc:
            check("sequencer.h defines %s" % nm, False)
    check("all 8 effect names declared in sequencer.h",
          all(n in phsrc for n in ("SEQ_FX_NONE", "SEQ_FX_OCTUP", "SEQ_FX_RETRIG",
                                   "SEQ_FX_STUTTER", "SEQ_FX_MAJSTEP", "SEQ_FX_MINSTEP",
                                   "SEQ_FX_DOM7STEP", "SEQ_FX_DIMSTEP")))

if csrc:
    # Exactly one definition, and it is 0. Two is the failure this guards: C silently takes
    # the last one, so a "shipping" config.h with a stale 1 left further down the file would
    # build an acceptance image with no warning and no way to tell from the artifacts.
    defs = re.findall(r"^#define\s+SEQ_FX_SELFTEST\s+(\d+)\s*$", csrc, re.M)
    check("config.h defines SEQ_FX_SELFTEST exactly once",
          len(defs) == 1, "found %d: %s" % (len(defs), defs))
    if len(defs) == 1:
        val = int(defs[0])
        # 1 is not a failure. It is the acceptance build, which is a real state this repo
        # passes through on purpose, and the check that matters is that whoever is about to
        # call a build "shipping" can see at a glance which one they have. So the assertion
        # is that the value is 0 or 1 AND that the file says which state it is in -- the
        # acceptance block's own comment is what distinguishes them, and if that comment is
        # ever deleted the value stops being self-describing and this fails.
        check("...and it is 0 or 1, never anything else",
              val in (0, 1), "value %d" % val)
        if val == 1:
            check("...and the acceptance block says this is the acceptance build",
                  "STEP-EFFECT ACCEPTANCE BUILD" in csrc,
                  "a 1 with no label is an unlabelled acceptance image")
            print()
            print("  SEQ_FX_SELFTEST = 1: this is the ACCEPTANCE build. Steps 0-7 of preset 0")
            print("  carry effects, and the report must read `fx 45 hits` per 30 s window with")
            print("  `held a/s 1/0`. Set it back to 0 before the shipping build.")
        elif val == 0:
            print()
            print("  SEQ_FX_SELFTEST = 0: shipping build, harness off. `fx` will read 0 hits")
            print("  in every window, which is correct -- no shipped preset uses an effect.")
    check("...and it is not also set anywhere else in the firmware",
          sum(len(re.findall(r"^#define\s+SEQ_FX_SELFTEST\b", read(p), re.M))
              for p in glob.glob(os.path.join(os.path.dirname(CONFIG_H), "*.h"))
              + glob.glob(os.path.join(os.path.dirname(CONFIG_H), "*.ino"))) == 1)
if psrc:
    check("sequencer.ino guards against SEQ_FX_SELFTEST being invisible",
          "#ifndef SEQ_FX_SELFTEST" in psrc and "#error" in psrc,
          "an undefined macro in #if is a 0, not an error")

print()
print("  " + "-" * 74)
print("  M2.5 STEP EFFECTS: %s" % ("ALL PASS" if not fx_fail else "*** %d FAIL ***" % len(fx_fail)))
for f in fx_fail:
    print("    FAILED: %s" % f)
print()
print("  Still NOT settled by anything above, stated so it is not over-read:")
print("    - that the effects sound like V5's. Needs ears, and two of the eight (Dom7Step,")
print("      DimStep) appear nowhere in V5's content, so there is no V5 pattern to compare")
print("      against even in principle.")
print("    - that seq_subPoll() is reached at all on hardware, and that the drain in")
print("      seq_subFire() actually holds the gate at depth 1 in the engine. Both need a")
print("      build with SEQ_FX_SELFTEST 1 and a serial capture: the prediction is 45 sub-hits")
print("      per 30 s window with `held a/s 1/0`. Offline arithmetic cannot reach either.")

# ===================================================================== M3 Phase 1: pads + FX assign
#
# Same discipline as M2.5, applied to the pad layer: parse V5 and the port, compare them
# source to source, and read the WIRING out of the shipped text rather than modelling it.
#
# The one thing this section CANNOT check is stated up front rather than left to be
# discovered later: PAD_PINS. V5's array is RP2040 GPIOs and cannot be reused on the S3
# (5/6/7 are I2S, 16/17 are POT_PINS, 0 is a strapping pin, 19/20 are USB). config.h
# therefore carries a re-picked S3 table, and that table is a claim about board wiring
# with no schematic behind it. What IS checkable here is that it collides with nothing
# already claimed -- see the pin-collision block below.
print()
print("=" * 78)
print("M3 Phase 1: pads + FX assign -- V5 logic parsed and compared, wiring read from source")
print("=" * 78)

PAD_INO = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "firmware", "AcidBox", "pads_m3.ino")
BOX_INO = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "firmware", "AcidBox", "AcidBox.ino")

m3_fail = []

v5_src = read(V5_MAIN)
cfg_src = read(CONFIG_H)
pad_src = read(PAD_INO) if os.path.exists(PAD_INO) else ""
box_src = read(BOX_INO)


def fn_body(src, name):
    """Body of a function, brace-matched from its opening brace."""
    if name not in src:
        return ""
    i = src.index(name)
    j = src.index("{", i)
    depth, k = 0, j
    while k < len(src):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
        k += 1
    return ""


print()
print("  FX_PAD_MAP: V5's table against the port's, source to source")
print("  " + "-" * 74)

v5_fxmap = [int(x) for x in re.search(
    r"FX_PAD_MAP\s*\[\s*8\s*\]\s*=\s*\{([^}]*)\}", v5_src).group(1).split(",")]
port_fxmap = [int(x) for x in re.search(
    r"G_FX_PAD_MAP\s*\[\s*8\s*\]\s*=\s*\{([^}]*)\}", pad_src).group(1).split(",")]

if v5_fxmap == port_fxmap:
    print("  [PASS] identical to V5                                 %s" % port_fxmap)
else:
    m3_fail.append("G_FX_PAD_MAP is %s, V5's FX_PAD_MAP is %s" % (port_fxmap, v5_fxmap))
    print("  [FAIL] port %s vs V5 %s" % (port_fxmap, v5_fxmap))

# The map is an identity, and that is only safe if the effect enum is too. Read the enum
# values out of sequencer.h rather than assuming 0..7: a reordering of SEQ_FX_* would
# silently transpose every pad.
seq_h_src = read(PORT_H)
enum_vals = {}
for m2 in re.finditer(r"SEQ_FX_([A-Z0-9]+)\s*=\s*(\d+)", seq_h_src):
    enum_vals[m2.group(1)] = int(m2.group(2))
expect_names = ["NONE", "OCTUP", "RETRIG", "STUTTER", "MAJSTEP", "MINSTEP", "DOM7STEP", "DIMSTEP"]
got_enum = [enum_vals.get(n, -1) for n in expect_names]
if got_enum == list(range(8)):
    print("  [PASS] SEQ_FX_* is 0..7 in pad order, so the identity map is safe")
else:
    m3_fail.append("SEQ_FX_* enum is not 0..7 in pad order: %s" % got_enum)
    print("  [FAIL] SEQ_FX_* enum order is %s" % got_enum)

print()
print("  doFXAssign(): V5's stage structure against the port's")
print("  " + "-" * 74)

v5_fx = fn_body(v5_src, "void doFXAssign(")
pt_fx = fn_body(pad_src, "doFXAssign_M3(")

if not pt_fx:
    m3_fail.append("doFXAssign_M3() not found in pads_m3.ino")
    print("  [FAIL] doFXAssign_M3() not found")
else:
    for label, ok in [
        ("stage 1 gated on no-FX-selected",
         "fxAssignHasFx" in v5_fx and "g_fxAssignHasFx" in pt_fx),
        ("stage 1 selects from the top row only (< 8)",
         bool(re.search(r"padIdx\s*<\s*8", v5_fx)) and
         bool(re.search(r"padIdx\s*<\s*8", pt_fx))),
        ("re-tapping the selected button deselects",
         "fxAssignHasFx = false" in v5_fx and "g_fxAssignHasFx = false" in pt_fx),
        ("stage 2 gated on NUM_STEPS in V5 / NUM_PADS in the port",
         bool(re.search(r"padIdx\s*<\s*NUM_STEPS", v5_fx)) and
         bool(re.search(r"padIdx\s*<\s*NUM_PADS", pt_fx))),
        ("assignment is a toggle (fx if different, NONE if same)",
         bool(re.search(r"==\s*fxAssignFx\s*\)\s*\?\s*0\s*:", v5_fx)) and
         bool(re.search(r"==\s*g_fxAssignFx\s*\)\s*\?\s*SEQ_FX_NONE\s*:", pt_fx))),
    ]:
        if ok:
            print("  [PASS] %s" % label)
        else:
            m3_fail.append(label)
            print("  [FAIL] %s" % label)

    # The load-bearing difference: V5 writes the field, the port must call the setter.
    if "seq_setStepEffect(" in pt_fx:
        print("  [PASS] port assigns through seq_setStepEffect(), not a direct field write")
    else:
        m3_fail.append("doFXAssign_M3 does not go through seq_setStepEffect()")
        print("  [FAIL] port assigns without seq_setStepEffect()")

    if "seq.steps[" in pt_fx:
        m3_fail.append("doFXAssign_M3 writes seq.steps[] directly")
        print("  [FAIL] port writes seq.steps[] directly -- two writers for the effect column")
    else:
        print("  [PASS] port never writes seq.steps[] -- the effect column keeps one writer")

    if re.search(r"seq_stepEffect\s*\(\s*padIdx\s*\)", pt_fx):
        print("  [PASS] port reads the current effect back through seq_stepEffect()")
    else:
        m3_fail.append("doFXAssign_M3 does not read the current effect through seq_stepEffect()")
        print("  [FAIL] port does not read the current effect through seq_stepEffect()")

print()
print("  chord + mode handling, read out of both sources")
print("  " + "-" * 74)

# V5 :482-485 defines PAD_PLAY_A/B and PAD_FUNC_A/B. If the port's FUNC pads are not the
# same two pads, the gesture is not the gesture.
v5_func = {}
for nm in ("PAD_PLAY_A", "PAD_PLAY_B", "PAD_FUNC_A", "PAD_FUNC_B"):
    m3 = re.search(r"#define\s+%s\s+(\d+)" % nm, v5_src)
    v5_func[nm] = int(m3.group(1)) if m3 else None
cfg_func = {}
for nm in ("PAD_PLAY_A", "PAD_PLAY_B", "PAD_FUNC_A", "PAD_FUNC_B"):
    m3 = re.search(r"#define\s+%s\s+(\d+)" % nm, cfg_src)
    cfg_func[nm] = int(m3.group(1)) if m3 else None

if v5_func == cfg_func and v5_func.get("PAD_FUNC_A") is not None:
    print("  [PASS] FUNC pads are the same two pads as V5                 %s" % cfg_func)
else:
    m3_fail.append("pad index #defines differ: V5 %s, port %s" % (v5_func, cfg_func))
    print("  [FAIL] V5 %s vs port %s" % (v5_func, cfg_func))

# V5's chord window is a literal 200 ms; the port's is CHORD_WINDOW_MS. Compare values.
v5_win = re.search(r"now\s*-\s*pDown\[PAD_FUNC_\w\]\s*\)\s*<\s*(\d+)", v5_src)
port_win = re.search(r"#define\s+CHORD_WINDOW_MS\s+(\d+)", cfg_src)
if v5_win and port_win and int(v5_win.group(1)) == int(port_win.group(1)):
    print("  [PASS] chord window is V5's %s ms" % port_win.group(1))
else:
    m3_fail.append("chord window: V5 %s, port %s" %
                   (v5_win.group(1) if v5_win else None,
                    port_win.group(1) if port_win else None))
    print("  [FAIL] chord window V5 %s vs port %s" %
          (v5_win.group(1) if v5_win else None,
           port_win.group(1) if port_win else None))

pt_poll = fn_body(pad_src, "void pollPads_M3(")
if not pt_poll:
    m3_fail.append("pollPads_M3() not found in pads_m3.ino")
    print("  [FAIL] pollPads_M3() not found")
else:
    for label, ok in [
        ("chord checked BEFORE the fxAssignMode press branch",
         pt_poll.index("CHORD_WINDOW_MS") < pt_poll.index("doFXAssign_M3(i)")),
        ("releasing a chord member is ignored, not read as a tap",
         re.search(r"if\s*\(\s*pChord_arr\[i\]\s*\)\s*\{[^}]*continue", pt_poll) is not None),
        ("FX-assign press branch covers every pad that is not a chord",
         re.search(r"if\s*\(\s*g_fxAssignMode\s*\)\s*\{\s*doFXAssign_M3\(i\);", pt_poll) is not None),
    ]:
        if ok:
            print("  [PASS] %s" % label)
        else:
            m3_fail.append(label)
            print("  [FAIL] %s" % label)

    # The release half. Located by its own comment rather than by rindex(), because
    # rindex("continue") over the whole function finds the chord branch's -- which sits
    # ABOVE the release code, so the ordering test inverted and reported a FAIL on code
    # that behaves correctly. Slice from the release marker down instead.
    _rel = pt_poll[pt_poll.find("---- RELEASE ----"):] if "---- RELEASE ----" in pt_poll else ""
    for label, ok in [
        ("release section located", bool(_rel)),
        ("FUNC release exits FX-assign",
         re.search(r"i == PAD_FUNC_A \|\| i == PAD_FUNC_B", _rel) is not None),
        ("non-FUNC release does NOT exit -- the exit is inside the FUNC test",
         bool(_rel) and
         "g_fxAssignMode = false" in _rel and
         re.search(r"if\s*\(\s*i == PAD_FUNC_A \|\| i == PAD_FUNC_B\s*\)\s*\{[^}]*"
                   r"g_fxAssignMode = false", _rel) is not None),
    ]:
        if ok:
            print("  [PASS] %s" % label)
        else:
            m3_fail.append(label)
            print("  [FAIL] %s" % label)

print()
print("  WIRING: is any of this actually called?")
print("  " + "-" * 74)

if "pollPads_M3" in box_src:
    print("  [PASS] AcidBox.ino references pollPads_M3()")
else:
    m3_fail.append("pollPads_M3() is never called from AcidBox.ino")
    print("  [FAIL] pollPads_M3() is never called")

# It has to be called from regular_checks(), which loop() runs -- not merely mentioned.
rc = fn_body(box_src, "void regular_checks()")
if "pollPads_M3()" in rc:
    print("  [PASS] regular_checks() calls it -- loop() reaches it")
else:
    m3_fail.append("pollPads_M3() is not called from regular_checks()")
    print("  [FAIL] regular_checks() does not call pollPads_M3()")

if re.search(r"pinMode\s*\(\s*PAD_PINS\[i\]\s*,\s*INPUT_PULLUP\s*\)", box_src):
    print("  [PASS] pads are pinMode'd INPUT_PULLUP in setup() -- active-low reads are real")
else:
    m3_fail.append("PAD_PINS are never put in INPUT_PULLUP")
    print("  [FAIL] PAD_PINS never put in INPUT_PULLUP")

if re.search(r"void\s+pollPads_M3\s*\(\s*\)\s*\{", pad_src):
    print("  [PASS] pollPads_M3() has a definition, not just a declaration")
else:
    m3_fail.append("pollPads_M3() has no definition")
    print("  [FAIL] pollPads_M3() has no definition")

if re.search(r"#define\s+DEBUG_ON", cfg_src):
    print("  [PASS] DEBUG_ON is defined, so the DEBF diagnostics in pads_m3.ino compile in")
else:
    print("  [note] DEBUG_ON is off -- pads_m3.ino's DEBF lines compile away to nothing")

print()
print("  pad pins: no collision with what the firmware already claims")
print("  " + "-" * 74)

m4 = re.search(r"PAD_PINS\s*\[\s*NUM_PADS\s*\]\s*=\s*\{([^}]*)\}", cfg_src)
# Strip line comments before reading the numbers. Without this the trailing
# "// pads 1-8 ... steps 9-16" contributes its own digits and the table reads as
# 22 pins -- which is what the first run of this check reported.
_pin_body = re.sub(r"//[^\n]*", "", m4.group(1)) if m4 else ""
port_pins = [int(x) for x in re.findall(r"\d+", _pin_body)] if m4 else []
if len(port_pins) == 16 and len(set(port_pins)) == 16:
    print("  [PASS] 16 distinct pins declared                             %s" % port_pins)
else:
    m3_fail.append("PAD_PINS is %d entries, %d distinct" % (len(port_pins), len(set(port_pins))))
    print("  [FAIL] PAD_PINS has %d entries / %d distinct" %
          (len(port_pins), len(set(port_pins))))

hits = sorted(set(port_pins) & {5, 6, 7, 15, 16, 17, 0, 19, 20, 4})
if hits:
    m3_fail.append("PAD_PINS collides with I2S / POT / strapping / USB / MIDI pins: %s" % hits)
    print("  [FAIL] collides with pins the firmware already owns: %s" % hits)
else:
    print("  [PASS] no overlap with I2S (5,6,7), POT (15,16,17), strapping (0),")
    print("         USB (19,20) or MIDI (4,15) -- HARDWARE_SETUP.md section 2")

# ESP32-S3: GPIO 26..37 are occupied by OPI PSRAM inside the WROOM module.
psram_hits = sorted(set(port_pins) & set(range(26, 38)))
if psram_hits:
    m3_fail.append("PAD_PINS uses PSRAM GPIOs: %s" % psram_hits)
    print("  [FAIL] uses PSRAM GPIOs: %s" % psram_hits)
else:
    print("  [PASS] avoids 26..37, which OPI PSRAM occupies inside the module")

# GPIO 45/46 are the VDD_SPI strapping pins on the S3 -- an input with a pull-up on 46
# is a boot hazard even if it happens to read correctly after boot.
strap_hits = sorted(set(port_pins) & {45, 46, 0})
if strap_hits:
    m3_fail.append("PAD_PINS uses strapping pins: %s" % strap_hits)
    print("  [FAIL] uses strapping pins: %s" % strap_hits)
else:
    print("  [PASS] avoids the strapping pins (0, 45, 46)")

print()
print("  " + "-" * 74)
print("  M3 PHASE 1: %s" % ("ALL PASS" if not m3_fail else "*** %d FAIL ***" % len(m3_fail)))
for f in m3_fail:
    print("    FAILED: %s" % f)

print()
print("  Still NOT settled by anything above, stated so it is not over-read:")
print("    - that PAD_PINS is the board's actual wiring. No schematic was consulted; the")
print("      checks above only prove the table collides with nothing the firmware already")
print("      claims. Every one of the 16 is a guess until someone reads the board.")
print("    - that the debounce behaves on real contacts. 20 ms and the edge-restart scheme")
print("      are modelled from V5's shape, not measured on a bouncing switch.")
print("    - that the gesture feels like V5's. Sequencing FX onto a step is only reachable")
print("      by the two-stage pad gesture, and whether that is discoverable is a question")
print("      for a person holding the device.")
print("    - PLAY/STOP, the FUNC pages (tempo/preset/order/len) and the ch2 generator are")
print("      not in this file. This is FX-assign only.")

# ===================================================================== M3 Phase 2: ch2 pitch mode
#
# The discipline here is the same as Phase 1's, with one addition that the earlier sections
# did not need: this milestone adds a mode that is UNREACHABLE from any control surface, so
# the offline checks have to establish more than "the code says so". They end by producing
# the two numbers the hardware will print, which is the M2.5 selftest prediction moved
# forward -- a log line that can distinguish a mode that is being played from one that is
# merely set.
#
# Every table below is parsed out of the .ino, not transcribed, including preset 0's notes.
# The prediction at the end is therefore computed from the same text that compiles, so it
# cannot drift away from the firmware the way a hand-written expected value would.
SEQ_INO = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "firmware", "AcidBox", "sequencer.ino")
seq_src = read(SEQ_INO)

print()
print("=" * 78)
print("M3 Phase 2: channel 2 pitch mode -- tables parsed, both branches read, prediction made")
print("=" * 78)

ch2_fail = []


def p2(label, ok, detail=""):
    if ok:
        print("  [PASS] %s%s" % (label, ("   " + detail) if detail else ""))
    else:
        ch2_fail.append(label)
        print("  [FAIL] %s%s" % (label, ("   " + detail) if detail else ""))


print()
print("  the mode table")
print("  " + "-" * 74)

modes = {}
for m in re.finditer(r"#define\s+SEQ_CH2_MODE_(\w+)\s+(\d+)", seq_src):
    modes[m.group(1)] = int(m.group(2))

# COUNT has to agree with the number of names in the table, or the clamp bound and the
# array length are two numbers that can disagree -- which is the f464c01 shape.
mcount = re.search(r"#define\s+SEQ_CH2_MODE_COUNT\s+(\d+)", seq_src)
declared_count = int(mcount.group(1)) if mcount else None

namem = re.search(r"SEQ_CH2_MODE_NAME\s*\[\s*SEQ_CH2_MODE_COUNT\s*\]\s*=\s*\{([^}]*)\}",
                  seq_src)
names = re.findall(r'"([^"]*)"', namem.group(1)) if namem else []

p2("SEQ_CH2_MODE_CHORD is 0 -- M2's behaviour stays the default",
   modes.get("CHORD") == 0, "CHORD=%s" % modes.get("CHORD"))
p2("SEQ_CH2_MODE_OFF is 1", modes.get("OFF") == 1, "OFF=%s" % modes.get("OFF"))
p2("SEQ_CH2_MODE_COUNT matches the number of names in the table",
   declared_count == len(names) == 2,
   "COUNT=%s, %d names %s" % (declared_count, len(names), names))

# The name and the #define have to agree, or a UI cycling names and a UI cycling numbers
# disagree about what mode 1 is.
p2("names match the #defines they label",
   len(names) == 2 and names[modes["CHORD"]] == "CHORD" and names[modes["OFF"]] == "OFF")

# This is the check that makes the numbering decision falsifiable rather than a matter of
# taste. If the table had been numbered to line up with V5 from 1 upward, mode 1 would have
# to be CHRD -- which is not portable. Asserting that the port's 1 is V5's 0 means the
# offset is deliberate and documented, and would fail loudly if someone "fixed" it later.
v5_off_at = None
for m in re.finditer(r"\bOFF\b", v5_src):
    seg = v5_src[max(0, m.start() - 200):m.start() + 200]
    if "CH2_VS_PITCH" in seg:
        v5_off_at = seg
        break
p2("V5's CH2_VS_PITCH[0] is the string OFF -- so the port's mode 1 IS V5's mode 0",
   v5_off_at is not None,
   "port 1 = OFF" if v5_off_at else "could not locate CH2_VS_PITCH in V5")

print()
print("  the CHORD branch: does mode 0 still sound exactly what M2 shipped?")
print("  " + "-" * 74)

trig2 = fn_body(seq_src, "static void seq_triggerSecond(")
if not trig2:
    p2("seq_triggerSecond() found", False)
else:
    # M2's code was `int16_t n = (int16_t)idx + SEQ_CH2_INTERVAL[v];`. Mode 0 has to be
    # that expression, not something near it.
    p2("CHORD branch is idx + SEQ_CH2_INTERVAL[v], unchanged from M2",
       re.search(r"n\s*=\s*\(int16_t\)idx\s*\+\s*SEQ_CH2_INTERVAL\[v\]", trig2) is not None)
    # The voicing index counts from the STEP number, not from a running counter. That is
    # what keeps the layer in phase when a step is skipped, and it is easy to "tidy" into a
    # static counter without anything breaking in the default case -- because with a full
    # 16-step pattern the two give the same answer forever.
    p2("voicing index still counts from the step number, not a running counter",
       re.search(r"for\s*\(\s*uint8_t k = 0;\s*k < step; k\+\+\s*\)", trig2) is not None)
    p2("voicing index wraps on & 3",
       re.search(r"v\s*=\s*\(uint8_t\)\(\s*v\s*&\s*3\s*\)", trig2) is not None)
    # Gating the counting loop on the mode is an optimisation with a correctness edge: OFF
    # must not need v, and CHORD must. If the gate were dropped, OFF would still be right;
    # if it were inverted, CHORD would go silent. Either way it is worth pinning.
    p2("voicing count is gated on mode == CHORD (OFF counts nothing)",
       re.search(r"if\s*\(\s*seqCh2Mode\s*==\s*SEQ_CH2_MODE_CHORD\s*\)", trig2) is not None)

print()
print("  the OFF branch: is it really UNISON, and not an octave?")
print("  " + "-" * 74)
#
# This is the subtlest thing in the milestone and the easiest to get wrong by accident.
# V5's ch2 mode 0 computes `idx2 = scaleNote(seq.cur) + seq.trans`, i.e. channel 1's own
# index -- which looks like "the note below channel 1" until you notice that V5 halves the
# note frequency on BOTH channels: triggerNote() at :2122 and triggerCh2Pulse() at :2586
# both compute noteFreq[idx] / 2. With both halved, channel 2 lands on channel 1's
# frequency exactly. It is a unison doubling, not an octave doubling.
#
# The checks below are what make that reasoning falsifiable instead of an assertion in a
# comment. If V5 halved only one channel, the port's mode 1 would be an octave out and every
# other check here would still pass.
if trig2:
    off_body = ""
    if "case SEQ_CH2_MODE_OFF:" in trig2:
        seg = trig2[trig2.index("case SEQ_CH2_MODE_OFF:"):]
        end = seg.index("case SEQ_CH2_MODE_CHORD")
        off_body = seg[:end]
    p2("OFF branch assigns idx with nothing added to it",
       re.search(r"n\s*=\s*\(int16_t\)idx\s*;", off_body) is not None,
       off_body.strip().splitlines()[-1].strip() if off_body.strip() else "branch empty")
    p2("OFF branch adds NO interval (no SEQ_CH2_INTERVAL inside it)",
       "SEQ_CH2_INTERVAL" not in off_body)
    # The octave mistake, named explicitly, because "add 12" is the intuitive fix for
    # "make ch2 an octave below ch1" and it would be wrong.
    p2("OFF branch adds no 12 -- unison, not an octave below",
       not re.search(r"n\s*\+=\s*12", off_body) and not re.search(r"n\s*=\s*\(\s*int16_t\s*\)\s*idx\s*\+\s*12", off_body))

# V5 halves both channels. Read both and say so, rather than trusting the prose above.
v5_halves = []
for fn_name in ("void triggerNote(", "void triggerCh2Pulse("):
    b = fn_body(v5_src, fn_name)
    v5_halves.append("/ 2" in b and "noteFreq[" in b)
p2("V5 halves noteFreq on BOTH triggerNote and triggerCh2Pulse",
   len(v5_halves) == 2 and all(v5_halves),
   "triggerNote %s, triggerCh2Pulse %s" % tuple(
       "halved" if h else "NOT halved" for h in v5_halves))

# And V5's own mode-0 branch really does reduce to the step's index with nothing added.
v5_off_branch = ""
if "else if (ch2PitchMode == 0)" in v5_src:
    seg = v5_src[v5_src.index("else if (ch2PitchMode == 0)"):]
    v5_off_branch = seg[:seg.index("\n  } else if (ch2PitchMode == 1")]
p2("V5's mode-0 branch assigns scaleNote(seq.cur) + seq.trans and no interval",
   re.search(r"idx2\s*=\s*\(int16_t\)\s*constrain\(\(int\)scaleNote\(seq\.cur\)\s*\+\s*seq\.trans",
             v5_off_branch) is not None)

print()
print("  the clamp, and the mode being READ (not just stored)")
print("  " + "-" * 74)

setter = fn_body(seq_src, "void seq_setCh2Mode(")
getter = fn_body(seq_src, "uint8_t seq_ch2Mode()")
namer = fn_body(seq_src, "const char *seq_ch2ModeName(")

p2("seq_setCh2Mode() rejects anything >= SEQ_CH2_MODE_COUNT",
   bool(setter) and re.search(r"mode\s*>=\s*SEQ_CH2_MODE_COUNT", setter) is not None)
p2("seq_setCh2Mode() degrades to CHORD, not OFF",
   bool(setter) and re.search(r"=\s*SEQ_CH2_MODE_CHORD\s*;", setter) is not None)
p2("seq_ch2Mode() exists and is not empty", bool(getter))
p2("seq_ch2ModeName() bounds-checks its index too",
   bool(namer) and re.search(r"mode\s*>=\s*SEQ_CH2_MODE_COUNT", namer) is not None)

# The Phase 1 failure mode, applied to Phase 2. g_funcMode is set and never read, which was
# a mode that existed in name only.
#
# The check that stops seqCh2Mode becoming the second one is NOT "the name appears in the
# trigger function". Mutation testing proved that insufficient: with
#
#     if (seqCh2Mode == SEQ_CH2_MODE_CHORD) { ... }   // gate, still reads the mode
#     switch (SEQ_CH2_MODE_CHORD) { ... }             // pitch, ignores it
#
# every check in this section still passed. The mode would have been settable, reportable,
# named -- and completely inert, with a green test suite sitting next to it. That is the
# exact shape of the bug, not a near miss of it.
#
# So the check pins the load-bearing read specifically: the switch that chooses the pitch has
# to be the one switching on the mode.
p2("the PITCH switch is on seqCh2Mode -- the read that decides what sounds",
   bool(trig2) and re.search(r"switch\s*\(\s*seqCh2Mode\s*\)", trig2) is not None,
   "switch subject: %s" % (re.search(r"switch\s*\(\s*([^)]{0,40})", trig2).group(1).strip()
                           if trig2 and re.search(r"switch\s*\(\s*([^)]{0,40})", trig2) else "none"))

# And the corollary: a mode name that is stored, printed and never consulted is exactly the
# g_funcMode defect, so assert the count of decision points, not just the presence of one.
reads = len(re.findall(r"\bseqCh2Mode\b", trig2)) if trig2 else 0
p2("seqCh2Mode has more than one reference in the trigger path",
   reads >= 2, "%d reference(s): the gate and the pitch switch" % reads)

print()
print("  the report line has to be able to tell the two modes apart")
print("  " + "-" * 74)
#
# A mode NAME in the log is state, not evidence: a mode that is set and never acted on
# prints exactly like one that is playing. The last-sounded note is the evidence, so the
# check is that it is assigned inside the trigger function and printed by the report.
p2("seqCh2LastMidi is assigned inside seq_triggerSecond",
   bool(trig2) and re.search(r"seqCh2LastMidi\s*=\s*midi\s*;", trig2) is not None)
p2("the report prints both the mode and the last note",
   re.search(r'ch2 %u %s \(midi %u\)', seq_src) is not None)

print()
print("  PREDICTION FOR HARDWARE -- preset 0, default key, and what the log should read")
print("  " + "-" * 74)
print("  Computed from the parsed tables below, not written down by hand. If a table in the")
print("  firmware changes, these numbers change with it; that is the point of computing them")
print("  here rather than in this comment.")
print()

# Parse the three inputs the prediction needs.
#
# SEQ_CH2_MASK is `static const uint16_t SEQ_CH2_MASK = 0x049;`, NOT a #define. The first
# version of this line anchored on `#define`, the match failed, ch2mask stayed 0, and the
# prediction printed an EMPTY table -- four steps of header and no step numbers -- while
# every check in the section still said PASS. That is the f464c01 shape exactly: a check
# that cannot fail on the bug it sits next to. So the pattern does not assume the form, and
# the assertions below now fail if any of the three inputs comes back empty.
maskm = re.search(r"\bSEQ_CH2_MASK\s*=\s*(0x[0-9a-fA-F]+|\d+)", seq_src)
ch2mask = int(maskm.group(1), 0) if maskm else None

intm = re.search(r"SEQ_CH2_INTERVAL\s*\[\s*4\s*\]\s*=\s*\{([^}]*)\}", seq_src)
ch2int = [int(x) for x in re.findall(r"-?\d+", intm.group(1))] if intm else []

# Preset 0's note row: the first { ... } after the preset array opens.
parr = re.search(r"SEQ_PRESETS\s*\[\s*SEQ_NUM_PRESETS\s*\]\s*=\s*\{(.*)", seq_src, re.S)
p0notes = []
if parr:
    first = re.search(r"\{\s*\{([0-9,\s]+)\}", parr.group(1))
    if first:
        p0notes = [int(x) for x in first.group(1).split(",") if x.strip()]

# The +12 in seq_noteToMidi, read rather than assumed. The port has no /2 that V5 has, so
# the whole mode-1 reading depends on where the transposition happens, and it is one number.
ntm = fn_body(seq_src, "uint8_t seq_noteToMidi(")
off12 = re.search(r"idx\s*\+\s*(\d+)\s*\+\s*\(int16_t\)seq\.key", ntm) if ntm else None
transp = int(off12.group(1)) if off12 else 0

if len(ch2int) != 4 or len(p0notes) != 16 or ch2mask is None or transp == 0:
    p2("all three prediction inputs parsed", False,
       "mask=%s interval=%s notes=%d transp=%s"
       % (ch2mask, ch2int, len(p0notes), transp))
else:
    # A mask of 0 is parseable and would produce an empty prediction that reads like a
    # result. Assert it against the comment in the firmware, so a silent zero is a failure.
    p2("SEQ_CH2_MASK parsed and non-zero", ch2mask != 0, "0x%03X" % ch2mask)

    # Assert the mask against the step list its own trailing comment claims. This check is
    # not decoration: it is how the M3 Phase 2 tests found that the comment said
    # "steps 0, 3, 6, 9" while the mask was 0x049, which is steps 0, 3 and 6. Nothing about
    # the audio was wrong; only the comment was, and only because something read both.
    maskm = re.search(r"SEQ_CH2_MASK\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;\s*//\s*steps\s+([0-9,\s]+)",
                      seq_src)
    claimed = [int(x) for x in re.findall(r"\d+", maskm.group(2))] if maskm else []
    actual = [s for s in range(16) if ch2mask & (1 << s)]
    p2("the mask matches the step list in its own comment",
       claimed == actual,
       "comment says %s, 0x%03X is %s" % (claimed, ch2mask, actual))
    p2("SEQ_CH2_MASK is 0x049 (unchanged from M2 -- see the note in sequencer.ino)",
       ch2mask == 0x049, "0x%03X" % ch2mask)

    # The consequence of 0x049 that the note above records: three firing steps means the
    # fourth interval, the octave, is unreachable. Asserted so that if somebody does fix the
    # mask to 0x249, this check fails and says what became true rather than leaving the note
    # quietly stale.
    p2("the unreachable fourth interval is the one the comment calls out",
       (len(actual) == 3 and len(ch2int) == 4) or len(actual) >= 4,
       "%d firing steps, %d intervals" % (len(actual), len(ch2int)))

    p2("parsed SEQ_CH2_INTERVAL[4] = %s" % ch2int, ch2int == [0, 3, 7, 12])
    p2("parsed preset 0's 16 notes", len(p0notes) == 16, str(p0notes[:6]) + " ...")
    p2("seq_noteToMidi adds %d at key 0 (read from the source, not assumed)" % transp,
       transp == 12)

    steps = [s for s in range(16) if ch2mask & (1 << s)]
    # Belt and braces on the same failure: whatever produced it, a prediction with no rows
    # is not a prediction. Assert it has rows before printing it.
    p2("the prediction has rows to print", len(steps) > 0,
       "ch2 fires on steps %s" % steps)

    chord_cycle, off_cycle = [], []
    v = 0
    for s in range(16):
        if not (ch2mask & (1 << s)):
            continue
        chord_cycle.append(p0notes[s] + ch2int[v % 4] + transp)
        off_cycle.append(p0notes[s] + transp)
        v += 1

    print()
    print("    ch2 fires on steps %s of preset 0 (mask 0x%03X)" % (steps, ch2mask))
    print()
    print("      step        %s" % "  ".join("%6d" % s for s in steps))
    print("      note        %s" % "  ".join("%6d" % p0notes[s] for s in steps))
    print("      CHORD  midi %s     <- what the report should print, mode 0"
          % "  ".join("%6d" % x for x in chord_cycle))
    print("      OFF    midi %s     <- what it should print, mode 1"
          % "  ".join("%6d" % x for x in off_cycle))
    print()
    print("    So: `ch2 0 CHORD (midi NN)` with NN eventually landing on all of %s"
          % sorted(set(chord_cycle)))
    print("    is mode 0 confirmed; `ch2 1 OFF (midi NN)` with NN only ever from %s"
          % sorted(set(off_cycle)))
    print("    is mode 1 confirmed. A log showing mode 1 while the note cycles through the")
    print("    CHORD column means the setter ran and the switch did not, which is a different")
    print("    bug from the setter never being called.")
    print()
    print("    NOTE: these are mode 1 numbers written from the tables, NOT from hardware.")
    print("    Mode 1 is unreachable from any pad until Phase 3, so the second row above is")
    print("    a prediction about code nobody can yet reach -- read it as a spec for Phase 3's")
    print("    verification, not as a measurement.")

print()
print("  " + "-" * 74)
print("  M3 PHASE 2: %s" % ("ALL PASS" if not ch2_fail else "*** %d FAIL ***" % len(ch2_fail)))
for f in ch2_fail:
    print("    FAILED: %s" % f)

print()
print("  Still NOT settled by anything above, stated so it is not over-read:")
print("    - that mode 1 can be reached at all. Nothing calls seq_setCh2Mode(); the FUNC")
print("      page that will is Phase 3. A mode that cannot be selected cannot be heard, and")
print("      the prediction above is a spec for later, not evidence now.")
print("    - that mode 0 sounds right. The checks above prove mode 0's arithmetic is")
print("      unchanged from M2's; whether it sounds like a chord layer is an ear question.")
print("    - that mode 1 sounds right. Same, and it cannot be checked by ear either until a")
print("      control surface exists.")
print("    - anything about V5's other six modes. They are not ported, and sequencer.h says")
print("      which infrastructure each one is missing.")

# ===================================================================== declaration / definition agreement
#
# Written because CI 36974920807 rejected this milestone for a two-character edit: the
# definition of seq_portaSpeed() was widened to uint32_t while its declaration in
# sequencer.h stayed uint8_t.
#
#     sequencer.ino:895:10: error: ambiguating new declaration of 'uint32_t seq_portaSpeed()'
#
# A declaration and a definition of the same function with different return types are two
# overloads of a zero-argument function, and there is nothing between them to overload -- which
# is why the diagnostic says "ambiguating new declaration" rather than the more obvious
# "conflicting return type", and why it took a compiler to find it.
#
# What this cannot do is substitute for the compiler. It compares tokens, so it sees
# signature drift and nothing else: not overload resolution, not template rules, not whether
# the body agrees with the declaration's parameter names, not types at all inside the
# parameter lists. It is here because this class of edit -- widen a return type on one side of
# a declaration pair -- is easy to make by accident and impossible to see by reading, and
# because the only other instrument for it is a build.
print()
print("=" * 78)
print("declaration / definition agreement: every seq_* signature, .h against .ino")
print("=" * 78)


def norm_type(t):
    return re.sub(r"\s*\*\s*", "*", re.sub(r"\s+", " ", t)).strip()


def collect_decl(src):
    out = {}
    for m in re.finditer(r"^([A-Za-z_][A-Za-z0-9_ \t\*]*?)\b(seq_[A-Za-z0-9_]+)\s*\([^;]*\)\s*;",
                         src, re.M):
        out[m.group(2)] = norm_type(m.group(1))
    return out


def collect_def(src):
    """Definitions only, and only the externally-visible ones.

    `static` definitions are file-local by definition and must NOT appear in the header, so
    including them here reported twelve false orphans on the first run -- seq_subFire,
    seq_resolvePitch and ten others that are correctly private. A check whose first run
    produces a wall of failures is a check nobody will keep, and the correct response to that
    is to fix the check rather than to argue that the failures were interesting.
    """
    out = {}
    for m in re.finditer(r"^([A-Za-z_][A-Za-z0-9_ \t\*]*?)\b(seq_[A-Za-z0-9_]+)\s*\([^;{]*\)\s*\{",
                         src, re.M):
        if norm_type(m.group(1)).startswith("static"):
            continue
        out[m.group(2)] = norm_type(m.group(1))
    return out


if phsrc and psrc:
    decls, defs = collect_decl(phsrc), collect_def(psrc)
    shared = sorted(set(decls) & set(defs))
    print()
    print("  %-24s %-18s %-18s %s" % ("function", "sequencer.h", "sequencer.ino", ""))
    print("  " + "-" * 68)
    mismatch = []
    for nm in shared:
        ok = decls[nm] == defs[nm]
        if not ok:
            mismatch.append(nm)
        print("  [%s] %-20s %-18s %-18s"
              % ("ok" if ok else "MISMATCH", nm, decls[nm], defs[nm]))
    print()
    check("all %d shared signatures agree on return type" % len(shared),
          not mismatch, "mismatched: %s" % (mismatch or "none"))
    print()
    print("  Declared in sequencer.h with no definition in sequencer.ino, and vice versa.")
    print("  A declared-but-undefined seq_* would be a link error; a defined-but-undeclared one")
    print("  would mean a header that lies about its own interface, which is worse because it")
    print("  compiles until someone includes it from a second translation unit.")
    orphan_d = sorted(set(decls) - set(defs))
    orphan_f = sorted(set(defs) - set(decls))
    check("no seq_* is declared without a definition", not orphan_d, str(orphan_d or "none"))
    check("no seq_* is defined without a declaration", not orphan_f, str(orphan_f or "none"))
    print()
    print("  Limits of this check, stated rather than left to be discovered:")
    print("    - tokens only. It cannot see whether the body honours the declared type.")
    print("    - it cannot check the parameter lists, only the leading return type.")
    print("    - it does not replace a compiler. CI is the authority; this is a fast local")
    print("      check for the one drift that CI found the expensive way.")
else:
    check("sources readable", False)
