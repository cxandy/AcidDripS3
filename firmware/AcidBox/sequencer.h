/*
 * sequencer.h -- M2, the 16-step sequencer.
 *
 * This is V5's sequencer, ported. What "ported" means here is narrower than it sounds,
 * and the narrowness is the whole point of M2, so it is worth being precise about it.
 *
 * V5 ran its sequencer by writing a set of globals that updateControl() then pushed into
 * the synth 256 times a second: gFreq (the pitch), gGlideStep (how fast to walk it to
 * the new value), gEnvCutoff and gEnvRes (the accent's filter envelope), gVolSub. The
 * sequencer and the synth were the same code. You could not run the sequencer without
 * also running V5's control loop, its pad scanner, its pot handling and its UI, because
 * all of it was one function called from one place.
 *
 * Here the sequencer owns no engine state at all. It decides *what should happen* and
 * says so through engine_iface, and the engine layer applies it. That is the whole
 * decoupling: three trigger sites that used to reach into engine internals now post
 * semantic events, exactly like a MIDI cable does.
 *
 * What that buys, concretely, and it is not a style preference:
 *
 *   - Pitch, portamento and accent are now CCs with defined meanings rather than
 *     hand-rolled envelopes. A glide that used to step gFreqFP by a computed amount
 *     every 3.9 ms is one CC write.
 *
 *   - The engine has one writer (eng_apply), so a step cannot interleave halfway
 *     through a parameter change the way two cores writing the same globals can.
 *
 *   - The sequencer can be tested without the synth. That is what makes the drift
 *     measurement in seq_driftMaxUs() worth anything.
 *
 * NOT IN THIS MILESTONE, stated up front so it is not mistaken for an oversight:
 *
 *   - V5's BeatMachine2 (2,292 lines: swing, per-hit probability, Euclidean fills,
 *     six simultaneous drum voices, custom beat patterns, fill editing). M2's drums
 *     are a fixed 16-step pattern, one voice per step. The design doc's task 3 asks
 *     for bmTriggerStep() to become eng_noteOn(Ch::Drums, ...), and that is what this
 *     does; it does not ask for the drum machine, and the drum machine drags in the
 *     pad UI that is M3.
 *
 *   - V5's channel 2 in full (Euclidean generator, scale-following arpeggios, eight
 *     relative-interval key modes, auto-arpeggiator). M2 ships a fixed sparse chord
 *     layer so the third trigger seam is real and exercised; the generator waits for
 *     a milestone that has a UI to configure it.
 *
 *   - Step effects (OctUp / Retrigger / Stutter / the four chord-step modes). Carried in
 *     SeqStep, not acted on here. See the long note at the bottom of this file, which
 *     corrects a claim this comment used to make and which was wrong.
 *
 *   - Everything that reads a pad, a pot or a TFT. Not in this milestone by design.
 */

#pragma once

#include <Arduino.h>
#include "engine_iface.h"
// For CC_303_PORTAMENTO / CC_303_PORTATIME / CC_303_ENVMOD_LVL / CC_303_ACCENT_LVL.
//
// Explicit rather than transitive, and that is the entire reason this line exists.
// engine_iface.h includes only <Arduino.h>, and config.h does not include midi_config.h
// either -- so sequencer.ino's CC constants currently arrive by accident, because
// AcidBox.ino sorts first and includes synthvoice.h, which includes midi_config.h. That is
// a chain of three files happening to be concatenated in a friendly order, and this repo
// has already been bitten twice by relying on exactly that (an undeclared name inside an
// #if silently evaluating to 0 instead of erroring -- hence the explicit config.h in
// AcidBanger.ino and the explicit engine_iface.h / esp_flash.h in AcidBox.ino).
//
// midi_config.h is guarded, so including it here as well as there costs nothing.
#include "midi_config.h"

#define SEQ_NUM_STEPS 16

