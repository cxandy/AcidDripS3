/*

  AcidBox
  ESP32 acid combo of 303 + 303 + 808 like synths. MIDI driven. I2S output to DAC. No indication. Uses both cores of ESP32.

  To build the thing
  You will need an ESP32 with PSRAM (ESP32 WROVER module). Preferrable an external DAC, like PCM5102. In ArduinoIDE Tools menu select:

* * Board: "ESP32 Dev Module" or "ESP32S3 Dev Module"
* * Partition scheme: No OTA (1MB APP/ 3MB SPIFFS)
* * PSRAM: "enabled" or "OPI PSRAM" or what type you have


  !!!!!!!! ATTENTION !!!!!!!!!
  You will need to upload samples from /data folder to the ESP32 flash, otherwise you'll only have 40kB samples from samples.h. 
  To upload samples follow the instructions:
  
  https://github.com/lorol/LITTLEFS#arduino-esp32-littlefs-filesystem-upload-tool
  And then use Tools -> ESP32 Sketch Data Upload

*/
#pragma GCC optimize ("O2")
#include "config.h"
// M1: AcidBox.ino sorts BEFORE engine_iface.ino, so setup() and regular_checks() would not
// see eng_init()/eng_poll() from the concatenation alone. Without this the calls fail to
// compile -- but see AcidBanger.ino:4 for the same trap's quieter form, where an
// undeclared name inside #if evaluates to 0 instead of erroring. Do not rely on position.
#include "engine_iface.h"
#include "fx_delay.h"
#ifndef NO_PSRAM
#include "fx_reverb.h"
#endif
#include "compressor.h"
#include "synthvoice.h"
#include "sampler.h"
#include <Wire.h>


// =============================================================== MIDI interfaces ===============================================================

#if defined MIDI_VIA_SERIAL2 || defined MIDI_VIA_SERIAL || defined MIDI_USB_DEVICE
#include <MIDI.h>
#endif

#ifdef MIDI_USB_DEVICE
  #include "src/usbmidi/src/USB-MIDI.h"
  USBMIDI_CREATE_INSTANCE(0, MIDI_usbDev);
#endif

#ifdef MIDI_VIA_SERIAL

  struct CustomBaudRateSettings : public MIDI_NAMESPACE::DefaultSettings {
    static const long BaudRate = 115200;
    static const bool Use1ByteParsing = false;
  };

  MIDI_NAMESPACE::SerialMIDI<MIDI_PORT_TYPE, CustomBaudRateSettings> serialMIDI(MIDI_PORT);
  MIDI_NAMESPACE::MidiInterface<MIDI_NAMESPACE::SerialMIDI<MIDI_PORT_TYPE, CustomBaudRateSettings>> MIDI((MIDI_NAMESPACE::SerialMIDI<MIDI_PORT_TYPE, CustomBaudRateSettings>&)serialMIDI);

#endif

#ifdef MIDI_VIA_SERIAL2
// MIDI port on UART2,   pins 16 (RX) and 17 (TX) prohibited on ESP32, as they are used for PSRAM
struct Serial2MIDISettings : public midi::DefaultSettings {
  static const long BaudRate = 31250;
  static const int8_t RxPin  = MIDIRX_PIN;
  static const int8_t TxPin  = MIDITX_PIN;
  static const bool Use1ByteParsing = false;
};
MIDI_NAMESPACE::SerialMIDI<HardwareSerial> Serial2MIDI2(Serial2);
MIDI_NAMESPACE::MidiInterface<MIDI_NAMESPACE::SerialMIDI<HardwareSerial, Serial2MIDISettings>> MIDI2((MIDI_NAMESPACE::SerialMIDI<HardwareSerial, Serial2MIDISettings>&)Serial2MIDI2);
#endif


// lookuptables
static float DRAM_ATTR WORD_ALIGNED_ATTR midi_pitches[128];
static float DRAM_ATTR WORD_ALIGNED_ATTR  midi_phase_steps[128];
static float DRAM_ATTR WORD_ALIGNED_ATTR  midi_tbl_steps[128];
static float DRAM_ATTR WORD_ALIGNED_ATTR  exp_square_tbl[TABLE_SIZE+1];
//static float square_tbl[TABLE_SIZE+1];
static float DRAM_ATTR WORD_ALIGNED_ATTR  saw_tbl[TABLE_SIZE+1];
static float DRAM_ATTR WORD_ALIGNED_ATTR  exp_tbl[TABLE_SIZE+1];
static float DRAM_ATTR WORD_ALIGNED_ATTR  knob_tbl[TABLE_SIZE+1]; // exp-like curve
static float DRAM_ATTR WORD_ALIGNED_ATTR  shaper_tbl[TABLE_SIZE+1]; // illinear tanh()-like curve
static float DRAM_ATTR WORD_ALIGNED_ATTR  lim_tbl[TABLE_SIZE+1]; // diode soft clipping at about 1.0
static float DRAM_ATTR WORD_ALIGNED_ATTR  sin_tbl[TABLE_SIZE+1];
static float DRAM_ATTR WORD_ALIGNED_ATTR  norm1_tbl[16][16]; // cutoff-reso pair gain compensation
static float DRAM_ATTR WORD_ALIGNED_ATTR  norm2_tbl[16][16]; // wavefolder-overdrive gain compensation
//static float (*tables[])[TABLE_SIZE+1] = {&exp_square_tbl, &square_tbl, &saw_tbl, &exp_tbl};

