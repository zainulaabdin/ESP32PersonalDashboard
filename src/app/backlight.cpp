#include "backlight.h"
#include <Arduino.h>

#define TFT_BL_PIN 45
#define BACKLIGHT_LEDC_CHANNEL 0
#define BACKLIGHT_LEDC_FREQ_HZ 5000
#define BACKLIGHT_LEDC_RES_BITS 8 // duty range 0-255
// Below this the screen is effectively unreadable and touch becomes
// unusable (can't see what you're tapping) - clamped here too, not just in
// the settings slider's range, so no future caller can drive it lower.
#define MIN_BRIGHTNESS_PCT 10

static uint8_t brightnessPct = 100;
static bool backlightOn = false;

static uint32_t dutyForPct(uint8_t pct)
{
    if (pct > 100)
        pct = 100;
    return (uint32_t)pct * 255 / 100;
}

void backlightInit()
{
    ledcSetup(BACKLIGHT_LEDC_CHANNEL, BACKLIGHT_LEDC_FREQ_HZ, BACKLIGHT_LEDC_RES_BITS);
    ledcAttachPin(TFT_BL_PIN, BACKLIGHT_LEDC_CHANNEL);
    ledcWrite(BACKLIGHT_LEDC_CHANNEL, 0); // stay off until the UI is actually built and drawn
}

void backlightSetOn(bool on)
{
    backlightOn = on;
    ledcWrite(BACKLIGHT_LEDC_CHANNEL, on ? dutyForPct(brightnessPct) : 0);
}

void backlightSetBrightnessPct(uint8_t pct)
{
    if (pct > 100)
        pct = 100;
    if (pct < MIN_BRIGHTNESS_PCT)
        pct = MIN_BRIGHTNESS_PCT;
    brightnessPct = pct;
    if (backlightOn)
        ledcWrite(BACKLIGHT_LEDC_CHANNEL, dutyForPct(brightnessPct));
}

uint8_t backlightGetBrightnessPct()
{
    return brightnessPct;
}
