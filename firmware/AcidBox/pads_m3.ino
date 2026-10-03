#include "config.h"
#include "sequencer.h"

/* M3 Phase 1 -- pads + FX assign.
 *
 * SCOPE, stated so this file is not read as more than it is: the FX-assign sub-mode and
 * nothing else. PLAY/STOP (pads 1+2), the FUNC pages (tempo / preset / order / len) and
 * the ch2 generator are NOT here -- those are Phase 3. What IS here is the single path
 * that closes the loop M2.5 left open: the eight step effects exist in sequencer.h with
 * no way to reach them from a control surface, and this is the way to reach them.
 *
 * PORTED FROM V5 (Acid_Drip_Drum_Acid_Drift_V5.ino), by line:
 *   FX_PAD_MAP              :2115        {0,1,2,3,4,5,6,7}, identical to SEQ_FX_*
 *   doFXAssign()            :3614-3636   two-stage select-then-assign
 *   FUNC+FX chord           :5721-5745   pads 7+8 (indices 6+7) within 200 ms
 *   FX-assign owns presses  :5748-5750   every pad except the two FUNC pads
 *   release exits           :5868-5885   FUNC release exits, any other release does not
 *
 * ONE THING HERE IS NOT EVIDENCE-BACKED, and it is the pin table.
 *
 * V5 runs on an RP2040; PAD_PINS there are GPIOs 0-22. On the ESP32-S3 several of those
 * numbers are already spoken for -- 5/6/7 are I2S, 16/17 are POT_PINS, 0 is a strapping
 * pin, 19/20 are the native USB pair (HARDWARE_SETUP.md section 2). Copying V5's array
 * verbatim would put pads on pins the audio path owns, and the failure would present as
 * noise on the DAC rather than as a compile error. config.h therefore carries a re-picked
 * S3 table, and that array is a claim about this board's wiring that has NOT been checked
 * against a schematic. Everything else in this file is a line-for-line port.
 *
 * The 0..7 / 0..15 pad overlap is V5's, and it is on purpose: pads 1-8 are both the FX
 * picker and steps 1-8, and doFXAssign is two-stage precisely to tell those apart. A
 * first press with nothing selected picks the effect; only later presses assign it.
 * Collapsing that into one-pad-one-meaning would change the gesture, not just the code.
 */

/* Pad state, here rather than in AcidBox.ino because this file is the only thing that
 * reads or writes it. Placement is the whole reason it moved: a block of UI state
 * sitting 500 lines from its only writer is a block that stops being kept in step.
 *
 * Sized with NUM_PADS rather than a literal 16, and zero-initialised with {} rather than a
 * spelled-out list of sixteen falses. The list was tried first and is strictly worse: it
 * puts the array length in two places that can disagree, and it costs four lines of
 * screen to say "all false" sixteen times.
 */
static bool     pState_arr[NUM_PADS] = {};
static bool     pLast_arr[NUM_PADS]   = {};
static uint32_t pDeb_arr[NUM_PADS]    = {};
static uint32_t pDown_arr[NUM_PADS]   = {};
static bool     pChord_arr[NUM_PADS]  = {};

static bool     g_funcMode      = false;   // written, not yet read -- see note below
static bool     g_fxAssignMode  = false;
static bool     g_fxAssignHasFx = false;
static uint8_t  g_fxAssignFx    = 0;

/* g_funcMode is currently WRITE-ONLY, and that is a known gap rather than an oversight.
 *
 * In V5, funcMode gates the FUNC pages: it decides whether a pad press reaches doFuncSelect
 * (tempo / preset / order / len) or falls through to the step toggles, and it is tested
 * ahead of every other mode in the dispatch chain. None of those pages exist yet, so there
 * is nothing here for it to gate -- the only observable difference between funcMode set
 * and clear today is one DEBF line.
 *
 * It is kept, and set, for two reasons. Deleting it would mean the FUNC chord lands in FX
 * assign from a cold start with no intermediate state to port the pages onto, so Phase 3
 * would have to invent that state and re-derive the chord's meaning; and the FX-assign
 * entry path depends on it ("plain FUNC first, then FUNC again for FX") which is V5's
 * two-press idiom and would otherwise be unreachable.
 *
 * A write-only variable is a small lie in a file whose purpose is to be checkable, so the
 * honest form is the comment above rather than pretending it is load-bearing today.
 */

/* V5 :2115 -- FX_PAD_MAP[8] = {0,1,2,3,4,5,6,7}, an identity over the effect numbers,
 * with the names living in a UI string table three hundred lines away. It is reproduced
 * literally because the test compares this array to V5's source to source; the safety
 * that the enum in sequencer.h provides (SEQ_FX_NONE..SEQ_FX_DIMSTEP, each named) is
 * checked alongside it rather than assumed.
 *
 * The leading G_ is a rename, not a port detail: V5 has no other FX_PAD_MAP in this
 * translation unit but the port does have a sequencer.h, and a bare FX_PAD_MAP here
 * would be a name the reader has to go looking for.
 */
