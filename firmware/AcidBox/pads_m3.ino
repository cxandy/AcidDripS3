#include "config.h"
#include "sequencer.h"

/* M3 Phase 3 -- the gesture layer: PLAY/STOP, the FUNC pages, step editing.
 *
 * Phase 1 of this file did the FX-assign sub-mode and nothing else. This is the rest of what
 * a control surface needs before anything on the board is a sequencer you can play rather
 * than one you can hear.
 *
 * PORTED FROM V5 (Acid_Drip_Drum_Acid_Drift_V5.ino), by line:
 *   PAD_PLAY_A/B, PAD_FUNC_A/B   :482-485   already in config.h
 *   doPadRelease()               :2964-2976 step on/off, on RELEASE
 *   doPadLong()                  :2978-2989 accent/glide alternation
 *   long-press poll              :6197-6203 when a long press counts
 *   PLAY chord on press          :5650-5654 arm only
 *   PLAY chord on release        :5870-5981 short = play/stop, long = factory reset
 *   FUNC chord on press          :5720-5746 three arms, in V5's order
 *   FUNC mode press dispatch     :5830-5861 bottom row selects, top row applies
 *   FUNC mode release dispatch   :6020-6064 PLAY/FUNC pads deferred to release
 *   doFuncSelect()               :3380-3438 bottom row -> a page
 *   doFuncApply()                :3539-3611 top row -> a value for that page
 *   FuncSel enum + FUNCNAMES     :1115-1118, :1135
 *   TEMPO_PRESETS                :1171, :3603
 *   factory reset                :5872-5904
 *
 * TWO THINGS HERE ARE NOT V5, and one of them CORRECTS PHASE 1.
 *
 * 1. The FUNC page NUMBERS are V5's, which is the opposite of what M3 Phase 2 did with the
 *    ch2 pitch modes and is deliberate for the same reason read backwards. There, the port's
 *    behaviour matched none of V5's modes, so numbering had to be invented. Here, four of the
 *    five pages the port implements sit at exactly the index V5 puts them at: 1 RIFF, 5 TEMPO,
 *    6 PLEN, 7 PAT>. Keeping them means a pad that means TEMPO on a V5 means TEMPO here, and
 *    costs nothing. The table keeps V5's length and order, including the two holes, so the
 *    gaps are visible instead of compacted away.
 *
 * 2. PHASE 1 GOT THE FUNC CHORD WRONG, and this file fixes it. Phase 1 made the chord three
 *    arms -- cold = plain FUNC, FUNC again = enter FX assign -- and cited V5 :5721-5745 as
 *    its source. V5's middle arm is not that: :5737-5739 is `else if (funcMode) { close it }`
 *    and the third arm at :5742-5745 is plain FUNC again. V5 enters FX assign by SELECTING
 *    the FX page (doFuncSelect :3427-3431 sets fxAssignMode and drops funcMode), not by
 *    pressing FUNC twice. Phase 1's version made the FX page unreachable and invented an
 *    entry gesture that does not exist in V5. Nothing caught it because the Phase 1 tests
 *    pinned the chord WINDOW and the press/release ORDERING and never its three-arm shape;
 *    the shape is pinned now (see tools/test-sequencer.py, M3 Phase 3 section).
 *
 *    Phase 1 also gave a solo tap of a FUNC pad a second job -- leave FX assign -- which
 *    V5 does not do either. In V5 a solo FUNC-pad release inside FX assign reaches
 *    doFXAssign() (:5984-5986), which means pads 7 and 8 get to carry an effect like any
 *    other step. Phase 1's exit made two of sixteen steps unreachable through the natural
 *    gesture to make exiting slightly easier. V5's single escape -- the FUNC chord -- is
 *    reachable and is the only one, so that is what this file implements.
 *
 * ONE THING HERE IS NOT EVIDENCE-BACKED, and it is still the pin table.
 *
 * V5 runs on an RP2040; PAD_PINS there are GPIOs 0-22. On the ESP32-S3 several of those
 * numbers are already spoken for -- 5/6/7 are I2S, 16/17 are POT_PINS, 0 is a strapping pin,
 * 19/20 are the native USB pair (HARDWARE_SETUP.md section 2). Copying V5's array verbatim
 * would put pads on pins the audio path owns, and the failure would present as noise on the
 * DAC rather than as a compile error. config.h therefore carries a re-picked S3 table, and
 * that array is a claim about this board's wiring that has NOT been checked against a
 * schematic. Everything else in this file is a line-for-line port.
 *
 * NO DISPLAY. M3 task 2 in ESP32S3_FUSION_IMPLEMENTATION.md is the TFT renderer, and the
 * port has no display driver, no fonts and no confirmed panel. Every V5 branch that ends in
 * `ui.dirty = true` has been reduced to a DEBF line here, because a line in the log is the
 * only thing in this build that can be seen. That is a real reduction in what the port
 * proves -- V5's UI carries a lot of state (the active page, the selected effect, the
 * playhead) that here exists only as a variable -- and it is why the log lines are written
 * to be checkable rather than merely informative: each one names the pad, the resulting
 * value, and the value that was read back from the sequencer.
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
static bool     pLong_arr[NUM_PADS]   = {};
static uint8_t  pCycle_arr[NUM_PADS]  = {};

/* Mode flags.
 *
 * g_funcMode is no longer write-only -- Phase 3 is what it was being kept for, and
 * pads_m3.c had a note saying so. It gates the FUNC pages: it decides whether a pad press
 * reaches the page handlers or falls through to the step toggles, and it is tested ahead of
 * every other mode, exactly as V5 tests funcMode (:5748 onward). */
