#pragma once

#define PROG_NAME       "ESP32 AcidBox"
#define VERSION         "v.1.5.0 S3"

#define BOARD_HAS_UART_CHIP

// M2: the jukebox is OFF. It was "real-time endless auto-compose acid tunes" -- it picked
// notes at random and posted them straight at handleNoteOn(). Nothing about that survived
// contact with a real sequencer: it has no pattern, no step position, no accent and no
// slide, so there was no such thing as a bar, and no way to ask for one twice the same way.
//
// Replaced by the 16-step sequencer in sequencer.h, which posts through the same event
// layer (engine_iface) rather than pretending to be a MIDI cable. Kept in the tree, off,
// because it is the fallback if the sequencer ever needs to be compared against something
// that is known to work -- and "known to work" is a claim only the old thing can support.
//
// JUKEBOX itself is off, not just JUKEBOX_PLAY_ON_START. The first M2 commit turned off
// only the autostart and left the switch itself on, which was a lie told in two places:
// this comment already said "off" and it was not, and jukebox_tick() was still being called
// from regular_checks() every pass. midi_playing only ever gets set inside do_midi_start(),
// which is the JUKEBOX_PLAY_ON_START-gated call, so nothing was playing -- the symptom was
// absent, not the cause. What was still running was run_tick()'s 250 Hz button scan plus
// run_ui(), on core 0, the same core the sequencer clock runs on, which is the core whose
// timing the drift measurement is about. Dead load next to the thing being measured.
//
// Verified before flipping: all of AcidBanger.ino is inside #ifdef JUKEBOX, and every
// symbol it defines (instruments[], buttons[], button_pins[], send_midi_noteon/off,
// init_instruments, do_midi_stop, run_ui, do_midi_tick, current_drumkit) is referenced only
// from inside that same #ifdef. The mentions outside it -- sampler.ino:387, engine_iface.ino
// :196, midi_handler.ino:68, config.h:104, sequencer.ino:407 -- are all comments, and the one
// real outside call site, midi_handler.ino:106 do_midi_stop(), carries its own #ifdef JUKEBOX.
// Note that current_drumkit is not state the drums need: Sampler::NoteOff() is empty, the kit
// is chosen per note as kit*12 + slot, and the sequencer posts drum notes through eng_noteOn
// with the offset already applied.
//
// One side effect, in the good direction: init_midi() did pinMode(LED_BUILTIN, OUTPUT) and
// config.h:327 defines LED_BUILTIN as 0, so the jukebox was putting GPIO 0 into output mode
// during setup() on a pin that is the boot strap. That is now gone.
//#define JUKEBOX
//#define JUKEBOX_PLAY_ON_START

/* STEP-EFFECT ACCEPTANCE BUILD.
 *
 * Set to 1 for ONE flashed build whose only job is to prove the M2.5 sub-step scheduler
 * actually runs on hardware; capture the serial log; then set it back to 0 before the
 * shipping build. Nothing else changes between the two, so the diff is one token.
 *
 * The reason this build exists at all is in the long comment on SEQ_FX_SELFTEST further down.
 * Short version: every shipped preset step is SEQ_FX_NONE, so in a normal build the sub-step
 * code is never reached, and leaving it out entirely would leave every existing diagnostic
 * reading exactly as healthy as it is now. Three or four readings would look perfect with the
 * whole feature missing.
 */
#define SEQ_FX_SELFTEST 0

#define SEQUENCER               // the 16-step sequencer: M2, supersedes JUKEBOX

/* Play at boot. Was justified as "there is no pad UI until M3, so this is the only way to
 * hear it" -- true when written, false as of M3 Phase 3, which gave the pads PLAY/STOP.
 *
 * It stays on, for a different reason, and the reason is in AcidBox.ino next to seq_start():
 * PAD_PINS is a guess (see the note above it), so a build that boots silent AND waits for a
 * pad press would look identical to a dead board. Starting the clock keeps the log filling
 * and the speakers moving no matter what the pads turn out to be doing.
 *
 * Turning this off is a one-token change and is correct once the pin table has been checked
 * against the schematic AND by a hardware run in which all sixteen pads respond. Do not read
 * the line above as "this should be off by now". */
#define SEQUENCER_PLAY_ON_START
#define MIDI_RAMPS              // this is what makes automated Cutoff-Reso-FX turn
//#define TEST_POTS               // experimental interactivity with potentiometers connected to POT_PINS[] defined below

//#define USE_INTERNAL_DAC      // use this for testing, SOUND QUALITY SACRIFICED: NOISY 8BIT STEREO
//#define NO_PSRAM              // if you don't have PSRAM on your board, then use this define, but REVERB TO BE SACRIFICED, ONE SMALL DRUM KIT SAMPLES USED 

//#define FLASH_LED               // flash built-in LED
//#define LOLIN_RGB               // Flashes the LOLIN S3 built-in RGB-LED

// M0: DEBUG_ON stays on for the shipping build, which is a deliberate change from the
// plan above. The author's warning -- that debugging "eats ticks initially belonging
// to real-time tasks" -- was written for a tree where the audio tasks ran at priority 1
// alongside loopTask on the same core, so serial printing genuinely competed with the
// sequencer for the same time slice. audio_task1 and audio_task2 are now priority 5,
// pinned to core 0 and core 1 respectively and neither one busy-spinning, so the log
// runs from loop() at priority 1 without ever touching the audio path.
//
// What it buys, and it is worth more than the ticks: the boot log proves the sample
// kit loaded and how much cache it needed, and the [WARN] line in regular_checks()
// keeps reporting the sampler's bounds guard for the life of the unit. That guard is a
// real fix, and a fix with no signal attached to it is a fix nobody will notice
// breaking. Turn DEBUG_ON off once there is hardware to watch it on.
//
// DEBUG_SAMPLER is deliberately left off: it prints per-sample lines from
// Sampler::Init and would drown the numbers we actually want.
#define DEBUG_ON              // note that debugging eats ticks initially belonging to real-time tasks, so sound output will be spoild in most cases, turn it off for production build
//#define DEBUG_MASTER_OUT      // serial monitor plotter will draw the output waveform
//#define DEBUG_SAMPLER
//#define DEBUG_SYNTH
//#define DEBUG_JUKEBOX
//#define DEBUG_FX
//#define DEBUG_TIMING
//#define DEBUG_MIDI

// M0: MIDI_USB_DEVICE off, USB MIDI temporarily sacrificed for the debug log.
//
// The board turned out to have a USB-UART bridge after all: it boots reporting
// rst:0x15 (USB_UART_CHIP_RESET), which the ROM only reports when a bridge chip
// drives EN. So UART0 does reach the single USB connector, and DEBUG_PORT below
// is back on UART0 where it belongs. An earlier revision of this file claimed the
// opposite and pushed the log onto USBMode=hwcdc; that moved Serial to
// HWCDCSerial (HardwareSerial.h:444) and the log went silent while the IDF
// console on UART0 kept printing, which is how the mistake showed itself.
//
// USB MIDI still goes unused here, for a different reason: with the bridge
// owning that connector the native USB-OTG port is not what the cable is
// plugged into. Re-enabling it costs ~50 kB of RAM for a port this board does
// not expose.
//#define MIDI_USB_DEVICE       // use this option if you want to operate via USB with the sampler seen as a MIDI device (-50 kBytes of available RAM)
// #define MIDI_VIA_SERIAL       // use this option to enable Hairless MIDI on Serial port @115200 baud (USB connector), THIS WILL BLOCK SERIAL DEBUGGING as well
//#define MIDI_VIA_SERIAL2        // use this option if you want to operate by standard MIDI @31250baud, UART2 (Serial2),