// service variables and arrays
volatile uint32_t s1t, s2t, drt, fxt, s1T, s2T, drT, fxT, art, arT, c0t, c0T, c1t, c1T; // debug timing: if we use less vars, compiler optimizes them
volatile uint32_t prescaler;
static  uint32_t  last_reset = 0;
static  float     param[POT_NUM];
static int    ctrl_hold_notes;

#if M0_DIAG
// M0: reverb bypass, flipped on a timer from regular_checks(). Written on core 1, read
// on core 0 from the IRAM audio task, hence volatile. A bool is a single word load, so
// there is no tearing to worry about.
//
// It alternates on its own rather than waiting for a button: the M0 wiring is a DAC
// cable and nothing else, and this WROOM board breaks out no spare pin to switch on.
static volatile bool m0ReverbBypass = false;
volatile uint8_t  m0Mode = 0;
volatile uint32_t m0MixerCalls = 0;
volatile uint32_t m0NoteOn[2]  = { 0, 0 };
volatile uint32_t m0NoteOff[2] = { 0, 0 };
volatile uint32_t m0ShortWrites = 0;
volatile uint32_t m0ShortBytes = 0;
volatile uint32_t m0I2SCalls   = 0;
volatile uint32_t m0OobSample  = 0;
volatile uint32_t m0OobCache   = 0;

// Peak amplitude of each bus in mixer(), read and printed from regular_checks().
// Written from the IRAM audio task, so plain stores only: no printing, and no libm
// either -- M0_TRACK below does its own abs with a compare, because fabsf() is not
// guaranteed to be in IRAM.
volatile float m0pk_drums  = 0.0f;
volatile float m0pk_synth1 = 0.0f;
volatile float m0pk_synth2 = 0.0f;
volatile float m0pk_delay  = 0.0f;
volatile float m0pk_reverb = 0.0f;
volatile float m0pk_out    = 0.0f;
volatile uint32_t m0Bad     = 0;   // NaN or Inf samples seen on the final mix

// a > pk   is false for NaN, so a NaN never raises the peak; the second test is
// written as !(a < 1e9f) precisely because it is true for NaN and for Inf.
#define M0_TRACK(pk, v) { float m0a_ = ((v) < 0.0f) ? -(v) : (v); \
                          if (m0a_ > (pk)) (pk) = m0a_;             \
                          if (!(m0a_ < 1e9f)) m0Bad++; }
#endif

#if BENCH_AUDIO_HEADROOM
// See config.h for what these mean and why the worst buffer is the number that counts.
// bahFillUs and bahBlockUs are the hand-off: i2s_output() measures its own two phases
// and leaves them here, because that is the only place the float->int16 loop and the
// blocking write can be timed apart from each other. audio_task1 picks them up on the
// far side of the call.
volatile uint32_t bahMaxGenMixUs = 0;
volatile uint32_t bahMaxFillUs   = 0;
volatile uint32_t bahMaxBlockUs  = 0;
volatile uint32_t bahMaxCpuUs    = 0;
volatile uint32_t bahSumCpuUs    = 0;
volatile uint32_t bahCount       = 0;
volatile uint32_t bahOverruns    = 0;
volatile uint32_t bahFillUs      = 0;
volatile uint32_t bahBlockUs     = 0;
#endif

// Audio buffers of all kinds
volatile int current_gen_buf = 0; // set of buffers for generation
volatile int current_out_buf = 1 - 0; // set of buffers for output
static float DRAM_ATTR WORD_ALIGNED_ATTR  synth1_buf[2][DMA_BUF_LEN];    // synth1 mono
static float DRAM_ATTR WORD_ALIGNED_ATTR  synth2_buf[2][DMA_BUF_LEN];    // synth2 mono
static float DRAM_ATTR WORD_ALIGNED_ATTR  drums_buf_l[2][DMA_BUF_LEN];   // drums L
static float DRAM_ATTR WORD_ALIGNED_ATTR  drums_buf_r[2][DMA_BUF_LEN];   // drums R
static float DRAM_ATTR WORD_ALIGNED_ATTR  mix_buf_l[2][DMA_BUF_LEN];     // mix L channel
static float DRAM_ATTR WORD_ALIGNED_ATTR  mix_buf_r[2][DMA_BUF_LEN];     // mix R channel
static union {                              // a dirty trick, instead of true converting
  int16_t WORD_ALIGNED_ATTR _signed[DMA_BUF_LEN * 2];
  uint16_t WORD_ALIGNED_ATTR _unsigned[DMA_BUF_LEN * 2];
} out_buf[2];                               // i2s L+R output buffer
size_t bytes_written;                       // i2s result

volatile boolean processing = false;
#ifndef NO_PSRAM
volatile float rvb_k1, rvb_k2, rvb_k3;
#endif
volatile float dly_k1, dly_k2, dly_k3;

// tasks for Core0 and Core1
TaskHandle_t SynthTask1;
TaskHandle_t SynthTask2;

// 303-like synths
SynthVoice Synth1(0); 
SynthVoice Synth2(1); 

// 808-like drums
Sampler Drums( DEFAULT_DRUMKIT ); // argument: starting drumset [0 .. total-1]

