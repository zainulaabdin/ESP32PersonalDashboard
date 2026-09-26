// I2S mic capture via the onboard ES8311 codec. Pin assignments are this
// board's fixed hardware wiring (confirmed from the vendor page, see
// CLAUDE.md's audio hardware section), not a software choice.
#include "mic_capture.h"
#include "es8311.h"
#include "net_lock.h"
#include "wake_word.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <esp_intr_alloc.h>

#define AUDIO_ENABLE_PIN 1 // active-low: LOW enables the codec, HIGH disables it
#define I2S_MCLK_PIN 4
#define I2S_BCLK_PIN 5
#define I2S_WS_PIN 7
// The vendor's own PDF pin table has DIN/DOUT backwards from their actual
// working reference firmware (Example_17_echo's ESP_Panel_Board_Custom.h,
// read directly off the user's downloaded vendor package): that header
// defines I2S_DINT=6 and I2S_DOUT=8, the opposite of the PDF's "I2S_DO
// IO6 / I2S_DI IO8". This was the actual root cause of every "silent
// recording" test in this session - real mic data was on IO6 the whole
// time, and this code was reading IO8 (the unused speaker-output pin, per
// the vendor's own real firmware) instead.
#define I2S_DIN_PIN 6   // mic data in - confirmed against vendor reference firmware, not the (wrong) PDF table
#define I2S_DOUT_PIN 8  // speaker data out - wired up only so I2S_MODE_TX has a real pin (see MCLK comment below), nothing is ever transmitted

#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define CHANNELS 1
#define WAV_HEADER_SIZE 44
// Hard ceiling regardless of what the caller asks for - bounds both the
// PSRAM allocation (8s * 16kHz * 2 bytes = 256KB, trivial against this
// board's 8MB PSRAM) and the per-question OpenAI transcription cost.
#define MAX_RECORD_SECONDS_HARD_CAP 8

static volatile bool stopRequested = false;

void micRequestStop()
{
    stopRequested = true;
}

