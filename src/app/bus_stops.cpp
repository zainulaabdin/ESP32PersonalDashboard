#include "bus_stops.h"
#include "status_bar.h"
#include "net_lock.h"
#include "network_worker.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <FS.h>
#include <SPIFFS.h>
#include "secrets_select.h"
#include "app_config.h"

#define LTA_BUS_STOPS_URL "https://datamall2.mytransport.sg/ltaodataservice/BusStops?$skip="
#define CACHE_PATH "/busstops.bin"
#define CACHE_MAGIC 0x53425331UL // "1SBS" - bumped if the record layout ever changes
#define MAX_STOPS 6000           // real dataset is ~5208; some slack for growth

// Field sizes measured against the real dataset before writing this (see
// CLAUDE.md): BusStopCode is always 5 digits, Description (the actual
// human-readable name - RoadName is not what's wanted here) tops out at 36
// chars. lat/lon aren't used by anything yet, kept for the "nearby stops"
// feature discussed but not yet built.
struct BusStopInfo
{
    char code[6];
    char name[40];
    float lat;
    float lon;
};

struct CacheHeader
{
    uint32_t magic;
    uint32_t count;
};

static BusStopInfo *stops = nullptr;
static int stopCount = 0;
static bool ready = false;
static bool needsFetch = false;
static bool fetchAttempted = false;

static bool loadFromCache()
{
    if (!SPIFFS.exists(CACHE_PATH))
        return false;

    File f = SPIFFS.open(CACHE_PATH, FILE_READ);
    if (!f)
        return false;

    CacheHeader header;
    if (f.read((uint8_t *)&header, sizeof(header)) != sizeof(header) ||
        header.magic != CACHE_MAGIC || header.count == 0 || header.count > MAX_STOPS)
    {
        f.close();
        return false;
    }

    BusStopInfo *buf = (BusStopInfo *)ps_malloc(header.count * sizeof(BusStopInfo));
    if (!buf)
    {
        f.close();
        return false;
    }

    size_t expected = header.count * sizeof(BusStopInfo);
    size_t got = f.read((uint8_t *)buf, expected);
    f.close();
    if (got != expected)
    {
        free(buf);
        return false;
    }

    stops = buf;
    stopCount = header.count;
    Serial.printf("bus_stops: loaded %d stops from cache\n", stopCount);
    return true;
}

static void saveToCache()
{
    File f = SPIFFS.open(CACHE_PATH, FILE_WRITE);
    if (!f)
    {
        Serial.println("bus_stops: failed to open cache file for writing");
        return;
    }
    CacheHeader header{CACHE_MAGIC, (uint32_t)stopCount};
    f.write((const uint8_t *)&header, sizeof(header));
    f.write((const uint8_t *)stops, stopCount * sizeof(BusStopInfo));
    f.close();
    Serial.printf("bus_stops: wrote %d stops to cache\n", stopCount);
}

// ArduinoJson allocates its pool in chunks well under this SDK's 4KB
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL threshold, so a plain JsonDocument
// lands entirely in internal RAM. A ~69KB BusStops page grew it until TLS/
// lwIP could no longer receive (real log: stream died at 17541 of 69368
// bytes, "IncompleteInput") - so the document lives in PSRAM instead.
struct SpiRamAllocator : ArduinoJson::Allocator
{
    void *allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void *pointer) override { heap_caps_free(pointer); }
    void *reallocate(void *ptr, size_t new_size) override { return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM); }
};
static SpiRamAllocator spiRamAllocator;

