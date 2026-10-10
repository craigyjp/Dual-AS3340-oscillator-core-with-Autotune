#define NOTE_SF 548.8 // 1V/Octave
#define VEL_SF 516.0

#define LED_PIN 16    // Define the pin for the built-in RGB LED
#define NUM_PIXELS 1  // Number of WS2812 LEDs

#define FORMAT_LITTLEFS_IF_FAILED true

// Voices available
#define NO_OF_VOICES 1
#define trigTimeout 20

#define FM_INPUT 26  // ADC0  - now unused (internal LFO1 replaces external FM LFO)
#define PWM_INPUT 27  // ADC1  - now unused (internal LFO2 replaces external PWM LFO)
#define POLYMOD_INPUT 28  //ADC2

// LFO rate / clock outputs (reuse the freed FM/PWM ADC pins, already broken out).
// 50% square at the LFO rate - drive a rate-indicator LED (via series resistor).
#define LFO1_CLK 26  // was FM_INPUT
#define LFO2_CLK 27  // was PWM_INPUT

//Note DACS
#define DAC_NOTE1 5

// Gate LED
#define GATE_LED 6

//VCO 1 or 2
#define VCO_SEL 8

//Autotune input pin
#define AUTO_INPUT 7

//Trig output
#define TRIG_NOTE1 15

//Tune LED for front panel
#define TUNE_LED 11

// Mux enable
#define MUX_ENABLE 12

//Gate outputs
#define GATE_NOTE1 14

//Gate outputs
#define SYNC_PIN 9

//Gate outputs
#define CLONE_LED 10

void setupHardware() {

  analogReadResolution(12);

  pinMode(CLONE_LED, INPUT_PULLUP);

  pinMode(GATE_NOTE1, OUTPUT);
  digitalWrite(GATE_NOTE1, LOW);

  pinMode(TRIG_NOTE1, OUTPUT);
  digitalWrite(TRIG_NOTE1, LOW);

  pinMode(GATE_LED, OUTPUT);
  digitalWrite(GATE_LED, LOW);

  pinMode(VCO_SEL, OUTPUT);
  digitalWrite(VCO_SEL, LOW);

  pinMode(MUX_ENABLE, OUTPUT);
  digitalWrite(MUX_ENABLE, LOW);

  pinMode(SYNC_PIN, OUTPUT);
  digitalWrite(SYNC_PIN, LOW);

  pinMode(TUNE_LED, OUTPUT);
  digitalWrite(TUNE_LED, LOW);

  pinMode(LFO1_CLK, OUTPUT);
  digitalWrite(LFO1_CLK, LOW);

  pinMode(LFO2_CLK, OUTPUT);
  digitalWrite(LFO2_CLK, LOW);

  pinMode(DAC_NOTE1, OUTPUT);
  digitalWrite(DAC_NOTE1, HIGH);

  SPI.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE1));
  digitalWrite(DAC_NOTE1, LOW);
  SPI.transfer16(int_ref_on_flexible_mode >> 16);
  SPI.transfer16(int_ref_on_flexible_mode);
  delayMicroseconds(1);
  digitalWrite(DAC_NOTE1, HIGH);
  SPI.endTransaction();

}
