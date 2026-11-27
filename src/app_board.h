#pragma once
/*
 * Board — on-board hardware of the Waveshare ESP32-S3-Zero:
 *   - RGB status LED (WS2812 on GPIO21)
 *   - BOOT button (GPIO0): hold 5 s while running -> reset passwords only
 *
 * LED colours: orange — starting, blue — no router, yellow — router but no internet,
 *              green — all good, purple — OTA update, blinking red — password reset.
 */
#include <Arduino.h>

namespace Board {

void begin();
void loop();                                   // LED status + BOOT button
void setLed(uint8_t r, uint8_t g, uint8_t b);  // manual colour; normal status resumes after refreshLed()
void refreshLed();                             // repaint the status colour on the next loop()

}  // namespace Board