static bool     g_funcMode      = false;
static bool     g_fxAssignMode  = false;
static bool     g_fxAssignHasFx = false;
static uint8_t  g_fxAssignFx    = 0;

/* The selected FUNC page. G_FUNC_NONE rather than an int8_t -1, so the comparison in the
 * dispatch chain is one unsigned compare and the log prints a number rather than a
 * character. */
static uint8_t  g_funcSel       = G_FUNC_NONE;

/* =====================================================================
 * THE FUNC PAGES
 *
 * V5's FuncSel (:1115-1118) and FUNCNAMES (:1135):
 *
 *     enum FuncSel { FUNC_NONE=-1, FUNC_KEY=0, FUNC_PAT, FUNC_SOUND, FUNC_WALK,
 *                    FUNC_FX, FUNC_TEMPO, FUNC_PLEN, FUNC_PATMODE };
 *     const char* FUNCNAMES[] = {"KEY","RIFF","SOUND","WALK","FX","TEMPO","PLEN","PAT>"};
 *
 * The port implements five of the eight and keeps V5's indices. What each gap is:
 *
 *   0  KEY     not ported -- V5's KEY_MAP and the rootNote/origNote pair behind it
 *                  (:3554-3569) have no counterpart here. seq.key exists and
 *                  seq_noteToMidi() honours it, but there is no setter and no key map.
 *   2  SOUND   V5 selects between eight engines (:1120-1124). The port has one engine, so
 *                  the slot is free, and ch2 pitch mode takes it. This is the one rename:
 *                  a pad that said SOUND on a V5 says CH2 here. It has to, because there is
 *                  no engine to pick and a dead pad is worse than a repurposed one.
 *   3  WALK    not ported -- V5's key-walk generator (kwMode and its counters) needs a
 *                  scale table and an arpeggiator, which is M3 Phase 4 with the WALK bank.
 *   4  FX      ported in Phase 1. Entered by SELECTING this page, which is V5's path
 *                  (:3427-3431) and not the one Phase 1 invented.
 *
 * G_FUNC_IMPL is the set the port can actually service, so the dispatch chain can ask
 * "is this page one of mine" in one comparison instead of a switch with five empty arms.
 * It is a bitmask, not an array of bools, because the test reads it and a list of five
 * booleans with a comment is a list that can fall out of step with a switch elsewhere.
 */
#define G_FUNC_NONE   0xFF
#define G_FUNC_KEY    0      // V5 slot 0 -- not implemented
#define G_FUNC_PAT    1      // V5 FUNC_PAT     top row 1-8 loads preset 0-7
#define G_FUNC_CH2    2      // V5 FUNC_SOUND   top row 1-8 sets the ch2 pitch mode
#define G_FUNC_WALK   3      // V5 FUNC_WALK    not implemented
#define G_FUNC_FX     4      // V5 FUNC_FX      opens FX assign
#define G_FUNC_TEMPO  5      // V5 FUNC_TEMPO   top row 1-8 picks a tempo
#define G_FUNC_PLEN   6      // V5 FUNC_PLEN    BOTH rows set the length
#define G_FUNC_ORDER  7      // V5 FUNC_PATMODE top row 1-8 picks the step order