// Global effects
FxDelay Delay;
#ifndef NO_PSRAM
FxReverb Reverb;
#endif
Compressor Comp;

hw_timer_t * timer1 = NULL;            // Timer variables
hw_timer_t * timer2 = NULL;            // Timer variables
portMUX_TYPE timer1Mux = portMUX_INITIALIZER_UNLOCKED; 
portMUX_TYPE timer2Mux = portMUX_INITIALIZER_UNLOCKED; 
volatile boolean timer1_fired = false;
volatile boolean timer2_fired = false;

/*
 * Timer interrupt handler **********************************************************************************************************************************
*/

void IRAM_ATTR onTimer1() {
   portENTER_CRITICAL_ISR(&timer1Mux);
   timer1_fired = true;
   portEXIT_CRITICAL_ISR(&timer1Mux);
}
 
void IRAM_ATTR onTimer2() {
   portENTER_CRITICAL_ISR(&timer2Mux);
   timer2_fired = true;
   portEXIT_CRITICAL_ISR(&timer2Mux);
}

/* 
 * Core Tasks ************************************************************************************************************************
*/
// Core0 task 
// static void audio_task1(void *userData) {
static void IRAM_ATTR audio_task1(void *userData) {
  vTaskDelay(50);  
  while (true) {
    taskYIELD(); 
//    if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) { // we need all the generators to fill the buffers here, so we wait
      c0t = micros();
      
//      taskYIELD(); 
      
      current_gen_buf = current_out_buf;      // swap buffers
      current_out_buf = 1 - current_gen_buf;
      
    //  xTaskNotifyGive(SynthTask2);            // if we are here, then we've already received a notification from task2
      
      s1t = micros();
      synth1_generate();
      s1T = micros() - s1t;
      
      s2t = micros();
      synth2_generate();
      s2T = micros() - s2t;
      
  //    taskYIELD(); 

      drt = micros();
      drums_generate();
      drT = micros() - drt;
      
      c1t = micros();
      fxt = micros();
      mixer(); 
      fxT = micros() - fxt;
      
      i2s_output();

#if BENCH_AUDIO_HEADROOM
      // M1's precondition. s1T/s2T/drT/fxT already exist above, so this adds no
      // micros() calls of its own -- it reuses spans the author was already taking.
      // That matters for two reasons: the measurement barely perturbs what it
      // measures, and the four spans are each measured with a trailing micros() call
      // inside them, so this total is a slight OVER-estimate of pure work. Headroom
      // wants the pessimistic number.
      //
      // Deliberately NOT included: the time spent blocked inside I2S.write(). That is
      // the DMA pacing the loop, not the CPU working; counting it would report the
      // period back to us as if it were our own cost and always show 100%.
      {
        uint32_t bahG_ = s1T + s2T + drT + fxT;
        uint32_t bahC_ = bahG_ + bahFillUs;
        if ( bahG_  > bahMaxGenMixUs ) bahMaxGenMixUs = bahG_;
        if ( bahFillUs  > bahMaxFillUs   ) bahMaxFillUs   = bahFillUs;
        if ( bahBlockUs > bahMaxBlockUs  ) bahMaxBlockUs  = bahBlockUs;
        if ( bahC_  > bahMaxCpuUs    ) bahMaxCpuUs    = bahC_;
        bahSumCpuUs += bahC_;
        bahCount++;
        if ( bahC_ > (uint32_t)DMA_BUF_TIME ) bahOverruns++;
      }
#endif

 //   }
    
   // taskYIELD();

    taskYIELD();

    c0T = micros() - c0t;
  }
}

// task for Core1, which tipically runs user's code on ESP32
// static void IRAM_ATTR audio_task2(void *userData) {
static void IRAM_ATTR audio_task2(void *userData) {
  vTaskDelay(50);
  while (true) {
    // M0: this used to be taskYIELD(), which was the whole bug. taskYIELD() only
    // hands the CPU to a task of equal or higher priority, so it cannot yield to
    // anything. This task is pinned to core 1 at priority 5 while Arduino's loopTask
    // -- the task that runs loop() and therefore regular_checks() -- is on the same
    // core at priority 1. Spinning here at "priority 5 yielding to nothing" starved
    // loopTask permanently the moment this task started.
    //
    // That is not only why the diagnostics went quiet: it means everything the jukebox
    // needs from loop() stopped running after setup, which is a live candidate for the
    // original symptom (one hit at power-on, then noise) on its own.
    //
    // The matching vTaskDelay(1) is at the bottom of the loop, so every iteration
    // sleeps once: there is no path through here that can spin.
 /*   
    if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) { // wait for the notification from the SynthTask1

      
      taskYIELD();
    
      
      xTaskNotifyGive(SynthTask1); 
    }    
 */
    c1T = micros() - c1t;

    art = micros();
    
    if (timer2_fired) {
      timer2_fired = false;
#ifdef TEST_POTS      
       readPots();
#endif
       
#ifdef DEBUG_TIMING
        DEBF ("CORE micros: synt1, synt2, drums, mixer, DMA_LEN\t%d\t%d\t%d\t%d\t%d\r\n" , s1T, s2T, drT, fxT, DMA_BUF_TIME);
        //    DEBF ("TaskCore0=%dus TaskCore1=%dus DMA_BUF=%dus\r\n" , c0T , c1T , DMA_BUF_TIME);
        //    DEBF ("AllTheRestCore1=%dus\r\n" , arT);
#endif
    }    
    
//    taskYIELD();
    arT = micros() - art;
    // Replaces the taskYIELD() at the top of the loop; see the note there.
    vTaskDelay(1);
  }
}