/* MIDI DIN on UART1 -- GPIO18 TX, GPIO21 RX. PIN_PLAN.md section 6.
 *
 * Deliberately NOT on UART0's 43/44. The ROM console prints to U0TXD = GPIO43 before any
 * firmware runs, so a MIDI output circuit on 43 would put the boot log out as MIDI data on
 * every power-up. That also means the two pins are free for pads 15 and 16.
 *
 * MIDI RX additionally needs a 6.8k/10k divider on the board: a MIDI input is a 5V current
 * loop (the sender's optocoupler and 220R to 5V) and ESP32 GPIOs are not 5V tolerant.
 * Divided, 5V becomes 2.98V -- above the 2.475V input-high threshold, below 3.3V. Leaving
 * that divider off burns the pin.
 *
 * Both features are off below (MIDI_VIA_SERIAL2 and ENABLE_MIDI_OUT), so this is inert
 * until one is enabled -- which is also why the old MIDITX_PIN 15 colliding with POT_PINS[0]
 * never actually bit: nothing had opened MIDI to hit it. */
#define MIDIRX_PIN      21      // UART1 RX, input only when MIDI_VIA_SERIAL2 is defined
#define MIDITX_PIN      18      // UART1 TX, output when MIDI_VIA_SERIAL2 and ENABLE_MIDI_OUT
//#define ENABLE_MIDI_OUT 

#define POT_NUM 3
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define I2S_BCLK_PIN    5       // I2S BIT CLOCK pin (BCL BCK CLK)
#define I2S_DOUT_PIN    6       // to I2S DATA IN pin (DIN D DAT)
#define I2S_WCLK_PIN    7       // I2S WORD CLOCK pin (WCK WCL LCK)
/* CUT / RES / DECAY. On the ESP32-S3 ADC1 covers GPIO1-10, so that is the whole legal
 * range for a potentiometer here; the rest of it is left free for a fourth knob.
 *
 * The previous {15,16,17} sat entirely on ADC2 -- which is exactly where pads 7-13 now are
 * (PIN_PLAN.md section 6). Keeping it would have put the pots and half the control surface
 * on the same GPIOs. */
const uint8_t POT_PINS[POT_NUM] = {1, 2, 4};
#elif defined(CONFIG_IDF_TARGET_ESP32)
#define I2S_BCLK_PIN    5       // I2S BIT CLOCK pin (BCL BCK CLK)
#define I2S_WCLK_PIN    19      // I2S WORD CLOCK pin (WCK WCL LCK)
#define I2S_DOUT_PIN    18      // to I2S DATA IN pin (DIN D DAT)
const uint8_t POT_PINS[POT_NUM] = {34, 35, 36};
#endif

/* M3 PADS -- the 16-pad control surface. ONE PIN PER PAD.
 *
 * An earlier plan used a 4x4 matrix, on the belief that 16 direct pads did not fit (27
 * needed, 27 available, zero spare). That arithmetic was the only reason. GPIO0, GPIO3 and
 * GPIO45 had been held back out of caution rather than from the datasheet, and ESP32-S3
 * Table 3-1 / 3-4 show all three are usable as switch-to-ground inputs. That is three pins
 * of margin, so: 28 assigned against a pool of 30, two spare, and pads_m3.ino needed no
 * change at all.
 *
 * PIN_PLAN.md IS THE AUTHORITY for this table -- the pool, the reserve list, the chord table
 * and the PCB rules are all there with their sources, and tools/test-pinplan.py checks the
 * arithmetic against the document. This comment is the short version; do not derive numbers
 * from it.
 *
 * WHY V5's OWN ARRAY COULD NOT BE COPIED. V5 runs on an RP2040 whose PAD_PINS are GPIOs
 * 0-22 (Acid_Drip_Drum_Acid_Drift_V5.ino:477-480); on this S3 those same numbers are:
 *
 *     1, 2, 4      POT_PINS -- ADC1
 *     5, 6, 7      I2S_BCLK / I2S_DOUT / I2S_WCLK -- the audio path
 *     18, 21       MIDITX / MIDIRX -- UART1
 *     38, 39, 40, 41  TFT SCK / MOSI / CS / DC -- Phase 4, nothing wired to them yet
 *     19, 20       native USB pair -- the only USB connector this board has
 *     26-37        OPI PSRAM, soldered inside the WROOM module
 *     47, 48       1.8V domain on a `V`-suffix module, so deliberately left spare
 *
 * A collision here would surface as noise on the DAC, not as a compile error.
 *
 * THE THREE STRAPPING PINS ARE USED: GPIO0 on pad 3, GPIO3 on pad 4, GPIO45 on pad 5. Each
 * one's datasheet default is preserved by switch-to-ground (PIN_PLAN.md 2.5 carries the
 * tables). Three PCB rules travel with them and are NOT visible from this file:
 *
 *     - GPIO45's net must carry NO pull-up. Pulled high, the chip looks for 3.3V flash on a
 *       1.8V rail and does not boot at all.
 *     - GPIO3 and GPIO0 want a 10k pull-up to 3V3. GPIO3's default is "Floating", i.e. no
 *       default bit value at all.
 *     - Every pad is switch-to-ground with INPUT_PULLUP. That arrangement is what makes the
 *       strapping pins safe, so it is a premise of the plan, not a preference.
 *
 * Pads 3, 4 and 5 were picked for those three pins because they are the only pads in no
 * chord at all -- 3, 4, 5 and 6 (PIN_PLAN.md 5.3) -- so holding a gesture can never pull a
 * strapping pin low at reset. Holding pad 3 at power-up does enter the ROM download mode; it
 * is recoverable by releasing and pressing RST, and it is the one thing here that has to be
 * tried on hardware rather than reasoned about.
 *
 * WHAT IS AND IS NOT KNOWN ABOUT THE WIRING. Pads 14, 15 and 16 are GPIO42/43/44, which the
 * CURRENT dev board does not break out -- HARDWARE_SETUP.md section 4 proved UART0 is not
 * routed to the only connector, by three builds that printed nothing. An unconnected
 * INPUT_PULLUP pin reads high, so those three pads are silently dead here: no crash, no
 * diagnostic, they simply never fire. They come alive on the custom PCB this plan is written
 * for. The previous table put pads 13-16 on 38-41, which are the TFT pins, so those three
 * were not functional either -- this is a lateral move, not a regression.
 *
 * Two pads that are already claimed by the port and must not be moved without also updating
 * the tests: PAD_FUNC_A/B are indices 6 and 7, i.e. physical pads 7 and 8, which is V5's
 * FUNC chord (:484-485), and the test compares those two #defines against V5's.
 */
#define NUM_PADS 16
const uint8_t PAD_PINS[NUM_PADS] = {
  8, 9, 0, 3, 45, 10, 11, 12,    // pads  1-8
  13, 14, 15, 16, 17, 42, 43, 44 // pads  9-16
};

/* Debounce window, in ms. The scheme is V5's shape: any raw edge restarts the timer, and
 * the pad only changes state once the level has held for longer than this. 20 ms is the
 * usual figure for a tactile switch, and it has NOT been measured on these contacts --
 * see the note in pads_m3.ino about what is and is not evidence here. */
#define PAD_DEBOUNCE_MS 20

/* Chord window, in ms. V5 compares `now - pDown[other] < 200` at :5721-5722, so this is
 * V5's number rather than a fresh choice: the same two pads inside the same window has to
 * mean the same gesture, or the port's FUNC chord is not V5's FUNC chord. */
#define CHORD_WINDOW_MS 200

/* Long-press threshold, in ms. V5 spells it LG and gives it 500 (:1333), used in four
 * places: the PLAY chord's factory reset (:5872), the normal accent/glide cycle (:6199), the
 * note-edit engage window (:6208, 200 ms there and not this one) and the FX mode's
 * clear-all-effects (:6161). The port uses it for the first two; the other two need M4's
 * patch slots and pot reading.
 *
 * Named LONG_PRESS_MS rather than LG because a two-letter name in a header that also defines
 * PAD_PINS is a name somebody will read as "left gesture" in six months. */
#define LONG_PRESS_MS 500

#define PAD_PLAY_A  0   // pads 1+2 = PLAY/STOP  (V5 :482)
#define PAD_PLAY_B  1
#define PAD_FUNC_A  6   // pads 7+8 = FUNC toggle (V5 :484)
#define PAD_FUNC_B  7
#define MIX_PAD_A  14   // pads 15+16 = MIX EDIT chord (V5 :5711-5719)
#define MIX_PAD_B  15
#define BM_SW_A     8   // pads 9+10 = CH2 EDIT chord (V5 :5765-5793)
#define BM_SW_B     9

