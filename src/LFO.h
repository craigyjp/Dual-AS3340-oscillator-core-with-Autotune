//
//  LFO.h  --  Dual internal LFO engine
//
//  LFO1 : Electric Druid TAPLFO3-D style.
//         Rate, Depth (= "Level"), 16 waveforms,
//         Tempo multiplier (0.5 / 1 / 1.5 / 2 / 3 / 4), and the SuperSaw
//         delay machine (delay -> ramp-in -> full, CC55/56/57 + notes-held CC58).
//         Destinations (one LFO, several outputs, like a real TAPLFO3):
//           - FM on both VCO pitch CVs (channel_a + channel_b)
//           - Filter LFO CV, unipolar (0 at min), own depth (channel_g)
//           - Amp    LFO CV, bipolar, own depth (channel_h)
//
//  LFO2 : Simple triangle. Rate + TWO depths (one per AS3340, applied
//         downstream by PWM1 = CC24 -> channel_e and PWM2 = CC26 -> channel_f).
//
//  Output smoothing (de-click) is always on; wave distortion and CV invert
//  are not implemented (waveforms are symmetric).
//
//  These two LFOs REPLACE the external hardware LFOs that were read on
//  FM_INPUT (ADC0 / pin 26) and PWM_INPUT (ADC1 / pin 27).  Each LFO
//  synthesises a signal in the SAME 0..4095 range the ADCs used to return,
//  so every existing map()/routing line downstream keeps working unchanged.
//
//  Timing: the phase of each LFO is advanced by real elapsed time (micros()),
//  so the rate stays accurate regardless of main-loop jitter.
//
//  Include this once, after Parameters.h / Hardware.h, in the main .ino.
//

#ifndef LFO_H
#define LFO_H

#include <Arduino.h>
#include <math.h>

// ------------------------------------------------------------------ //
//  Waveform IDs  --  two banks of eight (16 total)                     //
// ------------------------------------------------------------------ //
//  NOTE: the TAPLFO3-D datasheet only draws its waveforms as a picture,
//  so this ordering is our own faithful re-creation of the shape
//  families (basic / exponential / multi-peak / stepped / random).
//  Re-order the enum freely to taste; CC104 selects 0..15 across it.
enum LfoWave {
  WAVE_TRI = 0,      // 0  Triangle
  WAVE_SINE,         // 1  Sine
  WAVE_RAMP_UP,      // 2  Ramp up   (sawtooth)
  WAVE_RAMP_DOWN,    // 3  Ramp down (reverse saw)
  WAVE_SQUARE,       // 4  Square (50%)
  WAVE_TRAP,         // 5  Trapezoid
  WAVE_EXP_DOWN,     // 6  Exponential decay ramp   (env-like)
  WAVE_EXP_UP,       // 7  Exponential rise ramp
  WAVE_LOG_SWEEP,    // 8  Logarithmic sweep (accelerating sine)
  WAVE_TWIN,         // 9  Twin-peak sine  (2 humps / cycle)
  WAVE_TRIPLE,       // 10 Triple-peak sine (3 humps / cycle)
  WAVE_STAIR_UP,     // 11 Staircase up   (8 steps)
  WAVE_STAIR_DOWN,   // 12 Staircase down (8 steps)
  WAVE_SH_RANDOM,    // 13 Sample & Hold random (stepped)
  WAVE_SMOOTH_RAND,  // 14 Smoothed random (random slopes)
  WAVE_PULSE,        // 15 Narrow pulse (~20%)
  WAVE_COUNT
};

// ------------------------------------------------------------------ //
//  Tempo multiplier ratios (TAPLFO3-D)                                 //
// ------------------------------------------------------------------ //
const float LFO_MULTIPLIERS[6] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };

// ------------------------------------------------------------------ //
//  How much pitch the FM LFO can move at full depth.                   //
//  NOTE_SF = 274.4 DAC counts / semitone, so 2 semitones ~= 549.       //
//  Increase for a wider max vibrato, decrease for subtler.             //
// ------------------------------------------------------------------ //
#define FM_LFO_MAX_COUNTS  1098.0f  // +/- 2 semitones at full LFO1 depth (NOTE_SF = 548.8 counts/semitone)

