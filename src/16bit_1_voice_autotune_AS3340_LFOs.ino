/*
       MIDI to CV with Autotune

       For RP2350

       Filesystem 3.5Mb/0.5Mb

       MIDI_CHANNEL 1-6 for each RP2350

       masterChan = 9 for CC messages, pitchbend, aftertouch etc.

      Version 1.1

      Copyright (C) 2020 Craig Barnes

      This program is free software: you can redistribute it and/or modify
      it under the terms of the GNU General Public License as published by
      the Free Software Foundation, either version 3 of the License, or
      (at your option) any later version.

      This program is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
      GNU General Public License <http://www.gnu.org/licenses/> for more details.
*/
#include <Arduino.h>
#include <iostream>
#include <cfloat>  // Include for DBL_MAX
#include <SPI.h>
#include "pico/mutex.h"  // DAC/SPI shared between core 0 and core 1
#include <MIDI.h>
#include "Parameters.h"
#include "Hardware.h"
#include "MidiCC.h"  // named CC numbers
#include "LFO.h"  // dual internal LFO engine (LFO1 FM/filter/amp, LFO2 PWM)
#include <Adafruit_NeoPixel.h>
#include <FS.h>
#include <LittleFS.h>

auto_init_mutex(dacMutex);  // one SPI/DAC user at a time across both cores

struct VoiceAndNote {
  int note;
  int velocity;
  long timeOn;
  bool sustained;  // Sustain flag
  bool keyDown;
  double noteFreq;  // Note frequency
  int position;
  bool noteOn;
};

// Struct to store note data with derivatives
struct NoteData {
  int value;
  double derivative;
};

struct VoiceAndNote voices[NO_OF_VOICES] = {
  { -1, -1, 0, false, false, 0, -1, false },
};

boolean voiceOn[NO_OF_VOICES] = { false };
bool initial_loop = 1;

// MIDI setup

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

#define MIDI_CHANNEL 6

Adafruit_NeoPixel pixels(NUM_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin()) {
    // FS not mounted - e.g. no filesystem reserved in Tools > Flash Size.
    // Do NOT return here: the synth must still start. Autotune values will
    // just live in RAM (not persist) until a filesystem is available.
    Serial.println("Failed to mount LittleFS - running without saved tuning");
    Serial.println("  (reserve a filesystem in Tools > Flash Size to persist autotune values)");
  } else {
    Serial.println("LittleFS mounted successfully");
  }

  pinMode(AUTO_INPUT, INPUT_PULLUP);

  SPI.setSCK(2);
  SPI.setRX(4);
  SPI.setTX(3);
  SPI.begin();

  randomSeed(micros());  // seed the LFO sample & hold / random-slope waveforms

  delay(10);

  setupHardware();

  calibratePolymodCentre();

  //MIDI 5 Pin DIN
  MIDI.begin(0);
  MIDI.setHandleNoteOn(myNoteOn);
  MIDI.setHandleNoteOff(myNoteOff);
  MIDI.setHandlePitchBend(myPitchBend);
  MIDI.setHandleControlChange(myControlChange);
  MIDI.setHandleAfterTouchChannel(myAfterTouch);
  Serial.println("MIDI In DIN Listening");

  pixels.begin();
  delay(500);
  led_state = digitalRead(CLONE_LED);
  if (led_state) {
    pixels.setPixelColor(0, pixels.Color(0, 255, 0));  // RED
  } else {
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));  // RED
  }
  pixels.show();

  loadTuningCorrectionsFromSD();

  core0Ready = true;  // let core 1 start the VCO1 / poly mod loop
}

// ------------------------------------------------------------------ //
//  Core 1: VCO1 pitch + audio-rate poly mod                           //
//  Reads pin 28 and writes DAC channel A as fast as possible.         //
//  Core 0 computes everything else into vco1BaseCounts.               //
// ------------------------------------------------------------------ //
// Measure pin 28's rest voltage (~1.65 V). Averages out noise and any audio.
// Called at power-up and at the start of every autotune (patch loaded, poly mod at rest).
void calibratePolymodCentre() {
  long sum = 0;
  for (int n = 0; n < 256; n++) sum += analogRead(POLYMOD_INPUT);
  polymodCentre = sum / 256;
  Serial.printf("Poly mod centre: %d\n", polymodCentre);
}

void setup1() {
  while (!core0Ready) delay(1);
}

void loop1() {
  if (autotuneStart) return;  // autotune owns channel A and the ADC

  int pm = 0;
  if (polymodOn) {
    pm = analogRead(POLYMOD_INPUT) - polymodCentre;
    if (abs(pm) <= POLYMOD_DEADBAND) pm = 0;
  }

  int32_t v = vco1BaseCounts + (int32_t)(pm * POLYMOD_SCALE);
  v = constrain(v, 0, 65535);

  mutex_enter_blocking(&dacMutex);
  if (!autotuneStart) {  // autotune drives channel A itself
    dacWrite(DAC_NOTE1, (channel_a & 0xFFF0000F) | ((v & 0xFFFF) << 4));
  }
  mutex_exit(&dacMutex);
}