/* Step effects -- M2.5.
 *
 * V5's own numbering, which is a bare 0..7 in a uint8_t whose names live in a UI string
 * table (main sketch:1128) and whose meanings live in an if-chain three hundred lines away
 * (main sketch:2925-2940). Reproduced here as one enum so the number and the name cannot
 * drift apart: the failure mode is a preset that says 4 meaning Dom7 in one file and
 * MajStep in another, which is a silent transposition rather than a compile error.
 *
 * The 8 comes from V5, not from a count of what is implemented -- V5's FX_PAD_MAP is
 * {0,1,2,3,4,5,6,7}, an identity map, so a ninth pad cannot assign one.
 */
enum {
  SEQ_FX_NONE     = 0,
  SEQ_FX_OCTUP    = 1,
  SEQ_FX_RETRIG   = 2,
  SEQ_FX_STUTTER  = 3,
  SEQ_FX_MAJSTEP  = 4,
  SEQ_FX_MINSTEP  = 5,
  SEQ_FX_DOM7STEP = 6,
  SEQ_FX_DIMSTEP  = 7,
  SEQ_NUM_FX      = 8
};

/* One step of the pattern.
 *
 * `note` is V5's index, not a MIDI note, and the difference is 12 semitones -- see
 * seq_noteToMidi() for why the indirection exists at all rather than storing MIDI notes
 * in the presets.
 *
 * `effect` is one of SEQ_FX_*, acted on as of M2.5. Every value in the eight shipped
 * presets is SEQ_FX_NONE -- see the long note at the bottom of this file.
 */
struct SeqStep {
  uint8_t note;
  bool    active;
  bool    accent;
  bool    glide;
  uint8_t effect;
};

struct Sequencer {
  SeqStep  steps[SEQ_NUM_STEPS];
  uint8_t  cur;
  uint8_t  len;
  bool     running;

  uint16_t tempo;      // BPM
  uint32_t interval;   // us per 16th, = 60000000 / tempo / 4
  uint32_t lastUs;     // nominal time of the current step, advanced by whole intervals

  int8_t   key;        // semitones of transposition, signed
  int8_t   octave;     // octaves of transposition, signed
  uint8_t  rrMode;     // step order, 0..7, see SEQ_ORDER_* below
  uint8_t  portaSpeed; // 1..8, indexes SEQ_PORTA_CC[]

  bool     rrPingFwd;  // direction latch for the bouncing orders
  bool     drumsOn;
  bool     secondOn;

  /* Diagnostics. The M2 acceptance criterion is "ten minutes with no accumulating timing
   * jitter", and a criterion you cannot measure is a criterion you cannot claim to have
   * met -- so the clock measures itself. seq_driftMaxUs() is the largest deviation
   * between a step's actual fire time and its nominal time, over the life of the run.
   *
   * It is expected to be bounded by loop()'s scheduling jitter, not to grow. That is the
   * whole point: a clock that did `lastUs = now` instead of `lastUs += interval` would
   * also look fine for a minute and then be a tenth of a beat out, forever.
   *
   * `catchups` counts the steps where the resync branch fired, and those are EXCLUDED
   * from driftMaxUs. Not to flatter the number -- a catch-up deliberately throws away up
   * to half an interval of accumulated phase, so folding it into the maximum would report
   * a worst case that the clock chose on purpose. It is counted and printed instead, so
   * "no drift" and "no drift because it kept snapping" cannot be confused for one another.
   */
  uint32_t stepsPlayed;
  uint32_t catchups;
  uint32_t driftMaxUs;
  int32_t  driftLastUs;   // signed, for the report line

  /* M2.5 -- the sub-step schedule, for Retrig and Stutter. These two effects are about WHEN
   * rather than WHAT, so they cannot be turned into a note-on at the moment the step fires:
   * they have to be remembered and fired later inside the same step.
   *
   * `subCount` is how many extra hits this step owes and `subIdx` which one is next.
   * `subStepUs` is the nominal time of the step that armed the schedule, and each hit's
   * nominal time is recomputed from it rather than accumulated from the previous hit -- so
   * a stutter's second hit sits at exactly twice the offset of its first with no rounding
   * carried forward. That is the main clock's "absolute nominal time" discipline applied
   * one level down, and for the same reason.
   *
   * `subNote` and `subStep` are copies rather than reads of seq.steps[] / seq.cur at fire
   * time. A preset load or a pad gesture between the step and its sub-hits would otherwise
   * redirect a pending hit at a note the pattern no longer has, and the symptom would be a
   * stray note in an unrelated key rather than anything that looks like a race.
   */
  uint8_t  subCount;
  uint8_t  subIdx;
  uint8_t  subFx;
  uint8_t  subNote;
  uint8_t  subStep;
  bool     subAccent;
  uint32_t subStepUs;

