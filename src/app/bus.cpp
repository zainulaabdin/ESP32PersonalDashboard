// LTA Bus Arrival tab. Fetches from DataMall's v3 BusArrival endpoint every
// 30s, shows up to 3 upcoming buses per service with a load (capacity)
// gauge, and ticks the countdown locally every second between fetches.
// Layout: a left sidebar (fixed green search box + scrollable recent-stop
// history) and a right-side scrollable table of services, with a header
// showing the current stop's real name (resolved via bus_stops.cpp/.h -
// the arrival endpoint itself never returns one, confirmed by inspecting
// its raw JSON).
#include "bus.h"
#include "bus_stops.h"
#include "status_bar.h"
#include "net_lock.h"
#include "speaker.h"
#include "ask.h"
#include "network_worker.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <ctype.h>
#include <esp_heap_caps.h>
#include "secrets_select.h"
#include "app_config.h"
#include "diag.h"

#define LTA_BUS_ARRIVAL_URL "https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival?BusStopCode="
#define MAX_SERVICES 16
#define MAX_HISTORY 20
#define SIDEBAR_WIDTH 140
#define DEFAULT_FETCH_INTERVAL_MS 30000UL
#define MIN_FETCH_INTERVAL_SEC 5
#define MAX_FETCH_INTERVAL_SEC 600

// Runtime-adjustable via the Settings tab (busSetRefreshIntervalSec()) -
// was a compile-time constant before that existed.
static unsigned long fetchIntervalMs = DEFAULT_FETCH_INTERVAL_MS;

void busSetRefreshIntervalSec(int seconds)
{
    if (seconds < MIN_FETCH_INTERVAL_SEC)
        seconds = MIN_FETCH_INTERVAL_SEC;
    if (seconds > MAX_FETCH_INTERVAL_SEC)
        seconds = MAX_FETCH_INTERVAL_SEC;
    fetchIntervalMs = (unsigned long)seconds * 1000UL;
}

int busGetRefreshIntervalSec()
{
    return (int)(fetchIntervalMs / 1000UL);
}

// LTA's Load field: SEA = Seats Available, SDA = Standing Available,
// LSD = Limited Standing (near/at capacity) - confirmed against the live
// API. Mapped to a green/orange/red gauge per the reference screenshot.
enum LoadLevel
{
    LOAD_UNKNOWN = 0,
    LOAD_SEATS_AVAILABLE,
    LOAD_STANDING_AVAILABLE,
    LOAD_LIMITED_STANDING,
};

struct BusService
{
    char serviceNo[8];
    time_t eta[3];
    char type[3];
    uint8_t load[3];
};

static lv_obj_t *stopNameLabel;
static lv_obj_t *tableList;
static lv_obj_t *historyList;
static lv_obj_t *rowContainers[MAX_SERVICES];
static lv_obj_t *svcLabels[MAX_SERVICES];
static lv_obj_t *valueLabels[MAX_SERVICES][3];
static lv_obj_t *loadBars[MAX_SERVICES][3];
static lv_obj_t *doubleLabels[MAX_SERVICES][3];
static lv_obj_t *autoRefreshSwitch;

static BusService services[MAX_SERVICES];
static int serviceCount = 0;
static unsigned long lastFetch = 0;
static unsigned long lastRender = 0;
static char lastError[64] = "";
static bool autoRefreshEnabled = false;
static bool forceFetch = false;
// Set once the very first fetch actually succeeds. Until then, busTick()
// keeps retrying on a short fixed interval regardless of tabIsActive/
// autoRefreshEnabled - see the retry logic in busTick() for why: without
// this, a single failed boot-time fetch (e.g. Wi-Fi reporting "connected"
// a moment before DNS/routing is actually usable) would never be retried
// at all while the user is looking at a different tab (e.g. Ask), since
// the normal periodic refresh is gated on the Bus tab being the visible
// one - "network never updates, never recovers" from the user's
// perspective even though nothing is actually stuck.
static bool everSucceeded = false;
// Whether the Bus tab is the one currently on screen - periodic
// auto-refresh is pointless (and just burns Wi-Fi/battery) while the user
// is looking at a different tab, so it's gated on this.
static bool tabIsActive = false;

// Same pattern/invariant as today.cpp: the fetch runs on a background
// FreeRTOS task so it can't block the UI (LVGL/touch) the way it used to,
// and every UI handler that reads/writes services[]/serviceCount checks
// fetchInProgress first rather than needing a mutex.
static volatile bool fetchInProgress = false;
static volatile bool fetchJustCompleted = false;
static unsigned long fetchStartedAtMs = 0;

static char activeStopCode[6] = "";
static char history[MAX_HISTORY][6];
static int historyCount = 0;

