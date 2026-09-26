#include "power.h"
#include <Arduino.h>
#include <esp_sleep.h>
#include <time.h>
#include "backlight.h"
#include "today.h"
#include "status_led.h"

// Duplicated from touch.h rather than shared, since touch.h has no include
// guard (it defines the global FT6336 `ts` object directly, so it's only
// ever included once, from main.cpp) - this just needs to stay in sync with
// TOUCH_FT6336_INT (touch.h).
#define TOUCH_INT_PIN 17

#define AWAKE_START_HOUR 8  // 08:00
#define AWAKE_END_HOUR 20   // 20:00
#define INACTIVITY_TIMEOUT_MS (15UL * 60UL * 1000UL)
// While blanked/light-sleeping, wake this often even without a touch, so a
// still-idle screen still notices crossing the 20:00 boundary and hands off
// to deep sleep instead of light-sleeping straight through it.
#define LIGHT_SLEEP_RECHECK_US (60ULL * 1000000ULL)

// The ONLY thing that decides whether the board sleeps is this inactivity
// timer - not the time of day by itself. Any touch, at any time on any day,
// resets it and keeps the board fully awake for one full window. Only once
// it elapses does the schedule get consulted, purely to pick *which* sleep
// mode to drop into. Booting counts as activity too (see powerInit()), so a
// fresh boot/reset always gets one full window awake before anything can
// send it back to sleep - this is also what makes it possible to reflash
// the board after a reset even if it was previously mid-schedule asleep.
static unsigned long lastTouchMs = 0;

void powerInit()
{
    lastTouchMs = millis();
}

void powerNoteTouch()
{
    lastTouchMs = millis();
}

// Sat/Sun (all day), or Mon-Fri outside 08:00-20:00 - the window during
// which going idle means DEEP sleep rather than light sleep.
static bool inDeepSleepWindow(const struct tm &t)
{
    if (t.tm_wday == 0 || t.tm_wday == 6) // Sunday, Saturday
        return true;
    return t.tm_hour < AWAKE_START_HOUR || t.tm_hour >= AWAKE_END_HOUR;
}

// Next weekday (Mon-Fri) 08:00 strictly after `now` - covers every case
// (a weekday night, or the whole Sat/Sun span) without special-casing which
// day it currently is.
static time_t nextWakeTime(time_t now, struct tm t)
{
    t.tm_hour = AWAKE_START_HOUR;
    t.tm_min = 0;
    t.tm_sec = 0;
    time_t candidate = mktime(&t);
    if (candidate <= now)
        candidate += 24 * 3600;

    struct tm ct;
    localtime_r(&candidate, &ct);
    while (ct.tm_wday == 0 || ct.tm_wday == 6)
    {
        candidate += 24 * 3600;
        localtime_r(&candidate, &ct);
    }
    return candidate;
}

// Never returns - esp_deep_sleep_start() reboots the board. Touch (ext0)
// and the scheduled timer are both armed, so a tap during the night/weekend
// reboots it just as well as the alarm does - either way powerInit() on the
// next boot starts a fresh awake window.
//
// The timer alarm is normally the next 08:00 weekday, but if a calendar
// event's 15-minute reminder window falls sooner than that (an evening or
// weekend meeting), the alarm is moved up to wake for it instead - deep
// sleep is a full reboot with nothing running in between, so this is the
// only way an overnight/weekend event can ever get its popup at all. Once
// awake, todayTick()'s own reminder check (same code path as the normal
// daytime case) shows the popup; the board then goes back to sleep on the
// usual inactivity timeout, at which point this function runs again and
// re-picks the next-nearest wake target (the following event, or 08:00).
static void enterDeepSleep(time_t now, struct tm t)
{
    time_t wake = nextWakeTime(now, t);
    const char *reason = "next 08:00 weekday";

    time_t nextEventEpoch = todayGetNextEventStartEpoch();
    if (nextEventEpoch != 0)
    {
        time_t eventReminderTime = nextEventEpoch - 15 * 60;
        if (eventReminderTime > now && eventReminderTime < wake)
        {
            wake = eventReminderTime;
            reason = "upcoming calendar event";
        }
    }

    uint64_t sleepSeconds = (uint64_t)(wake - now);
    Serial.printf("power: entering deep sleep for %llu s (wake reason: %s)\n",
                  (unsigned long long)sleepSeconds, reason);
    Serial.flush();
    backlightSetOn(false);
    statusLedDeepSleep();
    esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_INT_PIN, 0);
    esp_deep_sleep_start();
}

// Blanks the screen and light-sleeps, waking on touch (returns to the
// caller so normal operation resumes) or on the periodic recheck timer
// (loops back to sleep, or falls through to deep sleep if the 20:00
// boundary was crossed while idle).
static void lightSleepUntilTouchOrBoundary()
{
    backlightSetOn(false);
    statusLedLightSleep();
    for (;;)
    {
        esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_INT_PIN, 0); // FT6336 INT is active-low
        esp_sleep_enable_timer_wakeup(LIGHT_SLEEP_RECHECK_US);
        esp_light_sleep_start();

        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
        {
            backlightSetOn(true);
            lastTouchMs = millis();
            return;
        }

        // Real wake, not just a silent recheck: a calendar event has
        // crossed into its 15-minute reminder window while the screen was
        // blanked. Without this, todayTick() (which owns the actual popup)
        // would never run to notice it - light sleep only resumes loop()
        // for the instant between esp_light_sleep_start() calls, and the
        // popup logic lives in the normal awake code path. Turning the
        // backlight on and returning here lets the very next loop()
        // iteration reach todayTick() and show the popup like normal.
        time_t nextEventEpoch = todayGetNextEventStartEpoch();
        if (nextEventEpoch != 0 && nextEventEpoch - time(nullptr) <= 15 * 60)
        {
            backlightSetOn(true);
            lastTouchMs = millis();
            return;
        }

        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0) && inDeepSleepWindow(timeinfo))
            enterDeepSleep(time(nullptr), timeinfo); // does not return
        // else: still within the awake window, still idle - loop and
        // light-sleep again.
    }
}

// Shared by the automatic inactivity timeout and the manual "sleep now"
// button - same schedule check either way, so a manually-triggered sleep
// still wakes on touch exactly like an automatic one would.
static void sleepNow()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0))
        return; // can't safely pick a sleep mode without knowing the time

    if (inDeepSleepWindow(timeinfo))
        enterDeepSleep(time(nullptr), timeinfo); // does not return
    else
        lightSleepUntilTouchOrBoundary();
}

void powerTick()
{
    if (millis() - lastTouchMs <= INACTIVITY_TIMEOUT_MS)
        return; // recent activity (or a recent boot) - stay fully awake, whatever time it is
    sleepNow();
}

void powerSleepNow()
{
    sleepNow();
}