  /* Sub-step instrumentation, on the same argument as driftMaxUs above: M2.5's acceptance
   * criterion is "the eight effects behave as V5's do", and two of V5's eight are timed
   * behaviours, so the only honest way to check those two is to time them.
   *
   * `subHits` is how many extra hits have actually fired, and it is the number that says
   * whether the scheduler runs AT ALL. The failure mode of not calling the sub-step poll is
   * a flawless drift report, an unchanging gate depth, and complete silence where the
   * stutter should be -- three healthy-looking readings and one missing feature.
   *
   * `subMaxErrUs` is the worst lateness of a hit against its own nominal time, the
   * sub-step analogue of driftMaxUs. Bounded, not growing.
   *
   * `subDropped` counts hits abandoned because the step moved on first. Expected to stay 0
   * forever: the largest sub-step offset is 42/64, so a pending hit is always due before the
   * next boundary unless the loop was blocked for a whole interval. It is counted rather
   * than asserted, because a number that is asserted to be zero and a number that is
   * printed to be zero are different claims.
   */
  uint32_t subHits;
  uint32_t subMaxErrUs;
  uint32_t subDropped;
};

/* Step orders. V5 calls these FWD / CW / ALT / REV / SKIP2 / SKIP3 / PING / RND, which
 * are the pad labels, not the semantics -- CW is "bounce across pads 1-8 regardless of
 * pattern length", which is a different thing from a 2-step ping-pong. The names below
 * are the behaviour, not V5's, because SEQ_ORDER_CW reading as "clockwise" is a trap.
 */
enum {
  SEQ_ORDER_FORWARD = 0,
  SEQ_ORDER_BOUNCE8 = 1,  // V5's CW: ping-pong over indices 0-7 only, ignores len
  SEQ_ORDER_ALTERNATE = 2,// even indices first, then odd
  SEQ_ORDER_REVERSE = 3,
  SEQ_ORDER_SKIP2 = 4,
  SEQ_ORDER_SKIP3 = 5,
  SEQ_ORDER_PINGPONG = 6, // V5's PING: over the whole pattern
  SEQ_ORDER_RANDOM = 7
};

/* Create the sequencer. Call from setup() after eng_init(), before seq_start().
 *
 * Safe to call more than once; it resets to preset 0 stopped, which is what a preset
 * load wants anyway.
 */
void seq_init();

/* The clock. Call once per regular_checks() pass, before eng_poll().
 *
 * Must be called before the drain, not after, for the same reason MIDI is read before
 * the drain: a step that posts its events after eng_poll() has already run waits a whole
 * loop iteration to be heard, which at 1 ms of jitter is small but at 16th notes it is
 * exactly the kind of thing that reads as "the sequencer feels loose".
 */
void seq_poll();

void seq_start();
void seq_stop();
void seq_toggle();

/* Is the clock running? Read back rather than inferred.
 *
 * Exists because "PLAY/STOP works" is otherwise only observable as the ABSENCE of two log
 * lines, and an absence is the weakest kind of evidence there is: a firmware that never
 * called seq_toggle() at all would print exactly the same log as one that called it and
 * failed. One line with the answer in it is what makes the pad testable by somebody who
 * is not listening to it.
 */
bool seq_running();

