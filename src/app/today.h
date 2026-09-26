#pragma once
#include <lvgl.h>
#include <time.h>

void todayInit(lv_obj_t *tab);
// Call every loop() iteration - drives the auto-refresh timer.
void todayTick();
// Call when the Today tab becomes the active tab, so its agenda loads
// on first view instead of sitting empty until the user does something.
void todayOnTabShown();
// Call whenever the active tab changes, so periodic auto-refresh only
// fires while the Today tab is actually the one being looked at.
void todaySetTabActive(bool active);
// Settings tab controls - clamped to [1, 180] minutes.
void todaySetRefreshIntervalMin(int minutes);
int todayGetRefreshIntervalMin();

// Exposed for the Ask tab's context-injection (see ask.cpp): a short
// plain-text summary of the next few upcoming events (today or later, not
// just "today"), safe to read any time.
void todayGetContextSummary(char *out, size_t outSize);

// Same role as bus.h's busIsFetchInProgress(): lets ask.cpp wait out an
// ALREADY-RUNNING fetch before starting its own recording/playback, not
// just block a NEW one from starting (that's askIsBusy()'s job, the other
// direction). Real bug this fixes: both today's fetch task and every part
// of the Ask flow (recording, TTS download, playback) are pinned to core 0
// (xTaskCreatePinnedToCore(..., 0)) - askIsBusy() alone doesn't stop an
// already-in-flight Today fetch from continuing to compete for that same
// core's CPU time while Ask's own I2S sessions are running, which is
// timing-sensitive (real-time audio). User-reported, 100% reproducible:
// "if I rush to Ask before letting the calendar load" - i.e. tapping the
// mic while a Today fetch is still mid-flight - "audio comes out horrible"
// and a SPIFFS write can even fail outright (0 of N bytes written) from the
// same contention.
bool todayIsFetchInProgress();

// Epoch of the next upcoming timed (non-all-day) event's start, or 0 if
// there is none cached. Used by power.cpp to decide whether a deep-sleep
// timer needs to be armed earlier than the normal 08:00 wake alarm, so the
// 15-minute event reminder popup can still fire overnight/on weekends.
time_t todayGetNextEventStartEpoch();
