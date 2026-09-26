// Today tab: agenda pulled from the user's Outlook calendar via its
// "Publish a calendar" ICS feed (plain HTTPS GET, no OAuth - see
// CLAUDE.md for why this was chosen over the Microsoft Graph API).
// Fetches the whole feed on tab-show / header tap / the auto-refresh
// timer, keeps only events within a +-window of "today" in a small
// in-RAM cache, and left/right day navigation just re-filters that
// cache locally (no network round-trip per arrow tap).
//
// The fetch+parse (a 647KB body, line-by-line RFC5545 unfolding) used to
// run inline in todayTick(), which runs inline in loop() - the same
// thread driving LVGL's rendering and touch polling. That blocked the
// *entire UI*, every tab, with zero indication anything was happening
// ("it hangs"). Fixed by moving the fetch onto its own FreeRTOS task
// (fetchAgendaTask(), pinned to the core WiFi already runs on, core 0,
// leaving loop()'s core 1 free), plus a global spinner in the status bar
// (status_bar.h/main.cpp) so there's now a visible "yes, something is
// loading" signal. Safety invariant: fetchAgendaTask() is the ONLY thing
// that writes events[]/eventCount/todayDayNumberAtFetch/lastError, and
// only while fetchInProgress is true; every UI handler that would read or
// re-render them (arrows, Today button, header tap) checks
// `if (fetchInProgress) return;` first, so the main thread never touches
// those arrays while the background task might still be writing them -
// no mutex/critical-section needed, just "don't look while it's being
// written", enforced at the UI layer instead of the data layer. Nothing
// in fetchAgendaTask() ever calls an lv_* function - LVGL itself is not
// thread-safe, so all rendering stays on the main thread, triggered by
// the fetchJustCompleted flag.
#include "today.h"
#include "status_bar.h"
#include "net_lock.h"
#include "speaker.h"
#include "ask.h"
#include "network_worker.h"
#include "icons/today_icon_reminder.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <esp_heap_caps.h>
#include "secrets_select.h"
#include "app_config.h"
#include "diag.h"

#define WINDOW_BACK_DAYS 7
#define WINDOW_FWD_DAYS 14
#define MAX_EVENTS 96
#define DEFAULT_FETCH_INTERVAL_MS 600000UL // 10 min - a calendar doesn't need bus-tab-style 30s polling
#define MIN_FETCH_INTERVAL_MIN 1
#define MAX_FETCH_INTERVAL_MIN 180

// Runtime-adjustable via the Settings tab (todaySetRefreshIntervalMin()) -
// was a compile-time constant before that existed.
static unsigned long fetchIntervalMs = DEFAULT_FETCH_INTERVAL_MS;

void todaySetRefreshIntervalMin(int minutes)
{
    if (minutes < MIN_FETCH_INTERVAL_MIN)
        minutes = MIN_FETCH_INTERVAL_MIN;
    if (minutes > MAX_FETCH_INTERVAL_MIN)
        minutes = MAX_FETCH_INTERVAL_MIN;
    fetchIntervalMs = (unsigned long)minutes * 60000UL;
}

int todayGetRefreshIntervalMin()
{
    return (int)(fetchIntervalMs / 60000UL);
}

struct AgendaEvent
{
    char title[64];
    char location[48];
    time_t startEpoch;
    time_t endEpoch;
    long dayNumber; // local (SGT) calendar day, days since 1970-01-01 - the
                     // unit day-navigation and filtering both work in, so an
                     // event's displayed day never depends on exactly when
                     // "now" is re-evaluated mid-render.
    bool allDay;
};

// This tab's own tint, same pattern as Settings' SETTINGS_TINT - applied
// to the same class of elements (accent buttons/labels/switch), per
// explicit request.
#define TODAY_TINT 0x8F13FD

static lv_obj_t *dateLabel;
static lv_obj_t *eventList;
static lv_obj_t *autoRefreshSwitch;

// 12KB of plain data - allocated in PSRAM by todayInit(), since internal RAM
// is the scarce one on this board.
static AgendaEvent *events = nullptr;
static int eventCount = 0;
static long todayDayNumberAtFetch = 0;
static int viewedDayOffset = 0;
static unsigned long lastFetch = 0;
static bool autoRefreshEnabled = false;
static bool forceFetch = false;
// Whether the Today tab is the one currently on screen - periodic
// auto-refresh is pointless (and just burns Wi-Fi/battery) while the user
// is looking at a different tab, so it's gated on this.
static bool tabIsActive = false;
static char lastError[64] = "";
// Set once the very first fetch actually succeeds - see bus.cpp's
// identical everSucceeded/dueForInitialRetry pattern for why: without
// this, a single failed boot-time fetch would never retry at all while
// the user is on a different tab (e.g. Ask), since normal periodic
// refresh is gated on the Today tab being the visible one.
static bool everSucceeded = false;

// volatile: set/read from both the main loop and fetchAgendaTask()'s
// separate FreeRTOS task/core. See the file-header comment for the
// invariant that makes this safe without a mutex.
static volatile bool fetchInProgress = false;
static volatile bool fetchJustCompleted = false;
static unsigned long fetchStartedAtMs = 0;

static const char *weekdayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *monthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// ---- Calendar-day arithmetic (Howard Hinnant's days_from_civil /
// civil_from_days) - same TZ-independent technique as bus.cpp's
// daysFromCivil(), duplicated here rather than shared: this file's ICS
// date format (YYYYMMDD[THHMMSS[Z]], no dashes/colons) and bus.cpp's
// ISO8601 format (YYYY-MM-DDTHH:MM:SS+08:00) are different enough that a
// shared parser wouldn't simplify much, and it's ~15 lines either way. ----