// ------------------------------------------------------------------ //
//  LFO1 aux outputs (Filter on channel_g, Amp on channel_h).          //
//  Filter: UNIPOLAR -- 0 at the LFO minimum, rising to depth x         //
//    LFO1_AUX_UNIPOLAR_FS at the LFO peak (and 0 when depth = 0).      //
//  Amp: BIPOLAR -- centred at LFO1_AUX_CENTER, swinging +/- FULLSWING  //
//    at full depth. CENTER is the DAC code = "no modulation" at the    //
//    amp mod input; mid-scale (32768) is the widest symmetric swing.   //
// ------------------------------------------------------------------ //
#define LFO1_AUX_CENTER      13107.0f   // amp rest = 1V  (65535 = 5V full scale)
#define LFO1_AUX_FULLSWING   13107.0f   // +/-1V at full depth -> 0..2V about 1V
#define LFO1_AUX_UNIPOLAR_FS 65535.0f   // filter full-scale (unipolar: 0 .. this)

// ------------------------------------------------------------------ //
//  Output smoothing (always on, TAPLFO3-D style de-click).            //
//  One-pole low-pass on the LFO output that rounds the sharp edges of  //
//  the square / staircase / S&H waves. Small time constant so it       //
//  de-clicks without lagging the slower shapes. Larger = smoother.     //
// ------------------------------------------------------------------ //
#define LFO_SMOOTH_TAU  0.0020f     // seconds (~80 Hz corner)

// ------------------------------------------------------------------ //
//  LFO clock/LED outputs: HIGH for the first LFO_CLOCK_DUTY of each    //
//  cycle, so a driven LED blinks at the LFO rate. 0.5 = 50% square;    //
//  a smaller value gives a brief flash once per cycle instead.         //
// ------------------------------------------------------------------ //
#define LFO_CLOCK_DUTY  0.5f

// ------------------------------------------------------------------ //
//  Per-LFO state                                                       //
// ------------------------------------------------------------------ //
struct Lfo {
  float   phase;      // 0..1
  float   freqHz;     // effective frequency (after multiplier)
  uint8_t wave;       // 0..15
  float   depth;      // 0..1  overall level ("Depth")
  float   rndPrev;    // random-wave: value at start of current cycle
  float   rndTarget;  // random-wave: value at end of current cycle (held for S&H)
  float   smooth;     // smoothing-filter state (de-click)
};

// LFO1 parameters set from CC
volatile float    LFO1_baseFreq    = 1.0f;  // Hz, before multiplier
volatile uint8_t  LFO1_multIndex   = 1;     // index into LFO_MULTIPLIERS (x1)
volatile float    LFO1_filterDepth = 0.0f;  // 0..1  aux depth -> channel_g (filter)
volatile float    LFO1_ampDepth    = 0.0f;  // 0..1  aux depth -> channel_h (amp)
volatile float    LFO1_filterDepthAT = 0.0f;  // 0..1  aftertouch added to filter depth
volatile float    LFO1_ampDepthAT    = 0.0f;  // 0..1  aftertouch added to amp depth

// LFO2 parameters set from CC
volatile float    LFO2_baseFreq   = 2.0f;   // Hz

//                phase freq  wave       depth rndPrev rndTarget smooth
Lfo lfo1 = {      0.0f, 1.0f, WAVE_TRI,  1.0f, 0.5f,   0.5f,     0.0f };
Lfo lfo2 = {      0.0f, 2.0f, WAVE_TRI,  1.0f, 0.5f,   0.5f,     0.0f };

// Outputs, expressed in the old ADC 0..4095 range so existing code is happy
float LFO1_OUT   = 2048.0f;  // centred (2048 = no FM)
float LFO2_OUT   = 0.0f;     // unipolar 0..4095

// Direct FM contribution (DAC counts, bipolar) driven by LFO1 depth.
// Added into updateVoice1() alongside the existing FM terms.
float FM_LFO_VALUE = 0.0f;

// LFO1 aux DAC outputs (16-bit codes, bipolar around LFO1_AUX_CENTER)
float LFO1_FILTER_OUT = 0.0f;             // -> channel_g (unipolar, rest = 0)
float LFO1_AMP_OUT    = LFO1_AUX_CENTER;  // -> channel_h (bipolar, rest = centre)

// LFO rate/clock flags - drive a rate-indicator LED (pins wired in the .ino)
bool LFO1_CLOCK = false;   // HIGH for the first LFO_CLOCK_DUTY of each LFO1 cycle
bool LFO2_CLOCK = false;   // HIGH for the first LFO_CLOCK_DUTY of each LFO2 cycle