/* M0 diagnostics */
#define M0_DIAG 0     // the M0 diagnostic block. Off now: it has done its job and it is
                     // not shippable, because mode 2 writes a known-silent buffer to
                     // the DAC for a third of every cycle. Set back to 1 to re-enable
                     // everything below if the noise ever comes back -- the counters,
                     // the per-bus peaks and the three test modes are all still here,
                     // guarded by this one switch and by nothing else.
#define M0_DIAG_MS 500   // report window in ms: peaks are measured and printed once per
                        // this many.
                        // Cycles on its own because this board (ESP32-S3-WROOM) exposes
                        // no spare pin to press: GPIO23 exists on the chip and
                        // init_button() puts it in INPUT_PULLUP, but here it reads LOW
                        // with nothing attached and is not broken out.
#define M0_DIAG_MODE_MS 3000  // how long each test mode holds before advancing. Longer
                        // than the report window on purpose: mode 2 writes a known
                        // silent buffer to the DAC, and whether the amplifier is still
                        // noisy with it is a question only the operator can answer, so
                        // it has to be long enough to hear rather than merely log.
                        // The report stays at M0_DIAG_MS either way, so this just
                        // yields six lines per mode instead of one.
#if M0_DIAG
  // Which experiment is running, cycled from regular_checks() and read by mixer() and
  // by i2s_output(), so it is declared here rather than in AcidBox.ino.
  //
  //   0  normal
  //   1  reverb bypassed      is the reverb the noise?
  //   2  out_buf forced to 0  is the noise in the digital signal at all, or does it
  //                          survive a buffer we know is silent? That separates the
  //                          I2S write and the DAC module from everything upstream.
  extern volatile uint8_t m0Mode;
  extern volatile uint32_t m0MixerCalls;   // proves the audio task is actually running

  // Note-ons and note-offs per synth instrument, incremented in AcidBanger.ino.
  // A synth whose on-count climbs while its off-count does not is holding a voice
  // that will never decay, which is a continuous-noise source on its own.
  extern volatile uint32_t m0NoteOn[2];
  extern volatile uint32_t m0NoteOff[2];

  // Bytes I2S.write() refused. A short write means the audio task outran the DMA and
  // the ring buffer underran, which is the one remaining mechanism that produces
  // continuous noise while every digital sample in this program is provably correct.
  extern volatile uint32_t m0ShortWrites;
  extern volatile uint32_t m0ShortBytes;
  extern volatile uint32_t m0I2SCalls;

  // The two conditions sampler Process() guards against used to be declared here and
  // defined in AcidBox.ino, counted only under M0_DIAG. They now live on the Sampler
  // itself as oobSample / oobCache and are counted unconditionally, because the split is
  // the entire value: the first condition is a 1-2 frame tail overshoot and the second
  // is the cursor leaving RamCache, and a counter that only exists in the diagnostic
  // build cannot keep them apart in the build that ships. Read them through
  // Drums.GetOobSample() / Drums.GetOobCache().

  // Every I2S.write() in i2s_setup.ino goes through this, so the return value is
  // inspected in exactly one place. I2S.write() is documented to return the number of
  // bytes accepted in core 3.x and its signature has changed across versions; if that
  // turns out not to hold, this one macro is what has to change rather than the two
  // call sites and their surroundings. Plain stores only, no printing: this is IRAM.
  #define M0_I2S_WRITE() { \
    size_t m0w_ = I2S.write((uint8_t*)out_buf[current_out_buf]._signed, \
                            sizeof(out_buf[current_out_buf]._signed)); \
    if ( m0w_ < sizeof(out_buf[current_out_buf]._signed) ) { \
      m0ShortWrites++; \
      m0ShortBytes += (uint32_t)(sizeof(out_buf[current_out_buf]._signed) - m0w_); \
    } }
#else
  // With the diagnostics off, the write expands to exactly the upstream call, byte for
  // byte, so the shipping audio path carries no trace of the investigation: no extra
  // counter, no branch, no discarded return value. The macro stays defined because
  // i2s_setup.ino calls it unconditionally, and the point of routing both call sites
  // through it was that re-enabling M0_DIAG has to be a one-token change here rather
  // than an edit inside the I2S output path.
  #define M0_I2S_WRITE() { \
    I2S.write((uint8_t*)out_buf[current_out_buf]._signed, \
              sizeof(out_buf[current_out_buf]._signed)); }
#endif

/* SEQ_FX_SELFTEST: does the M2.5 step-effect machinery run, on this hardware, in this build?
 *
 * WHY A SEPARATE BUILD EXISTS AT ALL, since this is the same shape as M0_DIAG and
 * BENCH_AUDIO_HEADROOM above and the reason is worth stating once rather than three times.
 *
 * All eight of the ported presets use SEQ_FX_NONE on every step -- verified field by field
 * against V5, which has the same all-zero effect column. So in a normal build the sub-step
 * scheduler is never reached, and the acceptance criterion "the eight effects behave as V5's
 * do" cannot be checked by listening or by log at all. Not "hard to check": there is no
 * reachable path to it.
 *
 * That leaves three things worth verifying, and only one of them is arithmetic:
 *
 *   1. The resolution arithmetic -- which pitch each effect produces, and which sub-offsets
 *      each timed effect fires at. Pure arithmetic, checked offline in
 *      tools/test-sequencer.py and printed on the device at seq_start() by seq_fxDump(). Both
 *      are always on; no build flag involved.
 *
 *   2. That the scheduler is actually WIRED IN. The failure mode is not subtle in principle
 *      and completely invisible in practice: forget the seq_subPoll() call in seq_poll() and
 *      the drift report stays flawless, the gate depth stays at 1, the queue never overflows,
 *      and there is simply no retrigger. Nothing in the log says a hit went missing, because
 *      the thing that would have said it is the thing that did not run.
 *
 *   3. That a sub-hit DRAINS before its note-on. synthvoice.ino decides legato from held-note
 *      depth (synthvoice.ino:225), so a sub-hit that posted a bare note-on would leave depth at
 *      2, arm CC 65, and stop retriggering the envelope -- the exact degradation recorded in
 *      HANDOFF 6.4.2, reached from the opposite direction. It is observable only as
 *      `held a/s 2/0` on a report line, and only while a sub-hit is actually firing.
 *
 * Items 2 and 3 are the two this project has already been bitten by once each, in the two
 * ways it has already been bitten: the f464c01 compile failure, where every offline check was
 * modelling ALGORITHM while the shipped file could not build, and the mean-report overflow,
 * where a plausible constant read as a stable clock. Five green checks prove the thing you
 * checked, not the thing that ships. So items 2 and 3 get a build that actually runs the
 * code, read off the wire, and the switch goes back to 0.
 *
 * NOT DEFINED HERE ON PURPOSE. It is defined near the top of this file, at the acceptance
 * block, so that the switch being ON is the first thing anyone reading config.h sees and not
 * the fortieth. Two definitions of the same macro is not a warning in C, it is a silent
 * redefinition that takes whichever the preprocessor saw last -- which is a claim with no
 * diagnostic attached, in a file whose entire purpose is to make claims checkable.
 *
 * tools/test-sequencer.py asserts there is exactly one definition, and that it is 0 for a
 * shipping build.
 */