// Howard Hinnant's "days from civil" algorithm: turns a Y/M/D into a day
// count relative to the 1970-01-01 epoch, with no notion of timezone at
// all (pure calendar arithmetic). Used instead of timegm()/mktime() below,
// see the big comment on parseIso8601ToUtcEpoch() for why.
static long daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// LTA timestamps carry their own UTC offset (e.g. "...+08:00"). This used to
// go through mktime(), on the assumption that with no TZ set it treats the
// parsed fields as if they were already UTC - but this project's own
// setup() calls configTime(8*3600, 0, ...), which sets the device's TZ to
// Singapore (UTC+8). mktime() honors that TZ, so it was already converting
// the parsed local-looking fields from SGT to UTC on its own; subtracting
// the ISO string's +08:00 offset afterward then applied the same 8h shift
// a second time, landing every ETA ~8h in the past and permanently
// clamping every countdown to "Arr". Verified on-device: raw ETA
// "2026-09-17T00:06:24+08:00" (~8 min away) was parsing to an epoch 7h52m
// *before* time(nullptr). timegm() would be the standard fix (always treats
// the fields as UTC regardless of the device's TZ) but isn't available in
// this toolchain's newlib, so daysFromCivil() above does the same job with
// plain calendar arithmetic - genuinely TZ-independent, not just "TZ
// currently happens to match".
static time_t parseIso8601ToUtcEpoch(const char *iso)
{
    if (!iso || iso[0] == '\0')
        return 0;

    struct tm tmVal = {};
    int offsetHour = 0, offsetMin = 0;
    char offsetSign = '+';
    int matched = sscanf(iso, "%d-%d-%dT%d:%d:%d%c%d:%d",
                          &tmVal.tm_year, &tmVal.tm_mon, &tmVal.tm_mday,
                          &tmVal.tm_hour, &tmVal.tm_min, &tmVal.tm_sec,
                          &offsetSign, &offsetHour, &offsetMin);
    if (matched < 6)
        return 0;

    long days = daysFromCivil(tmVal.tm_year, (unsigned)tmVal.tm_mon, (unsigned)tmVal.tm_mday);
    time_t asIfUtc = days * 86400L + tmVal.tm_hour * 3600L + tmVal.tm_min * 60L + tmVal.tm_sec;

    long offsetSeconds = offsetHour * 3600L + offsetMin * 60L;
    if (offsetSign == '-')
        offsetSeconds = -offsetSeconds;

    return asIfUtc - offsetSeconds;
}

// Returns whether the eta is valid (so the caller knows whether to show the
// load gauge / "DOUBLE" label or hide them).
static bool formatEtaMinutes(char *out, size_t outSize, time_t eta)
{
    if (eta == 0)
    {
        snprintf(out, outSize, "NA");
        return false;
    }
    long mins = (eta - time(nullptr)) / 60;
    if (mins <= 0)
        snprintf(out, outSize, "Arr");
    else
        snprintf(out, outSize, "%ld", mins);
    return true;
}

static uint8_t parseLoadLevel(const char *load)
{
    if (!load)
        return LOAD_UNKNOWN;
    if (strcmp(load, "SEA") == 0)
        return LOAD_SEATS_AVAILABLE;
    if (strcmp(load, "SDA") == 0)
        return LOAD_STANDING_AVAILABLE;
    if (strcmp(load, "LSD") == 0)
        return LOAD_LIMITED_STANDING;
    return LOAD_UNKNOWN;
}

// Fills in the gauge's percentage and color for a load level. Unknown
// (shouldn't normally happen when the eta itself is valid) defaults to the
// green/low-load look rather than leaving the bar in an undefined state.
static void styleLoadBar(lv_obj_t *bar, uint8_t level)
{
    int pct;
    lv_color_t color;
    switch (level)
    {
    case LOAD_STANDING_AVAILABLE:
        pct = 66;
        color = lv_color_hex(0xfb8c00);
        break;
    case LOAD_LIMITED_STANDING:
        pct = 100;
        color = lv_color_hex(0xe53935);
        break;
    case LOAD_SEATS_AVAILABLE:
    default:
        pct = 33;
        color = lv_color_hex(0x43a047);
        break;
    }
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
}

// ---- Recent-stop history (persisted to NVS flash via Preferences, since
// the user wants it to survive a reboot) ----------------------------------

static void saveHistory()
{
    String joined;
    for (int i = 0; i < historyCount; i++)
    {
        if (i)
            joined += ",";
        joined += history[i];
    }
    Preferences prefs;
    prefs.begin("bushist", false);
    prefs.putString("codes", joined);
    prefs.end();
}

static void loadHistory()
{
    Preferences prefs;
    prefs.begin("bushist", true);
    String joined = prefs.getString("codes", "");
    prefs.end();

    historyCount = 0;
    int start = 0;
    while (start < (int)joined.length() && historyCount < MAX_HISTORY)
    {
        int comma = joined.indexOf(',', start);
        String code = (comma == -1) ? joined.substring(start) : joined.substring(start, comma);
        if (code.length() > 0)
        {
            snprintf(history[historyCount], sizeof(history[historyCount]), "%s", code.c_str());
            historyCount++;
        }
        if (comma == -1)
            break;
        start = comma + 1;
    }
}

