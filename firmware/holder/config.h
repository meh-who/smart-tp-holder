// config.h — everything you edit before flashing lives here.
#pragma once

// ---- Identity ---------------------------------------------------------------
// HOLDER_ID is the last path segment of the URL written to the NFC tag:
//     https://<your server>/r/<HOLDER_ID>
// HOLDER_KEY is a shared secret the server checks on every holder request.
// Register both on the server:  python manage.py add-holder <id> <key>
#define HOLDER_ID   "lisbon-01"
#define HOLDER_KEY  "change-me-to-a-long-random-string"

// ---- Network ---------------------------------------------------------------
#define WIFI_SSID   "your-wifi"
#define WIFI_PASS   "your-password"
#define SERVER_URL  "https://holder.example.org"   // no trailing slash
// For a self-signed or unknown CA on the venue network, set INSECURE_TLS 1
// (the holder plays whatever the server sends; this is a gallery, not a bank).
#define INSECURE_TLS 1

// ---- Power -----------------------------------------------------------------
// The exhibition runs from a wall adapter, so the holder stays awake, keeps
// WiFi up and asks the server every POLL_MAINS_S whether the visitor changed
// the source. On battery it reverts to the Revision C behaviour: light sleep
// between pulls and an hourly sync.
#define MAINS_POWERED   1
#define POLL_MAINS_S    20
#define POLL_BATTERY_S  3600

// ---- Pins -------------------------------------------------------------------
// Picked automatically from the board you select in the Arduino IDE.
#if defined(CONFIG_IDF_TARGET_ESP32)
// Adafruit HUZZAH32 – ESP32 Feather (original ESP32). Labels as printed on the board.
//   Avoid 12 (boot strap, pull-down), 0/2/5/15 (strapping), 34–39 (input only),
//   13 (drives the onboard red LED). 32/33 have internal pull-ups and can wake the chip.
#define PIN_ENC_A     32    // "32"  scroll-wheel encoder A
#define PIN_ENC_B     33    // "33"  scroll-wheel encoder B
#define PIN_BUTTON    27    // "27"  momentary to GND: short = repeat last, hold = new roll
#define PIN_I2S_BCLK  26    // "A0"  MAX98357A BCLK
#define PIN_I2S_LRC   25    // "A1"  MAX98357A LRC
#define PIN_I2S_DIN   4     // "A5"  MAX98357A DIN
#define PIN_AMP_SD    18    // "MO"  MAX98357A SD: high = on, low = shutdown (SPI is not used)
#define PIN_LED       21    // "21"  status LED through 330 Ω to GND (PWM via LEDC)
#define PIN_VBAT      35    // "A13" built-in VBAT/2 divider on the Feather (battery mode only)
#else
// ESP32-S3 boards (the Revision C plan).
#define PIN_ENC_A     4
#define PIN_ENC_B     5
#define PIN_BUTTON    6
#define PIN_I2S_BCLK  15
#define PIN_I2S_LRC   16
#define PIN_I2S_DIN   17
#define PIN_AMP_SD    18
#define PIN_LED       7
#define PIN_VBAT      1     // needs an external 2 × 100 kΩ divider from BAT+
#endif

// ---- Wheel calibration ------------------------------------------------------
// Surface travel per quadrature count = pi x wheel diameter / counts per rev.
// The default assumes a ~21 mm wheel and 96 counts/rev. The Logitech G403/G603/
// G703 replacement wheel is a different size, so calibrate once rather than
// trusting this: type  cal  on the serial console, pull exactly five sheets
// (570 mm), tear, type  cal 570 . The value is stored in flash and overrides this.
#define MM_PER_COUNT_DEFAULT  0.687f
#define ENCODER_INVERT        0     // set 1 if pulling paper counts negative

// ---- Playback ----------------------------------------------------------------
#define BACKLOG_CAP_MS   20000   // drop queued items beyond this much speech
#define LOW_PAPER_SHEETS 12      // speak the low-paper clip once below this
#define ROLL_SHEETS      200     // sheets on a fresh roll (used for the status page)
#define GAIN_DB          0.0f    // software trim. Hardware: GAIN open = 9 dB, GAIN to GND = 12 dB

// ---- Firmware -----------------------------------------------------------------
#define FW_VERSION "2.1-huzzah32"