/* BENCH_AUDIO_HEADROOM: how much of each buffer period does the audio actually cost? */
#define BENCH_AUDIO_HEADROOM 0  // Measured, four times, then switched off.
                        //
                        // It was built because the priority 1 -> 5 change that fixed the
                        // starving loop() was a guess, and guessing is what cost M0 a
                        // full round of bench diagnosis. Measure, then build.
                        //
                        // What "headroom" means here, precisely, because the obvious
                        // reading is the wrong one: audio_task1 is pinned to core 0 at
                        // priority 5 and spins on taskYIELD(), so a task-utilisation
                        // reading would just say core 0 is 100% busy and tell us
                        // nothing. The spin is not the cost. What matters is the time
                        // spent doing work per buffer against the 725 us the DMA gives
                        // it -- because when the work exceeds the period the buffer is
                        // not refilled in time, the DMA underruns, and the DAC holds its
                        // last sample. That is the noise M0 just spent a milestone on.
                        //
                        // The answer, across 29 + 65 + 53 + 100 windows:
                        //   mean cpu      594.6 us = 82.0%  (median 82.3%, 77.0-84.7%)
                        //   worst cpu     799 us    = 110%  in the worst single window
                        //   SHORTWRITES   0 in every window of every run
                        //   overruns      0.023% of buffers -- a budget warning, repaid
                        //                 by slack in later buffers, NOT a fault
                        //   fill          17-20 us = 2.6%, so do not go optimising the
                        //                 float->int16 loop
                        // Headroom for M3: ~111 us/buffer off the worst window mean
                        // observed. The M3 target of 10% (72 us) leaves 30-40 us spare.
                        //
                        // Two things this thing taught that are worth more than the
                        // number, and both are why it is off now:
                        //
                        // 1. Boot-to-boot noise on this measurement is about +/-3
                        //    percentage points. Two boots of the SAME binary measured
                        //    79.93% and 82.71%. So a single run's mean is not a result
                        //    and must never be compared against another single run.
                        //    This is what made a 5-point "regression" look real.
                        // 2. SHORTWRITES is the only direct dropout measurement. The
                        //    overruns number is a budget warning and was briefly
                        //    mislabelled as a fault; see HANDOFF.md 6.1.
                        //
                        // Everything stays behind this one switch, so it comes straight
                        // back if M3 needs re-measuring after the TFT work lands on
                        // core 0 -- and it should, because TFT is exactly the kind of
                        // change that moves this number.
#define BAH_MS 1000        // report window. Long enough that the worst buffer in it is
                        // a fair sample of the worst, short enough to watch a break or
                        // a fill arrive.
#if BENCH_AUDIO_HEADROOM
  // Written by audio_task1 (IRAM, plain stores only, never printed from there) and
  // read and cleared by regular_checks() in normal task context on core 1.
  //
  // bahMaxGenMixUs / bahMaxFillUs are kept apart on purpose: once M1 lands extra
  // engines, "we are at 80%" is useless, but "the generator side went from 200 us to
  // 600 us" tells you exactly where to look.
  extern volatile uint32_t bahMaxGenMixUs;   // worst buffer: synth1+synth2+drums+mixer
  extern volatile uint32_t bahMaxFillUs;     // worst buffer: float -> int16 conversion
  extern volatile uint32_t bahMaxBlockUs;    // worst buffer: time blocked inside I2S.write()
  extern volatile uint32_t bahMaxCpuUs;      // worst buffer: genMix + fill, the real cost
  extern volatile uint32_t bahSumCpuUs;      // total over the window, for the mean
  extern volatile uint32_t bahCount;         // buffers in the window
  extern volatile uint32_t bahOverruns;      // buffers whose CPU cost exceeded the period
  extern volatile uint32_t bahFillUs;        // hand-off from i2s_output() to audio_task1
  extern volatile uint32_t bahBlockUs;
  // bahShortWrites is the only DIRECT underrun measurement here, and the one that
  // matters. I2S.write() returns how many bytes it accepted; anything less than a whole
  // buffer means the DMA was not handed its data in time, i.e. audio was genuinely
  // dropped. Everything else in this file is inference from timings; this is the API
  // telling us outright.
  //
  // It is here, and not only under M0_DIAG, because bahOverruns -- which is what the
  // first measurement round was judged on -- turns out NOT to be a fault signal. A
  // buffer costing more than the period is repaid by the slack in the buffers after it,
  // so overruns > 0 is a budget warning, not proof of a dropout. The first bench run
  // (2026-10-02) logged 11 overruns in 29 windows and the noise stayed fixed, which is
  // exactly the distinction that needed proving.
  extern volatile uint32_t bahShortWrites;   // cumulative, NOT reset per window
  extern volatile uint32_t bahShortBytes;
  // bahMinBlockUs: the tail of the backpressure. Steady state the task works ~594 us,
  // then blocks ~131 us waiting for the DMA to free a slot, so this normally sits near
  // that. A reading of 0 means I2S.write() returned without ever waiting, i.e. there was
  // no slack left at that instant. Still inference rather than proof, which is why it is
  // reported next to the short-write count and not instead of it.
  extern volatile uint32_t bahMinBlockUs;
#endif

float bpm = 130.0f;

#ifdef USE_INTERNAL_DAC
#define SAMPLE_RATE     22050   // price for increasing this value having NO_PSRAM is less delay time, you won't hear the difference at 8bit/sample
#else
#define SAMPLE_RATE     44100   // 44100 seems to be the right value, 48000 is also OK. Other values haven't been tested.
#endif

const float DIV_SAMPLE_RATE = 1.0f / (float)SAMPLE_RATE;
const float DIV_2SAMPLE_RATE = 0.5f / (float)SAMPLE_RATE;
const float TWO_DIV_16383 = 1.22077763e-04f;
const float MS_TO_S = 0.001f;

#define TABLE_BIT  		        10UL				// bits per index of lookup tables for waveforms, exp(), sin(), cos() etc. 10 bit means 2^10 = 1024 samples
#define TABLE_SIZE            (1<<TABLE_BIT)        // samples used for lookup tables (it works pretty well down to 32 samples due to linear approximation, so listen and free some memory at your choice)
#define TABLE_MASK  	        (TABLE_SIZE-1)        // strip MSB's and remain within our desired range of TABLE_SIZE
#define CYCLE_INDEX(i)        (((int32_t)(i)) & TABLE_MASK ) // this way we can operate with periodic functions or waveforms without phase-reset ("if's" are pretty costly in the matter of time)

const float DIV_TABLE_SIZE =  1.0f / (float)TABLE_SIZE;
const int HALF_TABLE =  TABLE_SIZE/2;


// illinear shaper, choose preferred parameters basing on your audial experience
#define SHAPER_USE_TANH             // use tanh() function to introduce illeniarity into the filter and compressor, it won't impact performance as this will be pre-calculated 
//#define SHAPER_USE_CUBIC              // use the cubic curve to introduce illeniarity into the filter and compressor, it won't impact performance as this will be pre-calculated

// curve will be pre-calculated within -X..X range, outside this interval the function is assumed to be flat
#define SHAPER_LOOKUP_MAX 5.0f        // maximum X argument value for tanh(X) lookup table, tanh(X)~=1 if X>4 
const float SHAPER_LOOKUP_COEF = (float)TABLE_SIZE / SHAPER_LOOKUP_MAX;
#define DMA_BUF_LEN     32          // there should be no problems with low values, down to 32 samples, 64 seems to be OK with some extra
#define DMA_NUM_BUF     2           // I see no reasom to set more than 2 DMA buffers, but...

const uint32_t DMA_BUF_TIME = (uint32_t)(1000000.0f / (float)SAMPLE_RATE * (float)DMA_BUF_LEN); // microseconds per buffer, used for debugging output of time-slots

#define SYNTH1_MIDI_CHAN        1
#define SYNTH2_MIDI_CHAN        2

#define DRUM_MIDI_CHAN          10

const float TWOPI = PI*2.0f;
const float MIDI_NORM = 1.0f/127.0f;
const float ONE_DIV_PI = 1.0f/PI;
const float ONE_DIV_TWOPI = 1.0f/TWOPI;

const float  PI_DIV_TWO     =       HALF_PI;
const float  NORM_RADIANS = ONE_DIV_TWOPI * TABLE_SIZE;

#define FORMAT_LITTLEFS_IF_FAILED true

#define GROUP_HATS  // if so, instruments CH_NUMBER and OH_NUMBER will terminate each other (sampler module)
#define CH_NUMBER  6 // closed hat instrument number in kit (for groupping, zero-based)
#define OH_NUMBER  7 // open hat instrument number in kit (for groupping, zero-based)