/* Back to how the machine came up -- V5's PLAY-long-press, main sketch:5872-5904.
 *
 * Deliberately does NOT stop the sequencer, because V5's does not: the reset is "put the
 * pattern and the settings back", and a user who holds PLAY for half a second while the
 * thing is playing expects the next bar to be the default pattern, not silence.
 *
 * What it resets, and the two places it deliberately does not follow V5:
 *
 *   - notes / active / accent / glide / effect -> seq_loadPreset(0)
 *   - tempo -> 120
 *   - step order -> forward
 *   - portamento speed -> 4 (V5's gPortaSpeed = 4, :5900)
 *   - channel 2 pitch mode -> CHORD
 *
 *   - V5 also zeroes octave/scale/sound/trans/algo and clears its filter globals under
 *     noInterrupts() (:5891-5899). None of those exist here: the port has one engine and no
 *     global filter state to clear, and seq_loadPreset() already puts octave at 0. V5's
 *     own default is octave 1, which is a different tuning convention rather than a step
 *     this port skipped -- preset notes here are absolute, not C-relative.
 *   - V5 clears kwMode and the walk generator's counters (:5896). The walk generator is M3
 *     Phase 4 and does not exist yet.
 */
void seq_factoryReset();

/* Load one of the SEQ_NUM_PRESETS built-in riffs. Clamped, not an error: a bad index
 * from a pad handler should not be able to walk off the table.
 */
void seq_loadPreset(uint8_t preset);

/* Preset count, for a UI that has to draw a list of them. */
uint8_t seq_presetCount();

void seq_setTempo(uint16_t bpm);
void seq_setLen(uint8_t steps);
void seq_setOrder(uint8_t mode);
void seq_setDrums(bool on);
void seq_setSecond(bool on);

/* Channel 2's pitch mode -- M3 Phase 2.
 *
 * WHY THIS IS NOT V5'S NUMBERING, stated at the declaration rather than discovered later.
 *
 * V5 has eight pitch modes for the pulse layer, CH2_VS_PITCH at :1680:
 *
 *     {"OFF","INV","RNDH","CHRD","FOLW","PDL","ECH1","WALK"}
 *
 * and ch2PitchMode is a bare 0..7 index into it (:1691). Only the FIRST of those is
 * reachable in this port today, and the port's pre-existing behaviour is not any of them.
 *
 * What M2 shipped is a fixed layer: a sparse mask over a walking interval -- steps 0/3/6 out
 * of 0x049, over {0,3,7}. The interval table holds four entries and the fourth is
 * unreachable, because the mask only ever fires three steps; that is a long-standing M2
 * discrepancy and the note on SEQ_CH2_MASK in sequencer.ino records it.
 * V5's mode 0 (OFF) is something else entirely -- it plays the same pitch
 * channel 1 is playing on that step, with none of the interval logic on top. So there was
 * no V5 mode to preserve here; there was a constant, and a choice about what to call it.
 *
 * Hence:
 *
 *   mode 0  CHORD      the fixed walk M2 shipped. The default, so that M2 and
 *                      M2.5's hardware baselines -- both measured with this sounding --
 *                      continue to mean what they meant. Not V5's mode 0.
 *   mode 1  OFF        V5's mode 0, ported: the step's own pitch, doubled at the unison.
 *   2..7               not ported yet. See the note below on why each one is blocked.
 *
 * The numbering deliberately does NOT line up with V5's from 1 upward. A table that looks
 * like V5's but is off by one everywhere else is worse than one that plainly is not: the
 * first is a trap for anyone who knows V5, the second is announced by its own first entry.
 *
 * WHY 2..7 ARE NOT HERE. Each of V5's remaining modes needs at least one thing this port
 * does not have, and inventing it would be writing a feature rather than porting one:
 *
 *   INV / RNDH / FOLW  need gLastCh1Note -- the note channel 1 last SOUNDED, held through
 *                      channel 1's silent steps. This port has no equivalent, because
 *                      channel 1's note-on is posted through eng_noteOn() and the engine
 *                      keeps no history the sequencer can read back.
 *   RNDH / WALK        need random(). V5's are random, so a bit-exact reproduction is not
 *                      available even in principle -- only "same mechanism, different
 *                      output", which is a claim that needs saying out loud when it is made.
 *   CHRD / PDL / ECH1 / WALK  need seq.scale with SCALES[] / SCALE_LENS[], and therefore
 *                      scaleNote() and seq.origNote[] -- none of which this port has. It
 *                      has steps[].note, which already equals V5's scaleNote(cur) + trans
 *                      with scale and transposition at their defaults, so mode 1 needs
 *                      nothing new. The scale-aware modes need the whole scale machinery
 *                      added first.
 *
 * Set / read it, and a name for a UI label. Clamped rather than rejected, for the same
 * reason every other setter here is: these are going to be called from a pad handler, and
 * a pad handler is not a place where an out-of-range index should be able to walk off a
 * table.
 */
