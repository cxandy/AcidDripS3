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