#ifdef NO_PSRAM
  #define RAM_SAMPLER_CACHE  40000    // bytes, compact sample set is 132kB, first 8 samples is ~38kB
  #define SAMPLER_CACHE_SIZE RAM_SAMPLER_CACHE // M0: one name for the real size of RamCache, so the
                                      // bounds checks in sampler.ino work in both configurations
  #define DEFAULT_DRUMKIT 4           // /data/4/ folder
  #define SAMPLECNT       8           // how many samples we prepare (here just 8)
#else
  #define PRELOAD_ALL                 // allows operating all the samples in realtime, requires more time to start, recommended for OPI PSRAM of ESP32S3
  #define PSRAM_SAMPLER_CACHE 3145728 // bytes, we are going to preload ALL the samples from FLASH to PSRAM
  #define SAMPLER_CACHE_SIZE PSRAM_SAMPLER_CACHE // M0: see above
                                      // we divide samples by octaves to use modifiers to particular instruments, not just note numbers
                                      // i.e. we know that all the "C" notes in all octaves are bass drums, and CC_808_BD_TONE affects all BD's
  #define SAMPLECNT       (7 * 12)    // how many samples we prepare (8 octaves by 12 samples)
  #define DEFAULT_DRUMKIT 0           // in my /data /0 has a massive bassdrum , /6 = 808 samples
#endif

#define TINY 1e-32;

#ifndef LED_BUILTIN
#define LED_BUILTIN 0
#endif

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

#if (defined ARDUINO_LOLIN_S3_PRO)
#undef BOARD_HAS_UART_CHIP
#endif

#if (defined BOARD_HAS_UART_CHIP)
  #define MIDI_PORT_TYPE HardwareSerial
  #define MIDI_PORT Serial
  // M0: pinned to Serial0 rather than to Serial. The core defines Serial from
  // USBMode (HardwareSerial.h:444/447/451 in core 3.3.12), so DEBUG_PORT = Serial
  // silently became HWCDCSerial under USBMode=hwcdc and the log vanished while
  // the IDF console on UART0 carried on printing. Naming UART0 directly makes the
  // log path independent of that menu option.
  #define DEBUG_PORT Serial0
#else
  #if (ESP_ARDUINO_VERSION_MAJOR < 3)
    #define MIDI_PORT_TYPE HWCDC
    #define MIDI_PORT USBSerial
    #define DEBUG_PORT USBSerial
  #else
    #define MIDI_PORT_TYPE HardwareSerial
    #define MIDI_PORT Serial
    #define DEBUG_PORT Serial
  #endif
#endif

// M0: was `#ifdef MIDI_VIA_SERIAL || MIDI_USB_DEVICE`. `#ifdef` takes exactly
// one identifier; GCC ignores the rest with "extra tokens at end of #ifdef
// directive", so this only ever tested MIDI_VIA_SERIAL. Since MIDI_VIA_SERIAL
// is disabled by default, DEBUG_ON survived even with MIDI_USB_DEVICE enabled.
// Rewritten in the same style as the guard in AcidBox.ino:36.
//
// Left as-is. With both MIDI_USB_DEVICE and MIDI_VIA_SERIAL currently off this is
// inert, which is why enabling DEBUG_ON above works without editing this block.
// Re-enabling USB MIDI will silence the log again -- that is the intended
// behaviour of the author's guard, not a bug to work around.
#if defined(MIDI_VIA_SERIAL) || defined(MIDI_USB_DEVICE)
  #undef DEBUG_ON
#endif

// M0: the debug log goes to the USB-OTG connector, not UART0.
//
// Evidence, from the S3 sdkconfig this core is built with:
//   CONFIG_ESP_CONSOLE_UART_DEFAULT 1
//   CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG 1
// The console mirrors onto USB-Serial-JTAG, which is why the
// heap_caps_print_heap_info dumps out of Sampler::Init were visible on the
// USB-OTG cable while DEBUG_PORT on UART0 produced nothing at all. UART0 is not
// wired to the connector this board exposes. Reading the visible heap dumps as
// evidence that UART0 worked was the wrong inference, and cost two builds.
//
// HWCDCSerial is named directly rather than going through Serial, because the core
// defines Serial from USBMode (HardwareSerial.h:444) and that is how the log
// ended up pointed at a port that was never started. The guard matches the one
// HWCDC.h uses on its own class definition, so this falls back to Serial0 if the
// CDC is not enabled rather than failing to compile.
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  #undef DEBUG_PORT
  #define DEBUG_PORT HWCDCSerial
#endif

// debug macros
#ifdef DEBUG_ON
  #define DEB(...)    DEBUG_PORT.print(__VA_ARGS__) 
  #define DEBF(...)   DEBUG_PORT.printf(__VA_ARGS__)
  #define DEBUG(...)  DEBUG_PORT.println(__VA_ARGS__)
#else
  #define DEB(...)
  #define DEBF(...)
  #define DEBUG(...)
#endif


// normalizing matrices for TB filter and distortion/overdrive pairs
#define NORM1_DEPTH 1.0f 
#define NORM2_DEPTH 1.0f