void saveTuningCorrectionsToSD() {
  // Remove the file if it exists
  if (LittleFS.exists("tuning.txt")) {
    LittleFS.remove("tuning.txt");
  }

  // Open the file for writing
  File file = LittleFS.open("tuning.txt", "w");
  if (file) {
    for (int o = 0; o < (numOscillators + 1); o++) {
      for (int i = 0; i < 128; i++) {
        file.print(i);                       // Note
        file.print(",");                     // Delimiter
        file.print(o);                       // Oscillator
        file.print(",");                     // Delimiter
        file.println(autotune_value[i][o]);  // Value
      }
    }
    file.close();
    Serial.println("Autotune values saved to LittleFS.");
  } else {
    Serial.println("Error opening file for writing.");
  }
}

void loadTuningCorrectionsFromSD() {
  // Open the file for reading
  File file = LittleFS.open("tuning.txt", "r");
  if (file) {
    while (file.available()) {
      // Read a line from the file
      String line = file.readStringUntil('\n');

      // Parse the line
      int firstComma = line.indexOf(',');       // First delimiter
      int secondComma = line.lastIndexOf(',');  // Second delimiter

      // Extract note, oscillator, and value
      if (firstComma != -1 && secondComma != -1 && secondComma > firstComma) {
        int note = line.substring(0, firstComma).toInt();
        int osc = line.substring(firstComma + 1, secondComma).toInt();
        int value = line.substring(secondComma + 1).toInt();

        // Validate indices and populate the array
        if (note >= 0 && note < 128 && osc >= 0 && osc <= numOscillators) {
          autotune_value[note][osc] = value;
        } else {
          Serial.println("Invalid data in tuning.txt");
        }
      }
    }
    file.close();
    Serial.println("Tuning corrections loaded from LittleFS.");
  } else {
    Serial.println("Failed to open tuning.txt on LittleFS.");
  }
}

void startAutotune() {
    if (led_state) {
    pixels.setPixelColor(0, pixels.Color(255, 255, 0));  // GREEN
  } else {
    pixels.setPixelColor(0, pixels.Color(0, 255, 0));  // GREEN
  }
  pixels.show();
  digitalWrite(TUNE_LED, HIGH);
  allNotesOff();
  autotuneStart = true;
  oscillator = 0;
  tuneNote = 0;
  setVCOStolowestA();
  delay(200);
  calibratePolymodCentre();  // re-measure pin 28 rest point with the patch loaded (core 1 is paused)
  workingNote = A_NOTES[tuneNote];
  targetFrequency = midi_to_freqs[workingNote][1];
  digitalWrite(VCO_SEL, LOW);
  digitalWrite(MUX_ENABLE, HIGH);
  delayMicroseconds(200);
}

void autotune() {
  int midiNote = A_NOTES[tuneNote];
  float targetFrequency = midi_to_freqs[midiNote][1];

  // Warm start from the previous A's correction - VCO errors change smoothly across
  // the range, so this usually lands within a few cents on the first measurement.
  int error = (tuneNote > 0) ? autotune_value[A_NOTES[tuneNote - 1]][oscillator] : 0;
  int bestError = error;
  float bestCents = 1e9f;
  int iter;

  // Measure -> compute the exact correction in cents -> apply -> re-measure.
  // Converges in 2-3 passes and can correct up to +/-1 octave (AT_MAX_ERROR).
  for (iter = 0; iter < AT_MAX_ITERATIONS; iter++) {
    updateOscillator(midiNote, error, oscillator);
    delay(AT_SETTLE_MS);

    float measured = directCount(targetFrequency);
    if (measured <= 0.0f) break;  // no signal - keep the best so far

    float cents = 1200.0f * log2f(targetFrequency / measured);  // + = VCO is flat
    if (fabsf(cents) < fabsf(bestCents)) {
      bestCents = cents;
      bestError = error;
    }
    if (fabsf(cents) <= AT_TOLERANCE_CENTS) break;

    error += (int)lroundf(cents / 100.0f * NOTE_SF);  // NOTE_SF = DAC counts per semitone
    error = constrain(error, -AT_MAX_ERROR, AT_MAX_ERROR);
  }

  Serial.printf("Osc %d  note %3d  target %8.2f Hz  offset %6d  residual %+6.2f cents  (%d passes)\n",
                oscillator, midiNote, targetFrequency, bestError, bestCents, iter + 1);

  // Save the best error value
  autotune_value[midiNote][oscillator] = bestError;
  // Serial.print("Oscillator: ");
  // Serial.print(oscillator);
  // Serial.print(" | Note: ");
  // Serial.print(midiNote);
  // Serial.print(" | Best Tune Offset: ");
  // Serial.println(bestError);

  // Move to the next note/oscillator
  tuneNote++;
  if (tuneNote >= A_NOTES_COUNT) {
    tuneNote = 0;
    oscillator++;
    pixels.setPixelColor(0, pixels.Color(0, 0, 255));  // BLUE
    pixels.show();
    // Serial.print("Oscillator: ");
    // Serial.println(oscillator);
    if (oscillator > numOscillators) {
      autotuneStart = false;  // Autotune completed
      extrapolateNotes();
      //Serial.println("Auto Tune Finished");
      if (led_state) {
        pixels.setPixelColor(0, pixels.Color(0, 255, 0));  // RED
      } else {
        pixels.setPixelColor(0, pixels.Color(255, 0, 0));  // RED
      }
      pixels.show();
      digitalWrite(TUNE_LED, LOW);
      digitalWrite(MUX_ENABLE, LOW);
    } else {
      // Switch to the next oscillator
      selectMuxInput();
      delay(10);  // Stabilize MUX switching
    }
  }
}