void     seq_setCh2Mode(uint8_t mode);
uint8_t  seq_ch2Mode();
const char *seq_ch2ModeName(uint8_t mode);

/* Slide speed 1..8. This is V5's gPortaSpeed, and it is the replacement for V5's glide.
 *
 * See the long note on SEQ_PORTA_CC in sequencer.ino before changing the table: the
 * design doc's millisecond values do not fit the target's CC 5 range, and clamping them
 * would leave one dead control on a knob.
 */
void seq_setPortaSpeed(uint8_t speed);   /* 1..8, clamped */
uint8_t seq_portaSpeed();

/* V5 pattern index -> MIDI note number.
 *
 * V5 labels its own indices from C2: index 24 is C2, 27 is Eb2, 36 is C3. In MIDI
 * (A4 = 69) C2 is 36, so MIDI = index + 12, plus whatever transposition is dialled in.
 *
 * The reason this is a function and not just `note + 12` inlined at the call site is that
 * the offset is not obvious and the presets are full of bare numbers. Someone reading
 * {24,24,24,27,...} has no way to know that is a C minor riff two octaves down.
 *
 * A NOTE ON PITCH, because it is a real discrepancy and not a rounding curiosity:
 * V5's own engine does not sound what its labels say. noteFreq[] is built on a base of
 * ~274 Hz and triggerNote() halves it, and the acid engine's 16-bit phase accumulator
 * then divides by another 4 (computeAcidSample() does `cnt += gFreq` and the saw reads
 * `cnt >> 8`, so one cycle is 65536 counts at AUDIO_RATE 16384). So index 24 comes out
 * at 1097/2/4 = 137 Hz, which is C#3, not the C2 the pattern comment claims. That is
 * about two octaves and a semitone of V5 being wrong about itself.
 *
 * This port plays the DOCUMENTED pitch. Fixing V5's tuning bug is not M2's job, and
 * reproducing a two-octave error in new code on the grounds of fidelity would be a
 * strange kind of loyalty.
 */
uint8_t seq_noteToMidi(uint8_t idx);

/* Where the next step lands, for the order modes. Split out from the firing so the
 * advance logic can be read (and eventually unit-tested) without the engine in the way.
 */
uint8_t seq_nextPatStep();

/* Diagnostics, for the periodic report in seq_poll(). */
uint32_t seq_driftMaxUs();
uint32_t seq_stepsPlayed();

/* Reset the drift accumulator. Exists so a report is about a known window rather than
 * about everything since boot.
 */
void seq_driftReset();

/* Set / read one step's effect -- M2.5.
 *
 * This is the whole of M2.5's claim to "the effects are available". V5 only ever writes the
 * effect byte from doFXAssign(), which lives in the FUNC+FX pad sub-mode -- M3's territory,
 * arriving with the pads that can reach it. Without an entry point here the feature would be
 * unreachable from firmware at all, and unreachable code is exactly the kind that looks
 * correct and is never once run.
 *
 * Both arguments are clamped rather than rejected, for the same reason seq_loadPreset()
 * clamps: these are going to be called from a pad handler, and a pad handler is not a place
 * where an out-of-range index should be able to walk off a table.
 */
void    seq_setStepEffect(uint8_t step, uint8_t fx);
uint8_t seq_stepEffect(uint8_t step);

/* Step on/off, accent and glide -- M3 Phase 3.
 *
 * These complete the set that seq_setStepEffect() started. Without them the eight pads are
 * a transport and a parameter menu with no way to write a note, which is a sequencer you
 * can listen to and not play.
 *
 * V5 writes seq.steps[p].active / .accent / .glide directly from doPadRelease() (:2967) and
 * doPadLong() (:2985-2986). Here they go through setters, for the reason Phase 1 set the
 * precedent with the effect byte: seq.steps[] then has exactly one writer, and a pad
 * handler cannot disagree with the sequencer about what a step is.
 *
 * There is a setter AND a getter for each, not a toggle-only API. V5's doPadLong() needs to
 * know the current value to alternate between accent and glide, and a toggle-only setter
 * would make that alternation a property of how many times the pad was pressed rather than
 * of what the step is -- which is a state that survives a preset load in the wrong way.
 * The pad handler reads the value, decides, and writes.
 *
 * All clamped, all non-erroring: a pad index is not a place where a bad value can be
 * allowed to reach an array.
 */