#define G_FUNC_IMPL   ((1 << G_FUNC_PAT) | (1 << G_FUNC_CH2) | (1 << G_FUNC_FX) | \
                       (1 << G_FUNC_TEMPO) | (1 << G_FUNC_PLEN) | (1 << G_FUNC_ORDER))

/* V5's own eight strings, with slot 2 replaced. Kept at length 8 and in V5's order so the
 * test can compare this array to :1135 element by element and be told which entries are
 * the port's own; a compacted five-entry table would compare unequal everywhere and say
 * nothing. */
static const char *const G_FUNC_NAME[8] = {
  "-", "RIFF", "CH2", "-", "FX", "TEMPO", "PLEN", "PAT>"
};

/* V5 :1171 -- const uint16_t TEMPO_PRESETS[8] = {100,110,120,128,133,138,145,160};
 * Read by doFuncApply's TEMPO arm at :3603 as TEMPO_PRESETS[constrain(slot,0,7)]. Ported
 * literally; the clamp is this file's, and it is load-bearing because `slot` is a pad
 * index that has already been proven to be < 8 by the caller, but the array is declared
 * [8] and a future caller might not be. */
static const uint16_t G_TEMPO_PRESETS[8] = {100, 110, 120, 128, 133, 138, 145, 160};

/* =====================================================================
 * STEP EDITING -- V5's doPadRelease() and doPadLong()
 * ===================================================================== */

/* V5 :2964-2976. On RELEASE, never on press.
 *
 * The press carries no action at all in V5 -- doPadPress() only moves the UI cursor (:2960)
 * -- so a chord pad that is also a step pad toggles twice, once per finger. The port has no
 * cursor to move, so its doPadRelease_M3() is the whole of V5's doPadRelease().
 *
 * V5's guard is `!pNoteEdit[p] && !pLong[p]`. pNoteEdit means "the CUT pot moved while this
 * was held, so this press is editing the step's NOTE and not toggling it", and pLong means
 * "the long-press already fired", which stops a long press from also registering as a tap.
 * pLong is ported. pNoteEdit is NOT, and the omission is not an oversight to be papered
 * over with an always-false guard: a note cannot be edited by pot in this build, so the
 * gesture it guards does not exist. Adding `!false` would read as protection against
 * something. Pot reading is M3 task 3 and arrives with the display work. */
static void doPadRelease_M3(uint8_t pad) {
  if (pad < NUM_PADS) {
    if (!pLong_arr[pad]) {
      bool was = seq_stepActive(pad);
      seq_toggleStep(pad);
      DEBF("[M3] step %u %s -> %s\r\n",
           (unsigned)(pad + 1), was ? "on" : "off", seq_stepActive(pad) ? "on" : "off");
    }
  }
}

/* V5 :2978-2989. Long-press alternates between accent and glide, and ONLY between those:
 * V5's own comment is explicit that fx assignment is never reachable from a plain long
 * press on a step, and that is why the effect byte went behind the two-stage FUNC page in
 * the first place.
 *
 * The counter is PER PAD and is never reset in normal operation. V5 resets pCycle only in
 * its funcMode release branch (:6063), so a pad that has been long-pressed an odd number of
 * times ever starts on glide rather than accent. That is V5's behaviour and it is harmless,
 * because the two strictly alternate -- it changes which of the two you get first, not
 * whether the gesture works. Ported as-is rather than "fixed", with the note here so that
 * nobody later reads the asymmetry as a bug in this file. */
static void doPadLong_M3(uint8_t pad) {
  if (pad >= NUM_PADS) { return; }
  if ((pCycle_arr[pad] % 2) == 0) {
    bool was = seq_stepAccent(pad);
    seq_setStepAccent(pad, !was);
    DEBF("[M3] step %u accent %s -> %s\r\n",
         (unsigned)(pad + 1), was ? "on" : "off", seq_stepAccent(pad) ? "on" : "off");
  } else {
    bool was = seq_stepGlide(pad);
    seq_setStepGlide(pad, !was);
    DEBF("[M3] step %u glide %s -> %s\r\n",
         (unsigned)(pad + 1), was ? "on" : "off", seq_stepGlide(pad) ? "on" : "off");
  }
  pCycle_arr[pad]++;
}

