/*
 * engine_iface.ino -- M1, the semantic event layer. See engine_iface.h for the API and
 * for why the queue exists at all.
 *
 * An .ino and not the engine_iface.cpp the design doc asks for. The sketch has no .cpp
 * files at all; every source is a .ino, concatenated by arduino-cli into one translation
 * unit. A .cpp would be the first separate translation unit this sketch ever had, and it
 * would immediately need extern declarations for Synth1, Synth2, Drums, Comp, Delay and
 * Reverb -- all six instantiated in AcidBox.ino:173-184, which is inside the .ino
 * concatenation and therefore invisible from anywhere else. That is a linkage surface
 * invented for one file, and it would put the .ino ordering trap (documented in
 * AcidBanger.ino:4, and it has bitten twice) into a file that has no way to reason about
 * its own position in the concatenation.
 *
 * There is a second reason, which settles it. midi_handler.ino has to delegate INTO this
 * layer. Alphabetically "engine_iface" < "midi_handler", so in the concatenation this
 * file comes first and midi_handler.ino can see everything defined here. The reverse
 * direction would be a forward reference. Single translation unit is not a convenience
 * here; it is what makes the dependency point the right way round.
 *
 * Every include below is explicit even though the concatenation would supply most of
 * them. Same reasoning as the #include "config.h" added to AcidBanger.ino: relying on
 * alphabetical position to decide what a file can see is how you get a symbol that
 * silently evaluates to 0. All of these headers are guarded, so including them twice is
 * free.
 */

#include "engine_iface.h"
#include "config.h"
#include "midi_config.h"
#include "synthvoice.h"
#include "sampler.h"
#include "fx_delay.h"
#include "fx_reverb.h"
#include "compressor.h"
#include "freertos/queue.h"

static QueueHandle_t engq      = nullptr;
static uint32_t      engDropped = 0;

/* Upper bound on events applied per eng_poll() call.
 *
 * Bounded on purpose. An unbounded drain looks harmless and is not: if producers ever
 * outrun the drain, the drain never returns, loop() stops, and the note scheduler stops
 * with it. That is not a hypothetical failure mode, it is the M0 noise -- audio_task2
 * starves loop(), everything downstream of loop() goes silent. A drain that can starve
 * its own caller reintroduces the exact bug this project just spent a milestone closing.
 *
 * 32 per pass, and regular_checks() runs at well over 100 Hz, so the ceiling is above
 * 3000 events/s. A busy MIDI DIN wire at 31.25 kbaud carries about 3 messages/ms.
 */
#define ENG_DRAIN_MAX 32

void eng_init() {
  if ( engq != nullptr ) return;   // idempotent: setup() may run more than once on reset paths
  engq = xQueueCreate(ENGINE_QUEUE_LEN, sizeof(Ev));
  if ( engq == nullptr ) {
    // No queue means no ordering and no single-writer guarantee, but it must not mean no
    // sound. eng_post() degrades to applying immediately, which is the right answer to a
    // failed allocation and the wrong answer to a full queue -- see eng_post().
    DEBF("[ENGINE] queue alloc FAILED (%d bytes) -- events bypass the queue\r\n",
         (int)(ENGINE_QUEUE_LEN * sizeof(Ev)));
  }
}

/* Immediate implementations. These are what eng_apply() calls; nothing outside this file
 * should call them. Split out from the public functions so that the queue boundary is in
 * one place and it is obvious that these do not re-enter it.
 */
static void eng_setParamNow(Ch ch, uint8_t cc, uint8_t val);
static void eng_allNotesOffNow();
static void eng_apply(const Ev &e);

static bool eng_post(uint8_t op, uint8_t ch, uint8_t a, uint8_t b) {
  Ev e;
  e.op = op; e.ch = (uint8_t)ch; e.a = a; e.b = b;
  if ( engq != nullptr ) {
    if ( xQueueSend(engq, &e, 0) == pdTRUE ) return true;
    // Full. Drop it and count it, deliberately, instead of applying it here.
    //
    // The tempting alternative is "just apply it inline, we are already here" and it is
    // wrong: it makes eng_apply() no longer the only writer of engine state, and it does
    // so at exactly the moment things are already going wrong. A dropped note is a gap
    // you can hear; an event applied out of order behind a partially drained queue is a
    // bug you cannot. With 128 slots and a 32-per-pass drain, reaching full needs 128
    // events inside one loop() iteration, which nothing in this firmware can do -- so the
    // counter exists to prove that rather than to assume it. regular_checks() prints it.
    engDropped++;
    return false;
  }
  // Pre-init, or the allocation failed. No queue means no concurrent writer to violate,
  // so applying here is safe and keeps a panic before eng_init() from leaving the engine
  // ringing.
  eng_apply(e);
  return true;
}

void eng_noteOn(Ch ch, uint8_t note, uint8_t vel, bool accent) {
  eng_post(EV_NOTE_ON, (uint8_t)ch, note,
           accent ? (uint8_t)ENG_VEL_ACCENT : vel);
}

void eng_noteOff(Ch ch, uint8_t note) {
  eng_post(EV_NOTE_OFF, (uint8_t)ch, note, 0);
}

void eng_setParam(Ch ch, uint8_t cc, uint8_t val) {
  eng_post(EV_SET_PARAM, (uint8_t)ch, cc, val);
}

void eng_selectProgram(uint8_t prog) {
  eng_post(EV_SELECT_PROGRAM, (uint8_t)Ch::Drums, prog, 0);
}

