// M1: every handler below delegates to engine_iface. The include is redundant while
// "engine_iface" sorts before "midi_handler" -- which is the only reason this delegation
// points the way it does -- but the dependency should not rest on alphabetical order.
#include "engine_iface.h"

inline void MidiInit() {
  
#ifdef MIDI_VIA_SERIAL
  MIDI_PORT.begin(115200);
#endif
#ifdef MIDI_VIA_SERIAL2
  pinMode( MIDIRX_PIN , INPUT_PULLDOWN);
  pinMode( MIDITX_PIN , OUTPUT);
  Serial2.begin( 31250, SERIAL_8N1, MIDIRX_PIN, MIDITX_PIN ); // midi port
#endif

#ifdef MIDI_VIA_SERIAL
  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);
  MIDI.setHandleControlChange(handleCC);
  MIDI.setHandlePitchBend(handlePitchBend);
  MIDI.setHandleProgramChange(handleProgramChange);
  MIDI.begin(MIDI_CHANNEL_OMNI);
#endif
#ifdef MIDI_VIA_SERIAL2
  MIDI2.setHandleNoteOn(handleNoteOn);
  MIDI2.setHandleNoteOff(handleNoteOff);
  MIDI2.setHandleControlChange(handleCC);
  MIDI2.setHandlePitchBend(handlePitchBend);
  MIDI2.setHandleProgramChange(handleProgramChange);
  MIDI2.begin(MIDI_CHANNEL_OMNI);
#endif
#ifdef MIDI_USB_DEVICE
  // /* Change USB Device Descriptor Parameter
  USB.VID(0x1209);
  USB.PID(0x1305);
  USB.productName("AcidBox S3");
  USB.manufacturerName("copych");
  //USB.serialNumber("0000");
  //USB.firmwareVersion(0x0000);
  USB.usbVersion(0x0200);
  USB.usbClass(TUSB_CLASS_AUDIO);
  USB.usbSubClass(0x00);
  USB.usbProtocol(0x00);
  USB.usbAttributes(0x80);
  // */ 
  
  MIDI_usbDev.setHandleNoteOn(handleNoteOn);
  MIDI_usbDev.setHandleNoteOff(handleNoteOff);
  MIDI_usbDev.setHandleControlChange(handleCC);
  MIDI_usbDev.setHandlePitchBend(handlePitchBend);
  MIDI_usbDev.setHandleProgramChange(handleProgramChange);
  MIDI_usbDev.begin(MIDI_CHANNEL_OMNI);
#endif

}


// M1: every handler below now delegates to engine_iface instead of touching the engines
// directly, MIDI included. The reason MIDI is not left on the old path is in
// engine_iface.h -- one writer, and one implementation that both entry points share, so
// "the sequencer sounds like MIDI" stops being something to check by ear.
//
// Two behaviours deliberately stayed here rather than moving into the interface layer,
// because they are about the MIDI cable and not about the engines:
//
//   - the one-per-second rate limit on notes-off, via millis()-last_reset
//   - do_midi_stop() under JUKEBOX, because a MIDI panic should stop playback
//
// A sequencer calling eng_allNotesOff() gets plain engine silence and no rate limit.
// Dropping the jukebox is a decision the sequencer has no business making.

inline void handleNoteOn(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity) {
#ifdef DEBUG_MIDI
  DEB("MIDI note on ");
  DEBUG(inNote);
#endif
  Ch ch;
  if ( !chanToCh(inChannel, ch) ) return;   // omni mode: channels we don't own
  // accent=false on this path, and that is the faithful mapping. AcidBox expresses accent
  // as velocity >= 80 inside the voice (synthvoice.ino:223); it has no separate accent
  // flag on MIDI. Claiming accent here from velocity >= 80 would be the same test twice,
  // and would pin every accented MIDI note to exactly 127 and throw away how hard the
  // key was actually struck.
  eng_noteOn(ch, inNote, inVelocity, false);
}

inline void handleNoteOff(uint8_t inChannel, uint8_t inNote, uint8_t inVelocity) {
  (void)inVelocity;   // both engines discard it; see eng_apply() in engine_iface.ino
  Ch ch;
  if ( !chanToCh(inChannel, ch) ) return;
  eng_noteOff(ch, inNote);
}

inline void handleCC(uint8_t inChannel, uint8_t cc_number, uint8_t cc_value) {
  switch (cc_number) {
    // Only the stop group is handled here. Everything else -- including the global
    // compressor / delay / reverb CC_ANY_* parameters -- goes to eng_setParam, which
    // matches those on the CC number before any channel routing. Leaving them in this
    // function would have kept two implementations of the same dispatch, which is the
    // bug M1 exists to remove.
    case CC_ANY_RESET_CCS:
    case CC_ANY_NOTES_OFF:
    case CC_ANY_SOUND_OFF:
      if (inChannel == SYNTH1_MIDI_CHAN && millis()-last_reset>1000 ) {
#ifdef JUKEBOX
        do_midi_stop();
#endif
        eng_allNotesOff();
        last_reset = millis();
      }
      break;
    default: {
      Ch ch;
      if ( !chanToCh(inChannel, ch) ) return;
      eng_setParam(ch, cc_number, cc_value);
      break;
    }
  }
}

void handleProgramChange(uint8_t inChannel, uint8_t number) {
  // The channel check stays: which channel a program change arrived on is a MIDI fact,
  // and eng_selectProgram() takes none.
  if (inChannel == DRUM_MIDI_CHAN) {     eng_selectProgram(number);  }
}

inline void handlePitchBend(uint8_t inChannel, int number) {
  if (inChannel == DRUM_MIDI_CHAN )         {Drums.PitchBend(number);}
  else if (inChannel == SYNTH1_MIDI_CHAN )  {Synth1.PitchBend(number);}
  else if (inChannel == SYNTH2_MIDI_CHAN )  {Synth2.PitchBend(number);}
}
