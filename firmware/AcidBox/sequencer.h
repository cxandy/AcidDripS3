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
 *   - Step effects (OctUp / Retrigger / Stutter / the four chord-step modes). The
 *     effect byte is carried in SeqStep and the presets already populate it, so M2.5
 *     is a switch statement rather than a data migration. That ordering is the design
 *     doc's own (M2.5, a separate day).
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

/* One step of the pattern.
 *
 * `note` is V5's index, not a MIDI note, and the difference is 12 semitones -- see
 * seq_noteToMidi() for why the indirection exists at all rather than storing MIDI notes
 * in the presets.
 *
 * `effect` is carried but not yet acted on. See the note at the top of this file.
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
