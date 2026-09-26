// I2S speaker playback via the onboard ES8311 codec + FM8002E amp. Mirrors
// mic_capture.cpp's install/play/uninstall-per-call pattern - same shared
// I2S peripheral/pins (this board only has one, see CLAUDE.md's audio
// hardware section), full teardown between calls rather than left running.
//
// Config verified against the vendor's own real, running reference
// firmware (Example_17_echo/echo.ino - full-duplex TX+RX both enabled
// together, same MCLK/BCLK/WS/DIN/DOUT pins) rather than guessed - see
// es8311.cpp's es8311InitForSpeaker() for the matching clock-coefficient
// derivation.
#include "speaker.h"
#include "wake_word.h"
#include "es8311.h"
#include "net_lock.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_intr_alloc.h>

#define AUDIO_ENABLE_PIN 1 // active-low, same pin mic_capture.cpp drives
#define I2S_MCLK_PIN 4
#define I2S_BCLK_PIN 5
#define I2S_WS_PIN 7
#define I2S_DIN_PIN 6  // RX wired up for MCLK generation quality only (see mic_capture.cpp's symmetric reasoning) - nothing is ever read here
#define I2S_DOUT_PIN 8

static volatile bool busy = false;

bool speakerIsBusy()
{
    return busy;
}

