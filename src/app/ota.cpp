#include "ota.h"
#include "firmware_version.h"
#include "net_lock.h"
#include "network_worker.h"
#include "secrets_select.h"
#include "app_config.h"
#include "diag.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_heap_caps.h>
#include <stdio.h>

#define OTA_REPO "zainulaabdin/ESP32PersonalDashboard"
#define OTA_ASSET_NAME "firmware.bin"
#define OTA_FIRST_CHECK_DELAY_MS (5UL * 60UL * 1000UL) // after Wi-Fi settles - boot-time bus/calendar fetches are heavy (calendar alone can take 30s+) and a check during them failed to connect
#define OTA_CHECK_INTERVAL_MS (6UL * 3600UL * 1000UL)

static volatile bool checkQueued = false;
static volatile bool updating = false;
static volatile int progressPct = 0;
static volatile OtaCheckState checkState = OTA_IDLE;
static volatile unsigned checkCount = 0;
static unsigned long lastCheckMs = 0;
static bool firstCheckDone = false;
static unsigned long wifiSettledSinceMs = 0;
// Set by otaCheckOnly() (web page): report a newer release instead of
// installing it.
static volatile bool checkOnly = false;
static char latestTag[32] = "";

// "v1.2.3" / "1.2.3" -> comparable number; missing parts count as 0.
static uint32_t parseVersion(const char *v)
{
    if (*v == 'v' || *v == 'V')
        v++;
    unsigned a = 0, b = 0, c = 0;
    sscanf(v, "%u.%u.%u", &a, &b, &c);
    return (a << 20) | (b << 10) | c;
}

static void addGithubHeaders(HTTPClient &http, const char *accept)
{
    if (cfgGithubToken()[0]) // only needed while the repo is private
        http.addHeader("Authorization", String("Bearer ") + cfgGithubToken());
    http.addHeader("Accept", accept);
    http.addHeader("X-GitHub-Api-Version", "2022-11-28");
    http.setUserAgent("ESP32PersonalDashboard-OTA");
}

// Latest release's tag and firmware.bin asset id. false on any failure.
static bool fetchLatestRelease(char *tagOut, size_t tagSize, uint32_t *assetIdOut)
{
    WiFiClientSecure client;
    client.setInsecure(); // same convention as every other HTTPS call in this project
    HTTPClient http;
    http.useHTTP10(true); // plain (non-chunked) body, parsed straight off the stream
    http.setTimeout(15000); // stream parse hit the 5s default mid-body ("IncompleteInput")
    if (!http.begin(client, "https://api.github.com/repos/" OTA_REPO "/releases/latest"))
        return false;
    addGithubHeaders(http, "application/vnd.github+json");
    int code = http.GET();
    if (code != 200)
    {
        Serial.printf("ota: releases/latest returned %d%s\n", code,
                      code == 404 ? " (no release yet, or token can't see the repo)" : "");
        http.end();
        return false;
    }

    JsonDocument filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["id"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err)
    {
        Serial.printf("ota: release JSON parse error: %s\n", err.c_str());
        return false;
    }

    snprintf(tagOut, tagSize, "%s", (const char *)(doc["tag_name"] | ""));
    *assetIdOut = 0;
    for (JsonObject asset : doc["assets"].as<JsonArray>())
    {
        if (strcmp(asset["name"] | "", OTA_ASSET_NAME) == 0)
            *assetIdOut = asset["id"] | 0;
    }
    return tagOut[0] != '\0';
}

// Private release assets: the API answers with a 302 to a short-lived
// signed download URL. The redirect is followed by hand so the GitHub token
// isn't forwarded to the storage host (which rejects extra auth anyway).
static bool resolveAssetUrl(uint32_t assetId, String &urlOut)
{
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    char url[128];
    snprintf(url, sizeof(url), "https://api.github.com/repos/" OTA_REPO "/releases/assets/%u", (unsigned)assetId);
    if (!http.begin(client, url))
        return false;
    const char *keep[] = {"Location"};
    http.collectHeaders(keep, 1);
    addGithubHeaders(http, "application/octet-stream");
    int code = http.GET();
    urlOut = http.header("Location");
    http.end();
    if ((code != 302 && code != 301) || urlOut.isEmpty())
    {
        Serial.printf("ota: asset request returned %d, no redirect\n", code);
        return false;
    }
    return true;
}

