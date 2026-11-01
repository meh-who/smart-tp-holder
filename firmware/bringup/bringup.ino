// bringup.ino — bench test for the holder hardware, one part at a time.
// No wi-fi, no server, no SD card. Flash this first; flash holder/ after it passes.
//
// Board: "Adafruit Feather ESP32-S2", USB CDC On Boot: Enabled (first upload: hold
// BOOT, tap RESET, release BOOT). Also runs on a HUZZAH32. Libraries: ESP32Encoder,
// ESP8266Audio (the same two the real firmware uses, so a pass here means the
// real firmware's wiring is right too).
//
// Open Serial Monitor at 115200. What you should see / hear:
//   1. LED      breathes for 2 s, then double-flashes.                    -> LED wiring
//   2. Speaker  a short two-note chime.                                  -> amp + I2S + speaker
//   3. Encoder  turn the wheel: "enc +12 (+8.2 mm)" lines scroll by.      -> encoder + debounce
//               Forward pull must count UP. If it counts down, swap A/B
//               (or set ENCODER_INVERT 1 in holder/config.h later).
//   4. Button   "button down" / "button up".                              -> button
//   Every 1 mm-ish of wheel travel also gives a soft tick from the speaker,
//   so you can test the whole chain by dragging paper under the wheel.
#include <Arduino.h>
#include <ESP32Encoder.h>
#include "AudioOutputI2S.h"

// Same pins as holder/config.h.
#if defined(CONFIG_IDF_TARGET_ESP32S2)        // Adafruit ESP32-S2 Feather
#define PIN_ENC_A     5     // "5"
#define PIN_ENC_B     9     // "9"
#define PIN_BUTTON    11    // "11"
#define PIN_I2S_BCLK  18    // "A0"
#define PIN_I2S_LRC   17    // "A1"
#define PIN_I2S_DIN   16    // "A2"
#define PIN_AMP_SD    15    // "A3"
#define PIN_LED       8     // "A5"
#else                                         // Adafruit HUZZAH32 (ESP32)
#define PIN_ENC_A     32
#define PIN_ENC_B     33
#define PIN_BUTTON    27
#define PIN_I2S_BCLK  26
#define PIN_I2S_LRC   25
#define PIN_I2S_DIN   4
#define PIN_AMP_SD    18
#define PIN_LED       21
#endif
#define MM_PER_COUNT  0.687f   // rough; calibrate in the real firmware

static ESP32Encoder     enc;
static AudioOutputI2S*  out = nullptr;

static void tone_ms(float hz, int ms, float vol = 0.25f) {
  const int rate = 22050;
  int n = rate * ms / 1000;
  for (int i = 0; i < n; ++i) {
    float env = (i < 200) ? i / 200.0f : (i > n - 400 ? (n - i) / 400.0f : 1.0f);   // no clicks
    int16_t v = (int16_t)(sinf(2.0f * PI * hz * i / rate) * 32767 * vol * env);
    int16_t s[2] = {v, v};
    while (!out->ConsumeSample(s)) { }
  }
}

static void silence_ms(int ms) {
  int n = 22050 * ms / 1000;
  int16_t s[2] = {0, 0};
  for (int i = 0; i < n; ++i) while (!out->ConsumeSample(s)) { }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  delay(1500);   // native USB: give the Serial Monitor time to attach
  Serial.println("\n=== holder bring-up ===");

  // 1. LED
  ledcAttach(PIN_LED, 5000, 8);
  Serial.println("[1] LED: breathing...");
  uint32_t t0 = millis();
  while (millis() - t0 < 2000) {
    float ph = ((millis() - t0) % 1200) / 1200.0f;
    float v = 0.5f - 0.5f * cosf(ph * 2 * PI);
    ledcWrite(PIN_LED, (uint8_t)(4 + v * v * 200));
    delay(10);
  }
  for (int k = 0; k < 2; ++k) { ledcWrite(PIN_LED, 255); delay(90); ledcWrite(PIN_LED, 0); delay(110); }
  ledcWrite(PIN_LED, 120);
  Serial.println("    -> did it breathe, then flash twice? If not: LED polarity (long leg to the resistor) or the 330 Ω.");

  // 2. Amp + speaker
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, HIGH);
  out = new AudioOutputI2S();
  out->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DIN);
  out->SetRate(22050);
  out->SetBitsPerSample(16);
  out->SetChannels(2);
  out->begin();
  delay(50);
  Serial.println("[2] Speaker: chime...");
  tone_ms(660, 180); silence_ms(40); tone_ms(880, 260);
  silence_ms(100);
  Serial.println("    -> heard it? If not: VIN on the Feather's USB pin (5 V), BCLK/LRC/DIN order, SD wiring (see schematic).");

  // 3. Encoder
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  enc.attachFullQuad(PIN_ENC_A, PIN_ENC_B);
  enc.setFilter(1023);
  enc.clearCount();
  Serial.println("[3] Encoder: turn the wheel (or drag paper under it). Forward should count UP.");

  // 4. Button
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  Serial.println("[4] Button: press it.");
}

void loop() {
  static int64_t last = 0, tick_acc = 0;
  static bool btn_was = false;
  static uint32_t last_print = 0;

  int64_t c = enc.getCount();
  if (c != last) {
    int64_t d = c - last;
    last = c;
    tick_acc += d;
    if (millis() - last_print > 80) {
      Serial.printf("enc %+lld  total %lld  (%.1f mm)\n", (long long)d, (long long)c, c * MM_PER_COUNT);
      last_print = millis();
    }
    if (llabs(tick_acc) >= 2) { tick_acc = 0; tone_ms(1800, 6, 0.12f); }   // soft tick per ~1.4 mm
  }

  bool down = digitalRead(PIN_BUTTON) == LOW;
  if (down != btn_was) {
    Serial.println(down ? "button down" : "button up");
    if (down) { tone_ms(520, 60); }
    btn_was = down;
  }

  // Keep the I2S DMA fed with silence so the amp doesn't pop.
  int16_t s[2] = {0, 0};
  out->ConsumeSample(s);
}