/* =====================================================================
 * THE FUNC PAGES
 * ===================================================================== */

/* V5's doFuncSelect() (:3380-3438), minus the parts that are not V5's parts.
 *
 * Dropped, each for a reason rather than by omission:
 *   - the ch2EditMode arm (:3387-3401). It routes to ch2FuncSel, a second page system
 *     belonging to V5's CH2 EDIT sub-mode, which is not ported.
 *   - tap tempo (:3405-3421). Re-tapping the TEMPO pad within TAP_TIMEOUT computes a tempo
 *     from the gaps between taps. It is portable -- it is six lines and it needs no display
 *     -- and it is left out because it is a FEATURE ADD rather than a port, and Phase 3 has
 *     enough of those by accident already. It belongs with the tempo page's own milestone
 *     if it is wanted at all.
 *   - the `ui.dirty` fan-out. Every one of those is a DEBF line here.
 *
 * Kept: the FX page's side effect (:3427-3431), which is the whole reason doFuncSelect is
 * called at all here -- selecting FX opens FX assign and closes FUNC mode, which is V5's
 * real route into that sub-mode. */
static void doFuncSelect_M3(uint8_t padIdx) {
  if (padIdx < 8 || padIdx > 15) { return; }
  uint8_t newSel = (uint8_t)(padIdx - 8);

  // Selecting anything leaves FX assign, V5 :3423-3424. Unreachable from inside FX assign
  // (this mode sets funcMode = false), and kept anyway for the same reason V5 keeps it:
  // it is the difference between "one mode at a time" being true and being hoped for.
  g_fxAssignMode  = false;
  g_fxAssignHasFx = false;
  g_funcSel       = newSel;

  if (newSel == G_FUNC_FX) {
    // V5 :3427-3431. funcMode goes false HERE, not on the way out -- FX assign replaces
    // FUNC mode rather than layering on it, which is why selecting FX and then tapping the
    // FUNC chord closes FX assign instead of reopening FUNC.
    g_fxAssignMode = true;
    g_funcMode     = false;
    DEBF("[M3] func page %u %s -- pick an effect (pads 1-8), then a step\r\n",
         (unsigned)newSel, G_FUNC_NAME[newSel]);
    return;
  }

  if ((G_FUNC_IMPL & (1 << newSel)) == 0) {
    // A dead page SAYS it is dead. V5 has no equivalent because V5 has no dead pages; the
    // nearest thing is its legacy FXNAMES entries at indices 8-11, which its own comment
    // calls unreachable. A bottom-row pad that logs nothing is indistinguishable from a pad
    // that is not wired, and the pin table is already a guess -- this line is how the two
    // get told apart on first hardware.
    DEBF("[M3] func page %u (%s) is not implemented on this port\r\n",
         (unsigned)newSel, G_FUNC_NAME[newSel]);
    g_funcSel = G_FUNC_NONE;
    return;
  }

  DEBF("[M3] func page %u %s\r\n", (unsigned)newSel, G_FUNC_NAME[newSel]);
}

/* V5's doFuncApply() (:3539-3611), for the five arms the port has.
 *
 * Dropped: FUNC_KEY (:3554-3569), FUNC_SOUND (:3573-3586), FUNC_WALK (:3587-3597) -- no key
 * map, no second engine, no walk generator. They stay dead pages rather than becoming
 * something else, because their numbers are already spoken for.
 *
 * Every arm clamps its slot. V5 uses constrain(slot,0,7) at every index; here the callers
 * have already proven slot < NUM_PADS, but the clamps are kept because the cost is zero and
 * the alternative is an array whose safety depends on a fact three call frames away. */
