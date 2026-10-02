/*
 * engine_iface.h -- M1, the semantic event layer.
 *
 * This is the seam the rest of the project grows through. Today the only way to make
 * the engine make a sound is to feed it a MIDI message, which means a sequencer has to
 * lie and pretend to be a MIDI controller. That works, and it is also why M2's three
 * trigger sites ended up coupled to three different pieces of engine internals.
 *
 * Everything goes through here instead, including MIDI. That last part is the
 * deliberate bit, and it is what makes this worth building rather than just a wrapper:
 *
 *   - One writer. Engine parameters are mutated from exactly one place, inside
 *     eng_apply(). A sequencer and a MIDI cable can no longer interleave halfway
 *     through each other's parameter changes.
 *
 *   - Both entry points run the same code, so the acceptance test "the sequencer
 *     sounds like MIDI does" stops being a thing to verify by ear and becomes
 *     structurally true. There is only one implementation to get right.
 *
 *   - No added latency. eng_poll() drains at the end of the same regular_checks() pass
 *     that enqueued the events, so a MIDI note-on is applied in the same loop iteration
 *     it arrived in. The queue buys ordering and single-writer, not deferral. If you
 *     ever move the drain earlier or later than regular_checks(), you have added a tick
 *     of latency to every note and should know that.
 *
 * NOT an .h-pure header: the implementation lives in engine_iface.ino, deliberately,
 * not engine_iface.cpp as the design doc says. See the note at the top of the .ino.
 */

#pragma once

#include <Arduino.h>

/* Which engine. Values are engine indices, not MIDI channel numbers -- the two do not
 * numerically agree, and getting that backwards is easy:
 *
 *     Ch::Acid   -> MIDI channel  1  (SYNTH1_MIDI_CHAN, config.h:231)
 *     Ch::Second -> MIDI channel  2  (SYNTH2_MIDI_CHAN, config.h:232)
 *     Ch::Drums  -> MIDI channel 10  (DRUM_MIDI_CHAN,   config.h:234)
 *
 * Note 10, not 3. chanToCh() below is the only place that conversion belongs.
 */
enum class Ch : uint8_t { Acid = 0, Second = 1, Drums = 2 };

/* Accent velocities. 80 is the gate, not a round number chosen for looks:
 * synthvoice.ino:223 reads `mva_alloc(note, (velocity >= 80))`. These sit either side of
 * it with margin, so an accent can never land ambiguously near the threshold.
 *
 * ENG_VEL_PLAIN is 79, one below the gate -- the strongest non-accent that still is not
 * an accent. If a future edit bumps this to 80 the accent flag silently stops meaning
 * anything on every note in the kit.
 */
#define ENG_VEL_PLAIN  79
#define ENG_VEL_ACCENT 127

/* Depth of the pending queue, in events.
 *
 * 128 events is 512 bytes, against 266 KB still free. Draining is bounded at 32 per
 * regular_checks() pass, so the queue would have to be fed 128 events inside a single
 * loop iteration to ever overflow, and nothing in this firmware comes close -- a busy
 * MIDI DIN stream at 31.25 kbaud is 3 messages per millisecond. So this is sized to
 * make eng_drops() permanently zero, not to make overflow graceful. The drop counter is
 * there to prove that rather than to assume it.
 */
#ifndef ENGINE_QUEUE_LEN
#define ENGINE_QUEUE_LEN 128
#endif

/* The queued event. Four bytes, so the queue is a flat ring of words and never
 * allocates. Layout is fixed by size, not by taste:
 *
 *   op = EvOp, ch = Ch, a = first payload, b = second payload
 *
 * note-on / note-off:  a = note,        b = velocity
 * set param:           a = CC number,   b = value
 * select program:      a = program number
 * all notes off:       (none)
 */
struct Ev { uint8_t op, ch, a, b; };

enum EvOp : uint8_t {
  EV_NOTE_ON        = 0,
  EV_NOTE_OFF       = 1,
  EV_SET_PARAM      = 2,
  EV_SELECT_PROGRAM = 3,
  EV_ALL_NOTES_OFF  = 4
};

/* Create the queue. Must be called from setup(), after Synth1/Synth2/Drums are Init()ed
 * and before anything can post an event.
 *
 * Posting before this is not a crash -- eng_post() falls back to applying the event
 * immediately so that a panic before init still silences the engine -- but events that
 * arrive before it are not ordered against anything, so init early.
 */
void eng_init();

