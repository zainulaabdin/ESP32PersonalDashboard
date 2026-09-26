#pragma once
#include <stdint.h>
#include <stddef.h>

// I2S microphone capture via the onboard ES8311 codec (see es8311.h/.cpp).
// Fully synchronous and blocking for the duration of the recording - the
// caller (ask.cpp) must run this on its own FreeRTOS task, same as every
// other slow operation in this codebase (bus.cpp/today.cpp's fetch tasks).
struct MicRecording
{
    uint8_t *wavData; // ps_malloc()'d - caller must free() when done. NULL on failure.
    size_t wavSize;
};

// Called (if non-null) the instant the network/memory-contention lock is
// actually acquired and I2S/DMA setup is about to start - i.e. the point
// where recording is genuinely about to begin. Added because
// micCaptureRecord() can legitimately block for a while beforehand waiting
// on that lock (e.g. behind an in-flight Calendar fetch), and the caller
// needs a real signal for "now show Listening..." rather than assuming the
// call starts recording immediately.
typedef void (*MicRecordingStartedCb)();

// Drives the codec's active-low enable pin, brings up the ES8311 + I2S RX,
// records up to maxSeconds of 16kHz mono 16-bit audio (or less, if
// micRequestStop() is called first), wraps it in a 44-byte WAV header, then
// fully tears the codec/I2S back down - no lingering power draw or state
// between recordings, matching this project's general power-consciousness
// (power.cpp).
bool micCaptureRecord(MicRecording *out, int maxSeconds, MicRecordingStartedCb onRecordingStarted = nullptr);

// Call from the UI thread to end an in-progress recording early (e.g. the
// user taps the mic again while "Listening...").
void micRequestStop();

// Continuous 16kHz mono stream for the wake word listener (wake_word.cpp).
// Only one user of the codec/I2S at a time - wake_word.cpp closes this
// before micCaptureRecord()/speaker playback run (wakeWordRelease()).
bool micStreamOpen();
int micStreamRead(int16_t *mono, int samples); // returns samples read
void micStreamClose();