static bool downloadAndFlash(const String &url)
{
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(20000);
    if (!http.begin(client, url))
        return false;
    int code = http.GET();
    if (code != 200)
    {
        Serial.printf("ota: download returned %d\n", code);
        http.end();
        return false;
    }
    int total = http.getSize();
    if (total <= 0 || !Update.begin(total))
    {
        Serial.printf("ota: can't start update (size %d): %s\n", total, Update.errorString());
        http.end();
        return false;
    }

    updating = true;
    progressPct = 0;
    WiFiClient *stream = http.getStreamPtr();
    // PSRAM, and only for the download - a static 4KB array sat in internal
    // RAM permanently, which TLS/lwIP need far more.
    const size_t bufSize = 4096;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM);
    if (!buf)
    {
        Update.abort();
        http.end();
        return false;
    }
    int written = 0;
    unsigned long lastDataMs = millis();
    while (written < total && millis() - lastDataMs < 20000)
    {
        size_t avail = stream->available();
        if (!avail)
        {
            delay(2);
            continue;
        }
        int n = stream->readBytes(buf, avail < bufSize ? avail : bufSize);
        if (n <= 0)
            continue;
        if (Update.write(buf, n) != (size_t)n)
        {
            Serial.printf("ota: flash write failed: %s\n", Update.errorString());
            break;
        }
        written += n;
        lastDataMs = millis();
        progressPct = (int)((int64_t)written * 100 / total);
    }
    http.end();
    heap_caps_free(buf);

    if (written != total)
    {
        Serial.printf("ota: download incomplete (%d of %d bytes)\n", written, total);
        Update.abort();
        updating = false;
        return false;
    }
    if (!Update.end(true))
    {
        Serial.printf("ota: image rejected: %s\n", Update.errorString());
        updating = false;
        return false;
    }
    return true;
}

// Returns UP_TO_DATE, UPDATE_AVAILABLE (check-only) or CHECK_FAILED; a
// successful install restarts instead of returning.
static OtaCheckState otaCheck(bool installIfNewer)
{
    NetLockGuard netLock;
    if (!netLock.acquired())
        return OTA_CHECK_FAILED;
    char tag[32];
    uint32_t assetId = 0;
    if (!fetchLatestRelease(tag, sizeof(tag), &assetId))
        return OTA_CHECK_FAILED;
    Serial.printf("ota: running %s, latest release %s\n", FIRMWARE_VERSION, tag);
    strlcpy(latestTag, tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag, sizeof(latestTag));
    if (parseVersion(tag) <= parseVersion(FIRMWARE_VERSION))
        return OTA_UP_TO_DATE;
    if (!assetId)
    {
        Serial.printf("ota: release %s has no %s asset\n", tag, OTA_ASSET_NAME);
        return OTA_CHECK_FAILED;
    }
    if (!installIfNewer)
        return OTA_UPDATE_AVAILABLE;

    String url;
    if (!resolveAssetUrl(assetId, url))
        return OTA_CHECK_FAILED;
    Serial.printf("ota: installing %s...\n", tag);
    if (downloadAndFlash(url))
    {
        Serial.printf("ota: %s installed, restarting\n", tag);
        delay(500);
        ESP.restart();
    }
    return OTA_CHECK_FAILED;
}

static void otaJob()
{
    checkQueued = false;
    bool install = !checkOnly;
    checkOnly = false;
    checkState = OTA_CHECKING;
    checkState = otaCheck(install);
    diagReport(DIAG_OTA, checkState != OTA_CHECK_FAILED,
               checkState == OTA_UP_TO_DATE ? "up to date" : checkState == OTA_UPDATE_AVAILABLE ? "update available" : "check failed");
    checkCount++;
}

void otaCheckNow()
{
    if (checkQueued || updating)
        return;
    checkQueued = true;
    lastCheckMs = millis();
    networkWorkerSubmit(NET_JOB_OTA, "OTA", otaJob, true);
}

void otaCheckOnly()
{
    if (checkQueued || updating)
        return;
    checkOnly = true;
    otaCheckNow();
}

const char *otaLatestVersion()
{
    return latestTag;
}

void otaTick()
{
    if (!wifiSettled())
    {
        wifiSettledSinceMs = 0;
        return;
    }
    unsigned long now = millis();
    if (wifiSettledSinceMs == 0)
        wifiSettledSinceMs = now;

    if (!firstCheckDone)
    {
        if (now - wifiSettledSinceMs >= OTA_FIRST_CHECK_DELAY_MS)
        {
            firstCheckDone = true;
            otaCheckNow();
        }
        return;
    }
    if (now - lastCheckMs >= OTA_CHECK_INTERVAL_MS)
        otaCheckNow();
}

bool otaInProgress()
{
    return updating;
}

int otaProgressPct()
{
    return progressPct;
}

OtaCheckState otaCheckState()
{
    return checkState;
}

unsigned otaCheckCount()
{
    return checkCount;
}
