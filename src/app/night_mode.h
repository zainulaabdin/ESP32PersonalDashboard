#pragma once
#include <lvgl.h>

// Night mode: at night the backlight uses the night brightness and the tabs
// switch to dark colours - background, border and text colours only, never
// images (see night_mode.cpp). Overlays on lv_layer_top() (weather screen,
// QR codes, OTA/provisioning) keep their own look; the weather screen
// switches to its night background instead.
//
// Setting: Auto = night between sunset and sunrise (weather.cpp's computed
// times for Singapore); Night / Day = stay in that mode regardless of time.
enum NightModeSetting
{
    NIGHT_MODE_AUTO,
    NIGHT_MODE_NIGHT,
    NIGHT_MODE_DAY,
};

void nightModeInit(); // after settingsLoadPersisted(), before buildUi()
// Call every loop(), right before lv_refr_now().
void nightModeTick();

NightModeSetting nightModeGetSetting();
void nightModeSetSetting(NightModeSetting setting);
bool nightModeIsNight(); // night right now (per the setting / time)

// Brightness 10-100 for each period; the current one is applied at once.
int nightModeDayBrightness();
int nightModeNightBrightness();
void nightModeSetBrightness(int dayPct, int nightPct);
// Settings tab slider: changes whichever period is current (not saved
// until nightModeSave(), which the Settings tab calls when it saves).
void nightModeSetCurrentBrightness(int pct);
void nightModeSave();
// Serial "night" command: force night on/off regardless of the time.
void nightModeToggleTest();
