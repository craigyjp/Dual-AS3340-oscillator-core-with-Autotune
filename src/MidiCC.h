//
//  MidiCC.h  --  MIDI CC map for the AS3340 autotune voice
//
//  Named constants for every control-change number the firmware responds to,
//  so the switch() in myControlChange() can read as `case CC_LFO1_RATE:`
//  instead of `case 102:`.
//
//  Channels:
//    Notes / velocity / pitch-bend / aftertouch : MIDI_CHANNEL (7)   -- per voice
//    Control changes (everything below)         : masterChan   (1)   -- common/master
//
//  Switch/trigger CCs act on value == 127 (momentary). Continuous CCs use the
//  full 0-127 range. Ranges/meanings are noted per line.
//
//  Delay group (55-58) intentionally shares the numbers used by the SuperSaw
//  oscillator, so one master/controller template drives both instruments.
//
//  Free for future use: anything not listed here.
//

#ifndef MIDICC_H
#define MIDICC_H

// ------------------------------------------------------------------ //
//  Performance / expression                                           //
// ------------------------------------------------------------------ //
#define CC_MOD_WHEEL          1    // Mod wheel -> FM & TM vibrato depth (range set by CC17/CC18)
#define CC_PORTAMENTO_TIME    5    // Glide time (0 = instant .. ~10 s)
#define CC_PORTAMENTO_SW      65   // Portamento on/off (127 = on, 0 = off)
#define CC_BEND_RANGE         16   // Pitch-bend range, 0-12 semitones

// ------------------------------------------------------------------ //
//  Voice / tuning                                                     //
// ------------------------------------------------------------------ //
#define CC_INTERVAL           14   // Interval for oscillator B, 0-12 semitones
#define CC_DETUNE             15   // Detune amount for oscillator B
#define CC_OCTAVE_A           21   // Osc A octave: 0 = -12, 1 = 0, 2 = +12
#define CC_OCTAVE_B           22   // Osc B octave: 0 = -12, 1 = 0, 2 = +12
#define CC_SYNC               29   // Hard-sync on/off (127 = on, 0 = off)

// ------------------------------------------------------------------ //
//  Keytrack  (channel_c filter CV)                                    //
// ------------------------------------------------------------------ //
#define CC_KEYTRACK_AMOUNT    105  // Keytrack amount, 0 - 100 % (pivots around note 60)
#define CC_KEYTRACK_SW        107  // Keytrack on/off (127 = on, 0 = off -> fixed note-60 CV)

// ------------------------------------------------------------------ //
//  Modulation depth trims                                             //
// ------------------------------------------------------------------ //
#define CC_FM_MOD_WHEEL       17   // How much the mod wheel affects FM
#define CC_TM_MOD_WHEEL       18   // How much the mod wheel affects TM
#define CC_FM_AT_WHEEL        19   // Aftertouch depth, applied to the CC109 destination
#define CC_TM_AT_WHEEL        20   // How much aftertouch affects TM
#define CC_FM_ACTUAL          28   // Direct FM amount (independent of wheel/AT)

// ------------------------------------------------------------------ //
//  Pulse width (per AS3340)                                           //
//    channel_e = PW1 + PWM1 (summed) , channel_g = PW2 + PWM2 (summed) //
// ------------------------------------------------------------------ //
#define CC_PW1                23   // VCO1 static pulse width
#define CC_PWM1_DEPTH         24   // VCO1 LFO2 PWM depth  (LFO2 depth, AS3340 #1)
#define CC_PW2                25   // VCO2 static pulse width
#define CC_PWM2_DEPTH         26   // VCO2 LFO2 PWM depth  (LFO2 depth, AS3340 #2)

// ------------------------------------------------------------------ //
//  LFO1  (TAPLFO3-D style: FM + Filter + Amp)                         //
// ------------------------------------------------------------------ //
#define CC_LFO1_RATE          102  // LFO1 rate, 0.05 - 12.8 Hz (exponential)
#define CC_LFO1_DEPTH         103  // LFO1 depth / level (FM amount)
#define CC_LFO1_WAVEFORM      104  // LFO1 waveform, 0 - 127 (see LfoWave in LFO.h)
#define CC_LFO1_MULT          106  // LFO1 tempo multiplier: 0.5/1/1.5/2/3/4
#define CC_LFO1_FILTER_DEPTH  110  // LFO1 -> Filter CV depth (channel_f, bipolar)
#define CC_LFO1_AMP_DEPTH     111  // LFO1 -> Amp CV depth    (channel_h, bipolar)

// ------------------------------------------------------------------ //
//  LFO1 delay machine  (same CC numbers as the SuperSaw oscillator)   //
// ------------------------------------------------------------------ //
#define CC_LFO1_DELAY_TIME    55   // Delay before LFO1 onset (0 = none, else 0.1 - 5 s)
#define CC_LFO1_DELAY_RAMP    56   // Ramp / fade-in after delay (0 = instant, else 0.05 - 3 s)
#define CC_LFO1_RETRIG        57   // Delay retrigger: >=64 on, <64 legato
#define CC_NOTES_HELD         58   // Notes-held flag from master: >=64 held, <64 all released

// ------------------------------------------------------------------ //
//  LFO2  (triangle -> PWM)                                            //
//    depth is per-chip via CC_PWM1_DEPTH / CC_PWM2_DEPTH above         //
// ------------------------------------------------------------------ //
#define CC_LFO2_RATE          108  // LFO2 rate, 0.05 - 20 Hz (exponential)

// ------------------------------------------------------------------ //
//  Aftertouch routing  (depth = CC19)                                 //
// ------------------------------------------------------------------ //
#define CC_AT_DESTINATION     109  // Raw 0-4: 0 Off, 1 DCO Mod, 2 Cutoff (handled by assigner), 3 VCF Mod, 4 VCA Mod

// ------------------------------------------------------------------ //
//  System / actions  (act on value == 127)                            //
// ------------------------------------------------------------------ //
#define CC_AUTOTUNE_START     121  // Start the autotune routine
#define CC_AUTOTUNE_RESET     122  // Zero all autotune corrections
#define CC_ALL_NOTES_OFF      123  // Panic / all notes off

#endif  // MIDICC_H