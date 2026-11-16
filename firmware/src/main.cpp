// Nexus Band firmware
//
// The watch is the remote for the ATS 10 assistant running on the laptop
// (ats 10/dashboard/server.py). It uses the same API as the phone dashboard:
//
//   POST https://<laptop>:8001/login            {"pin": "<one-time key>"}  ->  {"token": "..."}
//   WSS  /ws?token=...                          JSON both ways
//        watch  -> {"type":"command","text":"open notepad"}
//        watch  -> {"type":"confirm","decision":"confirm" | "reject"}
//        laptop -> {"type":"sys" | ..., "text":"..."}            (activity log)
//        laptop -> {"type":"confirmation_request","title":"..","detail":".."}
//        laptop -> {"type":"confirmation_hide"}
//   WSS  /ws/phone-audio?token=...              raw 16 kHz 16-bit mono PCM from the microphones
//
// Hold the side button and speak: the audio goes to the assistant, which carries
// out the request on the laptop ("open Notepad, type hello and send it to ...").
// Anything risky comes back as a confirmation request with Approve / Reject.
//
// Status: written against the board files, not yet run on hardware.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <Adafruit_DRV2605.h>
#include <driver/i2s.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "pins.h"
#include "config.h"

// ---------------------------------------------------------------- display
class Screen : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel;
  lgfx::Bus_SPI bus;
  lgfx::Light_PWM light;
  lgfx::Touch_FT5x06 touch;

 public:
  Screen() {
    {
      auto cfg = bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.pin_sclk = PIN_LCD_SCK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_LCD_DC;
      bus.config(cfg);
      panel.setBus(&bus);
    }
    {
      auto cfg = panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.readable = false;
      cfg.invert = true;   // IPS panel
      panel.config(cfg);
    }
    {
      auto cfg = light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.freq = 20000;
      cfg.pwm_channel = 7;
      light.config(cfg);
      panel.setLight(&light);
    }
    {
      auto cfg = touch.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda = PIN_I2C_SDA;
      cfg.pin_scl = PIN_I2C_SCL;
      cfg.pin_int = PIN_TP_INT;
      cfg.freq = 400000;
      cfg.x_min = 0;
      cfg.x_max = 239;
      cfg.y_min = 0;
      cfg.y_max = 319;
      touch.config(cfg);
      panel.setTouch(&touch);
    }
    setPanel(&panel);
  }
};

static Screen lcd;
static Adafruit_DRV2605 haptic;
static WebSocketsClient cmdLink;    // /ws
static WebSocketsClient voice;   // /ws/phone-audio
static Preferences store;

static bool hapticOk = false;
static bool linkUp = false, voiceUp = false;
static String token;
static String lastLine = "Hold the button and speak";
static String askTitle, askDetail;     // pending confirmation, empty = none
static bool dirty = true;
static uint32_t lastStatus = 0, lastLogin = 0;

// The watch stays on all day; the screen is what eats the battery, so it goes dark
// when nobody is using it and comes back on the button, a touch or a laptop message.
static const uint32_t SCREEN_TIMEOUT_MS = 15000;
static const uint8_t SCREEN_BRIGHTNESS = 120;
static bool screenOn = true;
static uint32_t lastActive = 0;

static void wake() {
  lastActive = millis();
  if (screenOn) return;
  lcd.wakeup();
  lcd.setBrightness(SCREEN_BRIGHTNESS);
  screenOn = true;
  dirty = true;
}

// ---------------------------------------------------------------- power
static float batteryVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BAT_ADC);
  return (mv / 8) * 2.0f / 1000.0f;
}

static bool docked() { return analogReadMilliVolts(PIN_VBUS_SENSE) > 1500; }

static int batteryPercent(float v) {
  if (v >= 4.15f) return 100;
  if (v <= 3.30f) return 0;
  return (int)((v - 3.30f) / (4.15f - 3.30f) * 100.0f);
}

// ---------------------------------------------------------------- audio
static void audioBegin() {
  i2s_config_t mic = {};
  mic.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  mic.sample_rate = MIC_SAMPLE_RATE;
  mic.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  mic.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  mic.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  mic.dma_buf_count = 6;
  mic.dma_buf_len = 256;
  i2s_driver_install(I2S_NUM_0, &mic, 0, nullptr);
  i2s_pin_config_t micPins = {};
  micPins.mck_io_num = I2S_PIN_NO_CHANGE;
  micPins.bck_io_num = PIN_MIC_SCK;
  micPins.ws_io_num = PIN_MIC_WS;
  micPins.data_out_num = I2S_PIN_NO_CHANGE;
  micPins.data_in_num = PIN_MIC_SD;
  i2s_set_pin(I2S_NUM_0, &micPins);

  i2s_config_t spk = {};
  spk.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  spk.sample_rate = MIC_SAMPLE_RATE;
  spk.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  spk.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  spk.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  spk.dma_buf_count = 4;
  spk.dma_buf_len = 256;
  spk.tx_desc_auto_clear = true;
  i2s_driver_install(I2S_NUM_1, &spk, 0, nullptr);
  i2s_pin_config_t spkPins = {};
  spkPins.mck_io_num = I2S_PIN_NO_CHANGE;
  spkPins.bck_io_num = PIN_AMP_BCLK;
  spkPins.ws_io_num = PIN_AMP_LRCLK;
  spkPins.data_out_num = PIN_AMP_DIN;
  spkPins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_1, &spkPins);
}