/* 
 *  Quite an ordinary SETUP() *******************************************************************************************************************************
*/

void setup(void) {

#ifdef DEBUG_ON 
  DEBUG_PORT.begin(115200); 
  delay(50);
  // M0: the port probe that used to live here is gone. It existed only to find out
  // which USB path this board actually wires, and that is settled: DEBUG_PORT is
  // HWCDCSerial and the log arrives on the USB-OTG port. Its markers printed on every
  // boot forever to answer a question nobody asks any more.
  DEBUG_PORT.println("[M0] log port: HWCDC (native USB) on USB-OTG");
  DEBUG_PORT.flush();
#endif
delay(200);

  btStop(); // we don't want bluetooth to consume our precious cpu time 

  MidiInit(); // init midi input and handling of midi events

  /*
    for (int i = 0; i < GPIO_BUTTONS; i++) {
    pinMode(buttonGPIOs[i], INPUT_PULLDOWN);
    }
  */

  buildTables();

  for (int i = 0; i < POT_NUM; i++) pinMode( POT_PINS[i] , INPUT);

  Synth1.Init();
  Synth2.Init();
  Drums.Init();
  eng_init(); // M1: the event queue. After the engines are up, because anything posting an
              // event before this applies straight through instead of queueing.
#ifndef NO_PSRAM
  Reverb.Init();
#endif
  Delay.Init();
  Comp.Init(SAMPLE_RATE);
#ifdef JUKEBOX
  init_midi(); // AcidBanger function
#endif

  // silence while we haven't loaded anything reasonable
  for (int i = 0; i < DMA_BUF_LEN; i++) {
    drums_buf_l[current_gen_buf][i] = 0.0f ;
    drums_buf_r[current_gen_buf][i] = 0.0f ;
    synth1_buf[current_gen_buf][i] = 0.0f ;
    synth2_buf[current_gen_buf][i] = 0.0f ;
    out_buf[current_out_buf]._signed[i * 2] = 0 ;
    out_buf[current_out_buf]._signed[i * 2 + 1] = 0 ;
    mix_buf_l[current_out_buf][i] = 0.0f;
    mix_buf_r[current_out_buf][i] = 0.0f;
  }

  i2sInit();
  // i2s_write(i2s_num, out_buf[current_out_buf]._signed, sizeof(out_buf[current_out_buf]._signed), &bytes_written, portMAX_DELAY);

  //xTaskCreatePinnedToCore( audio_task1, "SynthTask1", 8000, NULL, (1 | portPRIVILEGE_BIT), &SynthTask1, 0 );
  //xTaskCreatePinnedToCore( audio_task2, "SynthTask2", 8000, NULL, (1 | portPRIVILEGE_BIT), &SynthTask2, 1 );
  // M0: priority was 1, equal to Arduino's loopTask (which runs regular_checks()
  // here). That is safe only while loop() stays trivial. Later milestones add
  // TFT redraws and a sequencer, so the audio tasks get headroom now.
  //
  // Raising this to 5 is only safe because audio_task2 no longer spins hot. It is
  // pinned to the same core as loopTask, so at equal-or-higher priority a busy task2
  // takes the core away from loop() for good -- taskYIELD() will not save it, since
  // taskYIELD only yields to tasks of equal or higher priority. The two priorities
  // here must be read together with audio_task2's vTaskDelay(1), not on their own.
  xTaskCreatePinnedToCore( audio_task1, "SynthTask1", 5000, NULL, 5, &SynthTask1, 0 );
  xTaskCreatePinnedToCore( audio_task2, "SynthTask2", 5000, NULL, 5, &SynthTask2, 1 );

  // somehow we should allow tasks to run
  xTaskNotifyGive(SynthTask1);
  //  xTaskNotifyGive(SynthTask2);
  processing = true;

#if ESP_ARDUINO_VERSION_MAJOR < 3 
  // timer interrupt
  /*
  timer1 = timerBegin(0, 80, true);               // Setup timer for midi
  timerAttachInterrupt(timer1, &onTimer1, true);  // Attach callback
  timerAlarmWrite(timer1, 4000, true);            // 4000us, autoreload
  timerAlarmEnable(timer1);
  */
  timer2 = timerBegin(1, 80, true);               // Setup general purpose timer
  timerAttachInterrupt(timer2, &onTimer2, true);  // Attach callback
  timerAlarmWrite(timer2, 200000, true);          // 200ms, autoreload 
  timerAlarmEnable(timer2);
  
#else 
  timer2 = timerBegin(1000000);               // Setup general purpose timer
  timerAttachInterrupt(timer2, &onTimer2);  // Attach callback
  timerAlarm(timer2, 200000, true, 0);          // 200ms, autoreload
#endif
DEBUG("setup done");
}

static uint32_t last_ms = micros();

/* 
 *  Finally, the LOOP () ***********************************************************************************************************
*/

void loop() { // default loopTask running on the Core1
  // you can still place some of your code here
  // or   vTaskDelete(NULL);
  
  // processButtons();
  regular_checks();    
  taskYIELD(); // this can wait
}