static void writeWavHeader(uint8_t *buf, uint32_t dataBytes, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t numChannels)
{
    uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t riffChunkSize = 36 + dataBytes;
    uint32_t subchunk1Size = 16;
    uint16_t audioFormat = 1; // PCM

    memcpy(buf + 0, "RIFF", 4);
    memcpy(buf + 4, &riffChunkSize, 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    memcpy(buf + 16, &subchunk1Size, 4);
    memcpy(buf + 20, &audioFormat, 2);
    memcpy(buf + 22, &numChannels, 2);
    memcpy(buf + 24, &sampleRate, 4);
    memcpy(buf + 28, &byteRate, 4);
    memcpy(buf + 32, &blockAlign, 2);
    memcpy(buf + 34, &bitsPerSample, 2);
    memcpy(buf + 36, "data", 4);
    memcpy(buf + 40, &dataBytes, 4);
}

// Powers the codec and installs the I2S RX driver (16kHz, both slots).
// Cleans up and returns false on any failure. Caller holds the net lock
// (DMA buffers come from the same internal heap as TLS).
// True while the mic I2S driver is installed - by the wake word stream or
// by micCaptureRecord(). A recording takes over an already-open stream
// instead of uninstalling/reinstalling: the DMA buffers freed by an
// uninstall aren't back in time for an immediate reinstall (real log:
// "i2s_alloc_dma_buffer: Error malloc dma buffer" right after "Jarvis").
static bool micDriverInstalled = false;

static bool installMicI2S()
{
    pinMode(AUDIO_ENABLE_PIN, OUTPUT);
    digitalWrite(AUDIO_ENABLE_PIN, LOW);
    delay(10); // let the codec's supply rail settle before I2C register access

    if (!es8311InitForMicCapture())
    {
        Serial.println("mic_capture: ES8311 init failed");
        digitalWrite(AUDIO_ENABLE_PIN, HIGH);
        return false;
    }

    // Captured as stereo (both I2S slots) rather than assuming mono data
    // sits in the left slot - a first version assumed
    // I2S_CHANNEL_FMT_ONLY_LEFT and got back near-silence (confirmed via
    // OpenAI returning an empty transcript on a real 8s clip, then via
    // this project's own per-sample amplitude stats: peak briefly nonzero
    // but average ~0, exactly what reading an unused/empty slot looks
    // like). Capturing both slots and picking whichever one actually has
    // signal (see the analysis below) is correct regardless of which
    // physical slot this codec's ADC output lands in.
    // Chip ID + register readback (see es8311.cpp) proved the codec's I2C
    // control path and register config are exactly right, and recordings
    // still came back silent - so the remaining suspect is the ESP32 side
    // of the clock generation. Two changes here, both cheap/low-risk and
    // both specifically about MCLK quality/generation:
    //   1. I2S_MODE_TX is enabled alongside RX (data_out_num wired to this
    //      board's real DOUT pin, even though nothing is ever transmitted)
    //      - some ESP32-S3 legacy-driver versions only reliably drive a
    //        real MCLK output when the TX unit is also active, not RX-only.
    //   2. use_apll = true - the integer clock divider used without APLL
    //      can't always hit an exact 4.096MHz MCLK, and driving an
    //      external codec's PLL from an imprecise/jittery reference clock
    //      is a documented common cause of a codec producing no valid
    //      audio output at all (not just pitch drift).
    i2s_config_t i2sConfig = {};
    i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX);
    i2sConfig.sample_rate = SAMPLE_RATE;
    i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2sConfig.dma_buf_count = 4;
    i2sConfig.dma_buf_len = 512;
    i2sConfig.use_apll = true;
    i2sConfig.tx_desc_auto_clear = true;
    i2sConfig.fixed_mclk = 0;
    // 384 * 16kHz = 6.144MHz - matches the vendor's own proven-working
    // reference firmware (echo.ino's EXAMPLE_MCLK_MULTIPLE), not the 256x
    // this code used before finding that reference. es8311.cpp's clock
    // divider registers are set to match this exact MCLK.
    i2sConfig.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    i2sConfig.bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT;

    // I2S_NUM_1, not I2S_NUM_0 - matches the vendor reference firmware
    // (ESP_Panel_Board_Custom.h's I2S_NUM), kept in case port 0 has some
    // other conflict on this board.
    if (i2s_driver_install(I2S_NUM_1, &i2sConfig, 0, NULL) != ESP_OK)
    {
    Serial.println("mic_capture: i2s_driver_install failed");
    digitalWrite(AUDIO_ENABLE_PIN, HIGH);
    return false;
    }

    i2s_pin_config_t pinConfig = {};
    pinConfig.mck_io_num = I2S_MCLK_PIN;
    pinConfig.bck_io_num = I2S_BCLK_PIN;
    pinConfig.ws_io_num = I2S_WS_PIN;
    pinConfig.data_out_num = I2S_DOUT_PIN; // TX enabled for MCLK generation only - nothing is ever written to it
    pinConfig.data_in_num = I2S_DIN_PIN;

    if (i2s_set_pin(I2S_NUM_1, &pinConfig) != ESP_OK)
    {
    Serial.println("mic_capture: i2s_set_pin failed");
    i2s_driver_uninstall(I2S_NUM_1);
    digitalWrite(AUDIO_ENABLE_PIN, HIGH);
    return false;
    }
    return true;
}

// ---- Continuous stream (wake word listener) -------------------------------

bool micStreamOpen()
{
    if (micDriverInstalled)
        return true; // still open from before (e.g. a handover nobody used)
    NetLockGuard netLock(3000);
    if (!netLock.acquired())
        return false;
    micDriverInstalled = installMicI2S();
    return micDriverInstalled;
}

