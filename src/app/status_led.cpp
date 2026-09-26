#include "status_led.h"
#include "ask.h"
#include "network_worker.h"
#include "ota.h"
#include "wifi_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

#define STATUS_LED_PIN 42 // single WS2812, confirmed from the manufacturer demo (see src/led_test)
#define MAX_LEVEL 40      // of 255 - it's a status light, not a torch

struct Rgb
{
    uint8_t r, g, b;
};

static const Rgb OFF = {0, 0, 0};
static const Rgb CYAN = {0, 255, 255};
static const Rgb BLUE = {0, 40, 255};
static const Rgb ORANGE = {255, 80, 0};
static const Rgb PINK = {255, 20, 120};
static const Rgb PURPLE = {150, 0, 255};
static const Rgb GREEN = {0, 255, 0};
static const Rgb YELLOW = {255, 200, 0};
static const Rgb RED = {255, 0, 0};

enum Mode
{
    STEADY,
    BLINK_SLOW,  // 1 Hz
    BLINK,       // 2 Hz
    BLINK_FAST,  // 4 Hz
    BREATHE,     // ~2.5 s cycle
};

static Rgb lastSent = {1, 1, 1}; // forces the first write

static void send(Rgb c)
{
    if (c.r == lastSent.r && c.g == lastSent.g && c.b == lastSent.b)
        return; // each write is an RMT transaction - only on change
    lastSent = c;
    neopixelWrite(STATUS_LED_PIN, c.r, c.g, c.b);
}

static Rgb scale(Rgb c, uint16_t level) // level 0..255 of MAX_LEVEL
{
    uint32_t k = (uint32_t)level * MAX_LEVEL;
    return {(uint8_t)(c.r * k / (255 * 255)), (uint8_t)(c.g * k / (255 * 255)), (uint8_t)(c.b * k / (255 * 255))};
}

static void show(Rgb c, Mode mode)
{
    unsigned long t = millis();
    uint16_t level = 255;
    switch (mode)
    {
    case STEADY:
        break;
    case BLINK_SLOW:
        level = (t % 1000) < 500 ? 255 : 0;
        break;
    case BLINK:
        level = (t % 500) < 250 ? 255 : 0;
        break;
    case BLINK_FAST:
        level = (t % 250) < 125 ? 255 : 0;
        break;
    case BREATHE:
    {
        uint32_t p = t % 2500;
        uint32_t tri = p < 1250 ? p : 2500 - p; // 0..1250..0
        level = 20 + tri * 235 / 1250;
        level = level * level / 255; // gamma-ish, so it lingers dim
        break;
    }
    }
    send(scale(c, level));
}

static bool jobIs(const char *job, const char *prefix)
{
    return job && strncmp(job, prefix, strlen(prefix)) == 0;
}

void statusLedInit()
{
    send(OFF);
}

void statusLedTick()
{
    const char *job = networkWorkerRunningJob();

    if (otaInProgress())
        show(CYAN, BLINK_FAST);
    else if (wifiProvisioningInProgress())
        show(BLUE, BREATHE);
    else if (WiFi.status() != WL_CONNECTED)
        show(ORANGE, BLINK_SLOW);
    else if (askIsBusy() || jobIs(job, "ASK"))
        show(PINK, BLINK);
    else if (jobIs(job, "CALENDAR") || jobIs(job, "USER_REFRESH(ca"))
        show(PURPLE, BLINK);
    else if (jobIs(job, "BUS") || jobIs(job, "USER_REFRESH(bu"))
        show(GREEN, BLINK);
    else if (jobIs(job, "WEATHER"))
        show(YELLOW, BLINK);
    else if (jobIs(job, "OTA"))
        show(CYAN, BLINK);
    else
        send(OFF);
}

void statusLedLightSleep()
{
    send(scale(RED, 255)); // steady red
}

void statusLedDeepSleep()
{
    for (int i = 0; i < 3; i++)
    {
        send(scale(RED, 255));
        delay(150);
        send(OFF);
        delay(150);
    }
}