void setOscillator(int midiNote, int oscillator) {
  switch (oscillator) {
    case 0:
      setDAC(DAC_NOTE1, (midiNote - 36), 0, 0);
      break;
    case 1:
      setDAC(DAC_NOTE1, (midiNote - 36), 0, 1);
      break;
  }
}

void updateOscillator(int note, int error, int oscillator) {
  switch (oscillator) {
    case 0:
      setDAC(DAC_NOTE1, (note - 36), error, 0);
      break;
    case 1:
      setDAC(DAC_NOTE1, (note - 36), error, 1);
      break;
  }
}

void setDAC(int chipSelect, int note, int error, int oscillator) {
  switch (oscillator) {
    case 0:
      mV = (unsigned int)(((float)(note)*NOTE_SF + 0.5) + (VOLTOFFSET + error));
      sample_data = (channel_a & 0xFFF0000F) | (((mV)&0xFFFF) << 4);
      outputDAC(chipSelect, sample_data);
      break;
    case 1:
      mV = (unsigned int)(((float)(note)*NOTE_SF + 0.5) + (VOLTOFFSET + error));
      sample_data = (channel_b & 0xFFF0000F) | (((mV)&0xFFFF) << 4);
      outputDAC(chipSelect, sample_data);
      break;
  }
}

//////////////////////////////////////////////////////////////////////////////

void extrapolateNotes() {
  for (int o = 0; o <= numOscillators; o++) {
    // Extrapolate and interpolate across the full MIDI note range (0-127)
    for (int i = 0; i < 128; i++) {
      // Find the nearest lower and higher known notes in A_NOTES
      int lowerNote = -1;
      int higherNote = -1;

      for (int j = 0; j < A_NOTES_COUNT; j++) {
        if (A_NOTES[j] <= i) {
          lowerNote = A_NOTES[j];
        }
        if (A_NOTES[j] > i) {
          higherNote = A_NOTES[j];
          break;
        }
      }

      if (lowerNote != -1 && higherNote != -1) {
        // Interpolation between lowerNote and higherNote
        int lowerValue = autotune_value[lowerNote][o];
        int higherValue = autotune_value[higherNote][o];
        double t = static_cast<double>(i - lowerNote) / (higherNote - lowerNote);
        autotune_value[i][o] = constrain(
          static_cast<int>(lowerValue + t * (higherValue - lowerValue)),
          -32768, 32767);
      } else if (lowerNote != -1) {
        // Extrapolation below the first known note
        int lowerValue = autotune_value[lowerNote][o];
        autotune_value[i][o] = constrain(lowerValue, -32768, 32767);
      } else if (higherNote != -1) {
        // Extrapolation above the last known note
        int higherValue = autotune_value[higherNote][o];
        autotune_value[i][o] = constrain(higherValue, -32768, 32767);
      }
    }
  }

  saveTuningCorrectionsToSD();
}

//////////////////////////////////////////////////////////////////////

void loadSDCardNow() {
  loadTuningCorrectionsFromSD();
}

// Timestamps rising edges for period measurement. Edges closer together than
// atMinIntervalUs are ignored (comparator chatter / double triggers).
void my_isr() {
  uint32_t t = micros();
  if (atEdgeCount != 0 && (t - atLastEdgeUs) < atMinIntervalUs) return;
  if (atEdgeCount == 0) atFirstEdgeUs = t;
  atLastEdgeUs = t;
  atEdgeCount++;
}