void    seq_setStepActive(uint8_t step, bool on);
void    seq_toggleStep(uint8_t step);
bool    seq_stepActive(uint8_t step);
void    seq_setStepAccent(uint8_t step, bool on);
bool    seq_stepAccent(uint8_t step);
void    seq_setStepGlide(uint8_t step, bool on);
bool    seq_stepGlide(uint8_t step);

/* Read-backs for a UI or a log line. Same reason as seq_running(): the value the pad
 * handler set and the value the sequencer holds are two facts, and only the second one is
 * evidence. */
uint16_t seq_tempo();
uint8_t  seq_len();
uint8_t  seq_order();

/* Effect name, for a UI label or a diagnostic print. Never NULL; "?" for fx >= SEQ_NUM_FX.
 * V5 keeps the same eight strings in a UI table (main sketch:1128) and this is that table.
 */
const char *seq_fxName(uint8_t fx);

/* Sub-step diagnostics, M2.5. Zero until an effect with sub-hits actually fires -- which,
 * for the eight shipped presets, is never. See the long note below. */
uint32_t seq_subHits();
uint32_t seq_subMaxErrUs();
uint32_t seq_subDropped();

/*
 * =====================================================================
 * A CORRECTION, and two facts about V5's shipped content that change what M2.5 can claim
 *
 * This file used to say: "Step effects ... The effect byte is carried in SeqStep and the
 * presets already populate it, so M2.5 is a switch statement rather than a data migration."
 *
 * The first clause is true. The second is false, and it was false about V5 as well as about
 * this port. Checked field by field against V5 rather than by eye: all eight of V5's factory
 * PRESETS have an all-zero effect column, and so do all eight of ours -- byte for byte on
 * all four columns, so the port is not at fault and never was.
 *
 * The error was in a comment, not in code, and it was still worth correcting, because the
 * comment is what set the expectation that implementing the switch would be enough. It is
 * not. Three things had to happen, and only one of them is the switch:
 *
 *   1. the effect switch itself;
 *   2. seq_setStepEffect(), because V5 only ever writes this byte from doFXAssign() in the
 *      FUNC+FX pad sub-mode, which arrives with M3. With no entry point, nothing in the
 *      effect path can ever run, so it could not be verified by listening or by log;
 *   3. instrumentation on the two timed effects, because "the effects behave as V5's do" is
 *      not checkable for those two without measuring when they fired.
 *
 * Two facts about V5's content, which are the reason the scope stopped here rather than
 * growing:
 *
 *   - V5 has a SECOND pattern set this port does not have: WALK_PATTERNS[8], "ACID WALKS",
 *     an easter egg reached by holding pads 11-14 together (main sketch:857). Seven of its
 *     eight patterns use effects, and between them they use 1, 2, 3, 4 and 5. So real V5
 *     content that exercises step effects exists, and M2 did not port it. Porting it is
 *     content work belonging with the gesture that makes it reachable, which is M3.
 *
 *   - Effects 6 (Dom7Step) and 7 (DimStep) appear NOWHERE in V5's content -- not in the
 *     factory presets, not in the walks. They exist only as values a pad can be assigned.
 *     Which means "all eight effects work" cannot be demonstrated by playing V5's patterns
 *     in EITHER direction, and no amount of porting V5's patterns will change that. It has
 *     to be demonstrated on the mechanism.
 *
 * So M2.5's claim is deliberately about the mechanism: all eight effects resolve to V5's
 * pitches and, for the two timed ones, fire at V5's offsets. The verification for that is in
 * tools/test-sequencer.py and in the boot-time dump in seq_start(); the verification that it
 * runs on hardware at all is the sub-step counters in the periodic report, which is why they
 * are in the report rather than behind an accessor nobody calls.
 * =====================================================================
 */
