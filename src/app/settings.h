#pragma once
#include <lvgl.h>

void settingsInit(lv_obj_t *tab);
// Loads brightness/volume/refresh-interval settings from NVS and applies
// them immediately (backlight, bus/calendar intervals) - call once in
// setup(), after backlightInit() and before settingsInit()/buildUi(), so
// the very first frame already reflects whatever was saved last session.
void settingsLoadPersisted();
// Writes any changed settings to NVS, but only if something actually
// changed since the last save - call from main.cpp's tabview
// LV_EVENT_VALUE_CHANGED handler when the tab being left is Settings.
// Frequent NVS writes (e.g. on every slider drag tick or stepper tap) wear
// the flash needlessly, so settings are batched and only flushed once the
// user navigates away from the tab instead.
void settingsOnTabLeave();
// Current speaker volume (0-100) - es8311.cpp reads this each time it
// initializes for playback, so the Settings tab's slider has a real,
// immediate effect (not just persisted-and-ignored, which was the actual
// bug behind a real user report of "volume settings has no effect").
int settingsGetVolumePct();
// Web dashboard: sets volume (0-100) live, moves the slider, saves shortly after.
void settingsSetVolumePct(int pct);
// Applies + persists bus (seconds) / calendar (minutes) refresh intervals
// and updates the tab's steppers - used by the web settings page.
void settingsSetIntervals(int busSec, int calMin);
// Moves the brightness slider to the current backlight level (night_mode.cpp
// changes it at sunrise/sunset and from the web page).
void settingsSyncBrightnessSlider();
// Polls wifiSmartConfigInProgress() and shows/hides a "Waiting for
// SmartConfig..." overlay accordingly - call every loop() iteration, same
// convention as bus/today/weather's own *Tick() functions.
void settingsTick();
