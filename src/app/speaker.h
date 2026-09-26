#pragma once
#include <stdint.h>
#include <stddef.h>

// Plays raw signed 16-bit mono PCM samples (e.g. OpenAI TTS's raw "pcm"
// response format) through the onboard ES8311 codec + FM8002E amp, at the
// given sample rate. Blocks until playback finishes - same one-shot
// install/play/uninstall pattern as mic_capture.h's micCaptureRecord(),
// not left running between calls (this project's general power-
// consciousness). See es8311.h for the shared codec driver.
bool speakerPlayPcm(const int16_t *samples, size_t sampleCount, uint32_t sampleRateHz);

// True while a playback (or its post-teardown settle delay) is in
// progress - lets bus.cpp/today.cpp suppress their automatic retries
// during this window even when it's NOT part of a real Ask flow (e.g. the
// 'replay'/'tone' Serial debug commands in main.cpp). Real bug this fixes:
// askIsBusy() alone only covers a genuine ask.cpp flow - repeatedly typing
// 'replay' during debug/testing left bus.cpp's/today.cpp's own automatic
// retry logic completely unprotected in the gaps between replay calls,
// each attempt (even ones net_lock.h's mutual exclusion correctly
// serialized) still adding its own internal-heap churn on top of what was
// already a heap-pressured window - a real report of Today's fetch cascade
// failing with "SSL - Memory allocation failed" repeatedly and "never
// com[ing] back" traced to exactly this gap during a burst of replay spam.
bool speakerIsBusy();