static void doFuncApply_M3(uint8_t slot) {
  if (slot >= 8) { return; }

  switch (g_funcSel) {
    case G_FUNC_PAT: {
      // V5 :3570-3572 -- loadPreset(slot), whole thing, no confirmation.
      seq_loadPreset(slot);
      DEBF("[M3] preset %u: len %u, tempo %u\r\n",
           (unsigned)slot, (unsigned)seq_len(), (unsigned)seq_tempo());
      break;
    }
    case G_FUNC_CH2: {
      // Only two modes exist, so slot 3-8 clamp to 1 rather than being rejected. The log
      // prints the mode the sequencer ended up holding, not the one that was asked for,
      // because those differ and the difference is the thing worth seeing.
      seq_setCh2Mode(slot);
      DEBF("[M3] ch2 pad %u -> mode %u %s\r\n",
           (unsigned)(slot + 1), (unsigned)seq_ch2Mode(), seq_ch2ModeName(seq_ch2Mode()));
      break;
    }
    case G_FUNC_TEMPO: {
      // V5 :3602-3607.
      seq_setTempo(G_TEMPO_PRESETS[slot]);
      DEBF("[M3] tempo pad %u -> %u BPM (%lu us/step)\r\n",
           (unsigned)(slot + 1), (unsigned)seq_tempo(),
           (unsigned long)(60000000UL / (uint32_t)seq_tempo() / 4UL));
      break;
    }
    case G_FUNC_ORDER: {
      // V5 :3598-3601.
      seq_setOrder(slot);
      DEBF("[M3] order pad %u -> %u\r\n", (unsigned)(slot + 1), (unsigned)seq_order());
      break;
    }
    case G_FUNC_PLEN: {
      // V5's PLEN is NOT a select-then-apply page: it takes the tap as the value, on both
      // rows (see the dispatch chain). This arm is reached only by pads 1/2 and 7/8, whose
      // apply V5 also defers to release, and exists so that path has somewhere to land.
      seq_setLen((uint8_t)(slot + 1));
      DEBF("[M3] length pad %u -> %u steps\r\n", (unsigned)(slot + 1), (unsigned)seq_len());
      break;
    }
    case G_FUNC_FX:
      // Selecting FX leaves FUNC mode, so this cannot be reached with g_funcSel == FX. It
      // is here so that if that ever changes, the page does something instead of nothing.
      break;
    default:
      // G_FUNC_NONE, or a page this port does not implement. V5 :3551 returns here too.
      break;
  }
}

/* V5's PLAY chord, and the only exit from FX assign.
 *
 * V5 :5720-5746, three arms in this order. The middle arm is the one Phase 1 got wrong --
 * see the note at the head of this file. Read the arms against V5 rather than against the
 * commit message that introduced them. */
static void funcChord_M3() {
  if (g_fxAssignMode) {
    g_fxAssignMode  = false;
    g_fxAssignHasFx = false;
    g_funcSel       = G_FUNC_NONE;
    DEBF("[M3] fx assign: off (chord)\r\n");
  } else if (g_funcMode) {
    g_funcMode = false;
    g_funcSel = G_FUNC_NONE;
    DEBF("[M3] func mode off\r\n");
  } else {
    g_funcMode = true;
    g_funcSel  = G_FUNC_NONE;
    DEBF("[M3] func mode on -- pads 9-16 pick a page, pads 1-8 set it\r\n");
  }
}

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
      DEBF("[M3] fx select pad %u -> %s\r\n",
           (unsigned)(padIdx + 1), seq_fxName(g_fxAssignFx));
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
         (unsigned)(padIdx + 1), seq_fxName(cur), seq_fxName(nw));
  }
}