// Read one block from both microphones, mix to 16-bit mono. Returns sample count.
// The two capsules sit 30 mm apart on the right edge; summing them favours sound
// arriving broadside (the wearer's mouth) over sound from along the arm.
static size_t micRead(int16_t *out, size_t maxSamples) {
  static int32_t raw[512];
  size_t got = 0;
  size_t want = min(sizeof(raw), maxSamples * 8);
  i2s_read(I2S_NUM_0, raw, want, &got, pdMS_TO_TICKS(20));
  size_t frames = got / 8;
  for (size_t i = 0; i < frames; i++) {
    int32_t l = raw[2 * i] >> 14, r = raw[2 * i + 1] >> 14;
    int32_t m = (l + r) / 2;
    out[i] = (int16_t)constrain(m, -32768, 32767);
  }
  return frames;
}

static void beep(int hz, int ms) {
  digitalWrite(PIN_AMP_SD, HIGH);
  const int n = MIC_SAMPLE_RATE * ms / 1000;
  static int16_t buf[128 * 2];
  for (int i = 0; i < n;) {
    int k = 0;
    for (; k < 128 && i < n; k++, i++) {
      int16_t s = (int16_t)(5000 * sinf(2 * PI * hz * i / MIC_SAMPLE_RATE));
      buf[2 * k] = s;
      buf[2 * k + 1] = s;
    }
    size_t out;
    i2s_write(I2S_NUM_1, buf, k * 4, &out, portMAX_DELAY);
  }
  delay(30);
  digitalWrite(PIN_AMP_SD, LOW);
}

static void buzz(uint8_t effect = 1) {
  if (!hapticOk) return;
  haptic.setWaveform(0, effect);
  haptic.setWaveform(1, 0);
  haptic.go();
}

// ---------------------------------------------------------------- laptop link
static bool login() {
  // The laptop shows a one-time key; put it in config_local.h as PAIR_KEY for the first boot.
  WiFiClientSecure tls;
  tls.setInsecure();             // the daemon uses a self-signed certificate on the LAN
  HTTPClient http;
  if (!http.begin(tls, String("https://") + LAPTOP_HOST + ":" + LAPTOP_PORT + "/login")) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(String("{\"pin\":\"") + PAIR_KEY + "\"}");
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && doc["ok"]) {
      token = doc["token"].as<String>();
      store.putString("token", token);
      ok = true;
    }
  }
  http.end();
  return ok;
}

static void onLink(WStype_t type, uint8_t *payload, size_t len) {
  if (type == WStype_CONNECTED) {
    linkUp = true;
  } else if (type == WStype_DISCONNECTED) {
    if (linkUp) lastLine = "Laptop link lost";
    linkUp = false;
  } else if (type == WStype_TEXT) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;
    const char *t = doc["type"] | "";
    if (!strcmp(t, "confirmation_request")) {
      askTitle = doc["title"] | "Confirm";
      askDetail = doc["detail"] | "";
      wake();
      buzz(14);
    } else if (!strcmp(t, "confirmation_hide")) {
      askTitle = "";
    } else if (doc["text"].is<const char *>()) {
      lastLine = doc["text"].as<String>().substring(0, 120);
      wake();
    }
  }
  dirty = true;
}

static void onVoice(WStype_t type, uint8_t *, size_t) {
  if (type == WStype_CONNECTED) voiceUp = true;
  if (type == WStype_DISCONNECTED) voiceUp = false;
}

static void connectLink() {
  String q = String("?token=") + token;
  cmdLink.beginSSL(LAPTOP_HOST, LAPTOP_PORT, (String("/ws") + q).c_str());
  cmdLink.onEvent(onLink);
  cmdLink.setReconnectInterval(3000);
  voice.beginSSL(LAPTOP_HOST, LAPTOP_PORT, (String("/ws/phone-audio") + q).c_str());
  voice.onEvent(onVoice);
  voice.setReconnectInterval(3000);
}

static void sendCommand(const String &text) {
  JsonDocument doc;
  doc["type"] = "command";
  doc["text"] = text;
  String out;
  serializeJson(doc, out);
  cmdLink.sendTXT(out);
}

static void sendDecision(bool approve) {
  cmdLink.sendTXT(approve ? "{\"type\":\"confirm\",\"decision\":\"confirm\"}"
                       : "{\"type\":\"confirm\",\"decision\":\"reject\"}");
}

