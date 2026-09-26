#pragma once
#include <stdint.h>

// Minimal ES8311 codec bring-up for microphone capture and speaker
// playback. Ported directly from Espressif's own official open-source
// driver (github.com/espressif/esp-bsp/tree/master/components/es8311):
// same register addresses, same init/power-up sequence, same clock-
// coefficient table values (the mic-capture path's 16kHz row and the
// speaker path's 24kHz row - see es8311.cpp - are both taken verbatim from
// that table, not guessed) - only the I2C transport was swapped from
// ESP-IDF's i2c_master_write_to_device to Arduino's Wire. Deliberately not
// using the third-party pschatzmann/arduino-audio-driver wrapper here:
// its ES8311 support had a GitHub issue (#27) showing a LoadProhibited
// crash matching a class of bug this project already burned hours
// root-causing once (see CLAUDE.md's LV_MEM_SIZE postmortem). Correction,
// checked directly rather than left on recall: that issue is closed, and
// the crash traced to the reporter's own double-initialization/wrong
// MCLK_SOURCE mistakes, not a confirmed library defect - so this is a
// smaller-blast-radius choice (auditable ~150 lines, no new dependency),
// not evidence the wrapper is actually broken.
//
// Talks over the global `Wire` instance already brought up by the FT6336
// touch driver (shared I2C bus, SDA=IO16 SCL=IO15 per the vendor page) -
// does not call Wire.begin() itself, so either function below must run
// after touch_init().
//
// Both return false on any I2C failure (e.g. codec not powered via its
// enable pin yet - see mic_capture.cpp/speaker.cpp, which drive that
// before calling either of these).
bool es8311InitForMicCapture();
// 24kHz - OpenAI's TTS API's raw PCM output rate (see speaker.cpp). Same
// 6.144MHz MCLK as the mic path (a coincidence confirmed against the
// vendor's own coefficient table, not assumed) - only the clock-divider
// register differs between the two.
bool es8311InitForSpeaker();
// Re-applies settingsGetVolumePct() to the DAC while the speaker is playing,
// so volume changes are heard immediately. No-op when idle.
void es8311ApplyLiveVolume();