static void historyRowClicked(lv_event_t *e);

// ---- Favourite stops: one for midnight-noon, one for noon-midnight -------
// The one for the current half of the day is shown first in the sidebar
// and becomes the active stop at boot and at 00:00 / 12:00. Picking
// another stop by hand sticks until the next switch. Defaults to
// FAVORITE_BUS_STOP from secrets.h; changed from the web settings page.
static char favAm[6] = "";
static char favPm[6] = "";
// 0 = morning, 1 = afternoon, -1 = not decided yet (clock not synced, or
// favourites just changed) - busTick() picks the favourite when it resolves.
static int favHalf = -1;

static const char *currentFavorite()
{
    return favHalf == 1 ? favPm : favAm;
}

static void loadFavorites()
{
    Preferences prefs;
    prefs.begin("bushist", true);
    String am = prefs.getString("favAm", FAVORITE_BUS_STOP);
    String pm = prefs.getString("favPm", FAVORITE_BUS_STOP);
    prefs.end();
    snprintf(favAm, sizeof(favAm), "%s", am.c_str());
    snprintf(favPm, sizeof(favPm), "%s", pm.c_str());
}

static void addHistoryRow(int historyIndex, const char *code, bool favorite)
{
    bool active = strcmp(code, activeStopCode) == 0;

    lv_obj_t *row = lv_obj_create(historyList);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 40);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, active ? 0 : 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0xd8dbde), 0);
    if (active)
        lv_obj_set_style_bg_color(row, lv_color_hex(0x43a047), 0);
    else
        lv_obj_set_style_bg_color(row, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, historyRowClicked, LV_EVENT_CLICKED, (void *)(intptr_t)historyIndex);

    lv_obj_t *codeLabel = lv_label_create(row);
    if (favorite)
        lv_label_set_text_fmt(codeLabel, LV_SYMBOL_HOME " %s", code);
    else
        lv_label_set_text(codeLabel, code);
    lv_obj_set_style_text_font(codeLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(codeLabel, active ? lv_color_white() : lv_color_hex(0x222222), 0);

    const char *name = busStopsLookupName(code);
    lv_obj_t *nameLabel = lv_label_create(row);
    lv_obj_set_width(nameLabel, SIDEBAR_WIDTH - 20);
    lv_label_set_long_mode(nameLabel, LV_LABEL_LONG_DOT);
    lv_label_set_text(nameLabel, (name && name[0]) ? name : "");
    lv_obj_set_style_text_font(nameLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(nameLabel, active ? lv_color_hex(0xe8f5e9) : lv_color_hex(0x888888), 0);
}

// Rebuilds the sidebar's history rows from scratch - simpler than
// maintaining a pool of show/hide objects now that the list is
// unbounded-ish and scrolls, and this only runs on a stop change, not
// every frame. Each row is two lines (stop code, bigger; stop name,
// smaller/ellipsized) styled like a tab, with the currently active stop
// highlighted so it's distinguishable from the rest.
// The current favourite always comes first (with a home symbol); the rest
// of the history keeps its own order.
static void renderHistoryList()
{
    lv_obj_clean(historyList);
    const char *fav = currentFavorite();
    if (fav[0])
        addHistoryRow(-1, fav, true);
    for (int i = 0; i < historyCount; i++)
        if (strcmp(history[i], fav) != 0)
            addHistoryRow(i, history[i], false);
}

static void busRender();

// Selecting a stop (from history or search) is also this tab's only
// "refresh" mechanism now - the standalone refresh button was removed
// since re-tapping the already-active stop's history row forces a fetch
// just as well.
//
// Explicitly does NOT reorder an already-present entry to the front -
// the user wants the list order stable when just picking a different
// active stop (the earlier "move to front on select" behavior was
// reshuffling their deliberately-chosen seed order every tap). A genuinely
// new code (typed via the search keypad) still gets added at the front,
// since there's no existing position for it to disturb.
static void selectStop(const char *code)
{
    // Bail out rather than race the background fetch task's writes to
    // services[]/serviceCount - same invariant as the Today tab's arrows
    // (see today.cpp's file-header comment for why this is safe without a
    // mutex).
    if (fetchInProgress)
        return;
    if (!code || code[0] == '\0')
        return;
    snprintf(activeStopCode, sizeof(activeStopCode), "%s", code);

    bool exists = false;
    for (int i = 0; i < historyCount; i++)
        if (strcmp(history[i], code) == 0)
        {
            exists = true;
            break;
        }

    if (!exists)
    {
        if (historyCount >= MAX_HISTORY)
            historyCount = MAX_HISTORY - 1;
        for (int i = historyCount; i > 0; i--)
            snprintf(history[i], sizeof(history[i]), "%s", history[i - 1]);
        snprintf(history[0], sizeof(history[0]), "%s", code);
        historyCount++;
        saveHistory();
    }

    forceFetch = true;
    renderHistoryList(); // re-render so the active-stop highlight moves even when order doesn't
    busRender();
}

static void historyRowClicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0)
        selectStop(currentFavorite());
    else if (idx < historyCount)
        selectStop(history[idx]);
}