// ------------------------------------------------------------------ //
//  LFO1 delay machine (ported from the SuperSaw oscillator).          //
//  Delay -> ramp-in -> full. Scales the whole LFO1 output (all         //
//  destinations). Driven by note-on and by the master's notes-held CC. //
// ------------------------------------------------------------------ //
enum Lfo1DelayState { LFO1_DLY_IDLE, LFO1_DLY_WAITING, LFO1_DLY_RAMPING, LFO1_DLY_FULL };
Lfo1DelayState lfo1DelayState   = LFO1_DLY_IDLE;
float          lfo1DelayTimeMs  = 0.0f;   // delay before onset (ms)
float          lfo1DelayRampMs  = 0.0f;   // fade-in time after delay (ms)
float          lfo1DelayCounter = 0.0f;   // countdown (ms)
float          lfo1DelayScale   = 1.0f;   // 0..1 depth scale applied to LFO1
bool           lfo1DelayRetrig  = true;   // true = restart on each note, false = legato

static unsigned long lfoLastMicros = 0;

// ------------------------------------------------------------------ //
//  Helpers                                                             //
// ------------------------------------------------------------------ //

// pseudo-random 0..1
static inline float lfoRand01() {
  return (float)random(0, 10001) / 10000.0f;
}

// CC 0..127 -> LFO1 rate, exponential 0.05 .. 12.8 Hz (8 octaves, TAPLFO3-D)
static inline float ccToRate1(byte v) {
  return 0.05f * powf(256.0f, (float)v / 127.0f);
}

// CC 0..127 -> LFO2 rate, exponential 0.05 .. 20 Hz
static inline float ccToRate2(byte v) {
  return 0.05f * powf(400.0f, (float)v / 127.0f);
}

// ------------------------------------------------------------------ //
//  Waveform generator  --  returns bipolar -1..+1                      //
// ------------------------------------------------------------------ //
static inline float lfoShape(const Lfo &L) {
  const float p = L.phase;
  const uint8_t w = L.wave;

  // --- Random waves ---
  if (w == WAVE_SH_RANDOM) {
    return L.rndTarget * 2.0f - 1.0f;                 // held for the whole cycle
  }
  if (w == WAVE_SMOOTH_RAND) {
    float y = L.rndPrev + (L.rndTarget - L.rndPrev) * p;  // slew prev -> target
    return y * 2.0f - 1.0f;
  }

  // All waveforms are symmetric (no wave distortion).
  const float pw = p;

  switch (w) {
    case WAVE_TRI:       return 1.0f - 4.0f * fabsf(pw - 0.5f);
    case WAVE_SINE:      return sinf(2.0f * PI * pw);
    case WAVE_RAMP_UP:   return 2.0f * pw - 1.0f;
    case WAVE_RAMP_DOWN: return 1.0f - 2.0f * pw;
    case WAVE_SQUARE:    return (pw < 0.5f) ? 1.0f : -1.0f;
    case WAVE_TRAP: {                                  // clipped triangle
      float t = (1.0f - 4.0f * fabsf(pw - 0.5f)) * 2.0f;
      return constrain(t, -1.0f, 1.0f);
    }
    case WAVE_EXP_DOWN:  return 2.0f * expf(-5.0f * pw) - 1.0f;
    case WAVE_EXP_UP:    return 2.0f * (1.0f - expf(-5.0f * pw)) - 1.0f;
    case WAVE_LOG_SWEEP: return sinf(2.0f * PI * pw * pw);
    case WAVE_TWIN:      return sinf(4.0f * PI * pw);  // 2 humps / cycle
    case WAVE_TRIPLE:    return sinf(6.0f * PI * pw);  // 3 humps / cycle
    case WAVE_STAIR_UP: {
      const int steps = 8;
      float q = floorf(pw * steps) / (float)(steps - 1);
      return 2.0f * constrain(q, 0.0f, 1.0f) - 1.0f;
    }
    case WAVE_STAIR_DOWN: {
      const int steps = 8;
      float q = floorf(pw * steps) / (float)(steps - 1);
      return 1.0f - 2.0f * constrain(q, 0.0f, 1.0f);
    }
    case WAVE_PULSE:     return (pw < 0.2f) ? 1.0f : -1.0f;  // ~20% duty
    default:             return 0.0f;
  }
}