/* 
const float cutoff_reso[16][16] = { // flat EQ linear amplitude
{4.8385, 4.88747, 4.92047, 4.76436, 4.8962, 5.01568, 4.98983, 5.01559, 5.10654, 5.03281, 5.01903, 4.95362, 4.81538, 4.8074, 4.74791, 4.54329},
{3.87221, 3.90254, 3.82171, 3.75677, 3.7406, 3.67065, 3.69104, 3.56865, 3.54271, 3.67638, 3.65262, 3.57059, 3.58953, 3.51315, 3.45127, 3.37038},
{3.1284, 3.14124, 3.14524, 3.09576, 3.02473, 3.06767, 3.04812, 3.06489, 3.0655, 2.99194, 2.95566, 2.83795, 2.68933, 2.75138, 2.64402, 2.47201},
{2.92398, 2.76649, 2.72365, 2.67662, 2.59435, 2.57171, 2.63079, 2.59806, 2.50945, 2.50648, 2.49537, 2.44069, 2.38885, 2.29797, 2.17515, 2.04705},
{2.69233, 2.58547, 2.62899, 2.56338, 2.56841, 2.48645, 2.40444, 2.36171, 2.26753, 2.17892, 2.22557, 2.13955, 2.05197, 1.9635, 1.877, 1.77883},
{2.49518, 2.41459, 2.35943, 2.48924, 2.4879, 2.30845, 2.28157, 2.2319, 2.19218, 2.12374, 2.04543, 1.96772, 1.87544, 1.72046, 1.71919, 1.58504},
{2.56721, 2.4846, 2.46367, 2.38851, 2.34092, 2.21967, 2.10531, 2.17264, 2.08794, 1.98999, 1.93158, 1.86221, 1.79165, 1.68922, 1.61215, 1.49858},
{2.44912, 2.41703, 2.38924, 2.60044, 2.47826, 2.26369, 2.26848, 2.08206, 2.01731, 1.95231, 1.78664, 1.81617, 1.68899, 1.58835, 1.48491, 1.39299},
{2.42586, 2.44112, 2.3695, 2.38807, 2.42516, 2.21484, 2.30564, 2.09487, 2.14824, 1.97235, 1.88533, 1.7607, 1.67901, 1.56326, 1.41019, 1.35799},
{2.51576, 2.53396, 2.47729, 2.66741, 2.33181, 2.21481, 2.31478, 1.98828, 2.12556, 1.97937, 1.8806, 1.76699, 1.70553, 1.57373, 1.48169, 1.35733},
{2.25554, 2.45139, 2.38947, 2.78224, 2.49177, 2.3555, 2.46807, 2.16115, 2.14116, 1.98996, 1.89712, 1.7226, 1.68581, 1.60263, 1.49875, 1.35428},
{2.51794, 2.50148, 2.46082, 2.69584, 2.30117, 2.27949, 2.5582, 2.17867, 2.31131, 2.21329, 2.02697, 1.9131, 1.75641, 1.61857, 1.52771, 1.35415},
{2.46675, 2.60235, 2.55729, 2.84951, 2.49979, 2.33553, 2.49566, 2.21991, 2.20328, 2.13907, 2.08274, 1.94865, 1.87065, 1.78613, 1.61446, 1.47914},
{2.40512, 2.40291, 2.40859, 2.97096, 2.52717, 2.39973, 2.88218, 2.37344, 2.43893, 2.30513, 2.12342, 1.99408, 1.90687, 1.7411, 1.70994, 1.5803},
{2.54366, 2.64905, 2.52548, 2.75611, 2.52512, 2.28283, 2.65487, 2.36714, 2.51868, 2.44883, 2.36448, 2.20553, 2.13651, 1.99002, 1.77779, 1.66373},
{2.43544, 2.58627, 2.48965, 3.20733, 2.63355, 2.50921, 2.83243, 2.43752, 2.50693, 2.39616, 2.26776, 2.28478, 2.22265, 2.13063, 2.08305, 2.01791}
};
const float cutoff_reso_avg = 2.506875f;


const float wfolder_overdrive[16][16] = { // flat EQ linear amplitude
{1.81553, 2.92774, 4.06149, 5.07091, 6.26805, 7.34979, 8.18204, 8.78785, 9.45271, 10.05631, 10.65088, 11.09989, 11.4481, 11.92296, 12.23205, 12.29738},
{2.6708, 4.41444, 5.95289, 7.47505, 8.39286, 9.33202, 10.16724, 10.7217, 11.29222, 12.31747, 12.93994, 13.2008, 13.66097, 14.02695, 14.27628, 14.42418},
{3.38126, 5.6452, 7.62784, 9.13709, 10.18128, 11.301, 12.05577, 12.97395, 13.60086, 14.02923, 14.41857, 14.53039, 14.12202, 14.99198, 14.77959, 14.68183},
{4.37828, 7.03035, 9.03751, 10.54589, 11.27255, 12.1775, 13.68013, 14.3049, 14.23863, 14.48288, 14.76953, 14.95656, 14.62193, 14.64976, 14.68791, 14.60861},
{6.15714, 9.96185, 13.47305, 14.8845, 14.73223, 14.25685, 14.59641, 15.15803, 15.03315, 13.94896, 14.08422, 13.62272, 13.85014, 14.56888, 15.05049, 14.73128},
{10.18297, 16.0993, 20.0271, 17.58701, 13.204, 9.45962, 11.62678, 15.84615, 18.96596, 17.04844, 14.1457, 11.65228, 12.7452, 15.39917, 18.01944, 16.55685},
{12.50609, 20.47274, 23.53324, 16.68068, 9.79378, 6.98393, 12.2715, 20.1654, 22.88357, 16.51044, 10.072, 7.87354, 13.03754, 20.20183, 22.35985, 16.68395},
{13.70303, 22.44605, 24.62353, 16.09332, 8.1773, 5.98764, 13.66198, 22.47247, 24.37917, 16.06878, 7.99532, 6.49434, 13.79997, 22.42913, 23.63804, 15.76187},
{14.30539, 23.34099, 25.17157, 15.41343, 7.29526, 5.57348, 13.81831, 23.37496, 25.28368, 15.90457, 7.36298, 6.03334, 14.22995, 23.69467, 23.89587, 15.23157},
{14.54421, 23.93977, 25.9227, 15.9401, 6.95805, 5.33766, 14.02165, 23.19047, 25.30687, 15.63357, 6.78814, 5.57623, 14.38434, 24.30402, 25.2735, 15.50198},
{14.32658, 24.08095, 26.12392, 15.84031, 6.72403, 5.14575, 14.19205, 24.49798, 25.84027, 15.76886, 6.62274, 5.35413, 13.98917, 24.50435, 25.33603, 15.37113},
{15.00247, 24.4431, 26.46044, 15.87536, 6.35671, 4.90051, 14.08632, 24.45546, 25.82815, 15.72268, 6.39308, 5.25604, 14.5012, 24.96537, 25.79327, 15.12743},
{14.97389, 24.50721, 26.50383, 15.8626, 6.40081, 4.9022, 14.26976, 24.82402, 25.15142, 15.14676, 6.22857, 5.13013, 14.37914, 24.82676, 25.89133, 15.38875},
{15.2131, 23.8389, 26.23546, 15.77076, 6.33121, 4.79957, 14.31421, 25.04027, 26.28032, 15.6678, 6.1222, 5.11645, 14.19984, 24.30534, 25.7695, 15.35306},
{15.17793, 24.84065, 26.53333, 15.91002, 6.31883, 4.62659, 13.85447, 24.78054, 25.97502, 15.46608, 6.03461, 5.06064, 14.63126, 25.18358, 26.04326, 15.42947},
{15.16892, 24.67783, 26.40122, 15.80733, 6.27843, 4.76886, 14.31771, 25.04004, 26.3323, 15.22194, 5.83839, 4.99684, 14.46052, 25.16456, 25.8343, 15.38862}
};
const float wfolder_overdrive_avg = 14.70303f;
*/
/*
const float cutoff_reso[16][16] = { // D-weighting curve linear amplitude
{5.088443, 4.748775, 4.213981, 3.806754, 3.814063, 3.891116, 3.607284, 3.705055, 4.047698, 3.973349, 4.032753, 4.297999, 4.399147, 4.164255, 4.265074, 4.560594},
{5.480893, 4.635213, 4.270601, 4.014409, 3.723213, 3.648567, 3.852473, 3.880512, 3.952992, 4.258348, 4.468791, 4.430753, 4.682340, 4.780585, 4.870942, 4.982014},
{5.801386, 4.982126, 4.173073, 3.927025, 3.821813, 3.830817, 3.813940, 4.052980, 4.182085, 4.149244, 4.385879, 4.558488, 4.619930, 4.595871, 4.747104, 5.007563},
{5.914363, 4.792323, 4.326102, 4.013857, 3.716824, 3.755425, 4.281695, 4.412211, 4.448431, 4.943832, 5.413869, 5.404469, 5.278687, 6.044177, 6.174179, 5.860119},
{6.225181, 5.118595, 4.179591, 3.819639, 3.778664, 3.988809, 3.826071, 4.089345, 4.387860, 4.304595, 4.444678, 4.724508, 4.909694, 4.960629, 5.133572, 5.380276},
{6.104742, 4.897842, 4.145612, 3.776814, 3.430487, 3.493173, 3.673556, 3.560862, 3.767686, 4.088268, 4.325448, 4.266427, 4.534279, 5.001377, 4.838728, 5.116366},
{6.388631, 5.000104, 3.923562, 3.373810, 3.552555, 3.384681, 3.475560, 3.943514, 4.346936, 4.514819, 4.676305, 5.487291, 5.773126, 5.864404, 6.348352, 6.885350},
{6.318010, 4.543581, 3.810727, 3.485013, 3.260142, 3.493744, 3.691319, 3.750343, 4.004967, 4.416778, 4.711856, 5.045936, 5.606477, 6.243788, 6.224719, 6.857349},
{6.351570, 4.719615, 3.677153, 3.361043, 3.569208, 3.525641, 3.911758, 4.485856, 4.996965, 5.380759, 5.652077, 6.842835, 7.078077, 7.665046, 8.502805, 9.156425},
{6.273955, 4.355492, 3.707555, 3.483126, 3.521939, 3.849478, 4.470348, 4.593672, 5.145930, 6.276892, 6.837686, 7.431805, 8.649154, 9.819227, 9.916071, 10.709123},
{6.466042, 4.654976, 3.505829, 3.753159, 3.846766, 4.059497, 4.547558, 5.086736, 5.859083, 6.392162, 7.265354, 8.778674, 8.991282, 10.449076, 11.504684, 12.867598},
{6.271001, 4.340880, 3.825479, 3.526718, 3.804931, 4.228046, 4.863353, 5.226099, 5.693886, 7.190523, 7.487588, 8.638903, 10.065550, 11.563904, 12.393667, 13.181049},
{6.692892, 4.329665, 3.611883, 3.737395, 3.935531, 4.151097, 4.419905, 5.393781, 5.887227, 6.507381, 7.729293, 9.264614, 10.000851, 11.397189, 13.108201, 14.891965},
{6.165781, 4.303358, 3.679214, 3.498906, 3.569861, 3.808628, 4.288381, 4.573395, 5.305214, 6.277582, 6.924616, 8.224044, 9.240870, 11.207167, 12.074885, 13.946956},
{6.507094, 4.104322, 3.525125, 3.297218, 3.403118, 3.408006, 3.558969, 4.307840, 4.580469, 5.175146, 5.903279, 7.325158, 7.869888, 8.849345, 11.131460, 12.524242},
{6.205086, 4.138503, 3.464386, 2.883092, 2.711778, 3.014128, 3.131652, 3.280128, 3.670246, 4.381391, 4.794806, 5.374605, 6.395000, 7.877948, 8.369201, 10.447331}
};
const float cutoff_reso_avg = 5.4167266f;
*/

