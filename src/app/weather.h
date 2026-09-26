#pragma once
#include <lvgl.h>

// Full-screen weather overlay, on lv_layer_top() (same "renders above
// everything, any tab" technique as profile.cpp's QR overlay and
// today.cpp's calendar reminder popup) rather than a tab - shown at boot,
// dismissed by any tap (which reveals the Profile tab underneath), and
// re-shown automatically after 5 minutes with no touch anywhere, or by a
// swipe down from the very top edge of the screen on any tab.
void weatherInit();
// Call every loop() iteration - drives the periodic NEA fetch and the
// 5-minute auto-reshow timer.
void weatherTick();
// Call from my_touchpad_read()'s press handling, same as powerNoteTouch(),
// so a touch anywhere resets the 5-minute auto-reshow clock even while a
// tab (not the weather screen) is what's actually on screen.
void weatherNoteTouch();
// Shows the overlay immediately (boot, swipe-down gesture, or the 5-minute
// timer firing). Safe to call if already showing (no-op).
void weatherShow();
// Closes the weather screen if it's showing (wake word -> Ask tab).
void weatherHide();
// Today's sunrise/sunset as minutes after midnight (defaults until the
// first weather fetch computes them) - used by night_mode.cpp.
int weatherSunriseMinute();
// Web dashboard settings: NEA 2-hour forecast area (one of the 47 NEA
// area names) and refresh interval (5-60 min). Both persisted in NVS.
void weatherLoadSettings(); // call before weatherInit()
int weatherAreaCount();
const char *weatherAreaNameAt(int index);
const char *weatherGetArea();
void weatherSetArea(const char *area);
int weatherGetIntervalMin();
void weatherSetIntervalMin(int minutes);
// Rain/showers/thunder in the chosen area's 2-hour forecast.
bool weatherRainSoon();
// Short plain-text weather summary for the Ask tab's model context.
void weatherGetContextSummary(char *out, size_t outSize);
int weatherSunsetMinute();