// Advance one LFO by dt seconds; refresh random targets on cycle wrap.
static inline void advanceLfo(Lfo &L, float dt) {
  L.phase += L.freqHz * dt;
  if (L.phase >= 1.0f) {
    L.phase -= floorf(L.phase);
    L.rndPrev   = L.rndTarget;      // last cycle's endpoint becomes new start
    L.rndTarget = lfoRand01();      // new endpoint / new S&H value
  }
}

// ------------------------------------------------------------------ //
//  LFO1 delay machine  (ported from SuperSaw's DCO engine, but paced   //
//  from millis() instead of audio-buffer sample counts)                //
// ------------------------------------------------------------------ //

// Start the delay timeline from zero, per the current delay params.
static inline void lfo1DelayRestart() {
  if (lfo1DelayTimeMs > 0.0f) {
    lfo1DelayState   = LFO1_DLY_WAITING;
    lfo1DelayCounter = lfo1DelayTimeMs;
    lfo1DelayScale   = 0.0f;
  } else if (lfo1DelayRampMs > 0.0f) {
    lfo1DelayState   = LFO1_DLY_RAMPING;
    lfo1DelayCounter = lfo1DelayRampMs;
    lfo1DelayScale   = 0.0f;
  } else {
    lfo1DelayState   = LFO1_DLY_FULL;
    lfo1DelayScale   = 1.0f;
  }
}

// --- CC setters (wire these to the same CCs as your SuperSaw) ---

// CC55: delay before onset. 0 = no delay; else exp 0.1 s .. 5 s.
static inline void lfo1SetDelayTime(byte v) {
  if (v == 0) {
    lfo1DelayTimeMs = 0.0f;
    lfo1DelayScale  = 1.0f;
    lfo1DelayState  = LFO1_DLY_FULL;
    return;
  }
  float t = (float)v / 127.0f;
  lfo1DelayTimeMs = 0.1f * powf(50.0f, t) * 1000.0f;   // 100 ms .. 5000 ms
}

// CC56: ramp/fade-in time after the delay. 0 = instant; else exp 0.05 s .. 3 s.
static inline void lfo1SetDelayRamp(byte v) {
  if (v == 0) { lfo1DelayRampMs = 0.0f; return; }
  float t = (float)v / 127.0f;
  lfo1DelayRampMs = 0.05f * powf(60.0f, t) * 1000.0f;  // 50 ms .. 3000 ms
}

// CC57: retrigger.  >=64 = restart on each note; <64 = legato (only the
// first note of a phrase arms it).
static inline void lfo1SetRetrig(byte v) {
  lfo1DelayRetrig = (v >= 64);
}

// CC58: notes-held flag broadcast by the master.
//   <64  = all notes released -> reset for the next phrase
//   >=64 = a note is down -> with retrig on, restart all voices together,
//          but only while the delay is still in flight (leave it alone once FULL).
static inline void lfo1SetNotesHeld(byte v) {
  if (v < 64) { lfo1DelayState = LFO1_DLY_IDLE; return; }
  if (!lfo1DelayRetrig)                 return;
  if (lfo1DelayState == LFO1_DLY_FULL)  return;
  lfo1DelayRestart();
}

// Call on note-on (from commandNote): first note of a phrase arms the delay.
static inline void lfo1NoteOn() {
  if (lfo1DelayState == LFO1_DLY_IDLE) lfo1DelayRestart();
}

// Advance the machine by dtMs; updates lfo1DelayScale (0..1).
static inline void advanceLfo1Delay(float dtMs) {
  switch (lfo1DelayState) {
    case LFO1_DLY_WAITING:
      lfo1DelayCounter -= dtMs;
      if (lfo1DelayCounter <= 0.0f) {
        if (lfo1DelayRampMs > 0.0f) {
          lfo1DelayState   = LFO1_DLY_RAMPING;
          lfo1DelayCounter = lfo1DelayRampMs;
          lfo1DelayScale   = 0.0f;
        } else {
          lfo1DelayState = LFO1_DLY_FULL;
          lfo1DelayScale = 1.0f;
        }
      }
      break;
    case LFO1_DLY_RAMPING:
      lfo1DelayCounter -= dtMs;
      lfo1DelayScale = 1.0f - (lfo1DelayCounter / lfo1DelayRampMs);
      if (lfo1DelayScale > 1.0f) lfo1DelayScale = 1.0f;
      if (lfo1DelayScale < 0.0f) lfo1DelayScale = 0.0f;
      if (lfo1DelayCounter <= 0.0f) {
        lfo1DelayState = LFO1_DLY_FULL;
        lfo1DelayScale = 1.0f;
      }
      break;
    case LFO1_DLY_FULL:
      lfo1DelayScale = 1.0f;
      break;
    case LFO1_DLY_IDLE:
    default:
      lfo1DelayScale = 1.0f;
      break;
  }
}

