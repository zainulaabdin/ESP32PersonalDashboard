#pragma once
#include <lvgl.h>

void askInit(lv_obj_t *tab);
void askTick();
// True from the moment a question starts (recording) until it's fully
// done (answer shown, or errored out) - bus.cpp/today.cpp check this
// before starting a new background fetch, so a voice interaction never
// has to race them for net_lock.h's shared lock (see mic_capture.cpp's/
// speaker.cpp's comments - a real user-reported bug traced to exactly
// that race, not a fixed timeout being wrong).
bool askIsBusy();
// Last spoken answer (kept in PSRAM until the next answer or a reboot).
bool askHasReplay();
// Plays it again; false if there's none, or a question/playback is running.
bool askReplayLast();
// Spoken answers on/off (the Ask tab's speaker toggle).
bool askSoundEnabled();
void askSetSoundEnabled(bool on);