// Measures frequency by timing whole periods (first edge -> last edge) rather than
// counting edges in a fixed window. Resolution is ~1 us over the gate, i.e. a few
// ppm (hundredths of a cent) even at A0, where the old counter was +/-30 cents.
float directCount(float targetFreq) {
  uint32_t gateUs = (uint32_t)(AT_MIN_PERIODS * 1e6f / targetFreq);
  if (gateUs < AT_MIN_GATE_US) gateUs = AT_MIN_GATE_US;
  if (gateUs > AT_MAX_GATE_US) gateUs = AT_MAX_GATE_US;

  // Reject edges faster than ~3x the target - copes with VCOs well sharp, kills chatter
  atMinIntervalUs = (uint32_t)(0.3f * 1e6f / targetFreq);
  atEdgeCount = 0;

  attachInterrupt(digitalPinToInterrupt(AUTO_INPUT), my_isr, RISING);
  uint32_t start = micros();
  while (micros() - start < gateUs) {}
  detachInterrupt(digitalPinToInterrupt(AUTO_INPUT));

  uint32_t n = atEdgeCount;
  uint32_t span = atLastEdgeUs - atFirstEdgeUs;
  if (n < 3 || span == 0) return 0.0f;  // no usable signal
  return (float)(n - 1) * 1e6f / (float)span;
}

void loop() {

  if (autotuneStart) {
    autotune();
  } else {
    updateTimers();
    MIDI.read(0);  //MIDI 5 Pin DIN
    updateLFOs();  // advance both internal LFOs (produces LFO1_OUT / LFO2_OUT / aux)
    digitalWrite(LFO1_CLK, LFO1_CLOCK ? HIGH : LOW);  // LFO1 rate/clock LED (GP26)
    digitalWrite(LFO2_CLK, LFO2_CLOCK ? HIGH : LOW);  // LFO2 rate/clock LED (GP27)
    mod_task();
    pwm_task();
    adjustInterval();
    updateVoice1();
  }
}

void setVCOStolowestA() {
  mV = (unsigned int)(((float)(33) * NOTE_SF + 0.5) + (VOLTOFFSET + autotune_value[33][0]));
  sample_data1 = (channel_a & 0xFFF0000F) | (((int(mV)) & 0xFFFF) << 4);
  outputDAC(DAC_NOTE1, sample_data1);
  mV = (unsigned int)(((float)(33) * NOTE_SF + 0.5) + (VOLTOFFSET + autotune_value[33][1]));
  sample_data1 = (channel_b & 0xFFF0000F) | (((int(mV)) & 0xFFFF) << 4);
  outputDAC(DAC_NOTE1, sample_data1);
}

void ResetAutoTuneValues() {
  for (int o = 0; o < (numOscillators + 1); o++) {
    for (int i = 0; i < 128; i++) {
      (autotune_value[i][o]) = 0;
    }
  }
  // Serial.print("All Autotune Values are 0");
  // Serial.println();
  // Serial.print("Writing all 0 to SD card");
  // Serial.println();
  saveTuningCorrectionsToSD();
}

void DisplayAutoTuneValues() {

  Serial.println("=== Autotune Values ===");
  if (displayvalues) {
    for (int o = 0; o < (numOscillators + 1); o++) {
      Serial.print("Oscillator ");
      Serial.print(o);
      Serial.println(":");
      for (int i = 0; i < 128; i++) {
        Serial.print("  Note ");
        Serial.print(i);
        Serial.print(" ");
        Serial.println(autotune_value[i][o]);
      }
    }
  }
}

void allowOsc1Through() {

  if (osc1Through) {
    digitalWrite(VCO_SEL, LOW);
    delayMicroseconds(100);
    digitalWrite(MUX_ENABLE, LOW);
  } else {
    digitalWrite(VCO_SEL, LOW);
    digitalWrite(MUX_ENABLE, HIGH);
  }
}

void myPitchBend(byte channel, int bend) {
  if (channel == masterChan) {
    // BEND_WHEEL = range in semitones (0-12, CC16); NOTE_SF = DAC counts per semitone
    bend_data = (int)((float)bend / 8192.0f * (float)BEND_WHEEL * NOTE_SF);
  }
}

void adjustInterval() {
  if (INTERVAL_POT != INTERVAL) {
  }
  switch (INTERVAL_POT) {
    case 12:
      INTERVAL = 12;
      break;

    case 11:
      INTERVAL = 11;
      break;

    case 10:
      INTERVAL = 10;
      break;

    case 9:
      INTERVAL = 9;
      break;

    case 8:
      INTERVAL = 8;
      break;

    case 7:
      INTERVAL = 7;
      break;

    case 6:
      INTERVAL = 6;
      break;

    case 5:
      INTERVAL = 5;
      break;

    case 4:
      INTERVAL = 4;
      break;

    case 3:
      INTERVAL = 3;
      break;

    case 2:
      INTERVAL = 2;
      break;

    case 1:
      INTERVAL = 1;
      break;

    case 0:
      INTERVAL = 0;
      break;
  }
}

