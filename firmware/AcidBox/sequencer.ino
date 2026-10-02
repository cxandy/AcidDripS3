/*
 * sequencer.ino -- M2. See sequencer.h for what this is and, more usefully, what it
 * deliberately is not.
 *
 * .ino and not .cpp, for the same reason engine_iface is: the sketch is concatenated into
 * one translation unit, and this file needs engine_iface's declarations ahead of it.
 * Alphabetically sequencer.ino sorts after sampler.ino and before synthvoice.ino, and
 * engine_iface.ino sorts well before both, so the plain concatenation order happens to be
 * right -- but AcidBox.ino sorts FIRST and calls seq_poll(), so AcidBox.ino includes
 * sequencer.h explicitly rather than relying on that.
 */

#include "config.h"
#include "sequencer.h"

// =====================================================================
// PRESETS
//
// V5's eight built-in riffs: note, flags (bit0 active, bit1 accent), glide, effect.
// Key C, absolute V5 indices -- index 24 is C2, so these are the bass register.
// =====================================================================
#define SEQ_NUM_PRESETS 8

struct PresetPattern {
  uint8_t note[16];
  uint8_t flags[16];   // bit0 = active, bit1 = accent
  uint8_t glide[16];
  uint8_t effect[16];
};

static const PresetPattern SEQ_PRESETS[SEQ_NUM_PRESETS] = {
  // P0: DFLT -- the default riff, all steps live, no accents. This is the pattern the
  // device already knows how to play, which makes it the right thing to start on.
  { {24,24,24,27,24,36,31,29,24,31,29,31,36,24,36,39}, {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
  // P1: SQNCE -- a tight 8-step motif looped twice, with accent+glide pairs on the push
  // notes so the slide path is exercised within the first two bars.
  { {24,24,31,24,29,27,24,29,24,24,31,24,29,27,24,29}, {3,0,1,0,3,1,0,1,3,0,1,0,3,1,0,1}, {0,1,0,0,0,1,1,0,0,1,0,0,0,1,1,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
  // P2: FUNK -- sparse, punchy, rests in the middle of phrases
  { {24,24,24,27,24,24,31,24,29,24,24,27,24,31,29,24}, {3,0,0,1,0,1,3,0,3,0,0,1,0,1,1,3}, {0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
  // P3: MINI -- hypnotic minimal loop, one accent shift
  { {24,24,27,24,24,27,24,29,24,24,27,24,29,31,29,27}, {1,1,1,1,1,1,1,3,1,1,1,1,1,3,1,1}, {0,0,1,0,0,1,0,0,0,0,1,0,0,0,1,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
  // P4: JUMP -- octave leaps, big energy
  { {24,36,24,36,27,39,27,31,24,36,29,36,24,34,36,24}, {3,1,1,1,3,1,1,1,3,1,1,1,3,1,1,3}, {0,0,0,1,0,0,0,0,0,0,1,0,0,0,1,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
  // P5: RAVE -- pumping sixteenths, accent on every downbeat-ish step
  { {24,36,24,27,36,24,34,36,24,36,27,36,29,36,34,39}, {3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,3}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
  // P6: SYNC -- syncopated, off-beat accents and rests on the downbeats
  { {24,24,24,29,27,24,31,29,24,27,24,29,31,31,34,36}, {0,3,0,1,3,0,1,1,0,3,0,1,3,0,1,3}, {0,0,0,0,0,0,1,0,0,0,0,1,0,0,0,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
  // P7: DARK -- sparse minor with an Ab passing note at step 12
  { {24,24,27,24,29,27,24,29,24,27,24,27,32,31,29,27}, {3,0,1,0,3,1,0,1,3,1,0,1,3,1,1,3}, {0,1,0,0,0,1,1,0,0,0,0,1,0,0,1,0}, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0} },
};

// =====================================================================
// DRUM PATTERN
//
// Twelve slots per kit, in the order Sampler::NoteOn walks them (note % 12):
//   0 BD  1 SD  2 LT  3 HT  4 RM  5 CP  6 CH  7 OH  8 RD  9 CR  10 CO  11 CL
// The names are read off /data/<kit>/, which is 001_BD9 .. 012_CL9. That directory listing
// is the only ground truth for the slot order -- there is no enum anywhere in the sampler
// to check it against, which is why it is written down here rather than left as folklore.
//
// A plain four-on-the-floor with a backbeat, which is the least presumptuous pattern that
// still proves the seam works: kick and snare land on different steps, so a clock that
// drifts shows up as the two voices separating instead of hiding inside a single hit.
//
// This is a fixed pattern, not BeatMachine2. See sequencer.h for what that leaves out.
// =====================================================================
static const uint16_t SEQ_DRUM_PATTERN[12] = {
  /* BD */ 0x0005,   // steps 0, 2
  /* SD */ 0x000A,   // steps 1, 3
  /* LT */ 0x0000,
  /* HT */ 0x0000,
  /* RM */ 0x0000,
  /* CP */ 0x0020,   // step 5
  /* CH */ 0x5555,   // 0b0101010101010101 -- every other step, the offbeat 8ths
  /* OH */ 0x0080,   // step 7
  /* RD */ 0x0000,
  /* CR */ 0x0000,
  /* CO */ 0x0000,
  /* CL */ 0x0000,
};

// =====================================================================
// CHANNEL 2 PATTERN
//
// V5's channel 2 is a rhythmic pulse layer with its own step pattern, and its pitch is
// built from channel 1's current note so a transposition cannot leave the two clashing.
// Both of those properties are kept; almost nothing else is.
//
// SEQ_CH2_MASK is much sparser than channel 1 on purpose. A chord stab every fourth step
// under a sixteenth-note bassline is a texture; a chord on every sixteenth is a wall.
// The intervals are a minor-ish voicing measured from the current step's own note, so it
// follows transposition for free and needs no generator to stay consonant.
// =====================================================================
static const uint16_t SEQ_CH2_MASK       = 0x049;    // steps 0, 3, 6, 9
static const int8_t    SEQ_CH2_INTERVAL[4] = { 0, 3, 7, 12 };  // root, m3, 5, oct

// =====================================================================
// SEQUENCER-OWNED PARAMETERS
//
// These are the sequencer's, not the patch's, and M2 is honest about what that costs: the
// sequencer writes CC 75 (filter envelope amount) on every step, so a live CC 75 from a
// cable gets overwritten on the next sixteenth. That is a real limitation, not a rounding
// error. M3 is where it gets fixed, by giving the sequencer its own depth parameter and
// having it stop touching the patch's value.
//
// SEQ_ENVMOD_BASE is the unaccented depth. Accent doubles it, capped, which is V5's
// `gEnvCutoff = accent ? min(gEnvCutNorm * 2, 255) : gEnvCutNorm` with the target being a
// 0-127 CC instead of a 0-255 global. 100 matches what both of AcidBox's stock patches
// already use, so switching the sequencer on does not change the unaccented sound.
// =====================================================================
#define SEQ_ENVMOD_BASE  100
#define SEQ_VEL_DRUMS    110

/*
 * SLIDE TIMES -- V5's glide, replaced.
 *
 * V5 slid by advancing a fixed-point pitch accumulator gFreqFP by gGlideStep, recomputed
 * once per trigger and applied by updateControl() once every 3.9 ms. gPortaSpeed 1-8 mapped
 * to step counts of 32/24/16/12/8/6/4/2, which times that 3.9 ms period comes out at
 * 125/94/63/47/31/23/16/8 ms.
 *
 * The design doc's replacement table is {200,150,110,80,60,45,30,20} ms, and it is both
 * slower and better: at 120 BPM a sixteenth is 125 ms, so V5's fastest slide finished in
 * 8 ms -- 6% of a step -- which is a pitch jump wearing a costume.
 *
 * BUT the table cannot be used as written. SynthVoice::ParseCC does
 * `_slideMs = (float)cc_value`, so CC 5 is milliseconds 1:1 and tops out at 127.
 *
 * Clamping is worse than it looks: 200 and 150 both become 127, so speeds 1 and 2 would be
 * the same control -- one position on an eight-position knob that does nothing, which is
 * the kind of defect nobody notices for a year because the knob still turns.
 *
 * So the doc's range is remapped linearly onto the CC's range instead, preserving what the
 * table is actually for -- eight monotonically decreasing slide speeds -- inside the range
 * the hardware has:
 *
 *   cc = 20 + (ms - 20) * 107 / 180
 *
 *   200 ms -> 127    150 ms ->  97    110 ms ->  73    80 ms ->  56
 *    60 ms ->  44     45 ms ->  35     30 ms ->  26     20 ms ->  20
 *
 * Same order, same curve, eight distinct speeds, and the fastest is still four times
 * slower than V5's -- so a slide is audible as a slide. This is the one place M2 deviates
 * from the design doc's literal numbers, and the reason is a hardware limit rather than a
 * preference. If CC 5 is ever given a wider range, put the doc's table back verbatim.
 */
static const uint8_t SEQ_PORTA_CC[8] = { 127, 97, 73, 56, 44, 35, 26, 20 };

// =====================================================================
// STATE
// =====================================================================
static Sequencer seq;

/*
 * HELD-NOTE BOOKKEEPING -- the least obvious thing in this file, so it gets the most words.
 *
 * SynthVoice keeps a monophonic-with-legato stack of held notes (mvaStack, MIDI_MVA_SZ 8).
 * Two consequences, both of which this code depends on:
 *
 *  1. on_midi_noteON computes `slide = (mvaStack.n > 1)`. So if notes accumulate, the
 *     stack stays deeper than 1 and EVERY subsequent note-on is legato -- no envelope
 *     retrigger, just a pitch move. A sequencer that posted note-ons and never posted
 *     note-offs would therefore fill the stack once and then degrade into one continuous
 *     glide for the rest of the session. Silent, and it sounds like a tuning fault rather
 *     than like a bug, which is the worst combination available.
 *
 *  2. on_midi_noteOFF, when the stack still has an older note under the one just freed,
 *     calls note_on() with `slide = 1` to glide back to it. That is correct for a held
 *     chord and is exactly what must NOT happen when a slide ends.
 *
 * The gate model here follows from those two, and it is not a stylistic choice:
 *
 *   - A slide step must NOT release the note it is sliding away from. Releasing puts the
 *     amp envelope into release, and note_on() with _slide set does not retrigger it, so
 *     the note would fade out while it was still gliding. The old note stays on the stack
 *     and the new one goes on top: depth 2, so slide is true from the stack as well as
 *     from CC 65. Both mechanisms agree, which is a nice thing to be able to say.
 *
 *   - A non-slide step MUST drain everything, or consequence (1) above kicks in. And it
 *     must drain OLDEST FIRST. Draining newest-first frees the top note while an older one
 *     is still underneath, which triggers consequence (2): the engine glides back to the
 *     old note and only then releases. Oldest-first means notes[0] never changes during
 *     the drain, so that path is never entered.
 *
 * Depth 8 matches MIDI_MVA_SZ. A slide chain longer than 8 steps would need more, but
 * mva_alloc() shifts at that point anyway, and 8 sixteenths of unbroken slide is not a
 * pattern anyone has written down.
 */
#define SEQ_HELD_MAX 8
static uint8_t seqAcidHeld[SEQ_HELD_MAX];
static uint8_t seqAcidHeldN = 0;
static uint8_t seqSecondHeld = 0;
static bool    seqSecondHeldOn = false;

/* Periodic-report window bookkeeping. Declared here rather than next to seq_report()
 * because seq_start() below has to reset it, and C++ will not let a function at line 600
 * touch a static first declared at line 700. The reporting itself, and the reasoning behind
 * every number it prints, lives with the clock at the bottom of this file.
 */
static uint32_t seqRepLastMs    = 0;
static uint32_t seqRepLastSteps = 0;
static uint32_t seqRepLastCatch = 0;
static uint32_t seqRepPrintUs   = 0;   // duration of the PREVIOUS print; 0 on the first

/* Push a note we are about to sound onto the held list. */
static inline void seqAcidHold(uint8_t note) {
  if (seqAcidHeldN < SEQ_HELD_MAX) { seqAcidHeld[seqAcidHeldN++] = note; }
  else                             { seqAcidHeld[SEQ_HELD_MAX - 1] = note; }
}

/* Release everything, oldest first. See note (2) above for why the order is not free. */
static void seqAcidReleaseAll() {
  for (uint8_t i = 0; i < seqAcidHeldN; i++) {
    eng_noteOff(Ch::Acid, seqAcidHeld[i]);
  }
  seqAcidHeldN = 0;
}

/* xorshift32 for SEQ_ORDER_RANDOM. Not Arduino's random(), because that shares global state
 * with myRandom() in AcidBanger.ino and, more to the point, because the jukebox used to be
 * the thing feeding entropy into it -- and the jukebox is off as of this milestone. A
 * sequencer whose random order depends on another subsystem being compiled in is a
 * sequencer that breaks silently when that subsystem goes away.
 */
static uint32_t seqRngState = 0x1D872B41u;

static inline uint32_t seqRand() {
  uint32_t x = seqRngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  seqRngState = x;
  return x;
}

// =====================================================================
// STEP ADVANCE
// =====================================================================

uint8_t seq_nextPatStep() {
  // Every branch has to survive len == 0, because len is settable from a future UI and a
  // length of zero is the obvious way to spell "no pattern loaded". V5's version computed
  // `last = len - 1` on a uint8_t and got 255, then compared cur against it -- harmless
  // only because nothing ever set len to 0.
  uint8_t len = seq.len;
  if (len == 0) { return 0; }
  uint8_t last = (uint8_t)(len - 1);

  switch (seq.rrMode) {
    case SEQ_ORDER_FORWARD:
      return (seq.cur >= last) ? 0 : (uint8_t)(seq.cur + 1);

    case SEQ_ORDER_BOUNCE8: {
      // V5's CW: bounces over the first 8 indices (pads 1-8) rather than following the
      // pattern straight, so a 4-step pattern plays 0,1,2,3,2,1 instead of 0,1,2,3,0,1,2,3.
      //
      // The bound is min(7, len-1), and the clamp is a correction rather than a stylistic
      // choice. V5 used a flat 7 "regardless of pattern length", which means a 4-step
      // pattern bounces through steps 4, 5, 6 and 7 -- which exist in the 16-entry array,
      // so nothing crashes, but which sit outside the pattern. Their active/accent flags
      // belong to whatever preset was loaded before, so the bounce plays either silence or
      // a stale note depending on history. It is a musical fault dressed up as an index.
      // Clamping keeps the character of the order (it still bounces, still ignores forward
      // order) while staying inside the pattern the user can actually see.
      const uint8_t CW_MAX = (last < 7) ? last : 7;
      if (seq.rrPingFwd) {
        if (seq.cur >= CW_MAX) {
          seq.rrPingFwd = false;
          return (CW_MAX > 0) ? (uint8_t)(CW_MAX - 1) : 0;
        }
        return (uint8_t)(seq.cur + 1);
      } else {
        // The CW_MAX > 0 guard is load-bearing at len == 1, where there is nowhere to
        // bounce to and returning 1 would be out of range.
        if (seq.cur == 0) { seq.rrPingFwd = true; return (CW_MAX > 0) ? 1 : 0; }
        return (uint8_t)(seq.cur - 1);
      }
    }

    case SEQ_ORDER_ALTERNATE: {
      if (len <= 1) { return 0; }
      uint16_t next = (uint16_t)seq.cur + 2;
      if ((seq.cur % 2) == 0) { return (uint8_t)((next >= len) ? 1 : next); }  // even -> odd
      return (uint8_t)((next >= len) ? 0 : next);                            // odd  -> even
    }

    case SEQ_ORDER_REVERSE:
      return (seq.cur == 0) ? last : (uint8_t)(seq.cur - 1);

    case SEQ_ORDER_SKIP2:
      return (uint8_t)(((uint16_t)seq.cur + 2) % len);

    case SEQ_ORDER_SKIP3:
      // True modulo in a wider type, not `next - len`. V5's own comment records that the
      // subtraction version undershot for len 1 and 2 and walked seq.cur straight out of
      // the 16-element array; the modulo cannot, and the cast is what stops cur+3 from
      // overflowing uint8_t before the mod ever sees it.
      return (uint8_t)(((uint16_t)seq.cur + 3) % len);

    case SEQ_ORDER_PINGPONG:
      if (seq.rrPingFwd) {
        if (seq.cur >= last) {
          seq.rrPingFwd = false;
          return (last > 0) ? (uint8_t)(last - 1) : 0;
        }
        return (uint8_t)(seq.cur + 1);
      } else {
        // The `last > 0` guard is load-bearing at len == 1. V5 returned 1 unconditionally
        // here, which is the only step index that exists at len == 1 when 0 does not --
        // an out-of-range cursor that reads steps[1] out of a one-step pattern. Caught by
        // simulating all eight orders against every length from 1 to 16, which is the only
        // reason it was found before it shipped: on the bench a one-step pattern that
        // wanders to step 1 looks like an off-by-one in the tempo, not like an array read.
        if (seq.cur == 0) { seq.rrPingFwd = true; return (last > 0) ? 1 : 0; }
        return (uint8_t)(seq.cur - 1);
      }

    case SEQ_ORDER_RANDOM: {
      // Collect the active steps and pick from those, not from all of them. Picking from
      // all of them would land on a rest often enough to be heard as the sequencer
      // dropping out, and "sometimes silent" is much worse to listen to than "sometimes
      // repeats the note you just played".
      uint8_t actv[SEQ_NUM_STEPS];
      uint8_t n = 0;
      for (uint8_t s = 0; s < len && s < SEQ_NUM_STEPS; s++) {
        if (seq.steps[s].active) { actv[n++] = s; }
      }
      if (n == 0)     { return seq.cur; }                 // nothing to jump to; stay put
      if (n == 1)     { return actv[0]; }
      uint8_t pick = actv[seqRand() % n];
      if (pick == seq.cur) { pick = actv[seqRand() % n]; } // one retry, then accept a repeat
      return pick;
    }

    default:
      return (seq.cur >= last) ? 0 : (uint8_t)(seq.cur + 1);
  }
}

// =====================================================================
// THE THREE TRIGGER SEAMS
//
// V5 wrote engine globals from all three of these. Each posts events instead. The comments
// name the V5 function each replaces, because "why is there a drum pattern in the
// sequencer" is a question the seam is the answer to.
// =====================================================================

/* SEAM 1 of 3 -- V5's triggerNote() (Acid_Drip_Drum_Acid_Drift_V5.ino:2120).
 *
 * V5 set gFreq / gTarget / gGlideStep for the pitch move, gEnvCutoff and gEnvRes for the
 * accent's filter envelope, and gAccentActive for its longer decay, then let
 * updateControl() walk all of it forward 256 times a second.
 *
 * Here: one CC to arm the slide, one CC for the accent's envelope depth, one note-on. The
 * longer decay is not written here because AcidBox already has it -- note_on() selects the
 * accent decay and attack times when, and only when, velocity clears the gate at
 * synthvoice.ino:223, and CC 76 sets how deep that accent is. Velocity is the gate and the
 * CC is the depth, which is what the design doc asked for and also what the engine was
 * built around. So accent rides on the note as a flag rather than arriving as a pile of
 * parameter writes that have to be undone again.
 */
static void seq_triggerNote(uint8_t idx, bool accent, bool glide) {
  uint8_t midi = seq_noteToMidi(idx);

  // A slide needs something to slide FROM. If nothing is held -- a rest, the first step
  // after a stop, the step after a slide chain ended -- then portamento would glide the
  // oscillator in from wherever it last was, which is a pitch artefact and not a slide.
  // The CC is therefore armed only when there is genuinely a held note to bend.
  bool doSlide = glide && (seqAcidHeldN > 0);

  // Portamento is channel state, not note state: CC 65 sticks until something clears it.
  // So it is written on EVERY step, not only on the steps that want it. A sequencer that
  // armed the slide and forgot to disarm it would have every later note sliding, and that
  // gets heard as "the pitch drifts" rather than as "a CC is stuck" -- which is why the M2
  // acceptance criterion is phrased in terms of pitch. Writing it unconditionally makes
  // that failure structurally impossible instead of merely testable.
  eng_setParam(Ch::Acid, CC_303_PORTAMENTO, doSlide ? 127 : 0);

  // Same argument for the accent's envelope depth: post the plain value on unaccented
  // steps, so the doubling cannot survive past the step that asked for it.
  uint8_t doubled = (uint8_t)(SEQ_ENVMOD_BASE * 2 > 127 ? 127 : SEQ_ENVMOD_BASE * 2);
  eng_setParam(Ch::Acid, CC_303_ENVMOD_LVL, accent ? doubled : (uint8_t)SEQ_ENVMOD_BASE);

  eng_noteOn(Ch::Acid, midi, ENG_VEL_PLAIN, accent);

  // The slide source stays on the held list; the new note goes on top. See the long note on
  // seqAcidHeld for why releasing it here would kill the very envelope the slide needs.
  seqAcidHold(midi);
}

/* SEAM 2 of 3 -- V5's bmTriggerStep() (BeatMachine2.ino:934), reduced to what M2 needs.
 *
 * V5 fired six drum voices per step with per-hit probability, swing, and a Euclidean fill
 * engine. What is left here is the part that is actually a sequencer: a pattern saying
 * which slots fire on which steps.
 *
 * No note-off, and that is not an oversight. Sampler::NoteOff() is an empty function with a
 * comment in it saying so -- it returns and does nothing. Drum samples are one-shots that
 * run to the end of their own waveform, so a gate here would post an event that provably
 * has no effect. Emitting it anyway would cost queue depth and imply a gate that does not
 * exist.
 */
static void seq_triggerDrums(uint8_t step) {
  if (!seq.drumsOn)    { return; }
  if (step >= SEQ_NUM_STEPS) { return; }

  // The note number is kit*12 + slot, which is the convention init_instruments() already
  // uses (AcidBanger.ino:474) and the only thing Sampler::NoteOn's `note % repeat`
  // arithmetic agrees with. seq_noteToMidi() does NOT apply here: a drum note is an index
  // into the kit, not a pitch, and running it through a +12 transposition would put every
  // hit on the wrong sample.
  const uint8_t base = (uint8_t)(DEFAULT_DRUMKIT * 12);

  for (uint8_t slot = 0; slot < 12; slot++) {
    if (SEQ_DRUM_PATTERN[slot] & ((uint16_t)1 << step)) {
      eng_noteOn(Ch::Drums, (uint8_t)(base + slot), SEQ_VEL_DRUMS, false);
    }
  }
}

/* SEAM 3 of 3 -- V5's triggerCh2Pulse() (Acid_Drip_Drum_Acid_Drift_V5.ino:2496).
 *
 * V5 built this pitch from the current step's root through a Euclidean generator, a
 * scale-following arpeggiator and eight relative-interval key modes. M2 keeps the two
 * properties that made it a layer rather than a fourth voice: it has its OWN step pattern,
 * independent of whether channel 1 fires on that step, and its pitch is built from
 * channel 1's current note, so a transposition moves both together and they cannot clash.
 *
 * The generator is M3 work, because a generator with no way to set its density or root is
 * not a feature, it is a constant.
 */
static void seq_triggerSecond(uint8_t step, uint8_t idx) {
  // Released unconditionally, on every step, whether or not this step fires. Two reasons:
  // it gives the stab a real gate instead of letting one ring into the next, and it keeps
  // this channel's held stack at depth 1, which is the same requirement the bass channel
  // has for a different and more dramatic reason (see the seqAcidHeld note).
  if (seqSecondHeldOn) { eng_noteOff(Ch::Second, seqSecondHeld); seqSecondHeldOn = false; }

  if (!seq.secondOn) { return; }
  if (step >= SEQ_NUM_STEPS) { return; }
  if (!(SEQ_CH2_MASK & ((uint16_t)1 << step))) { return; }

  // Which of the four voicings this step gets, counted from the step number rather than
  // from a running counter, so the layer stays in phase with the pattern instead of
  // drifting against it if a step is ever skipped.
  uint8_t v = 0;
  for (uint8_t k = 0; k < step; k++) { if (SEQ_CH2_MASK & ((uint16_t)1 << k)) v++; }
  v = (uint8_t)(v & 3);

  int16_t n = (int16_t)idx + SEQ_CH2_INTERVAL[v];
  if (n < 0)   { n = 0; }
  if (n > 127) { n = 127; }

  uint8_t midi = seq_noteToMidi((uint8_t)n);

  // No slide on this channel. V5 had none here either, and adding one would be inventing a
  // feature rather than porting one.
  eng_setParam(Ch::Second, CC_303_PORTAMENTO, 0);
  eng_noteOn(Ch::Second, midi, ENG_VEL_PLAIN, false);

  seqSecondHeld   = midi;
  seqSecondHeldOn = true;
}

// =====================================================================
// STEP EFFECTS -- M2.5
//
// A macro that is not defined inside #if is not a compile error. It is a 0, and this file
// would then ship with the selftest harness silently absent and every run of it reporting
// "fx 0 hits" as though that meant something. Named out loud instead of trusted; config.h is
// included at the top of this file and this line is what keeps that true rather than
// accidental.
#ifndef SEQ_FX_SELFTEST
#error "SEQ_FX_SELFTEST not visible: config.h must be included before the M2.5 code"
#endif
//
// V5's eight, split into two kinds that need two different mechanisms:
//
//   - WHAT effects (OctUp, the four chord steps) change the pitch and can be resolved
//     entirely before the note-on is posted. They are a lookup and an addition.
//   - WHEN effects (Retrig, Stutter) fire extra hits INSIDE a step, so they cannot be
//     turned into a note-on at step time and need a schedule the clock polls.
//
// V5 implemented both as one bare uint8_t in a Step struct with an if-chain for the pitch
// and, for the timing, edge detection inside a 256 Hz loop. The edge detection is the part
// that had to be rebuilt rather than copied; the reasoning is on seq_subPoll().
// =====================================================================

/* V5's arpeggio table, verbatim: Acid_Drip_Drum_Acid_Drift_V5.ino:780-785.
 *
 * NOTE ON A POINTER IN THE DESIGN DOC: ESP32S3_FUSION_IMPLEMENTATION.md:361 says the
 * interval table is at "主 sketch:2176-2210". It is not. That range is
 * ch2NearestScaleSemi() and ch2KbIntervalSemis() -- channel 2's scale-following key-mode
 * logic, a different feature that also deals in semitone intervals. The real table is at
 * :780-785, and the per-effect comments at :2930-2939 agree with it exactly, which is two
 * independent sources rather than one.
 *
 * Indexed [effect - SEQ_FX_MAJSTEP][step % 4]: root, third, fifth, octave. Four is the
 * octave on purpose -- it is the same as the root, so a four-step chord walk ends where it
 * started an octave up and the pattern's identity survives a full bar.
 */
static const int8_t SEQ_CHORD_INTERVAL[4][4] = {
  { 0,  4,  7, 12 },   // SEQ_FX_MAJSTEP  -- major
  { 0,  3,  7, 12 },   // SEQ_FX_MINSTEP  -- minor
  { 0,  4,  7, 11 },   // SEQ_FX_DOM7STEP -- dominant 7th
  { 0,  3,  6,  9 }    // SEQ_FX_DIMSTEP  -- diminished
};

/* Sub-step schedules, in 64ths of a step. V5's offsets, unchanged.
 *
 * V5 detects these by polling a fraction -- `micron = elapsed * 64 / interval` clamped to
 * 0..63, firing when it crosses 32 (Retrig) or 21 and 42 (Stutter), V5 :6229-6249. The
 * fractions are reproduced here as absolute offsets from the step's own nominal time.
 */
static const uint8_t SEQ_SUB_COUNT[8] = {
  0, 0, 1, 2, 0, 0, 0, 0
};
static const uint8_t SEQ_SUB_OFF[8][2] = {
  {  0,  0 },   // None
  {  0,  0 },   // OctUp
  { 32,  0 },   // Retrig -- 32/64 = exactly half a step
  { 21, 42 },   // Stutter -- 21/64 = 32.8%, 42/64 = 65.6%. NOT 1/3 and 2/3: 21.33 and 42.67
                // are not integers and V5 picked the integers. Reproducing the rounding is
                // the point -- "about a third" is what it sounds like, and a cleaner third
                // would be a different effect.
  {  0,  0 },   // MajStep
  {  0,  0 },   // MinStep
  {  0,  0 },   // Dom7Step
  {  0,  0 }    // DimStep
};

/* Whether a sub-hit inherits the step's accent. V5 :6239 against :6246:
 *
 *     Retrig:   triggerNote(rni, seq.steps[seq.cur].accent, false)
 *     Stutter:  triggerNote(rni, false,                            false)
 *
 * The asymmetry is deliberate in V5 and is reproduced rather than tidied. An accent is a
 * filter-envelope hit (CC 75 plus velocity, per seq_triggerNote), so an accented stutter
 * sweeps the filter three times inside one step, which reads as a machine-gun sweep rather
 * than as a stutter. Retrig doubles a note that was already accented and stays musical,
 * because two accent hits 62 ms apart still sound like one phrase.
 */
static const bool SEQ_SUB_KEEP_ACCENT[8] = {
  false, false, true, false, false, false, false, false
};

/* Resolve a step's effect into the V5 pattern index to actually sound.
 *
 * V5's whole pitch-side effect handling, in order (V5 :2924-2948):
 *
 *     baseNote = constrain(scaleNote(cur) + trans, 0, 59);
 *     if      (effect == 1) baseNote = constrain(baseNote + 12, 0, 59);   // Oct Up
 *     ni = baseNote;
 *     if      (effect == 4) ni = baseNote + arpeggio[0][cur % 4];
 *     else if (effect == 5) ni = baseNote + arpeggio[1][cur % 4];
 *     else if (effect == 6) ni = baseNote + arpeggio[2][cur % 4];
 *     else if (effect == 7) ni = baseNote + arpeggio[3][cur % 4];
 *     else seq.arpPos = 0;
 *
 * Oct Up adjusts baseNote and the chord branch then READS baseNote, so as written they
 * compose. They never do in V5 -- one effect byte per step -- so the distinction is
 * unobservable in shipped content either way. Written V5's way so that a future combined
 * effect does not need this function re-read.
 *
 * ON V5'S 0..59 CLAMP, which is deliberately NOT ported: it is a bound on noteFreq[60], not
 * a musical decision. This port's index runs 0..255 and the transposition is applied later,
 * by seq_noteToMidi(). Clamping here would clip pitches that are valid MIDI, and would clamp
 * BEFORE transposition, which V5 did not do either.
 *
 * Whether dropping it is observable is measured rather than assumed: the eight presets span
 * indices 24..39, the largest offset any effect adds is +12, so the worst case is 51 --
 * inside V5's 59. V5's clamp never binds on any shipped pattern, ported or walked. The one
 * clamp left is seq_noteToMidi()'s, on the final MIDI note, which is the one that is actually
 * about MIDI.
 *
 * The clamp to 255 below is only the return type's width. That is a type guard, not a pitch
 * decision, and every real index is far below it.
 */
static uint8_t seq_resolvePitch(uint8_t idx, uint8_t fx, uint8_t step) {
  if (fx >= SEQ_NUM_FX) { fx = SEQ_FX_NONE; }

  int16_t n = (int16_t)idx;
  if (fx == SEQ_FX_OCTUP) { n += 12; }

  if (fx >= SEQ_FX_MAJSTEP && fx <= SEQ_FX_DIMSTEP) {
    // `step & 3` rather than `step % 4`: step is a uint8_t that is never negative, and a
    // modulo on the promoted signed type would cost a real division for the same answer.
    n += (int16_t)SEQ_CHORD_INTERVAL[fx - SEQ_FX_MAJSTEP][step & 3];
  }

  if (n < 0)   { n = 0; }
  if (n > 255) { n = 255; }
  return (uint8_t)n;
}

/* Nominal time of a sub-hit, as an offset from the start of the step that owes it.
 *
 * 64ths, not a per-effect table of microsecond values, because the step length is not known
 * until the tempo is and V5's offsets are defined as fractions. At 120 BPM a 16th is 125000
 * us, so 32/64 is 62500 us exactly and 21/64 is 41015 us.
 *
 * Overflow: seq_setTempo()'s floor of 20 BPM puts the interval at 60000000/20/4 = 750000 us,
 * and the largest offset is 42, so the product tops out at 31,500,000 -- inside uint32 with
 * two orders of magnitude to spare. No 64-bit arithmetic needed, and none used: this file's
 * own periodic report had a 16.16 overflow in it once already (see seq_report()).
 *
 * Rounding: integer division truncates, so a hit is scheduled at most 1 us EARLY and never
 * late. V5's crossing test, elapsed*64/interval >= k, is quantised to whole microseconds the
 * same way, so this matches V5's own granularity rather than departing from it.
 */
static uint32_t seq_subOffsetUs(uint8_t off) {
  return (uint32_t)((uint32_t)seq.interval * (uint32_t)off / 64UL);
}

static uint32_t seq_subDueUs(uint8_t off) {
  return seq.subStepUs + seq_subOffsetUs(off);
}

/* Arm, or cancel, the sub-step schedule for the step firing now.
 *
 * Called unconditionally once per step, so a step with no timed effect also drops whatever
 * the previous step left pending. The alternative is a schedule that outlives its step, and
 * the symptom of that is a note from the previous bar landing in the middle of this one.
 */
static void seq_subArm(const SeqStep *s) {
  // Counted rather than asserted. The largest offset is 42/64, so a pending hit is always due
  // before the next boundary unless loop() was blocked for a whole interval -- which is a
  // claim about loop(), and this is a measurement of it.
  if (seq.subIdx < seq.subCount) {
    seq.subDropped += (uint32_t)(seq.subCount - seq.subIdx);
  }

  seq.subCount = 0;
  seq.subIdx   = 0;
  seq.subFx    = SEQ_FX_NONE;

  if (!s->active || s->effect >= SEQ_NUM_FX) { return; }

  uint8_t n = SEQ_SUB_COUNT[s->effect];
  if (n == 0) { return; }

  seq.subCount  = n;
  seq.subIdx    = 0;
  seq.subFx     = s->effect;
  // Copies, so that a preset load or a pad gesture landing between the step and its sub-hits
  // cannot redirect a pending hit at a note the pattern no longer holds.
  seq.subNote   = s->note;
  seq.subStep   = seq.cur;
  seq.subAccent = s->accent;
  seq.subStepUs = seq.lastUs;   // nominal time of THIS step, already advanced past by seq_poll()
}

/* Fire one scheduled sub-hit. */
static void seq_subFire() {
  // Lateness against this hit's own nominal time, taken BEFORE the post below, so the number
  // measures the clock rather than the queue write that follows it.
  int32_t late = (int32_t)(micros() - seq_subDueUs(SEQ_SUB_OFF[seq.subFx][seq.subIdx]));
  if (late > 0) {
    uint32_t mag = (uint32_t)late;
    if (mag > seq.subMaxErrUs) { seq.subMaxErrUs = mag; }
  }

  // THE DRAIN -- the part V5 does not have and this port must, and the whole reason a
  // sub-step effect needs a note-off at all.
  //
  // V5 has no gate. triggerNote() sets gVolSub, sets the filter envelope and zeroes the
  // oscillator phase; the note then keeps sounding until the next trigger rewrites it. There
  // is no note-off anywhere in the path, which is why V5 has no held-note stack, which is why
  // V5 has no `slide = (mvaStack.n > 1)`, and therefore why a V5 retrigger needs nothing
  // special to retrigger.
  //
  // synthvoice.ino decides legato from stack depth (synthvoice.ino:225), so a sub-hit that
  // posted a bare note-on would take the depth to 2 and the SUB-HIT ITSELF would become
  // legato: no envelope retrigger, and CC 65 left armed for whatever came next. Both are the
  // degradation recorded in HANDOFF 6.4.2, arrived at from the opposite direction. Draining
  // first is what makes this a retrigger rather than a second voice.
  seqAcidReleaseAll();

  uint8_t pitch = seq_resolvePitch(seq.subNote, seq.subFx, seq.subStep);
  bool    acc   = SEQ_SUB_KEEP_ACCENT[seq.subFx] ? seq.subAccent : false;
  seq_triggerNote(pitch, acc, false);   // glide = false for both effects, as in V5

  seq.subHits++;
  seq.subIdx++;
}

/* Fire every sub-hit that is due. Called on every pass of seq_poll().
 *
 * THIS IS A REBUILD, NOT A COPY, and the reason is worth writing down, because V5's version
 * works and looks like it ports.
 *
 * V5 polls a sub-step position from updateControl() and fires on an EDGE crossing:
 *
 *     if (micron >= 21 && lastMicron < 21) triggerNote(...);
 *
 * That is fine at 256 Hz and it has two properties this port will not accept:
 *
 *   1. It can MISS a hit. If the loop is late enough to jump from micron 15 to 40 in one
 *      pass, the crossing of 21 is never observed, the `lastMicron < 21` half of the test is
 *      false at both samples, and the stutter silently becomes a plain note. A missed
 *      threshold is a different failure from a late hit, and a much harder one to notice --
 *      there is nothing in the log to say a hit went missing.
 *
 *   2. Its timing error is the loop period, not the schedule. At 256 Hz that is 3.9 ms of
 *      quantisation on a hit meant to land 41 ms into a 125 ms step, and every hit in a
 *      stutter quantised independently, so the pair wobbles against each other.
 *
 * Scheduling against the step's own nominal time has neither problem: a hit that comes due
 * while loop() was blocked fires late but still fires, and it inherits the main clock's
 * no-accumulation property because each offset is computed from subStepUs rather than added
 * to the previous hit. The trade is the one below -- a late poll can deliver two hits in one
 * pass -- which is handled by draining between them, and is a far lesser failure than dropping
 * one silently.
 */
static void seq_subPoll(uint32_t us) {
  // `while`, not `if`: being more than one sub-step late makes two hits due at once, and
  // firing both is closer to the truth than firing one and skipping the other. Bounded by
  // subCount, which is at most 2.
  while (seq.subIdx < seq.subCount &&
         (int32_t)(us - seq_subDueUs(SEQ_SUB_OFF[seq.subFx][seq.subIdx])) >= 0) {
    seq_subFire();
  }
}

/* The M2.5 resolution table, printed once at seq_start().
 *
 * Eight effects that no shipped preset uses, described only in source comments, is a feature
 * nobody can check. This puts what each one resolves to -- on the device, at the interval
 * actually running -- into the serial log, which is the instrument this project has verified
 * everything else with since M0.
 *
 * The chord steps print as a phase table rather than one line each, because at any single
 * phase they are nearly indistinguishable: MajStep and Dom7Step are both +4 at cur%4==1, and
 * MinStep and DimStep are both +3 there too. The table diverges as the step number moves, and
 * that divergence is the entire audible difference between the four.
 */
static void seq_fxDump() {
  const uint8_t REF = 24;   // V5's C2, and the register every preset is written in

  DEBF("[M2.5] step effects, %u BPM, interval %lu us, reference index %u:\r\n",
       (unsigned)seq.tempo, (unsigned long)seq.interval, (unsigned)REF);

  for (uint8_t k = 0; k < 4; k++) {
    uint8_t fx = (uint8_t)(SEQ_FX_MAJSTEP + k);
    DEBF("[M2.5]   %u %-8s cur%%4 0..3: ", (unsigned)fx, seq_fxName(fx));
    for (uint8_t p = 0; p < 4; p++) {
      DEBF("%+3d ", (int)SEQ_CHORD_INTERVAL[k][p]);
    }
    DEBF("\r\n");
  }

  for (uint8_t fx = 0; fx < SEQ_NUM_FX; fx++) {
    uint8_t idx = seq_resolvePitch(REF, fx, 0);
    DEBF("[M2.5]   %u %-8s idx %u -> midi %u, sub ", (unsigned)fx, seq_fxName(fx),
         (unsigned)idx, (unsigned)seq_noteToMidi(idx));
    if (SEQ_SUB_COUNT[fx] == 0) {
      DEBF("-\r\n");
      continue;
    }
    for (uint8_t k = 0; k < SEQ_SUB_COUNT[fx]; k++) {
      DEBF("%s%u/64 = %lu us", (k ? ", " : ""),
           (unsigned)SEQ_SUB_OFF[fx][k], (unsigned long)seq_subOffsetUs(SEQ_SUB_OFF[fx][k]));
    }
    DEBF("%s\r\n", SEQ_SUB_KEEP_ACCENT[fx] ? ", keeps accent" : ", accent forced off");
  }
}

// =====================================================================
// STEP FIRING
// =====================================================================
static void seq_advanceStep() {
  seq.cur = seq_nextPatStep();
  if (seq.cur >= SEQ_NUM_STEPS) { seq.cur = 0; }   // belt and braces; nextPatStep is bounded

  // A pointer, and that is a house-style choice with NO correctness claim attached. I first
  // wrote a comment here saying references were unproven in this toolchain, and that was
  // wrong: adsr.h, engine_iface.h and sampler.h all declare reference parameters and
  // adsr.ino / engine_iface.ino / sampler.ino all define them, so references are thoroughly
  // proven here and a later reader must not go looking for a constraint that does not exist.
  // The pointer is kept because it matches the surrounding code, which is the only real
  // reason. `const SeqStep &s = ...` would compile and behave identically.
  // It was this. The first commit of M2 failed to compile here with six errors, all of the
  // form "request for member 'note' in 's', which is of pointer type" -- because switching
  // & to * without switching the member accesses from s.x to s->x is only a two-character
  // edit that does not survive contact with a compiler. What made it ship is that every
  // offline check I ran in this file was checking ALGORITHM, not spelling: the cursor
  // simulation, the gate-depth model and the clock model all reimplemented the logic
  // rather than compiling it, so all three passed on a file that would not build. A test
  // that cannot fail on the bug it is written next to is not a test of that bug.
  const SeqStep *s = &seq.steps[seq.cur];

  // Channel 2 first, then drums, then the bass. That order is V5's -- advanceStep() called
  // bmTriggerStep() before firing channel 1 -- so the bass lands on top of the kit rather
  // than under it, and the mix reads the way it did.
  seq_triggerSecond(seq.cur, s->note);

  if (s->active) {
    // A non-slide step drains first, and that is load-bearing rather than tidy: see the
    // seqAcidHeld note. Skipping the drain leaves the stack deeper than 1 and every later
    // note-on becomes legato with no retrigger, which sounds like a tuning fault.
    if (!s->glide || seqAcidHeldN == 0) { seqAcidReleaseAll(); }
    seq_triggerDrums(seq.cur);

    // M2.5: the effect resolves the pitch BEFORE the note-on, so OctUp and the chord steps
    // cost nothing at trigger time -- they are a lookup and an addition on the way past. Only
    // Retrig and Stutter need anything more, and that is what seq_subArm() below is for.
    uint8_t pitch = seq_resolvePitch(s->note, s->effect, seq.cur);
    seq_triggerNote(pitch, s->accent, s->glide);
  } else {
    // A rest still has to release. Skipping this holds the previous note for the length of
    // the whole loop, which on a 16-step pattern is two seconds of one note sounding
    // through four rests.
    seqAcidReleaseAll();
    eng_setParam(Ch::Acid, CC_303_PORTAMENTO, 0);
    seq_triggerDrums(seq.cur);   // drums still play on a rest; that is what a rest means
  }

  // Arm, or cancel, this step's sub-step schedule. Unconditional and last, so that a rest and
  // a step with no timed effect both drop whatever the previous step left pending. A pending
  // hit that survives its step would be a note from the previous bar arriving mid-bar.
  //
  // It is last rather than first so that seq.lastUs -- the step's nominal time, which
  // seq_subArm copies -- is read after everything above has had its say and before anything
  // below can change it.
  seq_subArm(s);
}

// =====================================================================
// PUBLIC API
// =====================================================================

uint8_t seq_noteToMidi(uint8_t idx) {
  // 12, not a guess: see the long note in sequencer.h. V5 labels index 24 as C2, and MIDI
  // C2 is 36.
  int16_t n = (int16_t)idx + 12 + (int16_t)seq.key + (int16_t)seq.octave * 12;
  if (n < 0)   { n = 0; }
  if (n > 127) { n = 127; }
  return (uint8_t)n;
}

void seq_loadPreset(uint8_t preset) {
  if (preset >= SEQ_NUM_PRESETS) { preset = 0; }

  for (uint8_t i = 0; i < SEQ_NUM_STEPS; i++) {
    seq.steps[i].note   = SEQ_PRESETS[preset].note[i];
    seq.steps[i].active = (SEQ_PRESETS[preset].flags[i] & 1) != 0;
    seq.steps[i].accent = (SEQ_PRESETS[preset].flags[i] & 2) != 0;
    seq.steps[i].glide  = (SEQ_PRESETS[preset].glide[i]) != 0;
    seq.steps[i].effect = SEQ_PRESETS[preset].effect[i];
  }
  // Full length, forward, C, no transposition. A preset is a pattern and nothing else;
  // carrying the previous preset's length or order into it is what makes a preset sound
  // wrong on a machine that touched a control in between.
  seq.len       = SEQ_NUM_STEPS;
  seq.key       = 0;
  seq.octave    = 0;
  seq.rrMode    = SEQ_ORDER_FORWARD;
  seq.rrPingFwd = true;
  if (seq.cur >= seq.len) { seq.cur = 0; }
}

uint8_t seq_presetCount() { return SEQ_NUM_PRESETS; }

void seq_setTempo(uint16_t bpm) {
  if (bpm < 20)  { bpm = 20; }
  if (bpm > 300) { bpm = 300; }   // V5's own clamp, from its SYNC IN tempo tracker
  seq.tempo    = bpm;
  seq.interval = 60000000UL / (uint32_t)bpm / 4UL;   // per 16th; V5's bpm2us()
}

void seq_setLen(uint8_t steps) {
  if (steps == 0 || steps > SEQ_NUM_STEPS) { steps = SEQ_NUM_STEPS; }
  seq.len = steps;
  if (seq.cur >= seq.len) { seq.cur = 0; }
}

void seq_setOrder(uint8_t mode) {
  if (mode > SEQ_ORDER_RANDOM) { mode = SEQ_ORDER_FORWARD; }
  seq.rrMode = mode;
  // Forward is the default direction for a bounce, so starting mid-pattern in reverse reads
  // as a bug rather than as a direction.
  seq.rrPingFwd = true;
}

void seq_setDrums(bool on)  { seq.drumsOn  = on; }
void seq_setSecond(bool on) { seq.secondOn = on; }

void seq_setPortaSpeed(uint8_t speed) {
  if (speed < 1) { speed = 1; }
  if (speed > 8) { speed = 8; }
  seq.portaSpeed = speed;
  eng_setParam(Ch::Acid, CC_303_PORTATIME, SEQ_PORTA_CC[speed - 1]);
}

// uint8_t here and in sequencer.h. An intermediate commit of this milestone widened the
// definition to uint32_t while the declaration stayed uint8_t, and CI 36974920807 rejected it:
//
//     sequencer.ino:895:10: error: ambiguating new declaration of 'uint32_t seq_portaSpeed()'
//
// A declaration and a definition of the same function with different return types are two
// overloads of a zero-argument function, and there is nothing to overload between -- hence
// "ambiguating new declaration" rather than the more obvious "conflicting return type".
//
// Worth noting how it got in: the two were identical at 60e2f7f, so nothing could complain
// before the edit, and the edit was made on the line above the drift accessors under the belief
// that it would read as tidying. It is the same failure mode as f464c01 -- a two-character edit
// to a signature -- and the only reason it cost a CI run rather than a debugging session is
// that CI ran.
//
// On "the offline suite could not have caught this, because it reads text": that was written
// as an excuse and it was wrong. A text-level comparison of declared against defined return
// type catches this bug exactly right, needs no compiler, and now does -- tools/test-sequencer.py
// has a signature-agreement section that walks all 25 seq_* pairs and fails on this one.
// Recording a limitation because it is convenient is the same error as recording a capability
// that was never exercised; the honest version of both is to go and find out.
uint8_t seq_portaSpeed() { return seq.portaSpeed; }

uint32_t seq_driftMaxUs()  { return seq.driftMaxUs; }
uint32_t seq_stepsPlayed() { return seq.stepsPlayed; }
void     seq_driftReset()  { seq.driftMaxUs = 0; seq.driftLastUs = 0; seq.catchups = 0; }

uint32_t seq_subHits()     { return seq.subHits; }
uint32_t seq_subMaxErrUs() { return seq.subMaxErrUs; }
uint32_t seq_subDropped()  { return seq.subDropped; }

const char *seq_fxName(uint8_t fx) {
  // V5's own eight strings, from the UI table at Acid_Drip_Drum_Acid_Drift_V5.ino:1128,
  // minus the spaces in V5's "Oct Up" / "Maj Step" -- a label that has to be compared against
  // a number in a log reads better without them, and there is nowhere here that needs a label
  // to line up with V5's on screen.
  static const char *const NAMES[SEQ_NUM_FX] = {
    "None", "OctUp", "Retrig", "Stutter", "MajStep", "MinStep", "Dom7Step", "DimStep"
  };
  return (fx < SEQ_NUM_FX) ? NAMES[fx] : "?";
}

void seq_setStepEffect(uint8_t step, uint8_t fx) {
  if (step >= SEQ_NUM_STEPS) { step = 0; }
  if (fx >= SEQ_NUM_FX)      { fx = SEQ_FX_NONE; }
  if (seq.steps[step].effect == fx) { return; }
  seq.steps[step].effect = fx;

  // If the change landed mid-step, the pending schedule belongs to the effect that was just
  // replaced, and leaving it would fire hits the new effect never asked for -- including, for
  // a Retrig turned into a None, a retrigger of a note that should now be plain. Re-armed
  // rather than merely cancelled, so that setting an effect is audible on the step you set it
  // on instead of one step later.
  if (step == seq.cur) { seq_subArm(&seq.steps[step]); }
}

uint8_t seq_stepEffect(uint8_t step) {
  if (step >= SEQ_NUM_STEPS) { return SEQ_FX_NONE; }
  return seq.steps[step].effect;
}

void seq_init() {
  // Seeded from micros() rather than from a fixed constant, because SEQ_ORDER_RANDOM with a
  // fixed seed replays the same "random" pattern on every boot, and a pattern that repeats
  // identically every power cycle stops sounding random within about two listens.
  seqRngState = (uint32_t)micros() ^ 0x9E3779B9u;
  if (seqRngState == 0) { seqRngState = 0x1D872B41u; }

  seq.cur       = 0;
  seq.tempo     = 120;
  seq.interval  = 60000000UL / 120UL / 4UL;   // 125000 us
  seq.lastUs    = 0;
  seq.running   = false;
  seq.drumsOn   = true;
  seq.secondOn  = true;
  seq.portaSpeed = 4;

  seq.stepsPlayed = 0;
  seq.catchups    = 0;
  seq.driftMaxUs  = 0;
  seq.driftLastUs = 0;

  // M2.5. Zeroed here rather than left to a global initialiser, so that a warm restart --
  // setup() called twice, which seq_init()'s own contract permits -- does not inherit a
  // schedule armed by the previous run and fire it into the middle of the first bar.
  seq.subCount    = 0;
  seq.subIdx      = 0;
  seq.subFx       = SEQ_FX_NONE;
  seq.subNote     = 0;
  seq.subStep     = 0;
  seq.subAccent   = false;
  seq.subStepUs   = 0;
  seq.subHits     = 0;
  seq.subMaxErrUs = 0;
  seq.subDropped  = 0;

  seqAcidHeldN    = 0;
  seqSecondHeldOn = false;

  seq_loadPreset(0);

#if SEQ_FX_SELFTEST
  // M2.5 acceptance harness -- see the long note in config.h. One of each effect on the first
  // eight steps of preset 0, and steps 8-15 left exactly as the preset has them.
  //
  // Eight is not arbitrary. Step 15 staying a plain active step is what keeps the report's
  // "held a" reading at 1, so the existing M2 acceptance number stays comparable between a
  // selftest build and a shipping build. And steps 0 and 1 carrying Retrig and Stutter make
  // the sub-hit COUNT per bar exactly 3, so a 30 s window at 120 BPM -- 240 steps, 15 bars --
  // has to read "fx 45 hits". 30 would mean the retrigger never fired; 60 would mean a
  // stutter hit fired twice. The number is checkable from the log with nobody listening, which
  // is the point: what is being verified is that the code runs at all, and code that runs
  // says so in a number rather than in an opinion about how it sounded.
  static const uint8_t FX_SELFTEST[8] = {
    SEQ_FX_RETRIG, SEQ_FX_STUTTER, SEQ_FX_OCTUP,    SEQ_FX_MAJSTEP,
    SEQ_FX_MINSTEP, SEQ_FX_DOM7STEP, SEQ_FX_DIMSTEP, SEQ_FX_NONE
  };
  for (uint8_t i = 0; i < 8; i++) { seq.steps[i].effect = FX_SELFTEST[i]; }
  DEBF("[M2.5] SELFTEST: steps 0-7 carry fx 2,3,1,4,5,6,7,0 on preset 0\r\n");
#endif

  // The accent depth is set ONCE, here, rather than per note. CC 76 is a patch parameter and
  // the sequencer has no business changing it every step -- unlike CC 75, which the accent
  // semantics genuinely require to move per step and which is therefore posted from
  // seq_triggerNote(). The asymmetry is the point: what changes per note is posted per
  // note, and what does not is set once.
  eng_setParam(Ch::Acid, CC_303_ACCENT_LVL, 40);

  // Portamento disarmed on both channels, and the slide time set. Arming it at boot would
  // make the very first note a slide from silence.
  eng_setParam(Ch::Acid,   CC_303_PORTAMENTO, 0);
  eng_setParam(Ch::Second, CC_303_PORTAMENTO, 0);
  seq_setPortaSpeed(seq.portaSpeed);
}

void seq_start() {
  // Start on the step AFTER the current one, not on step 0. nextPatStep() is what actually
  // moves the cursor, so setting cur = len-1 makes the first advance land on 0 -- which is
  // what you want, because "press play" should sound step 1 straight away rather than
  // sitting out a whole step of silence first. This is V5's convention and it is the right
  // one.
  seq.cur      = (seq.len > 0) ? (uint8_t)(seq.len - 1) : 0;
  seq.running  = true;
  seq.lastUs   = micros();
  seq.rrPingFwd = true;
  seq_driftReset();

  // Any schedule the stopped sequencer was holding is dropped, and NOT counted as dropped:
  // it was cancelled by a stop, which is a decision, not a miss. Counting it would put a
  // non-zero in a counter whose whole meaning is "a hit the clock lost".
  seq.subCount = 0;
  seq.subIdx   = 0;

  // The report window restarts here. Without this, stopping and restarting inside one 30 s
  // window would leave the first report counting steps from BOTH runs divided by an elapsed
  // time that covers only part of them -- which yields a plausible-looking mean interval
  // that is simply wrong, and wrong in the direction of looking like drift.
  seqRepLastMs    = millis();
  seqRepLastSteps = seq.stepsPlayed;
  seqRepLastCatch = seq.catchups;
  seqRepPrintUs   = 0;

  DEBF("[M2] seq start: %u steps, %u BPM (%lu us/step), slide %u -> %u ms\r\n",
       (unsigned)seq.len, (unsigned)seq.tempo, (unsigned long)seq.interval,
       (unsigned)seq.portaSpeed, (unsigned)SEQ_PORTA_CC[seq.portaSpeed - 1]);

  // The effect resolution table, once per start. It is 13 lines and it is the only place in
  // the whole firmware where the eight effects' actual behaviour is written down as data
  // rather than as source, which is the difference between a feature that can be checked and
  // a feature that has to be believed.
  seq_fxDump();
}

void seq_stop() {
  if (!seq.running) { return; }
  seq.running = false;

  // Cancelled, not counted as dropped -- see the note in seq_start(). A stop is a decision.
  seq.subCount = 0;
  seq.subIdx   = 0;

  // Silence through the event layer, like everything else, so the engine still has exactly
  // one writer. Going around it to the synths directly would reintroduce the very coupling
  // M2 exists to remove, in the one function whose entire job is stopping.
  seqAcidReleaseAll();
  if (seqSecondHeldOn) { eng_noteOff(Ch::Second, seqSecondHeld); seqSecondHeldOn = false; }
  eng_setParam(Ch::Acid, CC_303_PORTAMENTO, 0);
  eng_allNotesOff();

  DEBF("[M2] seq stop after %lu steps, max drift %lu us, %lu catch-ups\r\n",
       (unsigned long)seq.stepsPlayed, (unsigned long)seq.driftMaxUs,
       (unsigned long)seq.catchups);
}

void seq_toggle() {
  if (seq.running) { seq_stop(); } else { seq_start(); }
}

// =====================================================================
// THE CLOCK
// =====================================================================

// Defined below, called from the end of seq_poll(). The declaration is here rather than
// the definition being moved up because the report's own commentary belongs next to the
// clock it measures, and forward-declaring one static function is cheaper than reordering
// two of them.
static void seq_report();

void seq_poll() {
  if (!seq.running) { return; }

  uint32_t us = micros();

  // M2.5: sub-step hits FIRST, before the step check below. Not an ordering preference --
  // it is what keeps a hit attached to the step that owes it.
  //
  // A sub-hit at 21/64 or 42/64 is always due before the next step boundary, so in the normal
  // case the order makes no difference. It matters when loop() was late enough that a sub-hit
  // and the next boundary are both due in one pass: polling first keeps the sub-hit with its
  // own step, whereas advancing first would re-arm the schedule from the new step and drop it
  // into seq_subDropped. A late hit is recoverable; a silent one is not.
  seq_subPoll(us);

  if ((uint32_t)(us - seq.lastUs) < seq.interval) { return; }

  // Nominal time of the step that is firing NOW, as opposed to the moment we noticed it.
  // Advancing lastUs by a whole interval rather than to `us` is the entire no-drift
  // mechanism: the clock's error is the error in its DETECTION, and that error is not
  // carried into the next step.
  //
  // (An earlier version of this comment said loop()'s 1 ms period bounded the detection
  // error at about a millisecond. That was wrong, and worth correcting rather than leaving:
  // loop() runs at priority 1 with a taskYIELD() between passes and NOTHING in
  // regular_checks() blocks -- no vTaskDelay, no delay, no ulTaskNotifyTake. So the
  // resolution here is sub-millisecond, not 1 ms. The no-drift property does not depend on
  // that number either way, which is why the correction changes no behaviour.)
  //
  // Written the other way -- lastUs = us -- the same code passes a ten-minute listen and is
  // a tenth of a beat out by the end of it. That is precisely the failure the M2 acceptance
  // criterion is looking for, which is why the criterion is written about jitter rather
  // than about "does it keep time".
  uint32_t nominal = seq.lastUs + seq.interval;
  bool behind = ((uint32_t)(us - nominal) >= seq.interval);

  if (!behind) {
    int32_t err = (int32_t)(us - nominal);
    seq.driftLastUs = err;
    uint32_t mag = (uint32_t)((err < 0) ? -err : err);
    if (mag > seq.driftMaxUs) { seq.driftMaxUs = mag; }
  }

  seq.lastUs = nominal;

  if (behind) {
    // More than a whole interval late, so something blocked loop() for a while. Snap forward
    // instead of firing a burst of catch-up steps, which would sound like a glitch and would
    // leave the cursor somewhere the pattern never intended. Counted, and EXCLUDED from
    // driftMaxUs: this deliberately throws away up to half an interval of accumulated phase,
    // so folding it into the maximum would report a worst case that the clock chose on
    // purpose. Counting it separately is what keeps "no drift" and "no drift because it kept
    // snapping" from reading as the same claim.
    seq.catchups++;
    seq.lastUs = us - (seq.interval >> 1);
  }

  seq.stepsPlayed++;
  seq_advanceStep();

  // The report goes LAST, after the step that was due has been posted. Putting it first
  // would make the print itself land in the middle of the step that was about to fire,
  // which is the one arrangement guaranteed to inflate the very number being reported.
  seq_report();
}

// =====================================================================
// PERIODIC REPORT
//
// The M2 acceptance criterion is "ten minutes, no pitch drift, no accumulating jitter".
// Without this, a device left running for ten minutes prints one line at seq_start() and
// then nothing, so the criterion can only be judged by ear -- and by ear it is impossible
// to distinguish 0.1% timing error from 0.5% at 120 BPM, let alone to detect the specific
// failure this clock is built to avoid. The log is the instrument; this is it.
//
// FOUR NUMBERS, each answering a different question:
//
//   mean vs nominal   Real elapsed microseconds divided by steps actually fired, compared
//                     against the interval. THIS is the drift measurement, and it is the
//                     one that matters: it is a statement about real time, not about the
//                     clock's opinion of itself. A clock that wrote lastUs = us would still
//                     report a small detection error while this number walked steadily
//                     away from nominal for ten minutes.
//   max |err|         Worst detection error inside the window. This is the jitter figure.
//                     Bounded by how often loop() gets to run, so it should be small and
//                     should NOT grow from window to window.
//   catch             Steps that were more than a whole interval late and got snapped
//                     forward. Reported separately and excluded from max |err| -- see the
//                     snap branch in seq_poll() for why folding them in would flatter the
//                     number. "No drift because it kept snapping" has to be
//                     distinguishable from "no drift", and this is what distinguishes them.
//   held a/s          SynthVoice's note-stack depth on the acid and second channels. Drums
//                     are not in this field: see eng_heldDepth(). Depth above 1 on channel 1
//                     means the gate is NOT draining and every note-on is legato with no
//                     envelope retrigger -- which sounds like a tuning fault and not like a
//                     bug. This turns "does it sound right" into something checkable for
//                     the part of it that is arithmetic.
//
// A value is 1 or 2 on channel 1 and is CORRECT during a slide step, because a slide must
// keep the note it is sliding away from -- releasing it would put the amp envelope into
// release while the oscillator is still gliding. What matters is that depth returns to 1
// on the first non-slide step, and that it never pins at the MIDI_MVA_SZ cap of 8.
//
// PRINT COST IS REPORTED, because this print is not free and pretending otherwise would be
// the same kind of dishonesty as excluding its effect on the numbers. Over USB CDC a line
// this long costs milliseconds, which can and will show up as detection error on the
// following step. Rather than quietly subtracting it from the drift figures -- which would
// be me deciding the data is bad -- the duration of the previous print is printed alongside
// them, so a large max |err| can be attributed to the report that caused it instead of
// being mistaken for clock instability.
// =====================================================================
#define SEQ_REPORT_MS 30000UL

static void seq_report() {
  uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - seqRepLastMs) < SEQ_REPORT_MS) { return; }

  uint32_t elapsedMs    = (uint32_t)(nowMs - seqRepLastMs);
  uint32_t elapsedUs    = elapsedMs * 1000UL;
  uint32_t stepsInWin   = seq.stepsPlayed - seqRepLastSteps;
  uint32_t catchesInWin = seq.catchups    - seqRepLastCatch;
  uint32_t prevPrintUs  = seqRepPrintUs;

  // Advance the window first, so an early return below cannot re-report the same window.
  seqRepLastMs    = nowMs;
  seqRepLastSteps = seq.stepsPlayed;
  seqRepLastCatch = seq.catchups;
  seqRepPrintUs   = 0;

  if (stepsInWin == 0) {
    // Only reachable at a tempo slow enough that a whole report window holds less than one
    // step -- 30 s at under 2 BPM, which seq_setTempo()'s own 20 BPM floor already excludes.
    // Guarded anyway, because the failure mode of the alternative is a divide by zero in
    // the one function whose entire job is to be trusted about timing.
    DEBF("[M2] %lu s window: NO STEPS (interval %lu us -- tempo too slow to fill a window)\r\n",
         (unsigned long)(elapsedMs / 1000), (unsigned long)seq.interval);
    return;
  }

  // Mean realized step period. Integer division plus a separate remainder, NOT 16.16 fixed
  // point.
  //
  // The fixed point was here and it was wrong, and how it was wrong is worth writing down
  // because the shape of the bug looks correct. Scaling by 2^16 to keep two decimals needs
  // (realMeanUs << 16) to fit the 32 bits of a uint32_t. It does not, at any tempo this
  // report is used at. At 120 BPM the step is 125000 us, and 125000 << 16 = 8,192,000,000
  // against a ceiling of 4,294,967,295. The old code widened the numerator to 64 bits,
  // which fixed the multiply and left the quotient truncated on the way back down.
  //
  // The first report off the device read "mean 59464.00 us vs nominal 125000 us". That is
  // exactly the low 32 bits of 8,192,000,000 -- 3,897,032,704 -- shifted back down, so it
  // reproduced bit for bit rather than merely being near some wrong number. The real mean
  // was 125000, i.e. exact, and it was being reported as 48% of nominal. Truncation to a
  // power-of-two divisor produces a plausible-looking small integer, which is why nothing
  // about it read as broken at a glance.
  //
  // It breaks below about 228 BPM and seq_setTempo() accepts 20..300, so most of the range
  // printed a wrong mean. Note what did NOT break, in the same line: windowErrUs below is
  // computed independently in plain integer arithmetic and read +0 us on that very report.
  // Two drift numbers side by side, one right and one wrong, and only one of them is
  // trustworthy -- so the fix is the arithmetic, not the print format.
  //
  // Bounds, so neither half can overflow. elapsedUs is millis() * 1000 for a 30 s window:
  // fits uint32 to about 71 minutes, and SEQ_REPORT_MS is 30 s. meanInt <= elapsedUs.
  // meanFrac: the remainder is strictly below stepsInWin, so times 100 stays under
  // stepsInWin * 100 -- about 60,000 at the fastest tempo seq_setTempo() accepts, two
  // orders of magnitude inside uint32. No 64-bit needed anywhere here.
  uint32_t meanInt  = elapsedUs / stepsInWin;
  uint32_t meanFrac = ((elapsedUs % stepsInWin) * 100UL) / stepsInWin;

  // Signed error of the whole window: real time that elapsed minus the time the clock
  // claims it spent. Bounded arithmetic -- stepsInWin * seq.interval cannot overflow uint32
  // at any tempo seq_setTempo() accepts (worst case 600 steps x 50000 us = 30,000,000).
  int32_t windowErrUs = (int32_t)elapsedUs
                      - (int32_t)((uint32_t)stepsInWin * (uint32_t)seq.interval);

  uint32_t t0 = micros();
  // "held a/s/d": a and s are Synth1.mvaStack.n and Synth2.mvaStack.n, which are
  // measurements. The d slot prints a dash, deliberately, because eng_heldDepth(Ch::Drums)
  // is a hardcoded 0 -- Sampler::NoteOff() is empty and drum samples are one-shots, so a
  // held-note count for drums would be a concept that does not exist. It used to print that
  // 0 as if it were the third reading, next to two real numbers, which is exactly the shape
  // of thing a reader will quote as evidence. A slot that cannot be measured gets a dash.
  //
  // "fx N hits" is M2.5's presence proof, and it is here for the same reason: with the eight
  // shipped presets -- every step SEQ_FX_NONE -- this number is 0 forever, and a report line
  // that reads identically whether the sub-step scheduler runs or is not wired up at all is
  // not measuring the feature. Three readings would then look healthy while one thing was
  // missing. With SEQ_FX_SELFTEST it is 45 per 30 s window at 120 BPM and checkable from the
  // log; without it, 0 is the correct and expected answer, and knowing which of those two
  // builds produced a given log is a question the line itself now answers.
  DEBF("[M2] %lu s window: %lu steps, mean %lu.%02lu us vs nominal %lu us (%+ld us), "
       "max|err| %lu us, last %+ld us, catch %lu, total %lu, held a/s %u/%u (drums n/a), "
       "prev print %lu us, fx %lu hits (max late %lu us, dropped %lu)\r\n",
       (unsigned long)(elapsedMs / 1000), (unsigned long)stepsInWin,
       (unsigned long)meanInt, (unsigned long)meanFrac,
       (unsigned long)seq.interval, (long)windowErrUs,
       (unsigned long)seq.driftMaxUs, (long)seq.driftLastUs,
       (unsigned long)catchesInWin, (unsigned long)seq.stepsPlayed,
       (unsigned)eng_heldDepth(Ch::Acid),
       (unsigned)eng_heldDepth(Ch::Second),
       (unsigned long)prevPrintUs,
       (unsigned long)seq.subHits,
       (unsigned long)seq.subMaxErrUs,
       (unsigned long)seq.subDropped);
  seqRepPrintUs = micros() - t0;

  // Per-window, so "max |err|" in the next line covers the next window rather than
  // repeating the worst moment of the whole session. A worst-case-over-all-time figure is
  // useless for spotting a regression, because it can never improve and never distinguishes
  // "bad once at boot" from "bad continuously".
  seq.driftMaxUs = 0;

  // Same argument for the sub-step figure, and it matters MORE here, because a sub-step
  // lateness that only ever occurs at boot would otherwise sit in the number forever and
  // read as a permanent property of the scheduler. `fx` total and `dropped` are cumulative
  // and are NOT reset: they are totals, not worst cases, and a total that resets is a total
  // nobody can check against the step count beside it.
  seq.subMaxErrUs = 0;
}