// ---- Search box -> full-screen numeric keypad overlay --------------------

static void keypadEvent(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_target(e);
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);

    if (code == LV_EVENT_READY)
    {
        const char *val = lv_textarea_get_text(ta);
        if (val && val[0])
            selectStop(val);
        lv_obj_del(lv_obj_get_parent(kb));
    }
    else if (code == LV_EVENT_CANCEL)
    {
        lv_obj_del(lv_obj_get_parent(kb));
    }
}

static void openSearchKeypad(lv_event_t *e)
{
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ta = lv_textarea_create(overlay);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 5);
    lv_textarea_set_accepted_chars(ta, "0123456789");
    lv_textarea_set_placeholder_text(ta, "Bus stop code");
    lv_obj_set_width(ta, 200);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *kb = lv_keyboard_create(overlay);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_set_size(kb, LV_PCT(100), 150);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, keypadEvent, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, keypadEvent, LV_EVENT_CANCEL, NULL);
}

static void autoRefreshToggled(lv_event_t *e)
{
    autoRefreshEnabled = lv_obj_has_state(autoRefreshSwitch, LV_STATE_CHECKED);
}

void busSetTabActive(bool active)
{
    tabIsActive = active;
}

// ---- Exposed for the Ask tab's context-injection (see ask.cpp) ----------

void busSelectStopByCode(const char *code)
{
    selectStop(code);
}

bool busIsFetchInProgress()
{
    return fetchInProgress;
}

const char *busGetActiveStopCode()
{
    return activeStopCode;
}

void busGetContextSummary(char *out, size_t outSize)
{
    if (lastFetch == 0)
    {
        snprintf(out, outSize, "Bus: no data loaded yet.");
        return;
    }
    const char *name = busStopsLookupName(activeStopCode);
    char buf[300] = "";
    size_t used = 0;
    int shown = 0;
    for (int i = 0; i < serviceCount && shown < 6; i++)
    {
        char val0[8], val1[8];
        bool v0 = formatEtaMinutes(val0, sizeof(val0), services[i].eta[0]);
        bool v1 = formatEtaMinutes(val1, sizeof(val1), services[i].eta[1]);
        char line[64];
        if (!v0)
            continue;
        if (v0 && v1)
            snprintf(line, sizeof(line), "%s: %s & %s min; ", services[i].serviceNo, val0, val1);
        else
            snprintf(line, sizeof(line), "%s: %s min; ", services[i].serviceNo, val0);
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
        snprintf(out, outSize, "Bus stop %s (%s): no upcoming arrivals right now.", activeStopCode, name ? name : "");
    else
        snprintf(out, outSize, "Bus stop %s (%s) arrivals in minutes: %s", activeStopCode, name ? name : "", buf);
}

// Plain case-insensitive substring search (no strcasestr dependency - not
// guaranteed present in every newlib config this toolchain might use).
static bool containsCaseInsensitive(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0])
        return false;
    size_t hLen = strlen(haystack), nLen = strlen(needle);
    if (nLen > hLen)
        return false;
    for (size_t i = 0; i + nLen <= hLen; i++)
    {
        size_t j = 0;
        for (; j < nLen; j++)
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j]))
                break;
        if (j == nLen)
            return true;
    }
    return false;
}

bool busFindStopInHistoryByName(const char *text, char *outCode, size_t outCodeSize)
{
    if (!text || !text[0] || !busStopsReady())
        return false;
    for (int i = 0; i < historyCount; i++)
    {
        const char *name = busStopsLookupName(history[i]);
        if (name && name[0] && containsCaseInsensitive(text, name))
        {
            snprintf(outCode, outCodeSize, "%s", history[i]);
            return true;
        }
    }
    return false;
}

// ---- Data fetch --------------------------------------------------------