static long daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civilFromDays(long z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yy + (*m <= 2));
}

// Fixed Singapore offset (+08:00, no DST - same fact already relied on by
// bus.cpp/configTime()) applied by hand rather than via the C library's TZ
// machinery, consistent with this project's established approach to SGT
// conversions.
static long localDayNumber(time_t utcEpoch)
{
    time_t shifted = utcEpoch + 8 * 3600;
    struct tm tmOut;
    gmtime_r(&shifted, &tmOut);
    return daysFromCivil(tmOut.tm_year + 1900, tmOut.tm_mon + 1, tmOut.tm_mday);
}

// ---- ICS text unescaping (RFC 5545 3.3.11: \, \; \\ \n) ----

static String icsUnescape(const String &in)
{
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++)
    {
        char c = in[i];
        if (c == '\\' && i + 1 < in.length())
        {
            char n = in[i + 1];
            if (n == 'n' || n == 'N')
            {
                out += ' ';
                i++;
            }
            else if (n == ',' || n == ';' || n == '\\')
            {
                out += n;
                i++;
            }
            else
            {
                out += c;
            }
        }
        else
        {
            out += c;
        }
    }
    return out;
}

// Parses a DTSTART/DTEND line's value (after the last ':') in whichever of
// the three forms the feed uses (confirmed against the real feed, not
// assumed): "...;VALUE=DATE:20250917" (all-day), "...;TZID=Singapore
// Standard Time:20250925T143000" (floating local, since this org's
// calendar's VTIMEZONE is a fixed +08:00 with no DST), or a bare UTC
// "...Z" timestamp.
static void parseIcsDateTime(const String &line, time_t &epochOut, long &dayNumberOut, bool &allDayOut)
{
    int colon = line.lastIndexOf(':');
    if (colon < 0)
    {
        epochOut = 0;
        dayNumberOut = 0;
        allDayOut = false;
        return;
    }
    String params = line.substring(0, colon);
    String value = line.substring(colon + 1);
    value.trim();

    if (params.indexOf("VALUE=DATE") >= 0)
    {
        int y, mo, d;
        sscanf(value.c_str(), "%4d%2d%2d", &y, &mo, &d);
        dayNumberOut = daysFromCivil(y, mo, d);
        epochOut = dayNumberOut * 86400L;
        allDayOut = true;
        return;
    }

    int y, mo, d, h, mi, s;
    int matched = sscanf(value.c_str(), "%4d%2d%2dT%2d%2d%2d", &y, &mo, &d, &h, &mi, &s);
    allDayOut = false;
    if (matched < 6)
    {
        epochOut = 0;
        dayNumberOut = 0;
        return;
    }

    long days = daysFromCivil(y, mo, d);
    time_t rawEpoch = days * 86400L + h * 3600L + mi * 60L + s;
    if (value.endsWith("Z"))
    {
        epochOut = rawEpoch;
        dayNumberOut = localDayNumber(rawEpoch);
    }
    else
    {
        epochOut = rawEpoch - 8 * 3600L;
        dayNumberOut = days; // already the local calendar date, no round-trip needed
    }
}

// ---- Streamed ICS parsing --------------------------------------------

static bool parseInEvent = false;
static AgendaEvent parseCurrent;
static bool parseCancelled = false;

static void maybeStoreEvent()
{
    if (parseCurrent.title[0] == '\0')
        return;
    long delta = parseCurrent.dayNumber - todayDayNumberAtFetch;
    if (delta < -WINDOW_BACK_DAYS || delta > WINDOW_FWD_DAYS)
        return;
    if (eventCount >= MAX_EVENTS)
        return;
    events[eventCount++] = parseCurrent;
}

static void processIcsLine(const String &line)
{
    if (line == "BEGIN:VEVENT")
    {
        parseInEvent = true;
        memset(&parseCurrent, 0, sizeof(parseCurrent));
        parseCancelled = false;
        return;
    }
    if (line == "END:VEVENT")
    {
        parseInEvent = false;
        if (!parseCancelled)
            maybeStoreEvent();
        return;
    }
    if (!parseInEvent)
        return;

    if (line.startsWith("SUMMARY:"))
        snprintf(parseCurrent.title, sizeof(parseCurrent.title), "%s", icsUnescape(line.substring(8)).c_str());
    else if (line.startsWith("LOCATION:"))
        snprintf(parseCurrent.location, sizeof(parseCurrent.location), "%s", icsUnescape(line.substring(9)).c_str());
    else if (line.startsWith("STATUS:CANCELLED"))
        parseCancelled = true;
    else if (line.startsWith("DTSTART"))
        parseIcsDateTime(line, parseCurrent.startEpoch, parseCurrent.dayNumber, parseCurrent.allDay);
    else if (line.startsWith("DTEND"))
    {
        long dummyDay;
        bool dummyAllDay;
        parseIcsDateTime(line, parseCurrent.endEpoch, dummyDay, dummyAllDay);
    }
}

static int compareEvents(const void *a, const void *b)
{
    const AgendaEvent *ea = (const AgendaEvent *)a;
    const AgendaEvent *eb = (const AgendaEvent *)b;
    if (ea->dayNumber != eb->dayNumber)
        return (ea->dayNumber < eb->dayNumber) ? -1 : 1;
    if (ea->startEpoch != eb->startEpoch)
        return (ea->startEpoch < eb->startEpoch) ? -1 : 1;
    return 0;
}

static void renderDay();