/* 
 *  Some debug and service routines *****************************************************************************************************************************
*/

void readPots() {
  static const float snap = 0.003f;
  static int i = 0;
  float tmp;
  static const float NORMALIZE_ADC = 1.0f / 4096.0f;
//read one pot per call
  tmp = (float)analogRead(POT_PINS[i]) * NORMALIZE_ADC;
  if (fabs(tmp - param[i]) > snap) {
    param[i] = tmp;
  //  paramChange(i, tmp);
  }

  i++;
  // if (i >= POT_NUM) i=0;
  i %= POT_NUM;
}

void paramChange(uint8_t paramNum, float paramVal) {
  // paramVal === param[paramNum];
  DEBF ("param %d val %0.4f\r\n" , paramNum, paramVal);
  paramVal *= 127.0;
  // M1: routed through eng_setParam() rather than calling Synth2.ParseCC() directly.
  //
  // Nothing calls this function -- the only call site is commented out at AcidBox.ino:470
  // -- so it is not a second writer today. It is routed anyway, for two reasons. The
  // invariant that engine parameters have exactly one writer should not be something that
  // quietly stops holding the moment somebody revives a dead function, and the float here
  // was being handed to ParseCC(uint8_t, uint8_t) as an implicit truncation. Going
  // through the event layer makes that narrowing an explicit cast at the boundary, where
  // it can be seen.
  switch (paramNum) {
    case 0:
      //set_bpm( 40.0f + (paramVal * 160.0f));
      eng_setParam(Ch::Second, CC_303_CUTOFF, (uint8_t)paramVal);
      break;
    case 1:
      eng_setParam(Ch::Second, CC_303_RESO, (uint8_t)paramVal);
      break;
    case 2:
      eng_setParam(Ch::Second, CC_303_OVERDRIVE, (uint8_t)paramVal);
      eng_setParam(Ch::Second, CC_303_DISTORTION, (uint8_t)paramVal);
      break;
    case 3:
      eng_setParam(Ch::Second, CC_303_ENVMOD_LVL, (uint8_t)paramVal);
      break;
    case 4:
      eng_setParam(Ch::Second, CC_303_ACCENT_LVL, (uint8_t)paramVal);
      break;
    default:
      {}
  }
}


#ifdef JUKEBOX
void jukebox_tick() {
  run_tick();
  myRandomAddEntropy((uint16_t)(micros() & 0x0000FFFF));
}
#endif