// ---------------------------------------------------------------- UI (320 x 240, landscape)
static void draw(bool talking) {
  float v = batteryVolts();
  lcd.startWrite();
  lcd.fillScreen(0x0841);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextColor(0xBDF7);
  lcd.setCursor(8, 6);
  lcd.printf("%d%%%s", batteryPercent(v), docked() ? "  charging" : "");
  lcd.setCursor(190, 6);
  lcd.print(linkUp ? "laptop ok" : (WiFi.isConnected() ? "no laptop" : "no Wi-Fi"));
  lcd.setTextColor(TFT_WHITE);
  if (askTitle.length()) {
    lcd.setCursor(8, 44);
    lcd.print(askTitle);
    lcd.setTextColor(0xBDF7);
    lcd.setCursor(8, 72);
    lcd.print(askDetail.substring(0, 110));
    lcd.fillRoundRect(8, 176, 146, 56, 8, 0x2589);
    lcd.fillRoundRect(166, 176, 146, 56, 8, 0xA145);
    lcd.setTextColor(TFT_WHITE);
    lcd.setCursor(46, 196);
    lcd.print("Approve");
    lcd.setCursor(212, 196);
    lcd.print("Reject");
  } else {
    lcd.setCursor(8, 60);
    lcd.print(talking ? "Listening..." : lastLine);
    // three one-tap commands for when talking is not an option
    const char *quick[3] = {"Wake", "Screenshot", "Lock PC"};
    for (int i = 0; i < 3; i++) {
      lcd.fillRoundRect(8 + i * 104, 184, 96, 48, 8, 0x2124);
      lcd.setCursor(18 + i * 104, 200);
      lcd.print(quick[i]);
    }
  }
  lcd.endWrite();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AMP_SD, LOW);
  analogReadResolution(12);

  pinMode(PIN_TP_RST, OUTPUT);   // reset the touch controller before the driver probes it
  digitalWrite(PIN_TP_RST, LOW);
  delay(10);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(200);

  lcd.init();
  lcd.setRotation(1);            // landscape; use 3 if the picture is upside down in the case
  lcd.setBrightness(SCREEN_BRIGHTNESS);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  hapticOk = haptic.begin(&Wire);
  if (hapticOk) {
    haptic.useLRA();
    haptic.selectLibrary(6);
    haptic.setMode(DRV2605_MODE_INTTRIG);
  }

  audioBegin();
  beep(880, 80);
  buzz();

  store.begin("nexus", false);
  token = store.getString("token", "");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  static bool started = false;
  if (WiFi.isConnected() && !started) {
    if (token.length() || login()) {
      connectLink();
      started = true;
    } else if (millis() - lastLogin > 10000) {
      lastLogin = millis();
      lastLine = "Pairing failed, check PAIR_KEY";
      dirty = true;
    }
  }
  if (started) {
    cmdLink.loop();
    voice.loop();
    // a token the laptop no longer knows (it restarted) closes the socket straight away: pair again
    if (!linkUp && millis() - lastLogin > 15000) {
      lastLogin = millis();
      if (login()) {
        cmdLink.disconnect();
        voice.disconnect();
        connectLink();
      }
    }
  }

  // push to talk
  static bool wasDown = false;
  bool down = digitalRead(PIN_BUTTON) == LOW;
  if (down != wasDown) {
    wake();
    buzz(down ? 1 : 10);
    dirty = true;
    wasDown = down;
  }
  static int16_t pcm[256];
  size_t n = micRead(pcm, 256);   // always drain the I2S buffer
  if (down && voiceUp && n) voice.sendBIN((uint8_t *)pcm, n * 2);

  // touch
  int32_t x, y;
  static bool wasTouched = false;
  bool touched = lcd.getTouch(&x, &y);
  if (touched && !wasTouched && !screenOn) {
    wake();                       // the first tap on a dark screen only wakes it
  } else if (touched && !wasTouched && y > 170) {
    wake();
    if (askTitle.length()) {
      sendDecision(x < 160);
      askTitle = "";
    } else if (linkUp) {
      static const char *cmd[3] = {"wake up", "take a screenshot", "lock the computer"};
      sendCommand(cmd[constrain(x / 107, 0, 2)]);
    }
    buzz(10);
    dirty = true;
  }
  wasTouched = touched;

  if (touched) lastActive = millis();
  if (screenOn && (dirty || millis() - lastStatus > 5000)) {
    dirty = false;
    lastStatus = millis();
    draw(down);
  }

  // screen off when idle (never while talking or while a confirmation is waiting)
  if (screenOn && !down && !askTitle.length() && millis() - lastActive > SCREEN_TIMEOUT_MS) {
    lcd.setBrightness(0);
    lcd.sleep();
    screenOn = false;
  }

  // protect the cell: sleep when it is nearly empty and not charging
  if (!docked() && batteryVolts() < 3.35f && millis() > 5000) {
    lcd.setBrightness(0);
    esp_deep_sleep_start();
  }
}