// Reads MIDI CC messages
void myControlChange(byte channel, byte number, byte value) {
  if (channel == masterChan) {
    switch (number) {

      case CC_MOD_WHEEL:
        FM_RANGE_UPPER = int(value * FM_MOD_WHEEL);
        FM_RANGE_LOWER = (FM_RANGE_UPPER - FM_RANGE_UPPER - FM_RANGE_UPPER);
        TM_RANGE_UPPER = int(value * TM_MOD_WHEEL);
        TM_RANGE_LOWER = (TM_RANGE_UPPER - TM_RANGE_UPPER - TM_RANGE_UPPER);
        break;

      case CC_PORTAMENTO_TIME:                                           // Portamento time
        portamentoTime = map(value, 0, 127, 0, 10000);  // Map to a max of 2 seconds
        break;

      case CC_INTERVAL:
        INTERVAL_POT = value;
        break;

      case CC_DETUNE:
        DETUNE = value;
        break;

      case CC_BEND_RANGE:
        BEND_WHEEL = min(value, (byte)12);   // controller sends 0-12 semitones
        break;

      case CC_FM_MOD_WHEEL:
        // CC17 127 + wheel 127 = FM_LFO_MAX_COUNTS (same as LFO1 depth CC103 at max)
        FM_MOD_WHEEL = value * (FM_LFO_MAX_COUNTS / (127.0f * 127.0f));
        break;

      case CC_TM_MOD_WHEEL:
        TM_MOD_WHEEL = map(value, 0, 127, 0, 16.2);
        break;

      case CC_FM_AT_WHEEL:  // Aftertouch depth (applies to CC109 destination)
        atDepth = value / 127.0f;
        applyAftertouch();
        break;

      case CC_POLYMOD_SW:  // Poly mod on/off for VCO1
        polymodOn = (value >= 64);
        break;

      case CC_AT_DESTINATION:  // 0 Off, 1 DCO, 2 Cutoff (assigner), 3 VCF, 4 VCA
        atDestination = min(value, (byte)AT_DEST_VCA);
        applyAftertouch();
        break;

      case CC_TM_AT_WHEEL:
        TM_AT_WHEEL = map(value, 0, 127, 0, 16.2);
        break;

      case CC_OCTAVE_A:
        value = map(value, 0, 127, 0, 2);
        switch (value) {
          case 0:
            OCTAVE_A = -12;
            break;
          case 1:
            OCTAVE_A = 0;
            break;
          case 2:
            OCTAVE_A = 12;
            break;
        }
        break;

      case CC_OCTAVE_B:
        value = map(value, 0, 127, 0, 2);
        switch (value) {
          case 0:
            OCTAVE_B = -12;
            break;
          case 1:
            OCTAVE_B = 0;
            break;
          case 2:
            OCTAVE_B = 12;
            break;
        }
        break;

      case CC_PW1:
        PW1 = map(value, 0, 127, 0, 52428);
        break;

      case CC_PWM1_DEPTH:
        PWM1 = map(value, 0, 127, 0, 26000);
        break;

      case CC_PW2:
        PW2 = map(value, 0, 127, 0, 52428);
        break;

      case CC_PWM2_DEPTH:
        PWM2 = map(value, 0, 127, 0, 26000);
        break;

      case CC_FM_ACTUAL:
        FM_ACTUAL_STR = value;
        FM_ACTUAL = map(FM_ACTUAL_STR, 0, 127, 0, 16.2);
        FM_ACTUAL_UPPER = FM_ACTUAL * FM_ACTUAL_STR;
        FM_ACTUAL_LOWER = -FM_ACTUAL_UPPER;
        break;

      case CC_SYNC:
        switch (value) {
          case 127:
            sync = true;
            break;
          case 0:
            sync = false;
            break;
        }
        break;

      case CC_PORTAMENTO_SW:  // Portamento on/off
        switch (value) {
          case 127:
            portamentoOn = true;
            break;
          case 0:
            portamentoOn = false;
            break;
        }
        break;

      // ---- Internal LFO controls ----
      case CC_LFO1_RATE:  // LFO1 rate (0.05 - 12.8 Hz)
        LFO1_baseFreq = ccToRate1(value);
        break;

      case CC_LFO1_DEPTH:  // LFO1 depth / level (FM amount)
        lfo1.depth = value / 127.0f;
        break;

      case CC_LFO1_WAVEFORM:  // LFO1 waveform (0 - 15)
        lfo1.wave = map(value, 0, 127, 0, WAVE_COUNT - 1);
        break;

      case CC_LFO1_MULT:  // LFO1 tempo multiplier (0.5/1/1.5/2/3/4)
        LFO1_multIndex = map(value, 0, 127, 0, 5);
        break;

      case CC_LFO1_FILTER_DEPTH:  // LFO1 -> Filter depth (channel_g, unipolar)
        LFO1_filterDepth = value / 127.0f;
        break;

      case CC_LFO1_AMP_DEPTH:  // LFO1 -> Amp depth (channel_h, bipolar)
        LFO1_ampDepth = value / 127.0f;
        break;

      case CC_LFO2_RATE:  // LFO2 rate (0.05 - 20 Hz)
        LFO2_baseFreq = ccToRate2(value);
        break;

      // ---- Keytrack (channel_c) ----
      case CC_KEYTRACK_AMOUNT:  // Keytrack amount (0 - 100 %)
        keytrackAmount = value / 127.0f;
        break;

      case CC_KEYTRACK_SW:  // Keytrack on/off
        switch (value) {
          case 127:
            keytrackOn = true;
            break;
          case 0:
            keytrackOn = false;
            break;
        }
        break;

      // ---- LFO1 delay machine (same CCs as the SuperSaw oscillator) ----
      case CC_LFO1_DELAY_TIME:  // LFO1 delay time (0 = none, else 0.1 - 5 s)
        lfo1SetDelayTime(value);
        break;

      case CC_LFO1_DELAY_RAMP:  // LFO1 ramp / fade-in time (0 = instant, else 0.05 - 3 s)
        lfo1SetDelayRamp(value);
        break;

      case CC_LFO1_RETRIG:  // LFO1 delay retrigger (>=64 on, <64 legato)
        lfo1SetRetrig(value);
        break;

      case CC_NOTES_HELD:  // notes-held flag from master (>=64 held, <64 all released)
        lfo1SetNotesHeld(value);
        break;

      case CC_AUTOTUNE_START:
        switch (value) {
          case 127:
            startAutotune();
            break;
        }
        break;

      case CC_AUTOTUNE_RESET:
        switch (value) {
          case 127:
            ResetAutoTuneValues();
            break;
        }
        break;

      case CC_ALL_NOTES_OFF:
        switch (value) {
          case 127:
            allNotesOff();
            break;
        }
        break;
    }
  }
}

