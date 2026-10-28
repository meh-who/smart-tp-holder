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

// ---- Pins (ESP32-S3, as in the Revision C plan) ----------------------------
#define PIN_ENC_A     4     // scroll-wheel encoder A   (RTC-capable, wakes the chip)
#define PIN_ENC_B     5     // scroll-wheel encoder B
#define PIN_BUTTON    6     // momentary to GND: short = repeat last, hold = new roll
#define PIN_I2S_BCLK  15    // MAX98357A BCLK
#define PIN_I2S_LRC   16    // MAX98357A LRC
#define PIN_I2S_DIN   17    // MAX98357A DIN
#define PIN_AMP_SD    18    // MAX98357A SD_MODE: high = on (left ch), low = sleep
#define PIN_VBAT      1     // 2:1 divider from BAT+ (unused on mains)
#define PIN_LED       7     // status LED through 330 Ω to GND (PWM via LEDC)

// ---- Wheel calibration ------------------------------------------------------
// Surface travel per quadrature count. 21 mm wheel (pi*21 = 65.97 mm/rev) on a
// mouse encoder with 24 cycles/rev read x4 = 96 counts/rev -> 0.687 mm/count.
// Calibrate: pull exactly five sheets (570 mm) and type  cal 570  on the serial
// console; the value is stored in NVS and overrides this default.
#define MM_PER_COUNT_DEFAULT  0.687f
#define ENCODER_INVERT        0     // set 1 if pulling paper counts negative

// ---- Playback ----------------------------------------------------------------
#define BACKLOG_CAP_MS   20000   // drop queued items beyond this much speech
#define LOW_PAPER_SHEETS 12      // speak the low-paper clip once below this
#define ROLL_SHEETS      200     // sheets on a fresh roll (used for the status page)
#define GAIN_DB          0.0f    // software gain; hardware GAIN pin sets +12 dB

// ---- Firmware -----------------------------------------------------------------
#define FW_VERSION "2.0-nfc"