const float wfolder_overdrive[16][16] = { // D-weighting curve linear amplitude
{4.321596, 6.677420, 9.351027, 12.337818, 15.274008, 17.178272, 20.258532, 22.640339, 23.268341, 25.133560, 25.689850, 27.329815, 26.931023, 27.971588, 28.773928, 27.811522},
{6.072484, 10.221110, 14.169627, 17.745028, 20.698469, 24.349220, 25.056787, 26.135130, 27.644402, 29.212200, 28.163837, 28.859060, 30.591475, 30.274736, 30.926729, 32.161110},
{8.636417, 13.038910, 17.829748, 23.371164, 25.444685, 26.327213, 28.255512, 29.594391, 29.098421, 29.936840, 31.134794, 32.169270, 32.045223, 32.963749, 32.976822, 32.455048},
{10.054599, 16.592342, 22.589405, 25.882214, 28.521395, 29.180340, 29.164886, 30.660505, 31.349100, 32.824554, 32.293663, 33.904400, 33.067791, 32.399799, 33.414825, 33.234741},
{15.011400, 23.227974, 31.648169, 35.058456, 32.518459, 30.586367, 30.851610, 33.365906, 34.632706, 35.548817, 35.342865, 33.191872, 33.392647, 35.636063, 37.346981, 36.876877},
{23.186680, 38.978333, 48.210861, 40.115742, 31.781157, 24.790371, 31.183672, 40.898441, 47.043743, 41.951683, 33.172577, 28.403805, 32.496960, 41.487301, 46.415443, 41.383484},
{29.932003, 47.438541, 56.828445, 41.501617, 26.867907, 20.738359, 32.649918, 49.848667, 54.199947, 42.713993, 27.205708, 22.805727, 34.331593, 49.077122, 54.067410, 40.862373},
{33.698540, 54.806038, 60.328083, 41.728024, 22.957489, 17.200394, 33.867798, 54.327579, 60.292271, 40.527672, 22.979492, 19.049171, 35.523216, 54.576229, 58.380924, 40.722004},
{35.641998, 57.294373, 64.910187, 41.493423, 20.014410, 14.921355, 34.554081, 58.559727, 62.141014, 41.429733, 19.964781, 16.598537, 35.357327, 58.005657, 62.420906, 40.056889},
{37.487209, 59.607437, 65.128052, 41.079487, 18.249022, 13.841405, 35.108574, 61.368694, 64.131561, 39.774368, 18.559950, 14.785675, 36.562847, 60.325825, 64.121376, 39.700741},
{37.041679, 60.903545, 66.883095, 41.005798, 17.416222, 13.260203, 36.083569, 60.961777, 64.818130, 40.642086, 17.360371, 14.153852, 36.164448, 62.356262, 64.683060, 39.244789},
{38.588360, 62.251549, 66.640533, 40.079689, 16.864496, 13.063670, 35.713974, 63.281395, 65.684242, 40.100368, 16.357441, 13.569439, 36.963696, 62.407402, 66.269562, 38.965614},
{38.136345, 62.866852, 66.513802, 40.857845, 16.487530, 12.740461, 35.939171, 62.047729, 66.705620, 39.801647, 16.169365, 13.296426, 37.040180, 63.597023, 64.751793, 38.943562},
{38.979004, 63.323586, 66.772980, 40.269608, 16.517096, 12.285855, 35.989716, 64.168343, 67.227440, 39.479458, 15.763931, 13.077268, 36.944458, 63.458538, 66.844772, 39.394936},
{38.541519, 62.366325, 66.909378, 40.739624, 16.021036, 12.396644, 36.157871, 63.634758, 66.528000, 39.438278, 15.723740, 12.900184, 37.455818, 63.688789, 65.662186, 39.418522},
{38.486912, 63.623055, 67.025940, 40.878666, 15.853469, 12.048793, 36.591278, 63.678566, 67.363892, 39.386097, 15.598418, 12.830324, 36.486755, 63.888874, 67.060234, 39.503799}
};
const float wfolder_overdrive_avg = 36.59637f;

float cutoff_reso[16][16] = { // k-weigted mean quad
{6.434804, 4.714645, 3.947374, 2.694166, 2.351397, 2.500912, 2.929582, 2.654394, 2.284407, 1.838856, 2.644853, 2.766961, 2.814959, 2.350692, 1.996572, 2.199751},
{6.612917, 5.302139, 3.893952, 2.907703, 2.001597, 2.857677, 2.988061, 3.030843, 2.442840, 2.147922, 2.721878, 2.790063, 2.963842, 2.972988, 2.489201, 2.509848},
{7.350744, 5.508491, 4.067600, 2.890480, 2.176190, 2.566737, 2.546578, 2.596522, 2.278280, 1.956051, 2.581862, 2.548600, 3.036592, 2.538826, 2.439919, 1.898532},
{8.183912, 5.427793, 4.194474, 2.903769, 2.359180, 2.242266, 2.286241, 3.119573, 3.544204, 3.191301, 2.518959, 2.913494, 4.572999, 5.861769, 4.722514, 3.665691},
{7.280022, 4.675759, 3.778736, 3.111181, 2.431638, 2.358587, 2.270216, 2.901183, 3.002531, 2.652288, 1.998837, 2.844437, 3.059708, 3.341244, 2.923321, 2.497309},
{6.960493, 4.442485, 3.891997, 3.066457, 2.455318, 1.855044, 2.421693, 2.687097, 2.769465, 2.300333, 2.023683, 2.392432, 2.613516, 2.961050, 2.950050, 2.489161},
{6.600060, 4.763449, 4.023485, 3.143195, 2.520189, 1.899933, 2.254261, 2.552054, 2.955580, 3.250866, 2.693074, 2.982439, 2.930238, 4.417704, 5.190308, 4.485150},
{5.806700, 5.164190, 3.994584, 3.393530, 2.460938, 2.048136, 2.080137, 2.109953, 2.448718, 2.548533, 2.194179, 2.070688, 3.086098, 4.052840, 4.070950, 3.569471},
{6.268085, 4.558399, 3.507618, 2.848771, 2.446060, 1.948153, 2.073992, 2.101269, 2.966322, 3.262701, 3.049813, 2.411024, 3.578253, 4.676841, 5.917085, 5.060090},
{6.759351, 4.216856, 3.118927, 2.793170, 2.484806, 2.031502, 1.679651, 2.196030, 2.861528, 3.392502, 3.039659, 2.682046, 3.355549, 3.709728, 4.872293, 5.756225},
{6.830225, 4.422283, 3.082938, 2.982032, 2.476420, 2.165540, 1.598023, 2.144882, 2.456352, 2.995122, 2.919489, 2.632485, 3.150194, 3.483125, 5.059742, 6.296432},
{7.835934, 3.491278, 3.291606, 2.806850, 2.564085, 2.023950, 1.767282, 1.784284, 1.954406, 2.394825, 2.952084, 2.615514, 2.811204, 3.472478, 5.163306, 6.343526},
{8.392389, 3.504093, 3.044849, 2.386751, 2.225955, 1.887471, 1.583561, 1.604031, 1.729157, 2.363731, 2.771977, 2.687573, 2.257057, 3.599182, 5.251308, 6.766300},
{8.595600, 3.864176, 2.557690, 1.968429, 1.866560, 1.764059, 1.478550, 1.335737, 1.606975, 2.199224, 2.646084, 2.557677, 2.242978, 3.037829, 3.686971, 5.391070},
{9.063289, 3.624348, 2.342058, 1.823246, 1.851653, 1.604091, 1.497541, 1.124157, 1.500818, 1.905059, 2.288400, 2.217282, 2.097532, 2.477182, 2.971320, 4.915813},
{9.551075, 3.699403, 1.972151, 1.721147, 1.624085, 1.514310, 1.259587, 1.062701, 1.170411, 1.331594, 1.649718, 2.004076, 1.807243, 2.236462, 2.674345, 4.867586},
};
const float cutoff_reso_avg = 3.19f;