void myAfterTouch(byte channel, byte value) {
  if (channel == masterChan) {
    atValue = value;
    applyAftertouch();
    TM_AT_RANGE_UPPER = int(value * TM_AT_WHEEL);
    TM_AT_RANGE_LOWER = (TM_AT_RANGE_UPPER - TM_AT_RANGE_UPPER - TM_AT_RANGE_UPPER);
  }
}

// Routes aftertouch (x CC19 depth) to the CC109 destination, added on top of the
// panel setting. Called on aftertouch, CC19 and CC109 so changes apply immediately.
void applyAftertouch() {
  float amt = (atValue / 127.0f) * atDepth;  // 0..1

  // DCO Mod: same full-scale as mod wheel / LFO1 depth (FM_LFO_MAX_COUNTS)
  FM_AT_RANGE_UPPER = (atDestination == AT_DEST_DCO) ? (int)(amt * FM_LFO_MAX_COUNTS) : 0;
  FM_AT_RANGE_LOWER = -FM_AT_RANGE_UPPER;

  LFO1_filterDepthAT = (atDestination == AT_DEST_VCF) ? amt : 0.0f;
  LFO1_ampDepthAT    = (atDestination == AT_DEST_VCA) ? amt : 0.0f;
  // AT_DEST_CUTOFF: handled on the assigner
}

// FM modulation source: internal LFO1 (replaces the external LFO on FM_INPUT)
void mod_task() {

  MOD_VALUE = LFO1_OUT;  // 0..4095, centred at 2048 (was analogRead(FM_INPUT))
  FM_VALUE = map(MOD_VALUE, 0, 4095, FM_RANGE_LOWER, FM_RANGE_UPPER);
  FM_AT_VALUE = map(MOD_VALUE, 0, 4095, FM_AT_RANGE_LOWER, FM_AT_RANGE_UPPER);
  TM_VALUE = map(MOD_VALUE, 0, 4095, TM_RANGE_LOWER, TM_RANGE_UPPER);
  TM_AT_VALUE = map(MOD_VALUE, 0, 4095, TM_AT_RANGE_LOWER, TM_AT_RANGE_UPPER);
  FM_ACTUAL_VALUE = map(MOD_VALUE, 0, 4095, FM_ACTUAL_LOWER, FM_ACTUAL_UPPER);
}