void regular_checks() {
  timer1_fired = false;

#if M0_DIAG
  // Counted first, before anything else in this function can misbehave, so that a
  // rising loops= in the report proves loop() on core 1 is still running. Every other
  // number in this diagnostic assumes it is, and that was never checked. Three runs
  // produced no report line at all, and the question worth answering is whether this
  // function is reached: jukebox_tick() -> run_tick() -> run_ui() all sit ahead of
  // the reporting code, so a stall in any of them would be indistinguishable from
  // silence on the wire. (Those three were audited and none of them blocks, but the
  // counter settles it from the device rather than from reading the source.)
  static uint32_t m0Loops = 0;
  m0Loops++;
#endif
  
#ifdef MIDI_VIA_SERIAL
  MIDI.read();
#endif

#ifdef MIDI_VIA_SERIAL2
  MIDI2.read();
#endif
  
#ifdef MIDI_USB_DEVICE
  MIDI_usbDev.read();
#endif

#ifdef JUKEBOX
  jukebox_tick();
#endif

  // M1: apply everything that was posted above -- MIDI events from MIDI.read(), and
  // sequencer events from jukebox_tick() -- in one pass, in arrival order.
  //
  // Here rather than at the top of the function so that the drain happens in the SAME
  // loop() iteration the events were queued in. That is what keeps this from costing
  // MIDI a tick of latency: enqueue and dequeue inside one pass, and the queue is pure
  // bookkeeping. Move this call and the note-on path acquires a scheduling delay, which
  // is audible on fast passages and easy to misattribute to the sequencer.
  eng_poll();

  // Events dropped for want of queue space. Expected to stay zero; see ENGINE_QUEUE_LEN
  // in engine_iface.h for the arithmetic. Reported only when nonzero, and cumulative, so
  // a single quiet moment does not hide it -- a drop is an audible gap and it has to be
  // traceable to when it happened.
#ifdef DEBUG_ON
  static uint32_t engDropsSeen = 0;
  if ( eng_drops() != engDropsSeen ) {
    engDropsSeen = eng_drops();
    DEBF("[WARN] event queue overflow: %u events dropped (total). Drain is too slow for the "
         "event rate -- raise ENG_DRAIN_MAX or ENGINE_QUEUE_LEN.\r\n",
         (unsigned)engDropsSeen);
  }
#endif

  // M0: the sampler's play cursor ran off the end of a sample. Reported here rather
  // than in Sampler::Process(), which runs in the IRAM audio task and must not print.
  // Rate limited to every 2000 calls, and gated on DEBUG_ON rather than M0_DIAG: this
  // one earns its place permanently. The guard in sampler.ino is a real fix, and this
  // is how you find out it is firing. It was last seen at roughly one call per two
  // seconds, which is the routine end-of-sample overshoot that fractional pitch makes
  // and not a fault -- so it is a warning, not an error, and it should stay quiet on a
  // healthy kit. If it goes from occasional to continuous, that is new information.
#ifdef DEBUG_ON
  static uint32_t tick = 0;
  static uint32_t reported = 0;
  if ( ++tick >= 2000 ) {
    tick = 0;
    if ( Drums.GetOobReads() != reported ) {
      DEBF("[WARN] sampler play cursor left the sample or the cache: %d (new since last report)\r\n",
           (int)(Drums.GetOobReads() - reported));
      reported = Drums.GetOobReads();
    }
  }
#endif

#if BENCH_AUDIO_HEADROOM
  // M1's precondition, reported from here for the same reason as the warning above:
  // audio_task1 is IRAM and must not print. Read-and-clear rather than accumulate, so
  // the maxima are per-window and a single bad buffer shows up in the window it
  // happened instead of being averaged away by the rest of the run.
  static uint32_t bahLast = 0;
  static bool     bahLegend = false;
  uint32_t bahNow = millis();
  if ( bahNow - bahLast >= (uint32_t)BAH_MS ) {
    bahLast = bahNow;
    if ( !bahLegend ) {
      bahLegend = true;
      DEBF("[BAH] core-0 load: worst buffer vs the %d us the DMA gives it. "
           "cpu = generators + mixer + float->int16. block = time parked in I2S.write(), "
           "which is the DMA pacing us and is NOT our cost.\r\n", (int)DMA_BUF_TIME);
    }
    if ( bahCount > 0 ) {
      // Percentages as hundredths of a percent so no float formatting is needed here;
      // this is printf territory on core 1, but keeping it integer keeps it honest.
      uint32_t bahWorstPct = (bahMaxCpuUs   * 10000u) / (uint32_t)DMA_BUF_TIME;
      uint32_t bahMeanUs   = bahSumCpuUs / bahCount;
      uint32_t bahMeanPct  = (bahMeanUs    * 10000u) / (uint32_t)DMA_BUF_TIME;
      DEBF("[BAH] worst cpu=%u us = %u.%02u%% (gen+mix %u / fill %u)  "
           "mean cpu=%u us = %u.%02u%%  block max=%u us  overruns=%u of %u buffers\r\n",
           (unsigned)bahMaxCpuUs,    (unsigned)(bahWorstPct / 100u), (unsigned)(bahWorstPct % 100u),
           (unsigned)bahMaxGenMixUs, (unsigned)bahMaxFillUs,
           (unsigned)bahMeanUs,      (unsigned)(bahMeanPct / 100u), (unsigned)(bahMeanPct % 100u),
           (unsigned)bahMaxBlockUs,  (unsigned)bahOverruns, (unsigned)bahCount);
      // bahCount is also an independent check on the sample rate: it counts buffer
      // refills, so count * DMA_BUF_LEN / window seconds should come out at 44100.
      // If this number ever disagrees with the mixer() rate M0 measured, one of the
      // two is lying.
      //
      // 64-bit on purpose. The 32-bit form (count * DMA_BUF_LEN * 1000 / BAH_MS) works
      // at BAH_MS 1000 and silently wraps if anyone raises the window, which is exactly
      // the kind of thing that is correct on the bench and wrong in the field.
      DEBF("[BAH] buffer rate check: %u buffers in %d ms = %u Hz (expect %d)\r\n",
           (unsigned)bahCount, (int)BAH_MS,
           (unsigned)(((uint64_t)bahCount * (uint64_t)DMA_BUF_LEN * 1000ULL)
                      / (uint64_t)BAH_MS),
           (int)SAMPLE_RATE);
    } else {
      DEBF("[BAH] core0: no buffers completed in %d ms -- audio_task1 is not running\r\n",
           (int)BAH_MS);
    }
    bahMaxGenMixUs = 0; bahMaxFillUs = 0; bahMaxBlockUs = 0; bahMaxCpuUs = 0;
    bahSumCpuUs = 0; bahCount = 0; bahOverruns = 0;
  }
#endif

#if M0_DIAG
  // M0: report the measured peaks, then advance to the next test mode.
  //
  // This cycles on a timer instead of waiting for a button because the M0 wiring is a
  // DAC cable and USB only. An earlier attempt switched on a momentary short of GPIO23;
  // on this WROOM board that pin is not broken out and reads LOW regardless, so the
  // bypass was stuck ON from the first loop and the experiment never ran.
  //
  // One pass through modes 0, 1, 2 is a complete experiment, and comparing the reverb
  // row and the out row across them settles the reverb question without anyone having
  // to judge loudness by ear.
  // The first report must NOT fire on the first loop(). m0LastReport used to start at
  // 0 while millis() is already past M0_DIAG_MS by the time setup() returns, so it
  // sampled immediately -- right after setup() zeroed every buffer to
  // "silence while we haven't loaded anything reasonable", and before the audio task
  // had produced a sample. Every bus duly read 0.0000, which measured the pre-roll
  // rather than the fault. Start the clock here so the first window is a real one.
  static uint32_t m0LastReport = 0;
  static uint32_t m0LastMode   = 0;
  static bool     m0Clocked = false;
  static bool     m0Legend = false;
  uint32_t m0NowMs = millis();
  if ( !m0Clocked ) { m0LastReport = m0NowMs; m0LastMode = m0NowMs; m0Clocked = true; }
  else if ( (uint32_t)(m0NowMs - m0LastReport) >= (uint32_t)M0_DIAG_MS ) {
    m0LastReport = m0NowMs;
    // The mode advances on its own, slower clock than the report. Mode 2 writes a
    // known-silent buffer and whether the amplifier is still noisy with it is the one
    // question no log can answer, so it needs three seconds of ear time rather than
    // half a second of log time.
    if ( (uint32_t)(m0NowMs - m0LastMode) >= (uint32_t)M0_DIAG_MODE_MS ) {
      m0LastMode = m0NowMs;
      m0Mode = (uint8_t)((m0Mode + 1u) % 3u);
      m0ReverbBypass = (m0Mode == 1);
    }
    // Say what the modes mean once, then never again. A log that arrives without the
    // operator having read a commit message should still be readable on its own.
    if ( !m0Legend ) {
      m0Legend = true;
      DEBF("[M0] mode holds %d ms, reported every %d ms: 0=normal 1=reverb bypassed 2=out_buf forced to 0 (DAC should be SILENT)\r\n",
           (int)M0_DIAG_MODE_MS, (int)M0_DIAG_MS);
    }
    // m0Loops first: a rising count proves loop() on core 1 is alive, which separates
    // "the reporting code never runs" from "it runs and the audio is quiet". m0MixerCalls
    // second: a rising count proves the core-0 audio task is alive, which separates
    // "audio never ran" from "audio ran and was silent".
    DEBF("[M0] mode=%u loops=%u mixer=%u drums=%.4f synth1=%.4f synth2=%.4f delay=%.4f reverb=%.4f out=%.4f bad=%d\r\n",
         (unsigned)m0Mode, (unsigned)m0Loops, (unsigned)m0MixerCalls,
         (double)m0pk_drums, (double)m0pk_synth1, (double)m0pk_synth2,
         (double)m0pk_delay, (double)m0pk_reverb, (double)m0pk_out,
         (int)m0Bad);
    // M0: voice liveness. env=1 means the amp envelope is still running, so the voice
    // is still producing samples. n is the note-allocator depth and note the note it
    // is holding. on/off are this window's counts, so "on climbs, off stays 0" is a
    // sequencer that never releases, and "env=1 with off climbing" is a release that
    // is not reaching the envelope. Those are different faults and this separates them.
    DEBF("[M0] s1 on=%u off=%u env=%d n=%d note=%d | s2 on=%u off=%u env=%d n=%d note=%d\r\n",
         (unsigned)m0NoteOn[0], (unsigned)m0NoteOff[0],
         Synth1.AmpEnv.isRunning() ? 1 : 0, (int)Synth1.mvaStack.n, (int)Synth1.mvaStack.notes[0],
         (unsigned)m0NoteOn[1], (unsigned)m0NoteOff[1],
         Synth2.AmpEnv.isRunning() ? 1 : 0, (int)Synth2.mvaStack.n, (int)Synth2.mvaStack.notes[0]);
    // M0: the write side. Every bus above is healthy, so if the amplifier is noisy the
    // fault is here. short=0 means I2S accepted every byte and the digital stream to
    // the DAC is intact, which moves the fault to the DAC module or the analog side.
    // oobCache>0 would mean the sampler cursor left the cache, which nothing else in
    // this run has shown; oobSample is the routine end-of-sample case.
    DEBF("[M0] i2s=%u short=%u shortBytes=%u oobSample=%u oobCache=%u\r\n",
         (unsigned)m0I2SCalls, (unsigned)m0ShortWrites, (unsigned)m0ShortBytes,
         (unsigned)m0OobSample, (unsigned)m0OobCache);
    m0MixerCalls = 0;
    m0I2SCalls = 0;
    m0ShortWrites = 0;
    m0ShortBytes = 0;
    m0OobSample = 0;
    m0OobCache = 0;
    m0NoteOn[0] = 0;  m0NoteOn[1] = 0;
    m0NoteOff[0] = 0; m0NoteOff[1] = 0;
    m0pk_drums = 0.0f;  m0pk_synth1 = 0.0f;  m0pk_synth2 = 0.0f;
    m0pk_delay = 0.0f;  m0pk_reverb = 0.0f;  m0pk_out = 0.0f;
    m0Bad = 0;
  }
#endif

}