// Runs on the shared Network Worker task now (see fetchAgendaJob() and
// network_worker.h) - must never call an lv_* function (LVGL isn't
// thread-safe) and must not touch events[]/eventCount/
// todayDayNumberAtFetch/lastError except here, per the invariant
// documented at the top of this file. Otherwise unchanged from before.
static void fetchAgendaWork()
{
    // See net_lock.h: two concurrent TLS handshakes (e.g. this and
    // bus.cpp's fetch, both firing around boot) were found to exhaust the
    // ESP32's internal heap and fail both with "SSL - Memory allocation
    // failed" - held for the whole function, not just the handshake, since
    // only one HTTPS call is useful at a time here anyway.
    NetLockGuard netLock;
    if (!netLock.acquired())
    {
        snprintf(lastError, sizeof(lastError), "Busy, try again");
        Serial.println("today: could not acquire network lock in time");
        return;
    }

    // Internal RAM before the TLS handshake (see bus.cpp).
    Serial.printf("today: free internal heap before fetch: %u bytes, largest free block: %u bytes\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    Serial.println("today: fetch start");
    WiFiClientSecure client;
    client.setInsecure();
    // Real bug found here too: WiFiClientSecure's default handshake
    // timeout is 120 SECONDS (WiFiClientSecure.cpp:
    // `handshake_timeout = 120000`), and nothing in this file was
    // overriding it - only the streaming-read loop below had its own
    // (20s) timeout, which only applies *after* a successful
    // connect+handshake. A stalled handshake (flaky Wi-Fi, a slow
    // response, whatever) would silently block this task for up to two
    // full minutes before recovering on its own - which from the UI's
    // perspective looks exactly like "keeps loading forever", since
    // nobody's going to wait two minutes to find out it was never
    // actually stuck for good. Cut to a real, tight timeout.
    client.setHandshakeTimeout(8);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    if (!http.begin(client, cfgIcsUrl()))
    {
        snprintf(lastError, sizeof(lastError), "begin() failed");
        Serial.println("today: http.begin() failed");
        return;
    }

    int code = http.GET();
    Serial.printf("today: HTTP GET returned %d\n", code);
    if (code != 200)
    {
        snprintf(lastError, sizeof(lastError), "HTTP %d", code);
        http.end();
        return;
    }
    lastError[0] = '\0';

    int contentLength = http.getSize();
    if (contentLength > 0)
        addNetworkBytes((uint32_t)contentLength);

    todayDayNumberAtFetch = localDayNumber(time(nullptr));
    eventCount = 0;

    WiFiClient *stream = http.getStreamPtr();
    parseInEvent = false;

    // Streamed in 1KB blocks through two small fixed line buffers instead of
    // readStringUntil(): the feed is ~630KB (a year of history plus a month
    // ahead) and ~70% of it is DESCRIPTION text the display never uses, and
    // the old per-line String grew one character at a time for all of it.
    // Only logical lines starting with one of kKeepPrefixes reach
    // processIcsLine(); everything else is dropped as it arrives. Lines
    // longer than the buffers are truncated (only ever DESCRIPTION-sized
    // text - SUMMARY/LOCATION/DT* fit).
    static const char *const kKeepPrefixes[] = {"BEGIN:VEVENT", "END:VEVENT", "DTSTART", "DTEND", "SUMMARY", "LOCATION", "STATUS"};
    static char physLine[256];
    static char logicalLine[256];
    static uint8_t block[1024];
    size_t physLen = 0, logicalLen = 0;
    bool haveLogical = false, keepLogical = false;
    size_t totalBytes = 0;

    auto flushLogical = [&]() {
        if (haveLogical && keepLogical)
        {
            logicalLine[logicalLen] = '\0';
            processIcsLine(String(logicalLine));
        }
    };
    auto endPhysicalLine = [&]() {
        if (physLen > 0 && physLine[physLen - 1] == '\r')
            physLen--;
        if (physLen > 0 && (physLine[0] == ' ' || physLine[0] == '\t'))
        {
            // RFC 5545 folding: continuation of the previous logical line
            if (keepLogical)
            {
                size_t n = physLen - 1;
                if (logicalLen + n > sizeof(logicalLine) - 1)
                    n = sizeof(logicalLine) - 1 - logicalLen;
                memcpy(logicalLine + logicalLen, physLine + 1, n);
                logicalLen += n;
            }
        }
        else
        {
            flushLogical();
            memcpy(logicalLine, physLine, physLen);
            logicalLen = physLen;
            haveLogical = true;
            keepLogical = false;
            for (const char *prefix : kKeepPrefixes)
            {
                size_t pl = strlen(prefix);
                if (physLen >= pl && memcmp(physLine, prefix, pl) == 0)
                {
                    keepLogical = true;
                    break;
                }
            }
        }
        physLen = 0;
    };

    // 50s cap (was 20s): the upcoming events are at the END of the feed, so
    // cutting a slow download short dropped exactly the events that matter.
    // Still under todayTick()'s own 65s UI watchdog.
    unsigned long fetchStart = millis();
    unsigned long lastYieldMs = millis();
    while ((stream->connected() || stream->available()) && millis() - fetchStart < 50000)
    {
        // Done once the whole body is in: the server keeps the connection
        // open afterwards, so connected() alone kept this loop idling until
        // the time cap (real log: "read 630143 of 630143 bytes in 50000ms").
        if (contentLength > 0 && totalBytes >= (size_t)contentLength)
            break;
        int avail = stream->available();
        if (avail <= 0)
        {
            delay(1);
            continue;
        }
        // Yield regularly - with data arriving continuously this loop never
        // hits the delay() above, and starving core 0's idle task trips the
        // task watchdog (real log: "task_wdt: Aborting" from this loop).
        if (millis() - lastYieldMs >= 50)
        {
            vTaskDelay(1);
            lastYieldMs = millis();
        }
        int n = stream->read(block, avail < (int)sizeof(block) ? avail : (int)sizeof(block));
        if (n <= 0)
            continue;
        totalBytes += n;
        for (int i = 0; i < n; i++)
        {
            char c = (char)block[i];
            if (c == '\n')
                endPhysicalLine();
            else if (physLen < sizeof(physLine) - 1)
                physLine[physLen++] = c;
        }
    }
    if (physLen > 0)
        endPhysicalLine();
    flushLogical();
    Serial.printf("today: read %u of %d bytes in %lums\n", (unsigned)totalBytes, contentLength, millis() - fetchStart);

    http.end();

    qsort(events, eventCount, sizeof(AgendaEvent), compareEvents);
    Serial.printf("today: fetch ok, %d events cached\n", eventCount);
}

// Runs on the shared Network Worker task now (network_worker.h/.cpp), not
// its own xTaskCreatePinnedToCore() task - removes the internal-heap
// contention between bus.cpp's/today.cpp's/ask.cpp's own task stacks that
// was causing real, confirmed task-creation failures. fetchAgendaWork()
// itself is unchanged.
static void fetchAgendaJob()
{
    fetchAgendaWork();
    fetchJustCompleted = true;
    fetchInProgress = false;
}

// Main-thread only: shows the loading state immediately (synchronously,
// before the fetch has done anything) and submits the job to the Network
// Worker's queue.
static void startFetch(bool userTriggered)
{
    if (fetchInProgress)
        return;
    fetchInProgress = true;
    fetchStartedAtMs = millis();
    // The status-bar spinner (setLoadingVisible) is the sole loading
    // indicator now - this used to also clear eventList and show
    // "Refreshing agenda..." on every fetch, including the tab-show
    // trigger that fires on every single tab switch, so the screen went
    // blank/white for the duration of every visit rather than just
    // showing the spinner over the still-valid previous data.
    // Color set here, at the trigger, not on tab-switch - so the spinner's
    // color always identifies which subsystem's background fetch is
    // actually running, even if you're looking at a different tab.
    setLoadingSpinnerColor(TODAY_TINT);
    setLoadingVisible(true);
    // CALENDAR priority, or USER_REFRESH if this came from a deliberate
    // tap (tab-show/header tap/day-nav forcing a refresh) rather than the
    // periodic auto-refresh timer - matches this app's existing
    // forceFetch-vs-auto distinction, now expressed as queue priority.
    if (userTriggered)
        networkWorkerSubmit(NET_JOB_USER_REFRESH, "USER_REFRESH(calendar)", fetchAgendaJob, false);
    else
        networkWorkerSubmit(NET_JOB_CALENDAR, "CALENDAR", fetchAgendaJob, true);
}

// ---- Rendering -----------------------------------------------------------

static void formatEventTime(const AgendaEvent &ev, char *out, size_t outSize)
{
    if (ev.allDay)
    {
        snprintf(out, outSize, "All day");
        return;
    }
    time_t s = ev.startEpoch + 8 * 3600;
    time_t e = ev.endEpoch + 8 * 3600;
    struct tm startTm, endTm;
    gmtime_r(&s, &startTm);
    gmtime_r(&e, &endTm);
    char startBuf[12], endBuf[12];
    strftime(startBuf, sizeof(startBuf), "%I:%M %p", &startTm);
    strftime(endBuf, sizeof(endBuf), "%I:%M %p", &endTm);
    snprintf(out, outSize, "%s - %s", startBuf, endBuf);
}

static void addEventCard(lv_obj_t *parent, const AgendaEvent &ev, int index)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    // Alternating row tint derived from TODAY_TINT (lightened toward white)
    // rather than a hardcoded old-palette purple, so it actually follows
    // this tab's real tint if that ever changes again.
    lv_obj_set_style_bg_color(card, (index % 2 == 0) ? lv_color_hex(0xffffff) : lv_color_lighten(lv_color_hex(TODAY_TINT), 225), 0);
    lv_obj_set_style_radius(card, 4, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *timeLabel = lv_label_create(card);
    char timeBuf[32];
    formatEventTime(ev, timeBuf, sizeof(timeBuf));
    lv_label_set_text(timeLabel, timeBuf);
    lv_obj_set_style_text_font(timeLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(timeLabel, lv_color_hex(TODAY_TINT), 0);

    lv_obj_t *titleLabel = lv_label_create(card);
    lv_obj_set_width(titleLabel, LV_PCT(100));
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_WRAP);
    lv_label_set_text(titleLabel, ev.title);

    if (ev.location[0])
    {
        lv_obj_t *locLabel = lv_label_create(card);
        lv_obj_set_width(locLabel, LV_PCT(100));
        lv_label_set_long_mode(locLabel, LV_LABEL_LONG_DOT);
        char locBuf[56];
        snprintf(locBuf, sizeof(locBuf), LV_SYMBOL_GPS " %s", ev.location);
        lv_label_set_text(locLabel, locBuf);
        lv_obj_set_style_text_font(locLabel, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(locLabel, lv_color_hex(0x888888), 0);
    }
}

static void renderDay()
{
    long viewedDay = todayDayNumberAtFetch + viewedDayOffset;
    int y;
    unsigned m, d;
    civilFromDays(viewedDay, &y, &m, &d);
    // 1970-01-01 (day 0) was a Thursday (index 4 with Sunday=0).
    int weekday = (int)(((viewedDay % 7) + 7 + 4) % 7);

    char buf[48];
    const char *prefix = (viewedDayOffset == 0) ? "Today, " : (viewedDayOffset == 1) ? "Tomorrow, "
                                                : (viewedDayOffset == -1)             ? "Yesterday, "
                                                                                       : "";
    snprintf(buf, sizeof(buf), "%s%s %u %s", prefix, weekdayNames[weekday], d, monthNames[m - 1]);
    lv_label_set_text(dateLabel, buf);

    lv_obj_clean(eventList);

    if (lastError[0])
    {
        lv_obj_t *err = lv_label_create(eventList);
        lv_label_set_text(err, lastError);
        lv_obj_set_style_text_color(err, lv_color_hex(TODAY_TINT), 0);
        return;
    }

    int shown = 0;
    for (int i = 0; i < eventCount; i++)
    {
        if (events[i].dayNumber != viewedDay)
            continue;
        addEventCard(eventList, events[i], shown);
        shown++;
    }

    if (shown == 0)
    {
        lv_obj_t *empty = lv_label_create(eventList);
        lv_label_set_text(empty, "No meetings");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x999999), 0);
    }
}