// ------------------------------------------------------------------ //
//  Main update  --  call once per loop() BEFORE mod_task()/pwm_task()  //
// ------------------------------------------------------------------ //
static inline void updateLFOs() {
  unsigned long now = micros();
  if (lfoLastMicros == 0) lfoLastMicros = now;
  float dt = (now - lfoLastMicros) * 1e-6f;   // seconds since last update
  lfoLastMicros = now;
  if (dt < 0.0f)  dt = 0.0f;
  if (dt > 0.1f)  dt = 0.1f;                  // clamp after a long block (autotune)

  // Effective rates
  lfo1.freqHz = LFO1_baseFreq * LFO_MULTIPLIERS[LFO1_multIndex % 6];
  lfo2.freqHz = LFO2_baseFreq;
  lfo2.wave   = WAVE_TRI;                      // LFO2 is always a triangle

  advanceLfo(lfo1, dt);
  advanceLfo(lfo2, dt);
  advanceLfo1Delay(dt * 1000.0f);             // advance the delay machine (ms)

  // Rate/clock outputs: phase-locked square, independent of depth/delay.
  // LFO1 follows its tempo multiplier automatically (it's baked into phase).
  LFO1_CLOCK = (lfo1.phase < LFO_CLOCK_DUTY);
  LFO2_CLOCK = (lfo2.phase < LFO_CLOCK_DUTY);

  // One-pole smoothing coefficient for this dt (de-click, always on)
  float alpha = 1.0f - expf(-dt / LFO_SMOOTH_TAU);

  // ---- LFO1 ----  smooth the raw shape once, then fan out to destinations
  lfo1.smooth += (lfoShape(lfo1) - lfo1.smooth) * alpha;        // -1..+1 (smoothed)
  float shapeDly = lfo1.smooth * lfo1DelayScale;              // -1..+1 (delay applied)

  // FM on both VCO pitch CVs (scaled by LFO1 depth / "Level")
  float s1 = shapeDly * lfo1.depth;                            // -1..+1
  FM_LFO_VALUE = s1 * FM_LFO_MAX_COUNTS;                        // direct FM (counts)
  // Mod-wheel / aftertouch source: full-amplitude LFO1, NOT scaled by LFO1 depth
  // (like the old external LFO on the ADC) - CC17/CC19/CC28 set those depths.
  LFO1_OUT     = (shapeDly * 0.5f + 0.5f) * 4095.0f;            // 0..4095, 2048 = centre

  // Aux CVs, each with its own depth, independent of FM depth.
  // (Drop the shapeDly delay term if you don't want filter/amp to fade in.)
  // Filter: UNIPOLAR -- 0 at the LFO trough, up to depth x full-scale at the peak.
  LFO1_FILTER_OUT = (shapeDly * 0.5f + 0.5f) * constrain(LFO1_filterDepth + LFO1_filterDepthAT, 0.0f, 1.0f) * LFO1_AUX_UNIPOLAR_FS;
  // Amp: BIPOLAR -- centred at LFO1_AUX_CENTER.
  LFO1_AMP_OUT    = LFO1_AUX_CENTER + shapeDly * constrain(LFO1_ampDepth + LFO1_ampDepthAT, 0.0f, 1.0f) * LFO1_AUX_FULLSWING;

  // ---- LFO2 -> PWM ----  full-scale triangle; the two independent
  //      per-AS3340 depths are applied downstream by PWM1 (CC24, summed
  //      into channel_e) and PWM2 (CC26, summed into channel_f).
  lfo2.smooth += (lfoShape(lfo2) - lfo2.smooth) * alpha;
  LFO2_OUT = (lfo2.smooth * 0.5f + 0.5f) * 4095.0f;             // 0..4095 unipolar
}

#endif  // LFO_H