void pwm_task() {

  PWM_VALUE = LFO2_OUT;  // 0..4095 unipolar (2048 = centre)
  // Bipolar depth: sweep the duty cycle symmetrically about the PW setting,
  // not just additively on top of it.
  PWM1_VALUE = map(PWM_VALUE, 0, 4095, -PWM1, PWM1);  // AS3340 #1 (CC24)
  PWM2_VALUE = map(PWM_VALUE, 0, 4095, -PWM2, PWM2);  // AS3340 #2 (CC26)
}

void commandNote(int noteMsg) {
  note1 = noteMsg;
  lfo1NoteOn();  // arm the LFO1 delay on the first note of a phrase
  digitalWrite(GATE_NOTE1, HIGH);
  digitalWrite(GATE_LED, HIGH);
}

// MIDI note on callback
void myNoteOn(byte channel, byte note, byte velocity) {
  if (channel == MIDI_CHANNEL) {

    //Check for out of range notes
    if (note < 0 || note > 127) return;

    // Running-status note-off (note on, velocity 0)
    if (velocity == 0) {
      myNoteOff(channel, note, velocity);
      return;
    }

    // Calculate new target mV values including all real-time corrections
    //oscanote1 = note + OCTAVE_A;
    targetMV_a = (unsigned int)(((float)(note + transpose + realoctave) * NOTE_SF + 0.5) + (autotune_value[note][0]));

    //oscbnote1 = note + INTERVAL + OCTAVE_B;
    targetMV_b = (unsigned int)(((float)(note + transpose + realoctave) * NOTE_SF + 0.5) + (autotune_value[note][1]));

    if (portamentoOn) {
      // If portamento is enabled, start glide from the **current** value
      initialMV_a = currentMV_a;
      initialMV_b = currentMV_b;
      glideStartTime = millis();  // Start the glide transition
      portamentoActive = true;
    } else {
      // If portamento is OFF, instantly set frequency
      currentMV_a = targetMV_a;
      currentMV_b = targetMV_b;
      portamentoActive = false;
    }

    // Note priority is handled by the assigner - this voice just plays what it's sent
    voices[0].velocity = velocity;
    voices[0].note = note;
    commandNote(note);
  }
}

// MIDI Note Off callback
void myNoteOff(byte channel, byte note, byte velocity) {
  if (channel == MIDI_CHANNEL) {
    // Only close the gate for the note that's sounding, so a late note-off
    // for a previous note can't cut off a new one
    if (note == note1) {
      digitalWrite(GATE_NOTE1, LOW);
      digitalWrite(GATE_LED, LOW);
      voices[0].note = -1;
      lfo1DelayState = LFO1_DLY_IDLE;  // re-arm LFO1 delay for the next note (no CC58 needed)
    }
  }
}

// Updates the sync output when it changes
void updateTimers() {

  if (sync != oldsync) {
    if (sync) {
      digitalWrite(SYNC_PIN, HIGH);
    } else {
      digitalWrite(SYNC_PIN, LOW);
    }
    oldsync = sync;
  }

}

