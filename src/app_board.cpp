#include "app_board.h"
#include "app_config.h"
#include "app_network.h"

namespace Board {

static const int      RGB_PIN    = 21;     // built-in WS2812 on the S3-Zero
static const uint32_t RESET_HOLD = 5000;   // ms to hold BOOT for a password reset
// BOOT_PIN (GPIO0) is already defined by the framework (esp32-hal.h)

static int s_ledState = -1;                // -1 = repaint on the next update

void setLed(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(RGB_PIN, r, g, b); }
void refreshLed() { s_ledState = -1; }

void begin() {
  setLed(40, 20, 0);                       // orange: starting
  pinMode(BOOT_PIN, INPUT_PULLUP);         // pressed = LOW
}

static void updateLed() {
  static uint32_t last = 0;                // 10 updates per second is plenty
  if (millis() - last < 100) return;
  last = millis();
  int state = !Net::staConnected() ? 0 : !Net::internetOk() ? 1 : 2;
  if (state == s_ledState) return;         // do not rewrite the LED needlessly
  s_ledState = state;
  if (state == 0)      setLed(0, 0, 40);   // blue
  else if (state == 1) setLed(40, 30, 0);  // yellow
  else                 setLed(0, 40, 0);   // green
}

// Hold BOOT for 5 s WHILE RUNNING -> erase only the passwords (AP name/password, panel password).
// Router, static IP, filter settings and both lists are kept.
// Note: holding BOOT while POWERING ON enters the ROM flashing mode instead.
static void passwordResetWatch() {
  static uint32_t pressedAt = 0;
  if (digitalRead(BOOT_PIN) == HIGH) {     // not pressed
    if (pressedAt) { pressedAt = 0; refreshLed(); }   // released early — back to the status colour
    return;
  }
  if (!pressedAt) pressedAt = millis();
  uint32_t held = millis() - pressedAt;
  setLed((held / 200) % 2 ? 40 : 0, 0, 0); // blink red while held
  if (held >= RESET_HOLD) {
    Serial.println("[RESET] password reset: AP and web panel");
    Config::resetPasswords();
    setLed(40, 0, 0);
    delay(1000);
    ESP.restart();
  }
}

void loop() {
  passwordResetWatch();
  updateLed();
}

}  // namespace Board
