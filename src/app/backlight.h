#pragma once
#include <stdint.h>

// PWM-driven TFT backlight (TFT_BL_PIN, GPIO45) - gives both the Settings
// tab's brightness slider and power.cpp's sleep/wake blanking one shared
// control path, instead of the plain digitalWrite() on/off this used to be
// (which can't do variable brightness at all).
void backlightInit();
// Full on/off at the last-set brightness level - what power.cpp calls to
// blank/restore the screen for light/deep sleep.
void backlightSetOn(bool on);
// 0-100. Takes effect immediately if the backlight is currently on.
void backlightSetBrightnessPct(uint8_t pct);
uint8_t backlightGetBrightnessPct();