// ---- UI event handlers -------------------------------------------------

// All four handlers below bail out while a fetch is running - see the
// file-header comment: events[]/eventCount are only safe to read on the
// main thread when fetchInProgress is false.

static void prevDayClicked(lv_event_t *e)
{
    if (fetchInProgress)
        return;
    viewedDayOffset--;
    renderDay();
}

static void nextDayClicked(lv_event_t *e)
{
    if (fetchInProgress)
        return;
    viewedDayOffset++;
    renderDay();
}

static void todayButtonClicked(lv_event_t *e)
{
    if (fetchInProgress)
        return;
    viewedDayOffset = 0;
    renderDay();
}

// Tapping the date/day title forces a real network refetch (not just a
// local re-render) - the explicit "refresh" affordance for this tab, same
// role the Bus tab's search/history taps serve there.
static void headerClicked(lv_event_t *e)
{
    if (fetchInProgress)
        return;
    forceFetch = true;
}

static void autoRefreshToggled(lv_event_t *e)
{
    autoRefreshEnabled = lv_obj_has_state(autoRefreshSwitch, LV_STATE_CHECKED);
}

// ---- UI construction -------------------------------------------------

void todayInit(lv_obj_t *tab)
{
    events = (AgendaEvent *)ps_calloc(MAX_EVENTS, sizeof(AgendaEvent));
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tab, 10, 0);
    // Pulls the header row up closer to the tab's top edge than the
    // uniform 10px pad_all above would otherwise leave it.
    lv_obj_set_style_pad_top(tab, 4, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 6, 0);

    // One row: [Today] on the left, [< date >] centered, [switch] on the
    // right - all three at a glance instead of split across two rows.
    lv_obj_t *topRow = lv_obj_create(tab);
    lv_obj_remove_style_all(topRow);
    lv_obj_set_size(topRow, LV_PCT(100), 32);
    lv_obj_set_flex_flow(topRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(topRow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *todayBtn = lv_btn_create(topRow);
    lv_obj_set_size(todayBtn, 56, 26);
    // Default theme's button radius is much rounder than the rest of this
    // app's buttons (e.g. the Bus tab's search box/history rows use 6) -
    // match that instead of the theme default.
    lv_obj_set_style_radius(todayBtn, 6, 0);
    lv_obj_set_style_bg_color(todayBtn, lv_color_hex(TODAY_TINT), 0);
    lv_obj_add_event_cb(todayBtn, todayButtonClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *todayLabel = lv_label_create(todayBtn);
    lv_label_set_text(todayLabel, "Today");
    lv_obj_set_style_text_color(todayLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(todayLabel);

    lv_obj_t *navGroup = lv_obj_create(topRow);
    lv_obj_remove_style_all(navGroup);
    lv_obj_set_size(navGroup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(navGroup, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(navGroup, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(navGroup, 4, 0);
    lv_obj_clear_flag(navGroup, LV_OBJ_FLAG_SCROLLABLE);

    // Deliberately dull/flat (no fill, muted gray) rather than solid
    // buttons - day-stepping is a secondary action and shouldn't compete
    // for attention with the date itself or the Today/auto-refresh
    // controls. Widened (56x40, was 40x32) per explicit request for an
    // easier tap target - the glyph itself stays the same small/dull icon,
    // only the button's own hit area grows.
    lv_obj_t *prevBtn = lv_btn_create(navGroup);
    lv_obj_remove_style_all(prevBtn);
    lv_obj_set_size(prevBtn, 56, 40);
    lv_obj_set_ext_click_area(prevBtn, 8);
    lv_obj_clear_flag(prevBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(prevBtn, prevDayClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prevLabel = lv_label_create(prevBtn);
    lv_label_set_text(prevLabel, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(prevLabel, lv_color_hex(0xb0aab3), 0);
    lv_obj_set_style_text_color(prevLabel, lv_color_hex(TODAY_TINT), LV_STATE_PRESSED);
    lv_obj_center(prevLabel);

    dateLabel = lv_label_create(navGroup);
    lv_label_set_text(dateLabel, "Loading...");
    lv_obj_set_style_text_font(dateLabel, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(dateLabel, lv_color_hex(TODAY_TINT), 0);
    lv_obj_add_flag(dateLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dateLabel, headerClicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *nextBtn = lv_btn_create(navGroup);
    lv_obj_remove_style_all(nextBtn);
    lv_obj_set_size(nextBtn, 56, 40);
    lv_obj_set_ext_click_area(nextBtn, 8);
    lv_obj_clear_flag(nextBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(nextBtn, nextDayClicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nextLabel = lv_label_create(nextBtn);
    lv_label_set_text(nextLabel, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(nextLabel, lv_color_hex(0xb0aab3), 0);
    lv_obj_set_style_text_color(nextLabel, lv_color_hex(TODAY_TINT), LV_STATE_PRESSED);
    lv_obj_center(nextLabel);

    // Off by default, same rationale/pattern as the Bus tab: a background
    // poll shouldn't be silently on without the user choosing it.
    autoRefreshSwitch = lv_switch_create(topRow);
    lv_obj_set_size(autoRefreshSwitch, 34, 18);
    lv_obj_set_style_bg_color(autoRefreshSwitch, lv_color_hex(TODAY_TINT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(autoRefreshSwitch, autoRefreshToggled, LV_EVENT_VALUE_CHANGED, NULL);

    eventList = lv_obj_create(tab);
    lv_obj_remove_style_all(eventList);
    lv_obj_set_size(eventList, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(eventList, 1);
    lv_obj_set_flex_flow(eventList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(eventList, 4, 0);
    // remove_style_all() above also wipes the scrollbar part's default
    // look (same gotcha documented in bus.cpp) - restyle it explicitly.
    lv_obj_set_scrollbar_mode(eventList, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(eventList, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(eventList, lv_color_hex(0x9e9e9e), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(eventList, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(eventList, 2, LV_PART_SCROLLBAR);

    // Load the calendar once at boot regardless of which tab is shown
    // first - matches bus.cpp's busInit(), which already does this for bus
    // data. Without this, the Ask tab's context-injection would have
    // nothing to say about "what's my next meeting" until the user
    // happened to visit the Today tab at least once. Auto-refresh (off by
    // default) and todayOnTabShown()'s own forceFetch are unaffected -
    // this only changes when the *first* fetch happens, not the ongoing
    // refresh policy.
    forceFetch = true;

    renderDay();
}

void todayOnTabShown()
{
    // The ICS fetch is slow - once this session already has data (or has
    // at least tried), re-showing the tab shouldn't force another slow
    // refetch. Auto-refresh and the title-click "refresh" affordance
    // (headerClicked) are untouched - both still always force a fetch.
    //
    // Real bug fixed here: guarding on lastFetch==0 alone caused a genuine
    // double fetch at boot every time. todayInit() already sets
    // forceFetch=true unconditionally at boot; lastFetch only gets set once
    // that fetch actually *completes* (network round-trip, ~1-2s), not when
    // it starts. Adding tabs via lv_tabview_add_tab() during setup() can
    // itself fire a transient LV_EVENT_VALUE_CHANGED where the active index
    // briefly reads as Today's index while the button matrix rebuilds -
    // confirmed via a real boot serial capture showing "today: fetch ok"
    // twice back-to-back with no user interaction. That transient event hit
    // this function while lastFetch was still 0 (boot fetch in flight, not
    // yet complete), re-arming forceFetch a second time right as the first
    // fetch cleared it - a second fetch fired immediately after the first.
    // Also checking fetchInProgress/forceFetch closes that whole window.
    if (lastFetch == 0 && !fetchInProgress && !forceFetch)
        forceFetch = true;
}

void todaySetTabActive(bool active)
{
    tabIsActive = active;
}

bool todayIsFetchInProgress()
{
    return fetchInProgress;
}

// Safe to call any time - reads events[]/eventCount, which are only ever
// written by fetchAgendaTask() while fetchInProgress is true (same
// invariant as the rest of this file); callers just get a stale-but-valid
// snapshot if a fetch happens to be running at the exact moment this runs.
void todayGetContextSummary(char *out, size_t outSize)
{
    if (lastFetch == 0)
    {
        snprintf(out, outSize, "Calendar: not loaded yet.");
        return;
    }

    time_t now = time(nullptr);
    char buf[280] = "";
    size_t used = 0;
    int shown = 0;

    for (int i = 0; i < eventCount && shown < 3; i++)
    {
        const AgendaEvent &ev = events[i];
        // Skip anything that's already fully over - "next meeting" should
        // never point backward in time.
        time_t endRef = ev.allDay ? (ev.startEpoch + 86400) : ev.endEpoch;
        if (endRef < now)
            continue;

        char timeBuf[32];
        formatEventTime(ev, timeBuf, sizeof(timeBuf));

        long delta = ev.dayNumber - todayDayNumberAtFetch;
        char dayBuf[16];
        if (delta == 0)
            snprintf(dayBuf, sizeof(dayBuf), "today");
        else if (delta == 1)
            snprintf(dayBuf, sizeof(dayBuf), "tomorrow");
        else
        {
            int y;
            unsigned m, d;
            civilFromDays(ev.dayNumber, &y, &m, &d);
            int weekday = (int)(((ev.dayNumber % 7) + 7 + 4) % 7);
            snprintf(dayBuf, sizeof(dayBuf), "%s %u %s", weekdayNames[weekday], d, monthNames[m - 1]);
        }

        char line[100];
        snprintf(line, sizeof(line), "%s %s: %s%s%s; ", dayBuf, timeBuf, ev.title,
                 ev.location[0] ? " @ " : "", ev.location);
        size_t lineLen = strlen(line);
        if (used + lineLen < sizeof(buf))
        {
            memcpy(buf + used, line, lineLen);
            used += lineLen;
            buf[used] = '\0';
        }
        shown++;
    }

    if (shown == 0)
        snprintf(out, outSize, "Calendar: no upcoming events in the next few days.");
    else
        snprintf(out, outSize, "Upcoming calendar events: %s", buf);
}

// ---- 15-minutes-to-event popup reminder -----------------------------------

// Real bug avoided here: without tracking which event has already been
// reminded about, this would pop up again on every single todayTick() call
// (many times a second) for the entire 15-minute window leading up to an
// event, not just once. startEpoch is a stable, unique-enough key for a
// single reminder per event instance (recurring events each get their own
// expanded startEpoch already, same as everywhere else in this file).
static time_t remindedEventStartEpoch = 0;

// The overlay itself and the event it's counting down to, so the 1s LVGL
// timer below can keep updating the MM:SS label after the popup is shown -
// cleared (set NULL/0) when the popup is closed so the timer callback
// knows to stop touching a deleted object.
static lv_obj_t *reminderOverlay = nullptr;
static lv_obj_t *reminderCountdownLabel = nullptr;
static time_t reminderTargetEpoch = 0;
static lv_timer_t *reminderCountdownTimer = nullptr;

static void closeReminderOverlay(lv_event_t *e)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_del(overlay);
    reminderOverlay = nullptr;
    reminderCountdownLabel = nullptr;
    if (reminderCountdownTimer)
    {
        lv_timer_del(reminderCountdownTimer);
        reminderCountdownTimer = nullptr;
    }
}

static void reminderCountdownTimerCb(lv_timer_t *timer)
{
    if (!reminderCountdownLabel)
        return;
    time_t remaining = reminderTargetEpoch - time(nullptr);
    if (remaining < 0)
        remaining = 0;
    int mm = (int)(remaining / 60);
    int ss = (int)(remaining % 60);
    lv_label_set_text_fmt(reminderCountdownLabel, "%02d:%02d", mm, ss);
}

// Full-screen popup, same dark-backdrop/tap-anywhere-to-close pattern as
// profile.cpp's QR overlay (showQrOverlay()) - placed on lv_layer_top() so
// it renders above the tab bar/status strip regardless of which tab is
// active, since the user could be anywhere in the app when a meeting is
// about to start. Layout (per explicit request): icon + live MM:SS
// countdown centered on top (biggest font), event time centered below it
// (slightly larger font), then title and location, all centered.
static void showEventReminderOverlay(const AgendaEvent &ev)
{
    char timeBuf[32];
    formatEventTime(ev, timeBuf, sizeof(timeBuf));

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, closeReminderOverlay, LV_EVENT_CLICKED, overlay);
    reminderOverlay = overlay;

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, 360, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE); // taps on the card fall through to the backdrop, same as the QR overlay's image
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *titleRow = lv_obj_create(card);
    lv_obj_remove_style_all(titleRow);
    lv_obj_set_size(titleRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(titleRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(titleRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(titleRow, 8, 0);

    lv_obj_t *icon = lv_img_create(titleRow);
    lv_img_set_src(icon, &today_icon_reminder);

    reminderTargetEpoch = ev.startEpoch;
    lv_obj_t *countdown = lv_label_create(titleRow);
    lv_obj_set_style_text_color(countdown, lv_color_hex(TODAY_TINT), 0);
    lv_obj_set_style_text_font(countdown, &lv_font_montserrat_24, 0);
    lv_label_set_text(countdown, "00:00");
    reminderCountdownLabel = countdown;
    reminderCountdownTimerCb(nullptr); // set the real value immediately, don't wait 1s for the first tick
    reminderCountdownTimer = lv_timer_create(reminderCountdownTimerCb, 1000, nullptr);

    lv_obj_t *timeLabel = lv_label_create(card);
    lv_label_set_text(timeLabel, timeBuf);
    lv_obj_set_style_text_font(timeLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(timeLabel, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_align(timeLabel, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *eventTitleLabel = lv_label_create(card);
    lv_label_set_text(eventTitleLabel, ev.title);
    lv_obj_set_width(eventTitleLabel, LV_PCT(100));
    lv_label_set_long_mode(eventTitleLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(eventTitleLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(eventTitleLabel, &lv_font_montserrat_16, 0);

    if (ev.location[0])
    {
        lv_obj_t *locLabel = lv_label_create(card);
        lv_label_set_text(locLabel, ev.location);
        lv_obj_set_style_text_color(locLabel, lv_color_hex(0x666666), 0);
        lv_obj_set_width(locLabel, LV_PCT(100));
        lv_label_set_long_mode(locLabel, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(locLabel, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *hint = lv_label_create(overlay);
    lv_label_set_text(hint, "Tap anywhere to close");
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
}

// Scans the already-cached events[] (no network call - this runs on every
// todayTick(), i.e. every loop() iteration, so it must stay cheap; MAX_EVENTS
// is small enough that a linear scan is negligible) for one whose start time
// has just crossed into the 15-minute window. Real-world timing note: since
// this only runs once a second-ish in practice (loop()'s own pacing) and
// the window is checked as "0 to 15 minutes away" (not "exactly 15"), a
// slow tick or a fetch landing mid-window can't cause the reminder to be
// skipped entirely the way an exact-equality check would.
static void checkUpcomingEventReminder(unsigned long /*nowMs*/)
{
    time_t now = time(nullptr);
    for (int i = 0; i < eventCount; i++)
    {
        const AgendaEvent &ev = events[i];
        if (ev.allDay)
            continue; // no single "start time" to count down to
        if (ev.startEpoch == remindedEventStartEpoch)
            continue; // already reminded about this exact event instance
        time_t secondsUntilStart = ev.startEpoch - now;
        if (secondsUntilStart > 0 && secondsUntilStart <= 15 * 60)
        {
            remindedEventStartEpoch = ev.startEpoch;
            showEventReminderOverlay(ev);
            break; // one popup at a time - if two events are both due, the next tick catches the other
        }
    }
}

time_t todayGetNextEventStartEpoch()
{
    time_t now = time(nullptr);
    time_t best = 0;
    for (int i = 0; i < eventCount; i++)
    {
        const AgendaEvent &ev = events[i];
        if (ev.allDay || ev.startEpoch <= now)
            continue;
        if (best == 0 || ev.startEpoch < best)
            best = ev.startEpoch;
    }
    return best;
}

void todayTick()
{
    unsigned long now = millis();

    // Staggered ~5s behind bus.cpp's own boot-time fetch (which fires as
    // soon as wifiSettled() is true, no extra stagger) - real serial
    // evidence showed a single, uncontended fetch could still fail with
    // "SSL - Memory allocation failed" even with ~70KB free internal heap
    // (fragmentation, not scarcity - see CLAUDE.md's postmortem). Each
    // fetch task's own 20KB stack is reserved from that same internal heap
    // for the task's whole lifetime, so both tabs' boot-time fetch tasks
    // existing at the literal same instant (right when Wi-Fi's own
    // post-connect churn is also happening) was making that fragmentation
    // worse than it needed to be. This doesn't fully eliminate the
    // failure mode (the framework doesn't expose a way to shrink mbedTLS's
    // own buffer sizes from sketch code) but reduces how often it's hit.
    static unsigned long todayEligibleAtMs = 0;
    if (todayEligibleAtMs == 0 && wifiSettled())
        todayEligibleAtMs = now + 5000;
    bool todayNetworkEligible = todayEligibleAtMs != 0 && now >= todayEligibleAtMs;

    // Real bug fixed here: this completion-handling block used to run AFTER
    // the "start a new fetch?" check below, in the same function call. On
    // the exact tick a fetch finishes, fetchInProgress flips to false but
    // everSucceeded was still stale (not yet updated this tick) when the
    // retry check ran - so dueForInitialRetry (below) saw !everSucceeded
    // still true and immediately started a SECOND fetch, same tick, before
    // the first one's success was even recorded. Confirmed via a real boot
    // capture: two fetches back-to-back, first returning 0 events (likely
    // genuinely incomplete - fired before Wi-Fi/DNS was fully settled),
    // second returning the correct 16 - the forced immediate retry just
    // happened to land after more time had passed. Moved above the retry
    // check so everSucceeded is current before dueForInitialRetry reads it.
    if (fetchJustCompleted)
    {
        if (lastError[0] == '\0')
            everSucceeded = true;
        diagReport(DIAG_CALENDAR, lastError[0] == '\0', lastError);
        fetchJustCompleted = false;
        setLoadingVisible(false);
        renderDay();
    }

    bool dueForAutoFetch = tabIsActive && autoRefreshEnabled && (lastFetch == 0 || now - lastFetch > fetchIntervalMs);
    // Retry every 5s (was 20s), independent of tabIsActive/autoRefreshEnabled,
    // until the first fetch actually succeeds - see everSucceeded's comment.
    // Shortened because the failure mode above fails fast (sub-second, no
    // DNS wait involved) and is usually transient, so a quick retry is far
    // more likely to just work than a long wait is.
    bool dueForInitialRetry = !everSucceeded && (lastFetch == 0 || now - lastFetch > 5000UL);
    // askIsBusy() - see bus.cpp's identical comment: don't start a new
    // fetch while a voice question is using the shared net_lock.h lock for
    // its own mic/speaker I2S session, so it never has to race this one.
    // speakerIsBusy() - see its own header comment: covers debug-only
    // playback (the 'replay'/'tone' Serial commands) that askIsBusy() alone
    // doesn't, since those aren't part of a real ask.cpp flow.
    if (!fetchInProgress && todayNetworkEligible && !askIsBusy() && !speakerIsBusy() && (forceFetch || dueForAutoFetch || dueForInitialRetry))
    {
        bool userTriggered = forceFetch;
        forceFetch = false;
        lastFetch = now;
        viewedDayOffset = 0; // a fresh fetch re-centers the window on the real "today"
        startFetch(userTriggered);
    }

    // Backstop on top of the per-phase timeouts inside fetchAgendaTask
    // itself (handshake 8s + connect 8s + a 20s streaming-read cap). This
    // used to stop at ~36s/45s, missing a real component: DNS resolution
    // happens before any of those phases and isn't bounded by them - the
    // actual installed framework's WiFiGenericClass::hostByName()
    // (WiFiGeneric.cpp, read directly rather than assumed) can legitimately
    // block up to ~31s worst case (~16s waiting for a prior lookup to go
    // idle, then ~15s for its own - "real internal timeout in lwip library
    // is 14[s]", per that function's own comment). A too-short watchdog was
    // killing fetches that were still genuinely in progress on a slow
    // (not broken) connection, which then triggered this same watchdog's
    // disruptive WiFi.disconnect() recovery below for nothing. 65s
    // comfortably covers DNS(~31s) + connect(8s) + handshake(8s) +
    // read(20s) with margin.
    // Requirement: "do not cancel or interrupt a running HTTP request" - the
    // old version of this watchdog force-killed the fetch's own FreeRTOS
    // task. That's no longer possible or desired: fetchAgendaWork() now
    // runs ON the single shared Network Worker task, which every other
    // tab's network access also depends on - killing it would break
    // everything, not just Today. Relies instead on fetchAgendaWork()'s own
    // already-bounded timeouts to guarantee the job function itself always
    // returns. This is now a pure UI-side unstick, not a task kill.
    if (fetchInProgress && millis() - fetchStartedAtMs > 65000) // fresh millis(): `now` predates startFetch() above, and now - fetchStartedAtMs would underflow
    {
        Serial.println("today: fetchInProgress stuck past 65s - clearing UI state only (worker task untouched, per no-cancel requirement)");
        fetchInProgress = false;
        snprintf(lastError, sizeof(lastError), "Timed out, tap the date to retry");
        setLoadingVisible(false);
        renderDay();
    }

    checkUpcomingEventReminder(now);
}