static const uint8_t G_FX_PAD_MAP[8] = {0,1,2,3,4,5,6,7};

static void doFXAssign_M3(uint8_t padIdx) {
  if (!g_fxAssignHasFx) {
    // Stage 1 -- choose the effect. Top row only; V5 gates this on padIdx < 8.
    if (padIdx < 8) {
      g_fxAssignFx    = G_FX_PAD_MAP[padIdx];
      g_fxAssignHasFx = true;
      DEBF("[M3] fx select pad %u -> %s\r\n", (unsigned)padIdx, seq_fxName(g_fxAssignFx));
    }
    return;
  }
  // Re-tapping the selected effect button deselects it. V5 :3625-3629.
  if (padIdx < 8 && G_FX_PAD_MAP[padIdx] == g_fxAssignFx) {
    g_fxAssignHasFx = false;
    DEBF("[M3] fx deselect\r\n");
    return;
  }
  // Stage 2 -- toggle the effect on the tapped step. V5 :3631-3635, with the direct
  // field write swapped for the sequencer's own setter so the effect column keeps
  // exactly one writer.
  if (padIdx < NUM_PADS) {
    uint8_t cur = seq_stepEffect(padIdx);
    uint8_t nw  = (cur == g_fxAssignFx) ? SEQ_FX_NONE : g_fxAssignFx;
    seq_setStepEffect(padIdx, nw);
    DEBF("[M3] step %u effect %s -> %s\r\n",
         (unsigned)padIdx, seq_fxName(cur), seq_fxName(nw));
  }
}

void pollPads_M3() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < NUM_PADS; i++) {
    // Active low, as V5's pads switch to ground. INPUT_PULLUP is set in setup().
    bool s = (digitalRead(PAD_PINS[i]) == LOW);
    if (s != pLast_arr[i]) {          // any edge restarts the debounce window
      pDeb_arr[i]  = now;
      pLast_arr[i] = s;
    }
    if ((now - pDeb_arr[i]) <= PAD_DEBOUNCE_MS) continue;   // still bouncing
    if (s == pState_arr[i]) continue;                      // no settled edge

    pState_arr[i] = s;
    if (s) {
      /* ---- PRESS ---- */
      pDown_arr[i]  = now;
      pChord_arr[i] = false;

      // FUNC+FX chord: pads 7+8 (indices 6+7) within CHORD_WINDOW_MS of each other.
      // Same shape and window as V5 :5721, and checked BEFORE the fxAssignMode branch on
      // purpose -- inside FX mode those two pads ARE the exit gesture, so letting them
      // fall through to doFXAssign would give pad 7 the meaning "DimStep".
      if ((i == PAD_FUNC_A && pState_arr[PAD_FUNC_B]) ||
          (i == PAD_FUNC_B && pState_arr[PAD_FUNC_A])) {
        uint32_t other = (i == PAD_FUNC_A) ? pDown_arr[PAD_FUNC_B] : pDown_arr[PAD_FUNC_A];
        if ((now - other) < CHORD_WINDOW_MS) {
          pChord_arr[PAD_FUNC_A] = true;   // so release does not read these as taps
          pChord_arr[PAD_FUNC_B] = true;
          if (g_fxAssignMode) {            // already in FX assign -> the chord exits
            g_fxAssignMode = false; g_fxAssignHasFx = false; g_funcMode = false;
            DEBF("[M3] fx assign: off (chord)\r\n");
          } else if (g_funcMode) {         // FUNC then FUNC == enter FX assign
            g_funcMode = false; g_fxAssignMode = true; g_fxAssignHasFx = false;
            DEBF("[M3] fx assign: on -- pick an effect, then a step\r\n");
          } else {                         // plain FUNC
            g_funcMode = true;
            DEBF("[M3] func mode\r\n");
          }
          continue;
        }
      }

      if (g_fxAssignMode) { doFXAssign_M3(i); }
      continue;
    }

    /* ---- RELEASE ---- */
    if (pChord_arr[i]) { pChord_arr[i] = false; continue; }   // chord member, ignore
    if (!g_fxAssignMode) continue;
    if (i == PAD_FUNC_A || i == PAD_FUNC_B) {
      // V5 :5876-5880 -- letting go of either FUNC pad leaves FX assign.
      g_fxAssignMode = false; g_fxAssignHasFx = false; g_funcMode = false;
      DEBF("[M3] fx assign: off (FUNC released)\r\n");
    }
    // Any other release leaves FX assign standing, so a run of step taps does not need
    // the mode re-entered between them. V5 :5882-5884.
  }
}