/* =====================================================================
 * THE SCAN
 * ===================================================================== */

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
      pLong_arr[i]  = false;
      pChord_arr[i] = false;

      // PLAY chord (pads 1+2). V5 :5650-5654: arms only. The action is on release, so that
      // a long hold can be told from a tap -- which it cannot be on press, because on press
      // a long hold looks exactly like a tap that has not finished yet.
      //
      // First arm in V5's chain, and deliberately not an else-if with a window: V5 tests no
      // elapsed time here (:5651-5652), only that the other pad is down. The FUNC chord
      // below does carry a window. That asymmetry is V5's and is kept -- PLAY is a two-finger
      // press with no timing to speak of, while FUNC's window is what stops an ordinary
      // two-pad step-toggle from being read as a page change.
      if ((i == PAD_PLAY_A && pState_arr[PAD_PLAY_B]) ||
          (i == PAD_PLAY_B && pState_arr[PAD_PLAY_A])) {
        pChord_arr[PAD_PLAY_A] = true;   // so release does not read these as taps
        pChord_arr[PAD_PLAY_B] = true;
      }
      // FUNC chord (pads 7+8) within CHORD_WINDOW_MS of each other. V5 :5721-5722.
      else if ((i == PAD_FUNC_A && pState_arr[PAD_FUNC_B] &&
                (now - pDown_arr[PAD_FUNC_B]) < CHORD_WINDOW_MS) ||
               (i == PAD_FUNC_B && pState_arr[PAD_FUNC_A] &&
                (now - pDown_arr[PAD_FUNC_A]) < CHORD_WINDOW_MS)) {
        pChord_arr[PAD_FUNC_A] = true;
        pChord_arr[PAD_FUNC_B] = true;
        funcChord_M3();
      }
      // FX assign owns every other press. V5 :5748-5750, and checked before the FUNC and
      // normal branches on purpose -- inside FX mode every pad is either a step or an effect
      // button, and letting one fall through would give pad 7 the meaning "DimStep".
      else if (g_fxAssignMode) {
        // The FUNC pads are excluded here, exactly as V5 excludes them at :5749, and the
        // exclusion is load-bearing rather than cosmetic. doFXAssign_M3 is a TWO-STAGE flow
        // gated on one global: one tap selects an effect, the next tap applies it. A press
        // that acted would leave pad 7 to arrive at its own release -- which V5 routes here
        // deliberately, :5984-5986 -- with g_fxAssignHasFx already set, and re-tapping the
        // selected button DESELECTS it (V5 :3625-3629). One tap would select and then cancel,
        // logging both, and pad 7 would be the only pad of sixteen unable to carry an effect,
        // which is precisely what routing the FUNC pads to doFXAssign on release was for.
        if (i != PAD_FUNC_A && i != PAD_FUNC_B) { doFXAssign_M3(i); }
      }
      // FUNC mode input. V5 :5830-5861.
      else if (g_funcMode) {
        // Pads 7/8 and 1/2 do nothing on press: their actions are the chord handler above
        // and, for a value, the release path below. Letting a solo pad 7 toggle FUNC mode
        // here as well as close it on the second press would mean one finger could open and
        // close the menu.
        if (i == PAD_FUNC_A || i == PAD_FUNC_B ||
            i == PAD_PLAY_A || i == PAD_PLAY_B) {
          // deferred to release
        } else if (i >= 8) {
          if (g_funcSel == G_FUNC_PLEN) {
            // V5 :5836-5840 -- on PLEN the bottom row sets the length directly, 9-16.
            seq_setLen((uint8_t)((i - 8) + 9));
            DEBF("[M3] length pad %u -> %u steps\r\n", (unsigned)(i + 1), (unsigned)seq_len());
          } else {
            doFuncSelect_M3((uint8_t)i);
          }
        } else if (g_funcSel == G_FUNC_PLEN) {
          // V5 :5848-5854 -- top row sets length 1-8.
          seq_setLen((uint8_t)(i + 1));
          DEBF("[M3] length pad %u -> %u steps\r\n", (unsigned)(i + 1), (unsigned)seq_len());
        } else {
          doFuncApply_M3((uint8_t)i);
        }
      }
      // Normal mode does nothing on press. V5's doPadPress() only moves the UI cursor
      // (:2960-2962), and the port has no cursor.
      continue;
    }

    /* ---- RELEASE ---- */
    if (pChord_arr[i]) {
      // V5 :5869-5983. This arm is the PLAY chord's action, and it is not a no-op: the
      // chord was ARMED on press and only means anything now.
      //
      // Exactly one of the two pads does the work, and the reason is worth spelling out
      // because it looks like a double-toggle at a glance. Both pads were marked on press,
      // and this branch clears only pChord_arr[i] -- not its partner. So whichever finger
      // comes up first sees both flags still set and acts; the second one sees its partner's
      // flag already cleared and does nothing. Either order gives one action, because the
      // gate is the conjunction and one half of it is consumed by the first release.
      if ((i == PAD_PLAY_A || i == PAD_PLAY_B) &&
          pChord_arr[PAD_PLAY_A] && pChord_arr[PAD_PLAY_B]) {
        // holdMs is measured from the LATER of the two presses, V5 :5871 -- max(pDown[A],
        // pDown[B]). Measuring from the earlier one would make a two-finger press read as
        // longer than it was, and a deliberate long hold would have to be longer still.
        uint32_t holdMs = now - ((pDown_arr[PAD_PLAY_A] > pDown_arr[PAD_PLAY_B])
                                   ? pDown_arr[PAD_PLAY_A] : pDown_arr[PAD_PLAY_B]);
        if (holdMs >= (uint32_t)LONG_PRESS_MS) {
          // V5 :5872-5905. Long hold = factory reset. It does not stop the sequencer, in
          // V5 either: the reset is about the pattern and the settings, and the note at
          // seq_factoryReset() says what the port does and does not carry over.
          seq_factoryReset();
          // V5 also closes every mode here (:5902-5903). Resetting the pattern while a menu
          // is open would leave the menu describing values that no longer exist.
          g_funcMode = false;
          g_funcSel  = G_FUNC_NONE;
          g_fxAssignMode  = false;
          g_fxAssignHasFx = false;
        } else {
          // Short press: play/stop. V5 :5935-5979. V5's version has three sub-branches --
          // acid's own play state, DRIFT's, and the shared clock -- because V5 runs three
          // engines on one clock. The port has one, so seq_toggle() is the whole of it, and
          // V5's "stopping acid leaves drums and DRIFT running" bookkeeping has nothing to
          // keep alive.
          bool was = seq_running();
          seq_toggle();
          DEBF("[M3] play/stop pad %u: %s -> %s\r\n",
               (unsigned)(i + 1), was ? "playing" : "stopped",
               seq_running() ? "playing" : "stopped");
        }
      }
      pChord_arr[i] = false;
      continue;
    }

    // V5's release dispatch, in V5's order. The four arms V5 has between fxAssignMode and
    // doPadRelease are ch2EditMode, chainMode, funcMode and walksMode; ch2EditMode and
    // chainMode are not ported and walksMode is Phase 4.
    if (g_fxAssignMode) {
      // V5 :5984-5986. A solo tap of a FUNC pad here reaches doFXAssign, which means pads 7
      // and 8 can carry an effect like any other step. Phase 1 made this an exit instead;
      // see the note at the head of the file.
      if (i == PAD_FUNC_A || i == PAD_FUNC_B) { doFXAssign_M3(i); }
    }
    else if (g_funcMode) {
      // V5 :6020-6063, minus the ch2 and pad-11-defer arms.
      if (i == PAD_FUNC_A || i == PAD_FUNC_B ||
          i == PAD_PLAY_A || i == PAD_PLAY_B) {
        if (g_funcSel == G_FUNC_PLEN) {
          // V5 :6048-6051. Same one-row-later length rule as the press path.
          seq_setLen((uint8_t)(i + 1));
          DEBF("[M3] length pad %u -> %u steps\r\n", (unsigned)(i + 1), (unsigned)seq_len());
        } else if (g_funcSel != G_FUNC_NONE) {
          doFuncApply_M3((uint8_t)i);
        }
      }
      // V5 :6063. pCycle is cleared on release inside FUNC mode and NOT outside it; see
      // doPadLong_M3() for why that asymmetry is V5's and harmless.
      pCycle_arr[i] = 0;
    }
    else {
      // V5 :6070-6072 -> doPadRelease() -> the step toggle.
      doPadRelease_M3(i);
    }
  }

  /* ---- LONG-PRESS POLLS ---- V5 :6168-6203 */
  //
  // Outside the edge loop because a long press is not an edge: it is the absence of one for
  // LONG_PRESS_MS. Inside the loop it could never fire, since the loop body only runs when a
  // pad CHANGES state.
  for (uint8_t i = 0; i < NUM_PADS; i++) {
    // V5's gate, minus three clauses and with nothing added:
    //   pState[i] && !pLong[i] && !pChord[i] && (now-pDown[i]) > LG && !funcMode &&
    //   !fxAssignMode && !chainMode && !ch2EditMode
    // chainMode and ch2EditMode are not ported. The two V5 clauses that ARE missing are
    // pNoteEdit (no pot reading in this build) and the clear-all-slots chord test over
    // pads 3-6, which guards M4's patch slots -- there are none yet.
    if (!pChord_arr[i] && !pLong_arr[i] && pState_arr[i] &&
        !g_funcMode && !g_fxAssignMode &&
        (now - pDown_arr[i]) > (uint32_t)LONG_PRESS_MS) {
      pLong_arr[i] = true;
      doPadLong_M3(i);
    }
  }
}