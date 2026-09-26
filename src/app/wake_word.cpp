// "Jarvis" wake word (Espressif ESP-SR WakeNet9, lib/esp_sr). The model
// lives in the "model" flash partition (partitions_jarvis.csv), written
// once over USB - see readme.txt.
//
// A task on core 0 keeps the mic open (mic_capture.cpp's stream API) and
// feeds 32ms chunks to WakeNet. It lets go of the codec whenever anything
// else needs it: an Ask question (askIsBusy()), speaker playback
// (speakerIsBusy()), or an explicit wakeWordRelease() from those paths
// right before they install their own I2S driver.
#include "wake_word.h"
#include "mic_capture.h"
#include "ask.h"
#include "speaker.h"
#include "ota.h"
#include "network_worker.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

extern "C"
{
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
}

static const esp_wn_iface_t *wakenet = nullptr;
static model_iface_data_t *wakenetData = nullptr;
static int chunkSamples = 0;

static bool enabled = true;
static volatile bool streamOpen = false;
static volatile bool detected = false;
static volatile unsigned long holdUntilMs = 0; // no listening before this (after a release)
static volatile bool handoffRequested = false;  // stop reading but leave the stream open (recording takes it)

static bool shouldListen()
{
    return enabled && wakenetData && !askIsBusy() && !speakerIsBusy() && !otaInProgress() &&
           (long)(millis() - holdUntilMs) >= 0;
}

static TaskHandle_t wakeTaskHandle = nullptr;

// Something else needs the audio path (Ask question or speaker): the task
// ends itself so its 8KB internal-RAM stack is free during the HTTPS call
// and playback - both failed for lack of internal RAM with it alive (real
// log: "esp-aes: Failed to allocate memory", "speaker: i2s_driver_install
// failed"). wakeWordTick() starts it again afterwards.
static bool audioBusyElsewhere()
{
    return askIsBusy() || speakerIsBusy() || networkWorkerRunningJob() != nullptr;
}

static void wakeTask(void *)
{
    int16_t *chunk = (int16_t *)ps_malloc(chunkSamples * sizeof(int16_t));
    for (;;)
    {
        if (audioBusyElsewhere() && !handoffRequested)
        {
            if (streamOpen)
                micStreamClose(); // speaker playback - not a recording handover
            streamOpen = false;
            free(chunk);
            wakeTaskHandle = nullptr;
            vTaskDelete(NULL);
        }
        if (handoffRequested)
        {
            streamOpen = false; // driver stays installed - micCaptureRecord() takes it over
            handoffRequested = false;
            continue;
        }
        if (!shouldListen())
        {
            if (streamOpen)
            {
                micStreamClose();
                streamOpen = false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (!streamOpen)
        {
            if (!micStreamOpen())
            {
                vTaskDelay(pdMS_TO_TICKS(1000)); // net lock busy - try again shortly
                continue;
            }
            streamOpen = true;
            // (not wakenet->clean() - it crashes in this ESP-SR build, LoadProhibited in dl_convq_queue_bzero)
        }
        int got = micStreamRead(chunk, chunkSamples);
        if (got != chunkSamples)
            continue;
        if (wakenet->detect(wakenetData, chunk) == WAKENET_DETECTED)
        {
            Serial.println("wake: Jarvis detected");
            streamOpen = false; // left open for the recording that follows
            holdUntilMs = millis() + 1500;
            detected = true; // askTick() starts the recording
        }
    }
}

bool wakeWordInit()
{
    Preferences prefs;
    prefs.begin("wake", true);
    enabled = prefs.getBool("on", true);
    prefs.end();

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models)
    {
        Serial.println("wake: no model partition / models");
        return false;
    }
    char *name = esp_srmodel_filter(models, ESP_WN_PREFIX, "jarvis");
    if (!name)
    {
        Serial.println("wake: jarvis model not found");
        return false;
    }
    wakenet = esp_wn_handle_from_name(name);
    // WakeNet makes ~40KB of small allocations, which normally land in
    // internal RAM (allocations under 4KB do) - internal RAM is the scarce
    // one on this board, so route them to PSRAM just for the create.
    size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heap_caps_malloc_extmem_enable(16);
    wakenetData = wakenet->create(name, DET_MODE_90);
    heap_caps_malloc_extmem_enable(4096); // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
    Serial.printf("wake: model used %d bytes of internal RAM\n",(int)(freeBefore - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    if (!wakenetData)
    {
        Serial.println("wake: create failed");
        return false;
    }
    // More sensitive than the model default (~0.63): non-native accents
    // were missed while synthetic voices triggered every time.
    Serial.printf("wake: default threshold %.3f\n",wakenet->get_det_threshold(wakenetData, 1));
    wakenet->set_det_threshold(wakenetData, 0.52f, 1);
    chunkSamples = wakenet->get_samp_chunksize(wakenetData);
    Serial.printf("wake: %s ready, %d samples/chunk, listening %s\n", name, chunkSamples, enabled ? "on" : "off");
    holdUntilMs = millis() + 15000; // let boot-time fetches finish first
    return true; // wakeWordTick() starts the listener task
}

void wakeWordTick()
{
    if (!wakeTaskHandle && wakenetData && enabled && !audioBusyElsewhere())
        xTaskCreatePinnedToCore(wakeTask, "wakeWord", 8192, NULL, 1, &wakeTaskHandle, 0);
}

bool wakeWordAvailable()
{
    return wakenetData != nullptr;
}

bool wakeWordEnabled()
{
    return enabled;
}

void wakeWordSetEnabled(bool on)
{
    enabled = on;
    Preferences prefs;
    prefs.begin("wake", false);
    prefs.putBool("on", enabled);
    prefs.end();
}

void wakeWordRelease(bool handOver)
{
    holdUntilMs = millis() + 1500;
    if (handOver)
        handoffRequested = true;
    for (int i = 0; i < 50 && streamOpen; i++) // the task lets go within one 32ms chunk
        delay(20);
    handoffRequested = false;
    if (!handOver)
        micStreamClose(); // speaker needs a different I2S setup (no-op if already closed)
}

void wakeWordPauseForNetwork()
{
    // jobRunning is already set, so the task ends itself (closing the stream)
    // within one chunk - wait for it so the job starts with the RAM back.
    for (int i = 0; i < 50 && wakeTaskHandle; i++)
        delay(20);
    delay(100); // task stack + DMA are freed asynchronously
}

bool wakeWordTakeDetection()
{
    if (!detected)
        return false;
    detected = false;
    return true;
}