static bool fetchFromApi()
{
    BusStopInfo *buf = (BusStopInfo *)ps_malloc(MAX_STOPS * sizeof(BusStopInfo));
    if (!buf)
    {
        Serial.println("bus_stops: ps_malloc failed");
        return false;
    }

    int count = 0;
    int skip = 0;
    while (count < MAX_STOPS)
    {
        // See net_lock.h: this multi-page fetch runs around boot at the
        // same time as bus.cpp's/today.cpp's own first fetch - two
        // concurrent TLS handshakes were found to exhaust the ESP32's
        // internal heap. Acquired per-page (not once for the whole
        // function) so a long multi-page fetch doesn't starve the other
        // tabs' own boot-time fetches for its entire duration.
        NetLockGuard netLock;
        if (!netLock.acquired())
        {
            // fetchFromApi() is only ever attempted once (see
            // fetchAttempted in busStopsTick()) - giving up here means stop
            // names stay unavailable until the next reboot, same as any
            // other failure path in this function already does.
            Serial.println("bus_stops: could not acquire network lock in time, aborting dataset fetch");
            break;
        }

        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        // Parsed straight from getStream() below, which hands back the raw
        // socket bytes - under HTTP/1.1 chunked encoding those include the
        // chunk-size lines mid-JSON. HTTP/1.0 forces a plain body.
        http.useHTTP10(true);
        http.setTimeout(15000); // ~500-stop pages are large; don't cut the stream off mid-parse
        String url = String(LTA_BUS_STOPS_URL) + skip;
        if (!http.begin(client, url))
        {
            Serial.println("bus_stops: http.begin() failed");
            break;
        }
        http.addHeader("AccountKey", cfgLtaKey());
        http.addHeader("accept", "application/json");

        int code = http.GET();
        if (code != 200)
        {
            Serial.printf("bus_stops: HTTP GET %s returned %d\n", url.c_str(), code);
            http.end();
            break;
        }

        // getSize() reads the Content-Length header the server already
        // sent, not the stream itself - safe to call before/regardless of
        // consuming the body below.
        int contentLength = http.getSize();
        if (contentLength > 0)
            addNetworkBytes((uint32_t)contentLength);

        // Only the fields actually copied out below - skips RoadName etc.
        JsonDocument filter;
        JsonObject f = filter["value"].add<JsonObject>();
        f["BusStopCode"] = true;
        f["Description"] = true;
        f["Latitude"] = true;
        f["Longitude"] = true;

        JsonDocument doc(&spiRamAllocator);
        DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        http.end();
        if (err)
        {
            Serial.printf("bus_stops: JSON parse error: %s (skip=%d, free internal %u)\n",
                          err.c_str(), skip, heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            break;
        }

        JsonArray page = doc["value"].as<JsonArray>();
        if (page.size() == 0)
            break; // no more pages

        for (JsonObject s : page)
        {
            if (count >= MAX_STOPS)
                break;
            BusStopInfo &info = buf[count];
            snprintf(info.code, sizeof(info.code), "%s", (const char *)(s["BusStopCode"] | ""));
            snprintf(info.name, sizeof(info.name), "%s", (const char *)(s["Description"] | ""));
            info.lat = s["Latitude"] | 0.0;
            info.lon = s["Longitude"] | 0.0;
            count++;
        }

        Serial.printf("bus_stops: fetched page skip=%d, running total %d\n", skip, count);
        skip += 500;
    }

    if (count == 0)
    {
        free(buf);
        return false;
    }

    stops = buf;
    stopCount = count;
    return true;
}

void busStopsInit()
{
    if (!SPIFFS.begin(true))
    {
        Serial.println("bus_stops: SPIFFS mount failed");
        needsFetch = true;
        return;
    }

    if (loadFromCache())
    {
        ready = true;
        return;
    }

    Serial.println("bus_stops: no valid cache, will fetch from API once Wi-Fi connects");
    needsFetch = true;
}

// The actual Network Worker job (network_worker.h). Real, separate bug
// found and fixed while wiring this file into the worker refactor:
// busStopsTick() used to call fetchFromApi() DIRECTLY, inline, on whatever
// thread calls it - which is main.cpp's loop(), the same thread driving
// LVGL rendering and touch polling. A multi-page dataset fetch (up to
// MAX_STOPS/500 = 12 sequential HTTP requests) was blocking the entire UI
// for its whole duration, violating this app's own "no blocking HTTP on
// the UI thread" rule that every OTHER fetch in this codebase already
// followed. Moving it onto the Network Worker (queued at BUS priority,
// since it's part of Bus tab setup) fixes that for free as part of this
// refactor.
static void busStopsFetchJob()
{
    Serial.println("bus_stops: fetching full dataset from LTA (one-time, may take a while)...");
    if (fetchFromApi())
    {
        saveToCache();
        ready = true;
        Serial.printf("bus_stops: ready, %d stops\n", stopCount);
    }
    else
    {
        Serial.println("bus_stops: fetch failed, stop names unavailable until next reboot");
    }
}

void busStopsTick()
{
    if (ready || !needsFetch || fetchAttempted)
        return;
    // wifiSettled() (net_lock.h), not a bare WiFi.status() check - see its
    // comment. This fetch is only ever attempted once, so it's worth
    // waiting the extra ~3s to not burn that one attempt on a fetch that's
    // likely to fail right at the moment Wi-Fi just connected.
    if (!wifiSettled())
        return;

    fetchAttempted = true;
    // isPeriodic=true: this only ever runs once per boot anyway
    // (fetchAttempted latches immediately above), so duplicate-prevention
    // is a no-op in practice, but marking it periodic is the more honest
    // classification (it's not a user-initiated one-off like ASK).
    networkWorkerSubmit(NET_JOB_BUS, "BUS(stops)", busStopsFetchJob, true);
}

bool busStopsReady()
{
    return ready;
}

const char *busStopsLookupName(const char *code)
{
    if (!ready || !code)
        return "";
    for (int i = 0; i < stopCount; i++)
    {
        if (strcmp(stops[i].code, code) == 0)
            return stops[i].name;
    }
    return "";
}