inline void IRAM_ATTR drums_generate() {
    for (int i=0; i < DMA_BUF_LEN; i++){
      Drums.Process( &drums_buf_l[current_gen_buf][i], &drums_buf_r[current_gen_buf][i] );      
    } 
}

inline void IRAM_ATTR synth1_generate() {
    for (int i=0; i < DMA_BUF_LEN; i++){
      synth1_buf[current_gen_buf][i] = Synth1.getSample() ;      
    } 
}

inline void IRAM_ATTR synth2_generate() {
    for (int i=0; i < DMA_BUF_LEN; i++){
      synth2_buf[current_gen_buf][i] = Synth2.getSample() ;      
    } 
}

void IRAM_ATTR mixer() { // sum buffers 
#if M0_DIAG
  m0MixerCalls++;
#endif
#ifdef DEBUG_MASTER_OUT
  static float meter = 0.0f;
#endif
  static float synth1_out_l, synth1_out_r, synth2_out_l, synth2_out_r, drums_out_l, drums_out_r;
  static float dly_l, dly_r, rvb_l, rvb_r;
  static float mono_mix;
    dly_k1 = Synth1._sendDelay;
    dly_k2 = Synth2._sendDelay;
    dly_k3 = Drums._sendDelay;
#ifndef NO_PSRAM 
    rvb_k1 = Synth1._sendReverb;
    rvb_k2 = Synth2._sendReverb;
    rvb_k3 = Drums._sendReverb;
#endif
    for (int i=0; i < DMA_BUF_LEN; i++) { 
      drums_out_l = drums_buf_l[current_out_buf][i];
      drums_out_r = drums_buf_r[current_out_buf][i];

      synth1_out_l = Synth1.GetPan() * synth1_buf[current_out_buf][i];
      synth1_out_r = (1.0f - Synth1.GetPan()) * synth1_buf[current_out_buf][i];
      synth2_out_l = Synth2.GetPan() * synth2_buf[current_out_buf][i];
      synth2_out_r = (1.0f - Synth2.GetPan()) * synth2_buf[current_out_buf][i];

      
      dly_l = dly_k1 * synth1_out_l + dly_k2 * synth2_out_l + dly_k3 * drums_out_l; // delay bus
      dly_r = dly_k1 * synth1_out_r + dly_k2 * synth2_out_r + dly_k3 * drums_out_r;
      Delay.Process( &dly_l, &dly_r );
#ifndef NO_PSRAM
  #if M0_DIAG
      // M0: bypass, so the bench symptom can be attributed to the reverb or ruled out
      // without a rebuild. Reverb.Process() adds its own output back into the input
      // unconditionally (fx_reverb.h), so the tail never dies once anything excites it.
      if ( m0ReverbBypass ) {
        rvb_l = 0.0f;
        rvb_r = 0.0f;
      } else {
  #endif
      rvb_l = rvb_k1 * synth1_out_l + rvb_k2 * synth2_out_l + rvb_k3 * drums_out_l; // reverb bus
      rvb_r = rvb_k1 * synth1_out_r + rvb_k2 * synth2_out_r + rvb_k3 * drums_out_r;
      Reverb.Process( &rvb_l, &rvb_r );
  #if M0_DIAG
      }
  #endif

      mix_buf_l[current_out_buf][i] = (synth1_out_l + synth2_out_l + drums_out_l + dly_l + rvb_l);
      mix_buf_r[current_out_buf][i] = (synth1_out_r + synth2_out_r + drums_out_r + dly_r + rvb_r);
#else
      mix_buf_l[current_out_buf][i] = (synth1_out_l + synth2_out_l + drums_out_l + dly_l);
      mix_buf_r[current_out_buf][i] = (synth1_out_r + synth2_out_r + drums_out_r + dly_r);
#endif
#if M0_DIAG
      // M0: measure which bus is actually loud, instead of guessing by ear. Left
      // channel is representative -- the right differs only by pan. m0Bad counts NaN
      // and Inf on the final mix, which is what a numerical blow-up looks like and is
      // a candidate explanation for noise that never stops.
      M0_TRACK(m0pk_drums,  drums_out_l);
      M0_TRACK(m0pk_synth1, synth1_out_l);
      M0_TRACK(m0pk_synth2, synth2_out_l);
      M0_TRACK(m0pk_delay,  dly_l);
      M0_TRACK(m0pk_reverb, rvb_l);
#endif
      mono_mix = 0.5f * (mix_buf_l[current_out_buf][i] + mix_buf_r[current_out_buf][i]);
  //    Comp.Process(mono_mix);     // calculate gain based on a mono mix

      Comp.Process(drums_out_l*0.25f);  // calc compressor gain, side-chain driven by drums


      mix_buf_l[current_out_buf][i] = (Comp.Apply( 0.25f * mix_buf_l[current_out_buf][i]));
      mix_buf_r[current_out_buf][i] = (Comp.Apply( 0.25f * mix_buf_r[current_out_buf][i]));

      
#ifdef DEBUG_MASTER_OUT
      if ( i % 16 == 0) meter = meter * 0.95f + fabs( mono_mix); 
#endif
  //    mix_buf_l[current_out_buf][i] = fclamp(mix_buf_l[current_out_buf][i] , -1.0f, 1.0f); // clipper
  //    mix_buf_r[current_out_buf][i] = fclamp(mix_buf_r[current_out_buf][i] , -1.0f, 1.0f);
     mix_buf_l[current_out_buf][i] = fast_shape( mix_buf_l[current_out_buf][i]); // soft limitter/saturator
     mix_buf_r[current_out_buf][i] = fast_shape( mix_buf_r[current_out_buf][i]);
#if M0_DIAG
     // M0: measured HERE, not on the raw bus sum. The probe used to sit before the
     // 0.25f gain and before fast_shape, so it reported 2.5568 for a signal that was
     // actually about 0.64 and cleanly saturated -- it was reading pre-gain and
     // saying nothing about what reaches the DAC. This is the level that matters.
     M0_TRACK(m0pk_out, mix_buf_l[current_out_buf][i]);
#endif
   }
#ifdef DEBUG_MASTER_OUT
  meter *= 0.95f;
  meter += fabs(mono_mix); 
  DEBF("out= %0.5f\r\n", meter);
#endif
}