int micStreamRead(int16_t *mono, int samples)
{
    static int16_t *stereo = (int16_t *)ps_malloc(1024 * 2 * sizeof(int16_t)); // PSRAM - internal RAM is the scarce one
    if (!stereo)
        return 0;
    if (samples > 1024)
        samples = 1024;
    size_t bytesRead = 0;
    if (i2s_read(I2S_NUM_1, stereo, samples * 4, &bytesRead, pdMS_TO_TICKS(200)) != ESP_OK)
        return 0;
    int frames = bytesRead / 4;
    // Same "whichever slot carries the signal" choice as micCaptureRecord().
    int64_t sumA = 0, sumB = 0;
    for (int i = 0; i < frames; i++)
    {
        sumA += abs(stereo[i * 2]);
        sumB += abs(stereo[i * 2 + 1]);
    }
    int slot = sumA >= sumB ? 0 : 1;
    // 4x digital gain for the wake word only (recordings aren't boosted):
    // this mic is quiet - real peaks ~500-850 of 32767 - and WakeNet was
    // missing softer / non-native pronunciations of "Jarvis".
    const int WAKE_GAIN = 4;
    for (int i = 0; i < frames; i++)
    {
        int v = stereo[i * 2 + slot] * WAKE_GAIN;
        mono[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    return frames;
}

void micStreamClose()
{
    if (!micDriverInstalled)
        return;
    i2s_stop(I2S_NUM_1);
    i2s_driver_uninstall(I2S_NUM_1);
    digitalWrite(AUDIO_ENABLE_PIN, HIGH);
    micDriverInstalled = false;
    delay(300); // DMA buffers are freed asynchronously - let them come back before the next install
}

bool micCaptureRecord(MicRecording *out, int maxSeconds, MicRecordingStartedCb onRecordingStarted)
{
    out->wavData = NULL;
    out->wavSize = 0;
    stopRequested = false;

    wakeWordRelease(true); // the wake word listener hands its open stream over

    if (maxSeconds > MAX_RECORD_SECONDS_HARD_CAP)
        maxSeconds = MAX_RECORD_SECONDS_HARD_CAP;
    if (maxSeconds < 1)
        maxSeconds = 1;

    // See net_lock.h/speaker.cpp's identical comment - this driver's I2S
    // DMA buffer allocation competes for the same scarce internal heap as
    // bus.cpp's/today.cpp's background fetch tasks' stacks; serializing
    // against them here too, not just on the speaker side.
    //
    // Narrowed to two short scopes (install-only, then stop/uninstall+
    // drain-only) instead of covering the whole function: the only
    // internal-heap activity here is DMA buffer alloc at install and
    // dealloc at uninstall (see the delay(500) below) - the 8-second
    // i2s_read() loop in between does no internal-heap allocation, so
    // holding the lock across it was only ever blocking Calendar/Bus
    // fetches from making progress during a recording for no real reason.
    //
    // 21s, not NetLockGuard's 60s HTTPS-call default, and not a guessed
    // round number either: today.cpp's fetchAgendaWork() streams the ICS
    // feed line-by-line directly off the live TLS socket (its own
    // `while (... && millis() - fetchStart < 20000)` loop, today.cpp:391)
    // and only releases its NetLockGuard once that streaming read finishes
    // (http.end() then function return) - so a mic tap that races a
    // just-started Calendar fetch can legitimately need to wait up to
    // today.cpp's own real 20s worst case, not an invented shorter number.
    // 1s added as margin for the small amount of non-streaming work in
    // fetchAgendaWork() around that loop (connect, headers, qsort).
    // Earlier attempts at 3s and 10s were found (via real serial captures)
    // to be shorter than Calendar's actual in-flight duration and failed
    // outright instead of waiting it out - this value is read directly
    // from the code actually holding the lock, not tuned by trial and
    // error against one network's observed speed.
    {
        NetLockGuard netLock(21000);
        if (!netLock.acquired())
        {
            Serial.println("mic_capture: could not acquire network lock in time");
            return false;
        }

        if (micDriverInstalled)
            Serial.println("mic_capture: taking over the wake word stream");
        else if (!installMicI2S())
            return false;
        micDriverInstalled = true;
    } // NetLockGuard released here - install/setup done, recording loop below runs lock-free

    if (onRecordingStarted)
        onRecordingStarted(); // real recording is about to begin now - see mic_capture.h's comment

    // Mono target size (what the final WAV needs) vs. the raw stereo
    // capture size (2x - both slots, 2 bytes each) - see the
    // channel_format comment above for why this captures stereo at all.
    uint32_t maxMonoBytes = (uint32_t)maxSeconds * SAMPLE_RATE * (BITS_PER_SAMPLE / 8) * CHANNELS;
    uint32_t maxStereoBytes = maxMonoBytes * 2;
    uint8_t *stereoBuf = (uint8_t *)ps_malloc(maxStereoBytes);
    if (!stereoBuf)
    {
        Serial.println("mic_capture: stereo ps_malloc failed");
        i2s_driver_uninstall(I2S_NUM_1);
        digitalWrite(AUDIO_ENABLE_PIN, HIGH);
        return false;
    }

    Serial.printf("mic_capture: recording up to %ds\n", maxSeconds);
    uint32_t stereoRecorded = 0;
    const size_t chunkSize = 2048;
    while (stereoRecorded < maxStereoBytes && !stopRequested)
    {
        size_t remaining = (size_t)(maxStereoBytes - stereoRecorded);
        size_t toRead = (remaining < chunkSize) ? remaining : chunkSize;
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_NUM_1, stereoBuf + stereoRecorded, toRead, &bytesRead, pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytesRead == 0)
            break;
        stereoRecorded += bytesRead;
    }

    // Re-acquire for teardown: i2s_driver_uninstall() frees the same
    // internal-heap DMA buffers that install() allocated (asynchronously -
    // see the delay(500) below), so this window needs the same protection
    // against a concurrent TLS handshake as install did. Not held across
    // the recording loop above.
    {
        NetLockGuard netLock(10000);
        if (!netLock.acquired())
            Serial.println("mic_capture: could not acquire network lock for teardown in time - proceeding anyway");

        i2s_stop(I2S_NUM_1);
        i2s_driver_uninstall(I2S_NUM_1);
        digitalWrite(AUDIO_ENABLE_PIN, HIGH); // power codec back down between asks
        micDriverInstalled = false;

        // Same real bug already found and fixed in speaker.cpp's teardown (see
        // its identical comment) - i2s_driver_uninstall() doesn't synchronously
        // free its DMA buffers, which are allocated from the same scarce
        // internal heap mbedTLS needs one large contiguous block from for a new
        // TLS handshake. This function's caller (askWork()) immediately starts
        // transcribeAudio()'s HTTPS call right after this returns - real serial
        // evidence showed that handshake failing outright ("SSL - Memory
        // allocation failed") right after an 8-second recording (this driver's
        // largest/longest-held DMA buffers), then cascading into repeated
        // failures across today/bus's own retries for several more seconds.
        // speaker.cpp's teardown got this exact fix already; this one was
        // missed since the symptom only shows up on the very next HTTPS call
        // (transcription), not inside this function itself.
        delay(500);
    }

    Serial.printf("mic_capture: captured %lu stereo bytes\n", (unsigned long)stereoRecorded);

    // Pick whichever of the two interleaved slots actually carries signal,
    // based on real per-slot amplitude stats rather than assuming - see the
    // channel_format comment above.
    int16_t *stereoSamples = (int16_t *)stereoBuf;
    size_t frameCount = stereoRecorded / 4; // 2 slots * 2 bytes each
    int64_t sumAbsA = 0, sumAbsB = 0;
    int16_t peakA = 0, peakB = 0;
    for (size_t i = 0; i < frameCount; i++)
    {
        int16_t a = stereoSamples[i * 2];
        int16_t b = stereoSamples[i * 2 + 1];
        int16_t aa = a < 0 ? -a : a;
        int16_t ab = b < 0 ? -b : b;
        if (aa > peakA)
            peakA = aa;
        if (ab > peakB)
            peakB = ab;
        sumAbsA += aa;
        sumAbsB += ab;
    }
    int32_t avgAbsA = frameCount > 0 ? (int32_t)(sumAbsA / (int64_t)frameCount) : 0;
    int32_t avgAbsB = frameCount > 0 ? (int32_t)(sumAbsB / (int64_t)frameCount) : 0;
    bool useA = avgAbsA >= avgAbsB;
    Serial.printf("mic_capture: slotA peak=%d avgAbs=%ld, slotB peak=%d avgAbs=%ld - using slot%s\n",
                  peakA, (long)avgAbsA, peakB, (long)avgAbsB, useA ? "A" : "B");

    uint8_t *buf = (uint8_t *)ps_malloc(WAV_HEADER_SIZE + maxMonoBytes);
    if (!buf)
    {
        Serial.println("mic_capture: mono ps_malloc failed");
        free(stereoBuf);
        return false;
    }
    int16_t *monoSamples = (int16_t *)(buf + WAV_HEADER_SIZE);
    for (size_t i = 0; i < frameCount; i++)
        monoSamples[i] = useA ? stereoSamples[i * 2] : stereoSamples[i * 2 + 1];
    free(stereoBuf);

    uint32_t recorded = (uint32_t)frameCount * 2;
    writeWavHeader(buf, recorded, SAMPLE_RATE, BITS_PER_SAMPLE, CHANNELS);
    out->wavData = buf;
    out->wavSize = WAV_HEADER_SIZE + recorded;
    return recorded > 0;
}