// Runs on the shared Network Worker task now (see fetchBusJob() below and
// network_worker.h) - must never call an lv_* function. Unchanged from
// before other than where it's called from.
static void fetchBusWork()
{
    // See net_lock.h: two concurrent TLS handshakes (e.g. this and
    // today.cpp's fetch, both firing around boot) were found to exhaust
    // the ESP32's internal heap and fail both with "SSL - Memory
    // allocation failed" - held for the whole function, not just the
    // handshake, since only one HTTPS call is useful at a time here anyway.
    NetLockGuard netLock;
    if (!netLock.acquired())
    {
        snprintf(lastError, sizeof(lastError), "Busy, try again");
        Serial.println("bus: could not acquire network lock in time");
        return;
    }

    // Internal RAM before the TLS handshake (mbedTLS buffers come from it).
    Serial.printf("bus: free internal heap before fetch: %u bytes, largest free block: %u bytes\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    Serial.println("bus: fetch start");
    WiFiClientSecure client;
    client.setInsecure();
    // Same fix as today.cpp: WiFiClientSecure's default handshake timeout
    // is 120 seconds, which could leave a stalled connection blocking far
    // longer than expected with no visible recovery.
    client.setHandshakeTimeout(8);

    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    if (!http.begin(client, String(LTA_BUS_ARRIVAL_URL) + activeStopCode))
    {
        snprintf(lastError, sizeof(lastError), "begin() failed");
        Serial.println("bus: http.begin() failed");
        return;
    }
    http.addHeader("AccountKey", cfgLtaKey());
    http.addHeader("accept", "application/json");

    int code = http.GET();
    Serial.printf("bus: HTTP GET returned %d\n", code);
    if (code != 200)
    {
        snprintf(lastError, sizeof(lastError), "HTTP %d", code);
        http.end();
        return;
    }

    String body = http.getString();
    http.end();
    addNetworkBytes(body.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err)
    {
        snprintf(lastError, sizeof(lastError), "JSON: %s", err.c_str());
        Serial.printf("bus: JSON parse error: %s\n", err.c_str());
        return;
    }

    lastError[0] = '\0';
    serviceCount = 0;
    static const char *nextBusKeys[3] = {"NextBus", "NextBus2", "NextBus3"};
    for (JsonObject svc : doc["Services"].as<JsonArray>())
    {
        if (serviceCount >= MAX_SERVICES)
            break;
        BusService &s = services[serviceCount];
        snprintf(s.serviceNo, sizeof(s.serviceNo), "%s", (const char *)(svc["ServiceNo"] | ""));
        for (int i = 0; i < 3; i++)
        {
            JsonObject nb = svc[nextBusKeys[i]];
            const char *typeStr = nb["Type"] | "";
            s.eta[i] = parseIso8601ToUtcEpoch(nb["EstimatedArrival"] | "");
            s.type[i] = typeStr[0] ? typeStr[0] : '?';
            s.load[i] = parseLoadLevel(nb["Load"] | "");
        }
        serviceCount++;
    }
    Serial.printf("bus: fetch ok, %d services\n", serviceCount);
}

// Runs on the single shared Network Worker task now (network_worker.h/.cpp),
// not its own xTaskCreatePinnedToCore() task - see that header's comment on
// why (removes the internal-heap contention between bus.cpp's/today.cpp's/
// ask.cpp's own task stacks that was causing real, confirmed task-creation
// failures). fetchBusWork() itself is unchanged.
static void fetchBusJob()
{
    fetchBusWork();
    fetchJustCompleted = true;
    fetchInProgress = false;
}

static void startFetch(bool userTriggered)
{
    if (fetchInProgress)
        return;
    fetchInProgress = true;
    fetchStartedAtMs = millis();
    // Color set here, at the trigger, not on tab-switch - so the spinner's
    // color always identifies which subsystem's background fetch is
    // actually running, even if you're looking at a different tab.
    setLoadingSpinnerColor(0x43a047);
    setLoadingVisible(true);
    // USER_REFRESH (a tap on a stop) outranks periodic BUS auto-refresh -
    // matches this app's existing forceFetch-vs-auto distinction, now
    // expressed as the job's queue priority instead of just an internal
    // bool. Periodic auto-refresh is deduplicated (isPeriodic=true - only
    // one pending BUS job at a time); a user tap is not, since a second
    // deliberate tap should always get its own fresh fetch.
    if (userTriggered)
        networkWorkerSubmit(NET_JOB_USER_REFRESH, "USER_REFRESH(bus)", fetchBusJob, false);
    else
        networkWorkerSubmit(NET_JOB_BUS, "BUS", fetchBusJob, true);
}

// ---- Rendering -----------------------------------------------------------

static void busRender()
{
    const char *name = busStopsLookupName(activeStopCode);
    char headerBuf[80];
    if (lastError[0])
        snprintf(headerBuf, sizeof(headerBuf), "%s - %s", activeStopCode, lastError);
    else if (name && name[0])
        snprintf(headerBuf, sizeof(headerBuf), "%s - %s", activeStopCode, name);
    else
        snprintf(headerBuf, sizeof(headerBuf), "Stop %s", activeStopCode);
    lv_label_set_text(stopNameLabel, headerBuf);

    for (int i = 0; i < MAX_SERVICES; i++)
    {
        if (i >= serviceCount)
        {
            lv_obj_add_flag(rowContainers[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(rowContainers[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(svcLabels[i], services[i].serviceNo);

        for (int c = 0; c < 3; c++)
        {
            char val[8];
            bool valid = formatEtaMinutes(val, sizeof(val), services[i].eta[c]);
            lv_label_set_text(valueLabels[i][c], val);
            if (valid)
            {
                lv_obj_clear_flag(loadBars[i][c], LV_OBJ_FLAG_HIDDEN);
                styleLoadBar(loadBars[i][c], services[i].load[c]);
                if (services[i].type[c] == 'D')
                    lv_obj_clear_flag(doubleLabels[i][c], LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_add_flag(doubleLabels[i][c], LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_add_flag(loadBars[i][c], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(doubleLabels[i][c], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// ---- UI construction -------------------------------------------------

static void addServiceRow(lv_obj_t *parent, int index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    // Alternating rows tinted a light green (matching the new green bus
    // icon) instead of plain white/gray.
    lv_obj_set_style_bg_color(row, (index % 2 == 0) ? lv_color_hex(0xffffff) : lv_color_hex(0xe8f5e9), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    rowContainers[index] = row;

    lv_obj_t *svc = lv_label_create(row);
    lv_obj_set_style_text_font(svc, &lv_font_montserrat_16, 0);
    lv_obj_set_width(svc, 44);
    svcLabels[index] = svc;

    for (int c = 0; c < 3; c++)
    {
        lv_obj_t *cell = lv_obj_create(row);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 72, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 2, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *value = lv_label_create(cell);
        lv_obj_set_style_text_font(value, &lv_font_montserrat_16, 0);
        valueLabels[index][c] = value;

        // Small capacity gauge: green/orange/red per LTA's Load field
        // (SEA/SDA/LSD - seats available / standing available / limited
        // standing), filled proportionally as a rough "how full" cue.
        lv_obj_t *bar = lv_bar_create(cell);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 44, 6);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xe0e0e0), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, 100);
        loadBars[index][c] = bar;

        lv_obj_t *doubleLabel = lv_label_create(cell);
        lv_label_set_text(doubleLabel, "DOUBLE");
        lv_obj_set_style_text_font(doubleLabel, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(doubleLabel, lv_color_hex(0x2e7d32), 0);
        doubleLabels[index][c] = doubleLabel;
    }
}

void busInit(lv_obj_t *tab)
{
    Serial.println("bus: busInit start");

    // One-time migration: the very first version of this feature seeded
    // history with just the single configured BUS_STOP. Now that there's a
    // real starter set, overwrite whatever's there exactly once (tracked by
    // the "seeded" flag) rather than on every boot, so history built up
    // afterward through real use isn't wiped on a later reboot.
    Preferences seedCheck;
    seedCheck.begin("bushist", true);
    bool seeded = seedCheck.getBool("seeded", false);
    seedCheck.end();

    if (!seeded)
    {
        static const char *defaults[5] = {"18101", "11261", "43239", "43231", "18071"};
        historyCount = 5;
        for (int i = 0; i < 5; i++)
            snprintf(history[i], sizeof(history[i]), "%s", defaults[i]);
        saveHistory();
        Preferences p;
        p.begin("bushist", false);
        p.putBool("seeded", true);
        p.end();
    }
    else
    {
        loadHistory();
    }

    // Start on the morning favourite; busTick() switches to the afternoon
    // one as soon as the clock is synced, if it's past noon.
    loadFavorites();
    snprintf(activeStopCode, sizeof(activeStopCode), "%s", favAm);

    // Auto-refresh being off by default (per earlier request) must not
    // block the very first load - the initially active stop's arrivals
    // (and its name, once bus_stops' dataset is ready) should still show
    // up without the user having to tap anything first.
    forceFetch = true;

    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(tab, 8, 0);

    // --- Left sidebar: fixed search box + scrollable recent-stop history ---
    lv_obj_t *sidebar = lv_obj_create(tab);
    lv_obj_remove_style_all(sidebar);
    lv_obj_set_size(sidebar, SIDEBAR_WIDTH, LV_PCT(100));
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sidebar, 4, 0);
    lv_obj_clear_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *searchBox = lv_obj_create(sidebar);
    lv_obj_remove_style_all(searchBox);
    lv_obj_set_size(searchBox, LV_PCT(100), 40);
    lv_obj_set_style_radius(searchBox, 6, 0);
    lv_obj_set_style_bg_color(searchBox, lv_color_hex(0x43a047), 0);
    lv_obj_set_style_bg_opa(searchBox, LV_OPA_COVER, 0);
    lv_obj_clear_flag(searchBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(searchBox, openSearchKeypad, LV_EVENT_CLICKED, NULL);

    lv_obj_t *searchLabel = lv_label_create(searchBox);
    lv_label_set_text(searchLabel, LV_SYMBOL_KEYBOARD " Search stop");
    lv_obj_set_style_text_font(searchLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(searchLabel, lv_color_white(), 0);
    lv_obj_center(searchLabel);

    // Scrolls independently of the fixed search box above it.
    historyList = lv_obj_create(sidebar);
    lv_obj_remove_style_all(historyList);
    lv_obj_set_size(historyList, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(historyList, 1);
    lv_obj_set_flex_flow(historyList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(historyList, 4, 0);
    lv_obj_set_scrollbar_mode(historyList, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(historyList, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(historyList, lv_color_hex(0x9e9e9e), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(historyList, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(historyList, 2, LV_PART_SCROLLBAR);

    // --- Right side: header + scrollable arrival table ---
    lv_obj_t *content = lv_obj_create(tab);
    lv_obj_remove_style_all(content);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_height(content, LV_PCT(100));
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(content);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), 26);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    stopNameLabel = lv_label_create(header);
    lv_label_set_text(stopNameLabel, "Loading...");
    lv_label_set_long_mode(stopNameLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(stopNameLabel, 1);
    // LVGL's built-in montserrat fonts have no true bold weight compiled in,
    // so - same trick used for the service-number column - a bigger size is
    // the "bold" proxy here.
    lv_obj_set_style_text_font(stopNameLabel, &lv_font_montserrat_18, 0);

    // No standalone refresh button - tapping any stop in the sidebar
    // (including re-tapping the currently active one) already forces a
    // fetch via selectStop(), so that doubles as "refresh". On by default
    // (unlike Today's auto-refresh, which stays off by default).
    autoRefreshSwitch = lv_switch_create(header);
    lv_obj_set_size(autoRefreshSwitch, 34, 18);
    lv_obj_set_style_bg_color(autoRefreshSwitch, lv_color_hex(0x43a047), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_state(autoRefreshSwitch, LV_STATE_CHECKED);
    autoRefreshEnabled = true;
    lv_obj_add_event_cb(autoRefreshSwitch, autoRefreshToggled, LV_EVENT_VALUE_CHANGED, NULL);

    tableList = lv_obj_create(content);
    lv_obj_remove_style_all(tableList);
    lv_obj_set_size(tableList, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(tableList, 1);
    lv_obj_set_flex_flow(tableList, LV_FLEX_FLOW_COLUMN);
    // remove_style_all() above also wipes the scrollbar part's default
    // look, which is why it wasn't showing up even though the list was
    // genuinely scrollable - restyle it explicitly instead of relying on
    // theme defaults.
    lv_obj_set_scrollbar_mode(tableList, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(tableList, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(tableList, lv_color_hex(0x9e9e9e), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(tableList, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(tableList, 2, LV_PART_SCROLLBAR);

    for (int i = 0; i < MAX_SERVICES; i++)
    {
        addServiceRow(tableList, i);
        lv_obj_add_flag(rowContainers[i], LV_OBJ_FLAG_HIDDEN);
    }

    renderHistoryList();
    busRender();
    Serial.println("bus: busInit done");
}

void busSetFavorites(const char *morning, const char *afternoon)
{
    Preferences prefs;
    prefs.begin("bushist", false);
    if (strlen(morning) == 5)
    {
        snprintf(favAm, sizeof(favAm), "%s", morning);
        prefs.putString("favAm", favAm);
    }
    if (strlen(afternoon) == 5)
    {
        snprintf(favPm, sizeof(favPm), "%s", afternoon);
        prefs.putString("favPm", favPm);
    }
    prefs.end();
    favHalf = -1; // busTick() shows the favourite for the current time
}

const char *busGetFavorite(bool afternoon)
{
    return afternoon ? favPm : favAm;
}

// Switches to the matching favourite at boot (once the clock is synced),
// at 00:00 and at 12:00.
static void updateFavoriteForTime()
{
    time_t t = time(nullptr);
    struct tm local;
    localtime_r(&t, &local);
    if (local.tm_year + 1900 < 2024 || fetchInProgress)
        return; // clock not synced yet / selectStop() would bail - retry next tick
    int half = local.tm_hour >= 12 ? 1 : 0;
    if (half == favHalf)
        return;
    favHalf = half;
    if (strcmp(activeStopCode, currentFavorite()) != 0)
        selectStop(currentFavorite());
    else
        renderHistoryList(); // favourite row order may have changed
}

void busTick()
{
    unsigned long now = millis();
    bool needsRender = false;

    updateFavoriteForTime();

    // buildUi() (which builds the sidebar and calls renderHistoryList()
    // once) runs before busStopsInit() even starts loading the stop-name
    // dataset, so that first render always finds an empty lookup table.
    // busRender() (the header) recovers on its own since it already
    // re-runs every second below - but renderHistoryList() doesn't, so the
    // sidebar's names stayed permanently blank until this catches the
    // not-ready -> ready transition and rebuilds it exactly once.
    static bool busStopsWasReady = false;
    if (!busStopsWasReady && busStopsReady())
    {
        busStopsWasReady = true;
        renderHistoryList();
    }

    // Real bug fixed here (identical mechanism found and fixed in
    // today.cpp - see its matching comment): this completion block used to
    // run AFTER the "start a new fetch?" check below, in the same function
    // call. On the exact tick a fetch finishes, fetchInProgress flips to
    // false but everSucceeded was still stale (not yet updated this tick)
    // when the retry check ran below - so dueForInitialRetry saw
    // !everSucceeded still true and immediately fired a second fetch, same
    // tick, before the first one's success was even recorded. Moved above
    // the retry check so everSucceeded is current before dueForInitialRetry
    // reads it.
    if (fetchJustCompleted)
    {
        fetchJustCompleted = false;
        if (lastError[0] == '\0')
            everSucceeded = true;
        diagReport(DIAG_LTA, lastError[0] == '\0', lastError);
        setLoadingVisible(false);
        needsRender = true;
    }

    bool dueForAutoFetch = tabIsActive && autoRefreshEnabled && (lastFetch == 0 || now - lastFetch > fetchIntervalMs);
    // Retry every 5s (was 20s), independent of tabIsActive/autoRefreshEnabled,
    // until the first fetch actually succeeds - see everSucceeded's comment.
    // Shortened because the "SSL - Memory allocation failed" failure mode
    // (see CLAUDE.md's postmortem) fails fast (sub-second, no DNS wait
    // involved) and is usually transient heap fragmentation right after
    // Wi-Fi connects - a quick retry is far more likely to just work than a
    // long wait is.
    bool dueForInitialRetry = !everSucceeded && (lastFetch == 0 || now - lastFetch > 5000UL);
    // askIsBusy() - don't start a new fetch while a voice question is in
    // progress. Real user-reported bug: mic/speaker I2S sessions and this
    // fetch both draw on net_lock.h's shared lock (they compete for the
    // same scarce internal heap - see net_lock.h), and a fixed timeout on
    // the mic/speaker side was an unreliable way to referee that race
    // (either read as a hang if long, or silently dropped playback if
    // short). Not starting a *new* fetch here in the first place removes
    // the race for the common case entirely - an already-in-flight fetch
    // still runs to completion, not aborted, since it holds no more than
    // its own bounded duration on the lock.
    // speakerIsBusy() - see its own header comment: covers debug-only
    // playback (the 'replay'/'tone' Serial commands) that askIsBusy() alone
    // doesn't, since those aren't part of a real ask.cpp flow.
    if (!fetchInProgress && wifiSettled() && !askIsBusy() && !speakerIsBusy() && (forceFetch || dueForAutoFetch || dueForInitialRetry))
    {
        bool userTriggered = forceFetch;
        forceFetch = false;
        lastFetch = now;
        startFetch(userTriggered);
    }

    // Skipped while a fetch is running so the live countdown re-render
    // can't race the background task's writes to services[]/serviceCount -
    // the countdown just pauses for the (normally brief) duration of a
    // fetch instead of ticking.
    if (!fetchInProgress && now - lastRender > 1000)
    {
        lastRender = now;
        needsRender = true;
    }

    if (needsRender)
        busRender();

    // Backstop watchdog. This used to be 35000 based on just the per-phase
    // timeouts this file itself sets (handshake 8s + connect 8s + read
    // 8s = ~24s) - but that missed a real component: DNS resolution runs
    // *before* any of those and isn't bounded by them at all. Confirmed by
    // reading the actual installed framework's WiFiGenericClass::hostByName()
    // (WiFiGeneric.cpp) rather than assumed: it can legitimately block up
    // to ~16s waiting for a previous lookup to go idle, then up to another
    // ~15s for its own lookup ("real internal timeout in lwip library is
    // 14[s]", per that function's own comment) - up to ~31s just for DNS
    // in the worst case. A too-short watchdog was killing fetches that
    // were still genuinely in progress (just slow, e.g. on a weak signal),
    // which then triggered this same watchdog's disruptive WiFi.disconnect()
    // recovery below for a fetch that wasn't actually stuck - producing a
    // repeating disconnect/retry pattern instead of just letting a slow
    // fetch finish. 60s comfortably covers DNS(~31s worst case) + connect
    // (8s) + handshake (8s) + read (8s) with margin.
    // Requirement: "do not cancel or interrupt a running HTTP request" - the
    // old version of this watchdog force-killed the fetch's own FreeRTOS
    // task via vTaskDelete() on a stall. That's no longer possible (or
    // desired): fetchBusWork() now runs ON the single shared Network Worker
    // task (network_worker.h/.cpp) - killing it would kill the worker
    // itself, breaking every other tab's network access, not just Bus's.
    // Relying instead on fetchBusWork()'s own already-bounded timeouts
    // (connect 8s + handshake 8s + read 8s, ~24s, plus DNS's own real
    // worst-case ~31s per this comment's own prior investigation - see
    // above) to guarantee the job function itself always returns on its
    // own. This is now a pure UI-side unstick: if fetchInProgress somehow
    // stayed true well past every one of those real bounds (a bug
    // elsewhere, not an actual network hang), clear the local flag so the
    // tab isn't stuck showing a spinner forever - it does NOT touch the
    // worker task, which keeps running regardless.
    if (fetchInProgress && millis() - fetchStartedAtMs > 60000) // fresh millis(): `now` predates startFetch() above, and now - fetchStartedAtMs would underflow
    {
        Serial.println("bus: fetchInProgress stuck past 60s - clearing UI state only (worker task untouched, per no-cancel requirement)");
        fetchInProgress = false;
        snprintf(lastError, sizeof(lastError), "Timed out");
        setLoadingVisible(false);
        busRender();
    }
}