/* Note on.
 *
 * accent is an override, not a replacement: an accented note always goes out at
 * ENG_VEL_ACCENT, a non-accented one at whatever vel the caller passed.
 *
 * This is deliberately not the design doc's `accent ? 127 : 79`. That form ignores vel
 * entirely, which was fine when only the sequencer was going to call it -- the sequencer
 * has no velocity of its own, so it had nothing to lose. But MIDI routes through this
 * same function now, and throwing away the keyboard's real velocity would be a
 * regression in the one path that currently works. So M2's sequencer passes
 * ENG_VEL_PLAIN as vel and gets exactly the documented behaviour, and nobody else has to
 * know that.
 */
void eng_noteOn(Ch ch, uint8_t note, uint8_t vel, bool accent);

/* Note off.
 *
 * No velocity parameter, and that is not an oversight: SynthVoice::on_midi_noteOFF()
 * takes a velocity and never reads it (synthvoice.ino:231-238 frees the voice and moves
 * on), and Sampler::NoteOff() does not take one at all. Passing 0 loses nothing.
 */
void eng_noteOff(Ch ch, uint8_t note);

/* Set a parameter by CC number.
 *
 * The channel selects the engine, with one exception that is easy to mistake for a bug
 * and must not be "cleaned up": CC_ANY_* are matched on the CC number before any channel
 * routing happens, so they take effect whichever channel they arrive on. That is how
 * they already behave on the MIDI path, and compressor / all four delay params / both
 * reverb params / notes-off live in that group. Route them by channel instead and they
 * all become unreachable from the sequencer, silently -- nothing errors, the parameter
 * just stops responding.
 *
 * CC_ANY_RESET_CCS / _NOTES_OFF / _SOUND_OFF call eng_allNotesOff(), which is the plain
 * engine-wide silence. The MIDI path's extra behaviour on those (a one-per-second rate
 * limit and do_midi_stop() under JUKEBOX) stays in handleCC where it belongs: it is
 * about stopping the jukebox, which a sequencer has no reason to want.
 */
void eng_setParam(Ch ch, uint8_t cc, uint8_t val);

/* Select a program. No channel parameter, and the reason is worth stating rather than
 * leaving to inference: Drums is the only engine that has programs at all
 * (Sampler::SetProgram, midi_handler.ino:113). Both synths would have nothing to do with
 * the argument, so a channel here would be a parameter that can only ever have one legal
 * value -- which is how you get a caller passing the wrong one with no complaint.
 *
 * The signature is the design doc's. If a synth ever grows programs this needs a real
 * decision (per-engine program banks, or one shared bank) and then a channel parameter.
 * That is the moment to add one -- not before, and not silently.
 */
void eng_selectProgram(uint8_t prog);

/* Silence every engine. Queued like everything else, so engine state still has exactly
 * one writer. In practice the delay is the remainder of the current regular_checks()
 * pass, not a tick.
 */
void eng_allNotesOff();

/* Apply every pending event, in arrival order. Bounded, so that a flood of events
 * cannot stall loop() -- which is the failure that starves the note scheduler and is
 * exactly the M0 noise all over again.
 *
 * Returns true if anything was applied.
 */
bool eng_poll();

/* Events dropped because the queue was full. Expected to stay at zero forever; see
 * ENGINE_QUEUE_LEN. If it climbs, the drain rate is too low for the event rate, and
 * regular_checks() prints it rather than trusting the reader to notice.
 */
uint32_t eng_drops();

/* How many notes this engine currently has held, i.e. the depth of its SynthVoice
 * monophonic-with-legato stack. Read-only and diagnostic.
 *
 * This exists because of one specific fact about SynthVoice that is easy to miss and
 * expensive to get wrong: on_midi_noteON computes `slide = (mvaStack.n > 1)`, so a stack
 * deeper than 1 turns every subsequent note-on into a legato -- setFrequency, no envelope
 * retrigger. Any caller that posts note-ons without posting note-offs therefore fills the
 * stack once and then produces no attacks at all for the rest of the session.
 *
 * That failure is nearly unlistenable on its own terms: it sounds like a tuning fault, or
 * like a filter that will not open, and it gets investigated as a sound-design question
 * rather than as a gate question. The sequencer's own report prints this depth so the claim
 * "the gate is draining correctly" is a number in the log rather than a judgement.
 *
 * Ch::Drums returns 0 always: Sampler has no note stack, because Sampler::NoteOff() is an
 * empty function and drum samples are one-shots. There is genuinely nothing to hold.
 */
uint8_t eng_heldDepth(Ch ch);

/* MIDI channel number -> Ch. Returns false for a channel this engine does not own,
 * which happens legitimately: MIDI here is in omni mode, so any channel can carry a
 * message and handleNoteOn's if/else-if chain just ignores the rest. Callers use the
 * bool to skip rather than to error.
 */
bool chanToCh(uint8_t midiChan, Ch &out);