void eng_allNotesOff() {
  eng_post(EV_ALL_NOTES_OFF, (uint8_t)Ch::Drums, 0, 0);
}

bool eng_poll() {
  if ( engq == nullptr ) return false;
  bool     any = false;
  uint8_t  n   = 0;
  Ev       e;
  while ( n < ENG_DRAIN_MAX && xQueueReceive(engq, &e, 0) == pdTRUE ) {
    n++;
    any = true;
    eng_apply(e);
  }
  return any;
}

uint32_t eng_drops() { return engDropped; }

bool chanToCh(uint8_t midiChan, Ch &out) {
  switch (midiChan) {
    case SYNTH1_MIDI_CHAN: out = Ch::Acid;   return true;
    case SYNTH2_MIDI_CHAN: out = Ch::Second; return true;
    case DRUM_MIDI_CHAN:   out = Ch::Drums;  return true;
    // Not an error. MIDI here is in omni mode, so a message can arrive on any channel
    // and the original if/else-if chain simply ignored the ones this engine does not
    // own. Callers skip; they do not complain.
    default: return false;
  }
}

static void eng_setParamNow(Ch ch, uint8_t cc, uint8_t val) {
  // CC_ANY_* first, on the CC number alone, exactly as handleCC has always done it. The
  // channel is deliberately not consulted for this group.
  //
  // This is the one place in M1 where a "tidier" implementation would be a regression.
  // Routing these by channel -- which is what the shape of the code invites, since
  // everything below it is channel-routed -- makes the compressor, all four delay
  // parameters, both reverb parameters and notes-off unreachable from any caller that
  // has a channel to pass. Nothing errors. The parameter just stops responding, on both
  // the MIDI path and the sequencer path, and it looks like a firmware bug for ever.
  //
  // If you are reading this wondering whether the channel should be consulted, the answer
  // is no, and the reason is that this mirrors MIDI's own semantics: these parameters are
  // global on a real MIDI setup too, which is why they are named CC_ANY_*.
  switch (cc) {
    case CC_ANY_COMPRESSOR:
      Comp.SetRatio(3.0f + val * 0.307081f);
      break;
    case CC_ANY_DELAY_TIME:
      Delay.SetLength(val * MIDI_NORM);
      break;
    case CC_ANY_DELAY_FB:
      Delay.SetFeedback(val * MIDI_NORM);
      break;
    case CC_ANY_DELAY_LVL:
      Delay.SetLevel(val * MIDI_NORM);
      break;
#ifndef NO_PSRAM
    case CC_ANY_REVERB_TIME:
      Reverb.SetTime(val * MIDI_NORM);
      break;
    case CC_ANY_REVERB_LVL:
      Reverb.SetLevel(val * MIDI_NORM);
      break;
#endif
    case CC_ANY_RESET_CCS:
    case CC_ANY_NOTES_OFF:
    case CC_ANY_SOUND_OFF:
      // Plain engine-wide silence. The MIDI path's extras -- the one-per-second rate
      // limit, and do_midi_stop() under JUKEBOX -- stay in handleCC. Those are about
      // stopping the jukebox, which is a MIDI-cable concern; a sequencer that asked for
      // notes-off wants the engines quiet, not playback cancelled.
      eng_allNotesOffNow();
      break;
    default:
      switch (ch) {
        case Ch::Acid:   Synth1.ParseCC(cc, val); break;
        case Ch::Second: Synth2.ParseCC(cc, val); break;
        case Ch::Drums:  Drums.ParseCC(cc, val);  break;
      }
      break;
  }
}

static void eng_allNotesOffNow() {
  Synth1.allNotesOff();
  Synth2.allNotesOff();
}

static void eng_apply(const Ev &e) {
  const Ch ch = (Ch)e.ch;
  switch (e.op) {
    case EV_NOTE_ON:
      switch (ch) {
        case Ch::Acid:   Synth1.on_midi_noteON(e.a, e.b); break;
        case Ch::Second: Synth2.on_midi_noteON(e.a, e.b); break;
        case Ch::Drums:  Drums.NoteOn(e.a, e.b);          break;
      }
      break;

    case EV_NOTE_OFF:
      switch (ch) {
        // Velocity argument 0 is not a placeholder here. SynthVoice::on_midi_noteOFF()
        // accepts a velocity and never reads it (synthvoice.ino:231-238), and
        // Sampler::NoteOff() does not take one. Passing 0 loses nothing.
        case Ch::Acid:   Synth1.on_midi_noteOFF(e.a, 0); break;
        case Ch::Second: Synth2.on_midi_noteOFF(e.a, 0); break;
        case Ch::Drums:  Drums.NoteOff(e.a);              break;
      }
      break;

    case EV_SET_PARAM:
      eng_setParamNow(ch, e.a, e.b);
      break;

    case EV_SELECT_PROGRAM:
      // Drums-only, and eng_post() hardcodes Ch::Drums, so there is no channel to be
      // wrong here. See the note on eng_selectProgram() in the header for what to do if
      // a synth ever grows programs.
      Drums.SetProgram(e.a);
      break;

    case EV_ALL_NOTES_OFF:
      eng_allNotesOffNow();
      break;

    default:
      // Unreachable unless a caller posts a bad op, which would be a memory bug rather
      // than a logic one. Counted anyway, because a silent default in an event dispatcher
      // is how an event goes missing and nobody ever learns which one.
      engDropped++;
      break;
  }
}