/*
 float wfolder_overdrive[16][16] = { // k-weighted mean quad
{1.100069, 2.612626, 3.790118, 8.684330, 13.651937, 19.295862, 20.443464, 21.131121, 24.541002, 26.373375, 32.965790, 30.926220, 32.139011, 29.744080, 35.700516, 37.820103},
{2.634791, 5.464583, 9.096105, 15.762427, 21.788717, 26.992195, 29.971214, 28.517365, 30.403839, 33.226055, 39.615711, 34.655502, 39.475487, 34.761803, 40.932682, 41.794361},
{4.302970, 9.989371, 15.071154, 23.144535, 26.899090, 35.547665, 31.950285, 36.962143, 33.009865, 41.157677, 42.040821, 42.360878, 39.941982, 40.038044, 41.003162, 41.025238},
{5.831883, 14.910394, 21.974388, 28.007730, 33.020317, 39.290241, 35.500706, 39.943031, 36.429066, 42.511127, 42.573090, 41.172798, 38.672600, 38.115959, 38.328674, 38.419930},
{10.745278, 24.957157, 44.335899, 38.639091, 45.861202, 38.845505, 35.971352, 38.600151, 42.700455, 43.917099, 40.667526, 38.212841, 35.262058, 40.316406, 40.559158, 44.777973},
{21.580822, 52.749134, 76.591423, 48.345505, 33.019783, 23.045059, 26.508516, 49.446281, 60.178902, 46.360680, 31.648138, 27.352730, 26.257122, 51.410534, 54.258270, 49.397896},
{26.786173, 69.479034, 85.775749, 41.720451, 19.458248, 12.441231, 26.969410, 62.399029, 80.884926, 39.512096, 20.850580, 14.713130, 28.300602, 66.324745, 75.790932, 43.306309},
{29.055773, 74.197891, 87.453911, 35.326111, 11.756197, 7.153258, 29.866051, 70.803955, 92.299400, 34.215282, 12.947213, 9.325157, 29.023405, 75.806847, 81.992989, 37.460163},
{29.329697, 84.530838, 86.208633, 36.999393, 9.605332, 6.178853, 29.674660, 82.772247, 92.242180, 35.412563, 10.025507, 7.610517, 28.521023, 77.890633, 84.200706, 34.046375},
{30.291025, 88.884071, 89.562881, 36.682079, 7.239823, 5.217485, 29.093407, 82.466888, 90.862190, 33.188721, 8.243276, 5.953953, 31.746099, 77.590744, 95.853188, 32.116184},
{33.520702, 87.417305, 97.071663, 35.481602, 7.136596, 4.568522, 28.280918, 83.915421, 88.742973, 33.012093, 7.212764, 4.921861, 31.499189, 83.405777, 94.219452, 32.122295},
{32.871964, 85.562317, 94.751083, 34.303524, 6.551445, 4.023323, 27.467133, 92.656708, 86.027946, 35.518806, 6.443895, 4.631652, 30.416872, 87.334679, 92.183884, 32.532410},
{32.032509, 83.331741, 98.296570, 33.268421, 5.883899, 3.771136, 29.050014, 91.149338, 94.362953, 34.575657, 5.897426, 4.319282, 29.828932, 85.467506, 89.734940, 31.316317},
{31.227125, 81.005997, 104.792648, 31.702234, 6.083809, 3.492942, 29.658930, 89.070290, 94.944389, 33.290352, 5.556711, 4.071751, 28.984886, 91.882561, 86.876961, 33.439419},
{30.342409, 89.584282, 102.275230, 35.133881, 5.690284, 3.515618, 28.778267, 86.460449, 92.302498, 32.302811, 5.006345, 3.840481, 28.476063, 94.554810, 88.098221, 33.928406},
{30.110008, 88.525185, 99.403809, 34.329544, 5.596230, 3.220591, 28.182953, 84.471687, 102.308319, 31.231888, 5.534990, 3.560953, 31.054146, 92.313148, 95.137627, 32.929070},
};
const float wfolder_overdrive_avg = 41.225f;
*/

static const float tuning[128] = {
  0.500000f, 0.500000f, 0.500000f, 0.500000f, 0.500000f, 0.529732f, 0.529732f, 0.529732f, 
  0.529732f, 0.529732f, 0.561231f, 0.561231f, 0.561231f, 0.561231f, 0.561231f, 0.594604f, 
  0.594604f, 0.594604f, 0.594604f, 0.594604f, 0.594604f, 0.629961f, 0.629961f, 0.629961f, 
  0.629961f, 0.629961f, 0.667420f, 0.667420f, 0.667420f, 0.667420f, 0.667420f, 0.707107f, 
  0.707107f, 0.707107f, 0.707107f, 0.707107f, 0.749154f, 0.749154f, 0.749154f, 0.749154f, 
  0.749154f, 0.793701f, 0.793701f, 0.793701f, 0.793701f, 0.793701f, 0.840896f, 0.840896f, 
  0.840896f, 0.840896f, 0.840896f, 0.890899f, 0.890899f, 0.890899f, 0.890899f, 0.890899f, 
  0.943874f, 0.943874f, 0.943874f, 0.943874f, 0.943874f, 1.000000f, 1.000000f, 1.000000f, 
  1.000000f, 1.000000f, 1.000000f, 1.059463f, 1.059463f, 1.059463f, 1.059463f, 1.059463f, 
  1.122462f, 1.122462f, 1.122462f, 1.122462f, 1.122462f, 1.189207f, 1.189207f, 1.189207f, 
  1.189207f, 1.189207f, 1.259921f, 1.259921f, 1.259921f, 1.259921f, 1.259921f, 1.334840f, 
  1.334840f, 1.334840f, 1.334840f, 1.334840f, 1.414214f, 1.414214f, 1.414214f, 1.414214f, 
  1.414214f, 1.498307f, 1.498307f, 1.498307f, 1.498307f, 1.498307f, 1.587401f, 1.587401f, 
  1.587401f, 1.587401f, 1.587401f, 1.681793f, 1.681793f, 1.681793f, 1.681793f, 1.681793f, 
  1.681793f, 1.781797f, 1.781797f, 1.781797f, 1.781797f, 1.781797f, 1.887749f, 1.887749f, 
  1.887749f, 1.887749f, 1.887749f, 2.000000f, 2.000000f, 2.000000f, 2.000000f, 2.000000f
};

// =========================== forward declarations ================================================
// =========================== someday I refactor this *** =========================================
static float IRAM_ATTR bilinearLookup(float (&table)[16][16], float x, float y);
static float IRAM_ATTR lookupTable(float (&table)[TABLE_SIZE+1], float index );
static float IRAM_ATTR fclamp(float in, float minv, float maxv) ;
static float IRAM_ATTR fast_shape(float x);
static void  IRAM_ATTR fast_sincos(float x, float* sinRes, float* cosRes);
static float IRAM_ATTR fast_sin(float x);
static float IRAM_ATTR fast_cos(float x);
static float __attribute__((always_inline)) inline one_div(float);
float dB2amp(float dB);
float amp2dB(float amp);
float linToLin(float in, float inMin, float inMax, float outMin, float outMax);
float linToExp(float in, float inMin, float inMax, float outMin, float outMax);
float expToLin(float in, float inMin, float inMax, float outMin, float outMax);
float knobMap(float in, float outMin, float outMax);
static void IRAM_ATTR drums_generate();
static void IRAM_ATTR synth1_generate();
static void IRAM_ATTR synth2_generate();
static void IRAM_ATTR mixer();