// Updates the DAC with the note1 data and adds in all offsets, bend, modulation, aftertouch, octave range
void updateVoice1() {
  static unsigned long lastUpdateTime = 0;  // Tracks the last time this function was called
  unsigned long currentTime = millis();     // Get the current time in milliseconds
  unsigned long deltaTime = currentTime - lastUpdateTime;

  if (portamentoActive) {
    // Calculate the progress as a fraction of portamentoTime
    float progress = (float)(millis() - glideStartTime) / (float)portamentoTime;

    // Clamp progress between 0.0 and 1.0
    if (progress > 1.0) progress = 1.0;

    // Apply an **adjusted** exponential scaling for smooth glide
    float rate = 3.5;  // Tweak this value for a more natural feel
    float exponentialProgress = (1.0 - exp(-rate * progress)) / (1.0 - exp(-rate));

    // Interpolate currentMV values for an **exponential glide**
    currentMV_a = initialMV_a + (targetMV_a - initialMV_a) * exponentialProgress;
    currentMV_b = initialMV_b + (targetMV_b - initialMV_b) * exponentialProgress;

    // If the glide is **complete**, snap to final values and disable glide
    if (progress >= 1.0) {
      currentMV_a = targetMV_a;
      currentMV_b = targetMV_b;
      portamentoActive = false;
    }
  }

  // Now continuously apply parameters to the frequency calculation

  // Calculate mV for channel_a (Oscillator A)
  oscanote1 = note1 + OCTAVE_A;  // Apply the correct octave adjustment for this oscillator
  // Core 1 adds poly mod and writes channel A - here we only publish the base pitch.
  // (int) maths so negative offsets don't clamp to 0 when cast to unsigned on ARM.
  vco1BaseCounts = (int32_t)currentMV_a + (int32_t)(((float)(OCTAVE_A)*NOTE_SF + 0.5) + (VOLTOFFSET + bend_data + FM_VALUE + FM_AT_VALUE + FM_ACTUAL_VALUE + FM_LFO_VALUE));

  // Calculate mV for channel_b (Oscillator B)
  oscbnote1 = note1 + INTERVAL + OCTAVE_B;  // Apply interval and octave B adjustments
  additionalmV = (unsigned int)(((float)(INTERVAL + OCTAVE_B) * NOTE_SF + 0.5) + (VOLTOFFSET + bend_data + FM_VALUE + FM_AT_VALUE + FM_ACTUAL_VALUE + DETUNE + FM_LFO_VALUE));
  finalmV = (currentMV_b + additionalmV);
  sample_data1 = (channel_b & 0xFFF0000F) | (((int(finalmV)) & 0xFFFF) << 4);
  outputDAC(DAC_NOTE1, sample_data1);

  // channel_c : Keytrack CV (1V/oct note CV scaled by keytrack amount) + TM (mod-wheel/AT) modulation
  {
    float pivotmV = (float)(KEYTRACK_PIVOT_NOTE + transpose + realoctave) * KEYTRACK_SF;
    float notemV  = (float)(note1 + transpose + realoctave) * KEYTRACK_SF;
    float ktmV    = keytrackOn ? pivotmV + (notemV - pivotmV) * keytrackAmount : pivotmV;
    mV = (unsigned int)(constrain(ktmV + TM_VALUE + 0.5f, 0.0f, 65535.0f));
  }
  sample_data1 = (channel_c & 0xFFF0000F) | (((int(mV)) & 0xFFFF) << 4);
  outputDAC(DAC_NOTE1, sample_data1);

  // Apply velocity (velocity CV output)
  velmV = ((unsigned int)((float)voices[0].velocity) * VEL_SF);
  vel_data1 = (channel_d & 0xFFF0000F) | (((int(velmV)) & 0xFFFF) << 4);
  outputDAC(DAC_NOTE1, vel_data1);

  // channel_e : VCO1 pulse width = static PW1 + LFO2 sweep PWM1 (summed in firmware)
  {
    int pw1sum = constrain((int)PW1 + (int)PWM1_VALUE, 0, 65535);
    sample_data1 = (channel_e & 0xFFF0000F) | ((pw1sum & 0xFFFF) << 4);
    outputDAC(DAC_NOTE1, sample_data1);
  }

  // channel_f : VCO2 pulse width = static PW2 + LFO2 sweep PWM2 (summed in firmware)
  {
    int pw2sum = constrain((int)PW2 + (int)PWM2_VALUE, 0, 65535);
    sample_data1 = (channel_f & 0xFFF0000F) | ((pw2sum & 0xFFFF) << 4);
    outputDAC(DAC_NOTE1, sample_data1);
  }

  // channel_g : LFO1 -> Filter (unipolar, own depth CC110)
  {
    int gOut = constrain((int)LFO1_FILTER_OUT, 0, 65535);
    sample_data1 = (channel_g & 0xFFF0000F) | ((gOut & 0xFFFF) << 4);
    outputDAC(DAC_NOTE1, sample_data1);
  }

  // channel_h : LFO1 -> Amp (bipolar, own depth CC111)
  {
    int hOut = constrain((int)LFO1_AMP_OUT, 0, 65535);
    sample_data1 = (channel_h & 0xFFF0000F) | ((hOut & 0xFFFF) << 4);
    outputDAC(DAC_NOTE1, sample_data1);
  }

  // Update last update time
  lastUpdateTime = currentTime;
}

// Resets all the notes to off
void allNotesOff() {
  digitalWrite(GATE_NOTE1, LOW);
  digitalWrite(GATE_LED, LOW);

  voices[0].note = -1;

  voiceOn[0] = false;
}


void outputDAC(int CHIP_SELECT, uint32_t sample_data) {
  mutex_enter_blocking(&dacMutex);
  dacWrite(CHIP_SELECT, sample_data);
  mutex_exit(&dacMutex);
}

// Raw DAC write - caller must hold dacMutex
void dacWrite(int CHIP_SELECT, uint32_t sample_data) {
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE1));
  digitalWrite(CHIP_SELECT, LOW);
  delayMicroseconds(1);
  SPI.transfer16(sample_data >> 16);
  SPI.transfer16(sample_data);
  delayMicroseconds(1);  // Settling time delay
  digitalWrite(CHIP_SELECT, HIGH);
  SPI.endTransaction();
}

int mod(int a, int b) {
  int r = a % b;
  return r < 0 ? r + b : r;
}

void selectMuxInput() {
  switch (oscillator) {

    // Board 1 A
    case 0:
      digitalWrite(VCO_SEL, LOW);
      break;

    // Board 1 B
    case 1:
      digitalWrite(VCO_SEL, HIGH);
      break;
  }
}