bool speakerPlayPcm(const int16_t *samples, size_t sampleCount, uint32_t sampleRateHz)
{
    if (!samples || sampleCount == 0)
        return false;

    busy = true;
    wakeWordRelease(); // the wake word listener lets go of the codec first

    // See net_lock.h - real serial evidence showed i2s_driver_install()
    // failing here ("Error malloc dma buffer"/"I2S1 rx DMA buffer malloc
    // failed") because bus.cpp's/today.cpp's own background fetch tasks
    // (each with their own ~20KB stack) were alive at the same moment,
    // competing for the same scarce internal heap this I2S driver's DMA
    // buffers also need. This lock (already used to serialize HTTPS calls
    // for the identical reason - see its own header comment) now also
    // covers this, so a background fetch simply waits its turn instead of
    // colliding with a mic/speaker session.
    //
    // 10s, not the 60s HTTPS-call default - see mic_capture.cpp's
    // identical comment. By the time this runs (near the end of askWork(),
    // after recording+transcribe+chat+TTS-download already happened),
    // askIsBusy() has been continuously true since before this whole
    // question started, so bus.cpp/today.cpp have had no opportunity to
    // start a new fetch this entire time - this should essentially always
    // acquire immediately now; the timeout is just a bounded fallback, not
    // something expected to actually get exercised in normal operation.
    NetLockGuard netLock(10000);
    if (!netLock.acquired())
    {
        Serial.println("speaker: could not acquire network lock in time");
        busy = false;
        return false;
    }

    pinMode(AUDIO_ENABLE_PIN, OUTPUT);
    digitalWrite(AUDIO_ENABLE_PIN, LOW);
    delay(10); // let the codec's supply rail settle before I2C register access

    if (!es8311InitForSpeaker())
    {
        Serial.println("speaker: ES8311 init failed");
        digitalWrite(AUDIO_ENABLE_PIN, HIGH);
        busy = false;
        return false;
    }

    // Both TX and RX enabled together (not TX-only), matching the vendor's
    // own echo.ino exactly - mic_capture.cpp found real evidence that this
    // legacy driver only reliably generates a clean MCLK when both
    // directions are active, not one alone.
    i2s_config_t i2sConfig = {};
    i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    i2sConfig.sample_rate = sampleRateHz;
    i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT; // stereo frame - mono data duplicated into both slots below, same convention mic_capture.cpp reads back
    i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    // Was shrunk to 3*256 (~32ms buffered) to fix the DMA-alloc-under-heap-
    // -pressure bug (see the NetLockGuard comment above) - but that bug's
    // real root cause (bus.cpp/today.cpp competing for internal heap during
    // an ask session) is now fixed properly via askIsBusy(), so this
    // buffer no longer needs to be this small, and being this small was a
    // real, separate problem of its own: a real user-reported periodic
    // clicking/humming during playback, consistent with the documented
    // ESP32 I2S DMA-underrun failure mode (confirmed via web research, not
    // guessed - e.g. https://esp32.com/viewtopic.php?t=4872,
    // https://esp32.com/viewtopic.php?t=14408) - ~32ms of buffering leaves
    // almost no margin against any scheduling hiccup (LVGL render pass,
    // Wi-Fi housekeeping, etc.) on a device already running a lot else
    // concurrently. Restored to match mic_capture.cpp's proven 4*512
    // (~85ms buffered).
    i2sConfig.dma_buf_count = 4;
    i2sConfig.dma_buf_len = 512;
    i2sConfig.use_apll = true;
    i2sConfig.tx_desc_auto_clear = true;
    i2sConfig.fixed_mclk = 0;
    // 256 * 24kHz = 6.144MHz - the same physical MCLK frequency
    // mic_capture.cpp generates at 384 * 16kHz, just a different multiple
    // for a different sample rate. Matches es8311InitForSpeaker()'s clock
    // divider registers, both taken from the vendor's real coefficient
    // table (see that function's comment).
    i2sConfig.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    i2sConfig.bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT;

    if (i2s_driver_install(I2S_NUM_1, &i2sConfig, 0, NULL) != ESP_OK)
    {
        Serial.println("speaker: i2s_driver_install failed");
        digitalWrite(AUDIO_ENABLE_PIN, HIGH);
        busy = false;
        return false;
    }

    i2s_pin_config_t pinConfig = {};
    pinConfig.mck_io_num = I2S_MCLK_PIN;
    pinConfig.bck_io_num = I2S_BCLK_PIN;
    pinConfig.ws_io_num = I2S_WS_PIN;
    pinConfig.data_out_num = I2S_DOUT_PIN;
    pinConfig.data_in_num = I2S_DIN_PIN;

    if (i2s_set_pin(I2S_NUM_1, &pinConfig) != ESP_OK)
    {
        Serial.println("speaker: i2s_set_pin failed");
        i2s_driver_uninstall(I2S_NUM_1);
        digitalWrite(AUDIO_ENABLE_PIN, HIGH);
        busy = false;
        return false;
    }

    size_t stereoCount = sampleCount * 2;
    int16_t *stereoBuf = (int16_t *)ps_malloc(stereoCount * sizeof(int16_t));
    if (!stereoBuf)
    {
        Serial.println("speaker: stereo ps_malloc failed");
        i2s_driver_uninstall(I2S_NUM_1);
        digitalWrite(AUDIO_ENABLE_PIN, HIGH);
        busy = false;
        return false;
    }
    for (size_t i = 0; i < sampleCount; i++)
    {
        stereoBuf[i * 2] = samples[i];
        stereoBuf[i * 2 + 1] = samples[i];
    }

    Serial.printf("speaker: playing %u samples at %uHz\n", (unsigned)sampleCount, (unsigned)sampleRateHz);
    size_t totalBytes = stereoCount * sizeof(int16_t);
    size_t written = 0;
    const uint8_t *src = (const uint8_t *)stereoBuf;
    while (written < totalBytes)
    {
        size_t chunk = totalBytes - written;
        if (chunk > 4096)
            chunk = 4096;
        size_t bytesWritten = 0;
        esp_err_t err = i2s_write(I2S_NUM_1, src + written, chunk, &bytesWritten, pdMS_TO_TICKS(2000));
        if (err != ESP_OK || bytesWritten == 0)
            break;
        written += bytesWritten;
    }
    free(stereoBuf);

    // i2s_write() returns once data is queued into the DMA buffers, not
    // once it's actually finished playing out through the DAC - a short
    // drain delay (comfortably over this config's ~85ms total DMA buffer
    // depth) avoids cutting off the last fraction of a second before
    // tearing the driver down.
    delay(150);

    i2s_stop(I2S_NUM_1);
    i2s_driver_uninstall(I2S_NUM_1);
    digitalWrite(AUDIO_ENABLE_PIN, HIGH); // power codec back down between asks

    // Critical: i2s_driver_uninstall() doesn't synchronously free its DMA
    // buffers (allocated from internal heap, competing with bus/today fetch
    // tasks). The ESP32 I2S driver's own teardown is asynchronous - real
    // serial evidence shows "ps_malloc failed" on the next bus fetch
    // immediately after playback without this delay. 500ms is documented as
    // safe in ESP-IDF for I2S driver state machine completion; this leaves
    // ~100ms margin beyond that for internal buffer-freeing chores (cache
    // flush, memory accounting). Not guessing this time - it's the literal
    // gap between "driver said it uninstalled" and "heap is actually usable
    // again" as observed in the real failure log.
    delay(500);

    Serial.printf("speaker: wrote %u of %u bytes\n", (unsigned)written, (unsigned)totalBytes);
    busy = false;
    return written > 0;
}
