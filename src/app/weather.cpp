// Full-screen weather overlay, built to an explicit user-specified layout
// (12 numbered points, not freely designed): a fixed background photo, a
// semi-transparent bottom strip holding the 5-day forecast, a big icon top
// -left with wind/haze stats below it, and a clock/date/temp/high-low/
// sunrise-sunset block top-right. Text uses the same Montserrat sizes
// profile.cpp already uses (18 for emphasis, the project's LV_FONT_DEFAULT
// 14 for body text) rather than the much larger fonts an earlier attempt at
// this screen used.
//
// Data: Singapore's own NEA/data.gov.sg APIs (api-open.data.gov.sg) - free,
// no API key, more locally accurate than a global provider. Four calls per
// refresh, same NetLockGuard + Network Worker pattern as every other fetch
// in this codebase (today.cpp's fetchAgendaWork() is the closest template):
//   - air-temperature: real live "right now" reading from NEA's realtime
//     station network (nearest to central Singapore), used as the big
//     current-temperature number - the forecast endpoints below only give
//     a whole-day high/low, not a live reading.
//   - twenty-four-hr-forecast: today's real high/low/wind/condition (its
//     "general" block covers the current 24h window) - used for
//     forecastDays[0] ("Today" in the strip).
//   - four-day-outlook: the 4 days AFTER today (confirmed against the live
//     API - its forecasts[] array starts at tomorrow, not today), filling
//     forecastDays[1..4] for a genuine 5-day strip.
//   - psi: real regional PSI (haze) reading, shown as the haze stat.
// Sunrise/sunset: NEA has no API for this (confirmed - not in any of its
// real-time or forecast datasets), so it's computed locally via the
// standard NOAA solar-position formula for Singapore's fixed coordinates -
// accurate to within a minute or two, no network call needed, and
// Singapore's near-equatorial location means it barely varies year-round
// anyway (verified against real published Singapore sunrise/sunset times
// for today's date before relying on it).
#include "weather.h"
#include "diag.h"
#include "status_bar.h"
#include "net_lock.h"
#include "network_worker.h"
#include "power.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <math.h>
#include "icons/weather_bg_day.h"
#include "icons/weather_bg_night.h"
#include "night_mode.h"
#include "icons/weather_icon_uv.h"
#include <Preferences.h>
#include "icons/weather_icon_sunny.h"
#include "icons/weather_icon_cloudy.h"
#include "icons/weather_icon_partly_cloudy.h"
#include "icons/weather_icon_overcast.h"
#include "icons/weather_icon_hazy.h"
#include "icons/weather_icon_rain.h"
#include "icons/weather_icon_thunderstorm.h"
#include "icons/weather_icon_mist.h"
#include "icons/weather_icon_windy.h"
#include "icons/weather_icon_small_sunny.h"
#include "icons/weather_icon_small_cloudy.h"
#include "icons/weather_icon_small_partly_cloudy.h"
#include "icons/weather_icon_small_overcast.h"
#include "icons/weather_icon_small_hazy.h"
#include "icons/weather_icon_small_rain.h"
#include "icons/weather_icon_small_thunderstorm.h"
#include "icons/weather_icon_small_mist.h"
#include "icons/weather_icon_small_windy.h"
#include "icons/weather_icon_wind_speed.h"
#include "icons/weather_icon_humidity.h"
#include "icons/weather_icon_foggy_haze.h"
#include "icons/weather_icon_sunrise.h"
#include "icons/weather_icon_sunset.h"
#include "icons/weather_icon_thermostat_high.h"
#include "icons/weather_icon_thermostat_low.h"
#include "icons/wifi_connected_icon.h"
#include "icons/wifi_disconnected_icon.h"

// Digital-style fonts (font_digital_clock_56.c / font_digital_temp_96.c),
// generated via lv_font_conv --no-compress (compressed bitmaps rendered as
// blank glyphs on-device - see git history) from WDXL Lubrifont TC (clock,
// 56px, glyphs 0-9/:/deg) and Rubik Regular, not Bold - less heavy per
// explicit request (temp digits, 96px, glyphs -/0-9/:). Temp's degree sign
// is NOT in this font - it's rendered as a separate, smaller Montserrat
// label superscripted over the digit label (see tempLabel/tempDegLabel
// below), per explicit request for a smaller-than-digits degree mark. No
// header exists for these, just the extern decl needed here.
extern "C" const lv_font_t lv_font_digital_clock_66;
extern "C" const lv_font_t lv_font_digital_temp_96;

#define WEATHER_TINT 0x1976d2

// 5 minutes of no touch anywhere - separate from power.cpp's own sleep
// inactivity timer (a different concern: this reshows the weather screen,
// it doesn't sleep the board) but the same "reset on any touch" shape.
#define AUTO_RESHOW_MS (5UL * 60UL * 1000UL)
// Refresh interval (minutes) and area are set on the web dashboard - see
// weatherSetArea()/weatherSetIntervalMin().
static int fetchIntervalMin = 10;
#define FETCH_INTERVAL_MS ((unsigned long)fetchIntervalMin * 60UL * 1000UL)

// NEA's 2-hour forecast areas (api-open.data.gov.sg two-hr-forecast
// area_metadata). The chosen one drives the rain line on the top row and
// which station's live temperature is shown (nearest to the area).
static const char *const AREAS[] = {
    "Ang Mo Kio", "Bedok", "Bishan", "Boon Lay", "Bukit Batok", "Bukit Merah", "Bukit Panjang", "Bukit Timah",
    "Central Water Catchment", "Changi", "Choa Chu Kang", "City", "Clementi", "Geylang", "Hougang", "Jalan Bahar",
    "Jurong East", "Jurong Island", "Jurong West", "Kallang", "Lim Chu Kang", "Mandai", "Marine Parade", "Novena",
    "Pasir Ris", "Paya Lebar", "Pioneer", "Pulau Tekong", "Pulau Ubin", "Punggol", "Queenstown", "Seletar",
    "Sembawang", "Sengkang", "Sentosa", "Serangoon", "Southern Islands", "Sungei Kadut", "Tampines", "Tanglin",
    "Tengah", "Toa Payoh", "Tuas", "Western Islands", "Western Water Catchment", "Woodlands", "Yishun"};
static char areaName[32] = "Bishan";
static char areaForecast[40] = "";   // 2-hour forecast for areaName, e.g. "Thundery Showers"
static volatile bool rainSoon = false; // areaForecast mentions rain/showers/thunder
static int uvIndex = -1;             // latest hourly UV index, -1 = not loaded
static bool showUvOnStrip = false;   // PSI/UV strip alternates every 30 s
static lv_obj_t *areaLabel = nullptr;

#define MAX_FORECAST_DAYS 5

struct DayForecast
{
    char dayLabel[4]; // "Mon", "Tue", ...
    int8_t high, low;
    char conditionCode[3];
};

static char currentConditionText[32] = "";
static char currentConditionCode[3] = "";
static int liveTempC = -128; // -128 = not yet loaded (real Singapore temps never get remotely close)
static int8_t todayHigh = 0, todayLow = 0;
static int windLow = 0, windHigh = 0;
static int humidityLow = 0, humidityHigh = 0;
static int psiCentral = -1; // -1 = not yet loaded
static char sunriseStr[6] = "--:--";
static char sunsetStr[6] = "--:--";
static int sunriseMinuteOfDay = 6 * 60;  // defaults used only until the first computeSunriseSunset() call lands
static int sunsetMinuteOfDay = 19 * 60;
static DayForecast forecastDays[MAX_FORECAST_DAYS];
static int forecastDayCount = 0;
static char weatherLastError[64] = "";
static bool weatherEverSucceeded = false;

static volatile bool fetchInProgress = false;
static volatile bool fetchJustCompleted = false;
static unsigned long lastFetch = 0;
static unsigned long fetchStartedAtMs = 0;

static lv_obj_t *overlay = nullptr;
static lv_obj_t *bg = nullptr;
static lv_obj_t *bigClockLabel = nullptr;
static lv_obj_t *dateLabel = nullptr;
static lv_obj_t *networkStatusIcon = nullptr;
static lv_obj_t *conditionIcon = nullptr;
static lv_obj_t *conditionTextLabel = nullptr;
static lv_obj_t *windIconObj = nullptr;
static lv_obj_t *windLabel = nullptr;
static lv_obj_t *humidityIconObj = nullptr;
static lv_obj_t *humidityLabel = nullptr;
static lv_obj_t *hazeIconObj = nullptr;
static lv_obj_t *hazeLabel = nullptr;
static lv_obj_t *tempLabel = nullptr;
static lv_obj_t *tempDegLabel = nullptr;
static lv_obj_t *highLabel = nullptr;
static lv_obj_t *lowLabel = nullptr;
static lv_obj_t *sunriseLabel = nullptr;
static lv_obj_t *sunsetLabel = nullptr;
static lv_obj_t *errorLabel = nullptr;
static lv_obj_t *forecastRow = nullptr;
static lv_obj_t *leftCol = nullptr;
static lv_obj_t *rightCol = nullptr;
static lv_obj_t *bootingLabel = nullptr;
static lv_obj_t *sunRowDiag = nullptr; // temporary - see its use below

static unsigned long lastTouchMs = 0;
static bool showing = false;

// NEA's documented condition codes (Meteorological Service Singapore's own
// icon set - FA/FN/FW/PC/PN/CL/OC/HZ/SH/TL/HG/LR/MR/HR/WD/WC/WR/WS/MG/FG),
// mapped to the colorful icon set (Meteocons, MIT-licensed) rasterized for
// this project. Falls back to "cloudy" for anything unrecognized.
static const lv_img_dsc_t *iconForCode(const char *code, bool small)
{
    if (!strcmp(code, "FA") || !strcmp(code, "FN") || !strcmp(code, "FW"))
        return small ? &weather_icon_small_sunny : &weather_icon_sunny;
    if (!strcmp(code, "PC") || !strcmp(code, "PN"))
        return small ? &weather_icon_small_partly_cloudy : &weather_icon_partly_cloudy;
    if (!strcmp(code, "CL"))
        return small ? &weather_icon_small_cloudy : &weather_icon_cloudy;
    if (!strcmp(code, "OC"))
        return small ? &weather_icon_small_overcast : &weather_icon_overcast;
    if (!strcmp(code, "HZ"))
        return small ? &weather_icon_small_hazy : &weather_icon_hazy;
    if (!strcmp(code, "MG") || !strcmp(code, "FG"))
        return small ? &weather_icon_small_mist : &weather_icon_mist;
    if (!strcmp(code, "TL") || !strcmp(code, "HG"))
        return small ? &weather_icon_small_thunderstorm : &weather_icon_thunderstorm;
    if (!strcmp(code, "SH") || !strcmp(code, "LR") || !strcmp(code, "MR") || !strcmp(code, "RA") || !strcmp(code, "HR"))
        return small ? &weather_icon_small_rain : &weather_icon_rain;
    if (!strcmp(code, "WD") || !strcmp(code, "WC") || !strcmp(code, "WR") || !strcmp(code, "WS"))
        return small ? &weather_icon_small_windy : &weather_icon_windy;
    return small ? &weather_icon_small_cloudy : &weather_icon_cloudy;
}

// Standard NOAA solar-position formula, Singapore's fixed coordinates
// (1.3521N, 103.8198E - this project's existing fixed-location convention,
// see CLAUDE.md). No network call - Singapore's near-equatorial latitude
// means sunrise/sunset barely shifts year-round, and this was checked
// against real published Singapore sunrise/sunset times for today's date
// before relying on it (came out within a minute).
static void computeSunriseSunset(char *outSunrise, size_t sunriseSize, char *outSunset, size_t sunsetSize)
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0))
        return;
    const double lat = 1.3521, lon = 103.8198, utcOffsetHours = 8.0;
    double latR = lat * M_PI / 180.0;
    int dayOfYear = timeinfo.tm_yday + 1;

    for (int pass = 0; pass < 2; pass++)
    {
        bool isSunrise = pass == 0;
        double lngHour = lon / 15.0;
        double t = dayOfYear + ((isSunrise ? 6.0 : 18.0) - lngHour) / 24.0;
        double M = (0.9856 * t) - 3.289;
        double Mr = M * M_PI / 180.0;
        double L = M + (1.916 * sin(Mr)) + (0.020 * sin(2 * Mr)) + 282.634;
        L = fmod(L, 360.0);
        if (L < 0)
            L += 360.0;
        double Lr = L * M_PI / 180.0;
        double RA = atan(0.91764 * tan(Lr)) * 180.0 / M_PI;
        RA = fmod(RA, 360.0);
        if (RA < 0)
            RA += 360.0;
        double Lquadrant = floor(L / 90.0) * 90.0;
        double RAquadrant = floor(RA / 90.0) * 90.0;
        RA = RA + (Lquadrant - RAquadrant);
        RA = RA / 15.0;
        double sinDec = 0.39782 * sin(Lr);
        double cosDec = cos(asin(sinDec));
        double cosH = (cos(90.833 * M_PI / 180.0) - (sinDec * sin(latR))) / (cosDec * cos(latR));
        if (cosH > 1.0 || cosH < -1.0)
            continue; // no sunrise/sunset this day at this latitude - never happens in Singapore, but don't crash if it did
        double H = isSunrise ? 360.0 - (acos(cosH) * 180.0 / M_PI) : (acos(cosH) * 180.0 / M_PI);
        H = H / 15.0;
        double T = H + RA - (0.06571 * t) - 6.622;
        double UT = fmod(T - lngHour, 24.0);
        if (UT < 0)
            UT += 24.0;
        double local = fmod(UT + utcOffsetHours, 24.0);
        if (local < 0)
            local += 24.0;
        int hh = (int)local;
        int mm = (int)((local - hh) * 60.0);
        if (isSunrise)
        {
            snprintf(outSunrise, sunriseSize, "%02d:%02d", hh, mm);
            sunriseMinuteOfDay = hh * 60 + mm;
        }
        else
        {
            snprintf(outSunset, sunsetSize, "%02d:%02d", hh, mm);
            sunsetMinuteOfDay = hh * 60 + mm;
        }
    }
}

static void fetchWeatherWork()
{
    NetLockGuard netLock;
    if (!netLock.acquired())
    {
        snprintf(weatherLastError, sizeof(weatherLastError), "Busy, try again");
        Serial.println("weather: could not acquire network lock in time");
        return;
    }

    Serial.printf("weather: free internal heap before fetch: %u bytes, largest free block: %u bytes\n",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(8);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(8000);

    // ---- 2-hour forecast for the chosen area (also its coordinates, used
    // below to pick the nearest temperature station).
    char area[32];
    strlcpy(area, areaName, sizeof(area));
    double areaLat = 1.3521, areaLon = 103.8198; // central Singapore until found
    if (http.begin(client, "https://api-open.data.gov.sg/v2/real-time/api/two-hr-forecast"))
    {
        int twoHrCode = http.GET();
        Serial.printf("weather: two-hr-forecast GET returned %d\n", twoHrCode);
        if (twoHrCode == 200)
        {
            String twoHrBody = http.getString();
            addNetworkBytes((uint32_t)twoHrBody.length());
            JsonDocument doc;
            if (!deserializeJson(doc, twoHrBody))
            {
                for (JsonObject a : doc["data"]["area_metadata"].as<JsonArray>())
                    if (strcmp(a["name"] | "", area) == 0)
                    {
                        areaLat = a["label_location"]["latitude"] | areaLat;
                        areaLon = a["label_location"]["longitude"] | areaLon;
                    }
                for (JsonObject f : doc["data"]["items"][0]["forecasts"].as<JsonArray>())
                    if (strcmp(f["area"] | "", area) == 0)
                    {
                        strlcpy(areaForecast, f["forecast"] | "", sizeof(areaForecast));
                        rainSoon = strstr(areaForecast, "Rain") || strstr(areaForecast, "Shower") || strstr(areaForecast, "Thunder");
                    }
            }
        }
        http.end();
    }

    // ---- Live air temperature: real "right now" reading, nearest station
    // to central Singapore (matching this project's existing fixed-location
    // convention - see CLAUDE.md). The forecast endpoints below only give a
    // whole-day high/low, not a live number.
    if (!http.begin(client, "https://api-open.data.gov.sg/v2/real-time/api/air-temperature"))
    {
        snprintf(weatherLastError, sizeof(weatherLastError), "begin() failed");
        Serial.println("weather: air-temperature http.begin() failed");
        return;
    }
    int code = http.GET();
    Serial.printf("weather: air-temperature GET returned %d\n", code);
    if (code != 200)
    {
        snprintf(weatherLastError, sizeof(weatherLastError), "HTTP %d", code);
        http.end();
        return;
    }
    String body = http.getString();
    http.end();
    addNetworkBytes((uint32_t)body.length());
    {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err)
        {
            snprintf(weatherLastError, sizeof(weatherLastError), "parse error");
            Serial.printf("weather: air-temperature JSON parse failed: %s\n", err.c_str());
            return;
        }
        // "S109 Ang Mo Kio Avenue 5" and similar - pick the reading whose
        // station is geographically closest to central Singapore (1.3521N,
        // 103.8198E, this project's existing fixed-location convention)
        // rather than just taking the first one, since the array order
        // isn't documented as being distance-sorted.
        JsonArray stations = doc["data"]["stations"].as<JsonArray>();
        JsonArray readings = doc["data"]["readings"].as<JsonArray>();
        if (stations.size() == 0 || readings.size() == 0)
        {
            snprintf(weatherLastError, sizeof(weatherLastError), "no temp data");
            return;
        }
        JsonArray latestReadings = readings[readings.size() - 1]["data"].as<JsonArray>();
        const double centralLat = areaLat, centralLon = areaLon; // the chosen area's location
        double bestDist = 1e18;
        double bestValue = -128;
        for (JsonObject st : stations)
        {
            const char *stationId = st["id"] | "";
            double lat = st["location"]["latitude"] | 0.0;
            double lon = st["location"]["longitude"] | 0.0;
            double dist = (lat - centralLat) * (lat - centralLat) + (lon - centralLon) * (lon - centralLon);
            for (JsonObject r : latestReadings)
            {
                const char *rid = r["stationId"] | "";
                if (strcmp(rid, stationId) == 0)
                {
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        bestValue = r["value"] | -128.0;
                    }
                    break;
                }
            }
        }
        if (bestValue > -100)
            liveTempC = (int)lround(bestValue);
    }

    // ---- 24hr forecast: today's real high/low/wind/condition. Real bug
    // fixed here: four-day-outlook's own "forecasts" array is genuinely
    // 4 DAYS AHEAD (confirmed against the live API - its first entry is
    // tomorrow, not today), so treating forecasts[0] as "today" (the
    // earlier version of this code) was quietly showing tomorrow's numbers
    // labeled "Today" and only ever displaying 4 total days in the strip
    // no matter what. twenty-four-hr-forecast's own "general" block is the
    // only endpoint that actually covers the current 24h window, so that's
    // the real source for forecastDays[0]/todayHigh/todayLow/windLow/
    // windHigh now - four-day-outlook is used only for days 1-4 below.
    if (!http.begin(client, "https://api-open.data.gov.sg/v2/real-time/api/twenty-four-hr-forecast"))
    {
        Serial.println("weather: twenty-four-hr-forecast http.begin() failed - keeping live temp, skipping today's forecast");
    }
    else
    {
        code = http.GET();
        Serial.printf("weather: twenty-four-hr-forecast GET returned %d\n", code);
        if (code == 200)
        {
            body = http.getString();
            addNetworkBytes((uint32_t)body.length());
            JsonDocument doc;
            if (!deserializeJson(doc, body))
            {
                JsonArray records = doc["data"]["records"].as<JsonArray>();
                if (records.size() > 0)
                {
                    JsonObject general = records[0]["general"];
                    const char *text = general["forecast"]["text"] | "";
                    const char *fcode = general["forecast"]["code"] | "CL";
                    snprintf(currentConditionText, sizeof(currentConditionText), "%s", text);
                    snprintf(currentConditionCode, sizeof(currentConditionCode), "%s", fcode);
                    todayHigh = (int8_t)(general["temperature"]["high"] | 0);
                    todayLow = (int8_t)(general["temperature"]["low"] | 0);
                    windLow = general["wind"]["speed"]["low"] | 0;
                    windHigh = general["wind"]["speed"]["high"] | 0;
                    humidityLow = general["relativeHumidity"]["low"] | 0;
                    humidityHigh = general["relativeHumidity"]["high"] | 0;

                    DayForecast &today = forecastDays[0];
                    snprintf(today.dayLabel, sizeof(today.dayLabel), "Tod");
                    today.high = todayHigh;
                    today.low = todayLow;
                    snprintf(today.conditionCode, sizeof(today.conditionCode), "%s", fcode);
                    forecastDayCount = 1;
                }
            }
        }
        http.end();
    }

    // ---- 4-day outlook: the 4 days AFTER today, filling out the rest of
    // the 5-day strip (today, from the block above, is forecastDays[0]).
    if (!http.begin(client, "https://api-open.data.gov.sg/v2/real-time/api/four-day-outlook"))
    {
        Serial.println("weather: four-day-outlook http.begin() failed - keeping today's forecast, skipping the rest of the strip");
    }
    else
    {
        code = http.GET();
        Serial.printf("weather: four-day-outlook GET returned %d\n", code);
        if (code == 200)
        {
            body = http.getString();
            addNetworkBytes((uint32_t)body.length());
            JsonDocument doc;
            if (!deserializeJson(doc, body))
            {
                JsonArray records = doc["data"]["records"].as<JsonArray>();
                if (records.size() > 0)
                {
                    JsonArray forecasts = records[0]["forecasts"].as<JsonArray>();
                    for (JsonObject f : forecasts)
                    {
                        if (forecastDayCount >= MAX_FORECAST_DAYS)
                            break;
                        DayForecast &d = forecastDays[forecastDayCount];
                        const char *day = f["day"] | "";
                        snprintf(d.dayLabel, sizeof(d.dayLabel), "%.3s", day); // "Wednesday" -> "Wed"
                        d.high = (int8_t)(f["temperature"]["high"] | 0);
                        d.low = (int8_t)(f["temperature"]["low"] | 0);
                        const char *fcode = f["forecast"]["code"] | "CL";
                        snprintf(d.conditionCode, sizeof(d.conditionCode), "%s", fcode);
                        forecastDayCount++;
                    }
                }
            }
        }
        http.end();
    }

    // ---- PSI: fetched for a future detail screen, not shown here (see file header) ----
    if (http.begin(client, "https://api.data.gov.sg/v1/environment/psi"))
    {
        code = http.GET();
        if (code == 200)
        {
            body = http.getString();
            addNetworkBytes((uint32_t)body.length());
            JsonDocument doc;
            if (!deserializeJson(doc, body))
            {
                JsonArray items = doc["items"].as<JsonArray>();
                if (items.size() > 0)
                    psiCentral = items[0]["readings"]["psi_twenty_four_hourly"]["central"] | -1;
            }
        }
        http.end();
    }

    // ---- UV index (island-wide, hourly 7am-7pm; index[0] is the latest hour) ----
    if (http.begin(client, "https://api-open.data.gov.sg/v2/real-time/api/uv"))
    {
        code = http.GET();
        if (code == 200)
        {
            body = http.getString();
            addNetworkBytes((uint32_t)body.length());
            JsonDocument doc;
            if (!deserializeJson(doc, body))
                uvIndex = doc["data"]["records"][0]["index"][0]["value"] | -1;
        }
        http.end();
    }

    computeSunriseSunset(sunriseStr, sizeof(sunriseStr), sunsetStr, sizeof(sunsetStr));

    weatherLastError[0] = '\0';
    Serial.printf("weather: area %s: %s (rain %s), UV %d\n", area, areaForecast, rainSoon ? "yes" : "no", uvIndex);
    Serial.printf("weather: fetch ok, temp=%dC condition=%s high=%d low=%d wind=%d-%d humidity=%d-%d psi=%d sunrise=%s sunset=%s, %d forecast days\n",
                  liveTempC, currentConditionText, todayHigh, todayLow, windLow, windHigh, humidityLow, humidityHigh, psiCentral, sunriseStr, sunsetStr, forecastDayCount);
}

static void fetchWeatherJob()
{
    fetchWeatherWork();
    fetchJustCompleted = true;
    fetchInProgress = false;
}

static void startFetch()
{
    if (fetchInProgress)
        return;
    fetchInProgress = true;
    fetchStartedAtMs = millis();
    setLoadingSpinnerColor(WEATHER_TINT);
    setLoadingVisible(true);
    networkWorkerSubmit(NET_JOB_WEATHER, "WEATHER", fetchWeatherJob, true);
}

// ---- rendering --------------------------------------------------------

static void renderForecastRow()
{
    lv_obj_clean(forecastRow);
    for (int i = 0; i < forecastDayCount; i++)
    {
        const DayForecast &d = forecastDays[i];
        lv_obj_t *cell = lv_obj_create(forecastRow);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 84, LV_SIZE_CONTENT);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 3, 0);
        lv_obj_set_style_translate_y(cell, -3, 0); // cancels forecastRow's own +3px-down move (see forecastRow's own align() comment) so the visible forecast content stays in the same place

        // Day / H-L / icon, top to bottom. Text rows nudged down 8px
        // (per explicit request - the icon stays where it was).
        lv_obj_t *dayLbl = lv_label_create(cell);
        lv_label_set_text(dayLbl, i == 0 ? "Today" : d.dayLabel);
        lv_obj_set_style_text_color(dayLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(dayLbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_pad_top(dayLbl, 8, 0);

        lv_obj_t *hiLo = lv_label_create(cell);
        lv_label_set_text_fmt(hiLo, "%d\xC2\xB0/%d\xC2\xB0", d.high, d.low);
        lv_obj_set_style_text_color(hiLo, lv_color_hex(0xd8e0f0), 0);
        lv_obj_set_style_text_font(hiLo, &lv_font_montserrat_16, 0);

        lv_obj_t *icon = lv_img_create(cell);
        lv_img_set_src(icon, iconForCode(d.conditionCode, true));
    }
}

static void renderWeather()
{
    if (weatherLastError[0])
    {
        lv_label_set_text(errorLabel, weatherLastError);
        lv_obj_clear_flag(errorLabel, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(errorLabel, LV_OBJ_FLAG_HIDDEN);
    }

    // Nothing but the background shows until the first real NEA fetch
    // succeeds, per explicit request - placeholder text/icons/strips looked
    // "very odd" before real data existed, and their layout (sized/aligned
    // around real content) was also wrong with nothing real in it yet.
    // conditionIcon/leftCol/rightCol/forecastRow/tempDegLabel cover every
    // piece of content this overlay draws other than bg/networkStatusIcon
    // (networkStatusIcon stays visible always - see updateNetworkStatus())
    // and bootingLabel (shown instead while this is hidden - see below).
    if (!weatherEverSucceeded)
    {
        lv_obj_add_flag(conditionIcon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(leftCol, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(rightCol, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(forecastRow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(tempDegLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bootingLabel, LV_OBJ_FLAG_HIDDEN);
        return; // nothing real to show yet - error label (or blank) covers it
    }
    lv_obj_clear_flag(conditionIcon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(leftCol, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(tempDegLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bootingLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(rightCol, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(forecastRow, LV_OBJ_FLAG_HIDDEN);

    lv_img_set_src(conditionIcon, iconForCode(currentConditionCode[0] ? currentConditionCode : "CL", false));
    // The area's 2-hour forecast is more current than the 24h one - prefer it.
    const char *conditionText = areaForecast[0] ? areaForecast : currentConditionText;
    lv_label_set_text(conditionTextLabel, conditionText[0] ? conditionText : "--");
    if (liveTempC > -100)
        lv_label_set_text_fmt(tempLabel, "%d", liveTempC);
    else
        lv_label_set_text(tempLabel, "--");
    // tempDegLabel is a separate, smaller Montserrat label (the 96px digit
    // font has no degree glyph - only digits/-/: were converted, see its
    // extern decl comment) - re-positioned here since tempLabel's own width
    // changes with digit count (e.g. "9" vs "24"). Real bug fixed: a fixed
    // -8px x-offset was tuned against a specific digit count/width and
    // pulled the degree mark IN over tempLabel's own last digit for
    // narrower text (e.g. single-digit "9"), causing the reported overlap -
    // 0 offset sits it flush against tempLabel's real (just-measured) right
    // edge for any digit count instead.
    lv_obj_update_layout(tempLabel);
    lv_obj_align_to(tempDegLabel, tempLabel, LV_ALIGN_OUT_RIGHT_TOP, 0, -6);
    lv_label_set_text_fmt(highLabel, "%d\xC2\xB0", todayHigh);
    lv_label_set_text_fmt(lowLabel, "%d\xC2\xB0", todayLow);

    // Sunrise/sunset and thermometer H/L text match their own icon's fixed
    // color (per explicit request "keep icon native color and text will be
    // as icon color") rather than plain white - these 4 source SVGs are
    // each a single flat color (amber sunrise/sunset, red/blue thermometer,
    // confirmed by inspecting the icon PNGs), so the color is hardcoded to
    // match rather than sampled from the image at runtime.
    lv_label_set_text(sunriseLabel, sunriseStr);
    lv_obj_set_style_text_color(sunriseLabel, lv_color_hex(0xF84A06), 0); // per explicit request
    lv_label_set_text(sunsetLabel, sunsetStr);
    lv_obj_set_style_text_color(sunsetLabel, lv_color_hex(0xF9C50D), 0); // per explicit request
    lv_label_set_text_fmt(highLabel, "%d\xC2\xB0", todayHigh);
    lv_obj_set_style_text_color(highLabel, lv_color_hex(0xef5350), 0); // red, matches thermostat_high_24x24's own color
    lv_label_set_text_fmt(lowLabel, "%d\xC2\xB0", todayLow);
    lv_obj_set_style_text_color(lowLabel, lv_color_hex(0x42a5f5), 0); // blue, matches thermostat_low_24x24's own color

    // Wind/haze: icon + text colored green/orange/red by how good/bad the
    // reading actually is (per explicit request), not a fixed color.
    lv_label_set_text_fmt(windLabel, "%d-%d km/h", windLow, windHigh);
    uint32_t windColor = windHigh <= 20 ? 0x4caf50 : windHigh <= 40 ? 0xff9800
                                                                     : 0xf44336;
    lv_obj_set_style_text_color(windLabel, lv_color_hex(windColor), 0);
    lv_obj_set_style_img_recolor(windIconObj, lv_color_hex(windColor), 0);
    lv_obj_set_style_img_recolor_opa(windIconObj, LV_OPA_COVER, 0);
    lv_label_set_text_fmt(humidityLabel, "%d-%d%%", humidityLow, humidityHigh);

    // UV only between sunrise and sunset (NEA publishes it 7am-7pm); PSI otherwise.
    struct tm nowTm;
    bool daylight = false;
    if (getLocalTime(&nowTm, 0))
    {
        int minute = nowTm.tm_hour * 60 + nowTm.tm_min;
        daylight = minute >= sunriseMinuteOfDay && minute < sunsetMinuteOfDay;
    }
    if (showUvOnStrip && uvIndex >= 0 && daylight)
    {
        // NEA/WHO UV bands: 0-2 Low, 3-5 Moderate, 6-7 High, 8-10 Very High, 11+ Extreme.
        const char *band = uvIndex <= 2 ? "Low" : uvIndex <= 5 ? "Moderate" : uvIndex <= 7 ? "High"
                                                               : uvIndex <= 10 ? "Very High" : "Extreme";
        uint32_t uvColor = uvIndex <= 2 ? 0x4caf50 : uvIndex <= 5 ? 0xffc107 : uvIndex <= 7 ? 0xff9800
                                                                            : uvIndex <= 10 ? 0xf44336 : 0xab47bc;
        lv_img_set_src(hazeIconObj, &weather_icon_uv);
        lv_label_set_text_fmt(hazeLabel, "UV %d (%s)", uvIndex, band);
        lv_obj_set_style_text_color(hazeLabel, lv_color_hex(uvColor), 0);
        lv_obj_set_style_img_recolor_opa(hazeIconObj, LV_OPA_TRANSP, 0); // sun icon in its own colours
    }
    else if (psiCentral >= 0)
    {
        lv_img_set_src(hazeIconObj, &weather_icon_foggy_haze);
        // NEA's own PSI bands: 0-50 Good, 51-100 Moderate, 101-200
        // Unhealthy, 201-300 Very Unhealthy, 300+ Hazardous.
        const char *band = psiCentral <= 50 ? "Good" : psiCentral <= 100 ? "Moderate"
                                                     : psiCentral <= 200  ? "Unhealthy"
                                                     : psiCentral <= 300  ? "Very Unhealthy"
                                                                          : "Hazardous";
        lv_label_set_text_fmt(hazeLabel, "PSI %d (%s)", psiCentral, band);
        uint32_t hazeColor = psiCentral <= 50 ? 0x4caf50 : psiCentral <= 100 ? 0xff9800
                                                                              : 0xf44336;
        lv_obj_set_style_text_color(hazeLabel, lv_color_hex(hazeColor), 0);
        lv_obj_set_style_img_recolor(hazeIconObj, lv_color_hex(hazeColor), 0);
        lv_obj_set_style_img_recolor_opa(hazeIconObj, LV_OPA_COVER, 0);
    }
    else
    {
        lv_label_set_text(hazeLabel, "PSI --");
    }

    // Area name: left edge at the hero icon's right edge, top at the clock's top.
    lv_label_set_text(areaLabel, areaName);
    lv_obj_update_layout(overlay);
    lv_area_t heroArea, clockArea;
    lv_obj_get_coords(conditionIcon, &heroArea);
    lv_obj_get_coords(bigClockLabel, &clockArea);
    lv_obj_set_pos(areaLabel, heroArea.x2 + 1, clockArea.y1 + 15);
    // Never reaches the clock - long names wrap to a second line instead.
    lv_label_set_long_mode(areaLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(areaLabel, clockArea.x1 - (heroArea.x2 + 1) - 6);
    lv_obj_clear_flag(areaLabel, LV_OBJ_FLAG_HIDDEN);

    // Condition text: single line that must stop short of the temperature in
    // rightCol - if too long, cap its width and let it scroll horizontally.
    {
        const char *txt = lv_label_get_text(conditionTextLabel);
        lv_point_t natural;
        lv_txt_get_size(&natural, txt, &lv_font_montserrat_20, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_area_t condArea, tempArea;
        lv_obj_get_coords(conditionTextLabel, &condArea);
        lv_obj_get_coords(tempLabel, &tempArea);
        // 11px = conditionRow's pad_right, plus a 6px gap before the temperature.
        lv_coord_t maxW = tempArea.x1 - condArea.x1 - 11 - 6;
        if (maxW > 20 && natural.x > maxW)
        {
            lv_label_set_long_mode(conditionTextLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_set_style_anim_speed(conditionTextLabel, 25, 0); // px/s - slower than LVGL's default, per explicit request
            lv_obj_set_width(conditionTextLabel, maxW);
        }
        else
        {
            lv_label_set_long_mode(conditionTextLabel, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(conditionTextLabel, LV_SIZE_CONTENT);
        }
    }

    renderForecastRow();
}

// Swaps the fixed background photo between day/night versions depending on
// whether the current time falls within [sunrise, sunset) - computed locally
// via computeSunriseSunset() above (no network call needed). Tracks the last
// image actually set so this is a no-op most ticks (lv_img_set_src() isn't
// free - it re-decodes/re-caches the source).
static bool bgIsDayCurrentlyShown = true;
static void updateBackgroundForTimeOfDay()
{
    if (!bg)
        return;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0))
        return;
    bool isDay = !nightModeIsNight(); // follows the night mode setting (Auto = sunrise/sunset)
    if (isDay == bgIsDayCurrentlyShown && lv_img_get_src(bg) != nullptr)
        return;
    lv_img_set_src(bg, isDay ? (const void *)&weather_bg_day : (const void *)&weather_bg_night);
    bgIsDayCurrentlyShown = isDay;
}

static void updateClock()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0))
        return;
    char buf[16];
    // 24-hour HH:MM, no AM/PM shown - per explicit request, a deliberate
    // difference from the top status bar's own 12-hour clock (main.cpp's
    // clockLabel).
    strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
    lv_label_set_text(bigClockLabel, buf);
    char dateBuf[32];
    strftime(dateBuf, sizeof(dateBuf), "%a, %b %d", &timeinfo); // "Tue, Apr 27"
    lv_label_set_text(dateLabel, dateBuf);
    updateBackgroundForTimeOfDay();
}

static void updateNetworkStatus()
{
    bool connected = WiFi.status() == WL_CONNECTED;
    // No recolor - shown in the icon's own native grey/white color, matching
    // the top status bar's wifiIcon (main.cpp), which also doesn't recolor
    // this same asset.
    lv_img_set_src(networkStatusIcon, connected ? &wifi_connected_icon : &wifi_disconnected_icon);
    // bootingLabel (shown until the first real NEA fetch succeeds - see
    // renderWeather()'s weatherEverSucceeded gate) reads "Connecting..."
    // while Wi-Fi itself isn't up yet, then "Loading..." once Wi-Fi is
    // connected but the weather fetch hasn't completed yet - two genuinely
    // different milestones, per explicit request.
    if (bootingLabel)
        lv_label_set_text(bootingLabel, connected ? "Loading..." : "Connecting...");
}

// ---- overlay lifecycle --------------------------------------------------

static void hideWeather(lv_event_t *e)
{
    (void)e;
    if (overlay)
    {
        lv_obj_del(overlay);
        overlay = nullptr;
    }
    showing = false;
    lastTouchMs = millis();
    // Reveal whatever's underneath (Profile tab, per explicit request) -
    // nothing else to do here since the tab UI was never torn down, only
    // covered by this full-screen overlay sitting on lv_layer_top().
}

// ---- Web dashboard settings ----------------------------------------------

void weatherLoadSettings()
{
    Preferences prefs;
    prefs.begin("weather", true);
    String area = prefs.getString("area", areaName);
    fetchIntervalMin = constrain(prefs.getInt("mins", fetchIntervalMin), 5, 60);
    prefs.end();
    strlcpy(areaName, area.c_str(), sizeof(areaName));
}

int weatherAreaCount()
{
    return sizeof(AREAS) / sizeof(AREAS[0]);
}

const char *weatherAreaNameAt(int index)
{
    return AREAS[index];
}

const char *weatherGetArea()
{
    return areaName;
}

int weatherGetIntervalMin()
{
    return fetchIntervalMin;
}

void weatherSetArea(const char *area)
{
    bool known = false;
    for (int i = 0; i < weatherAreaCount(); i++)
        known |= strcmp(AREAS[i], area) == 0;
    if (!known || strcmp(area, areaName) == 0)
        return;
    strlcpy(areaName, area, sizeof(areaName));
    areaForecast[0] = '\0';
    rainSoon = false;
    Preferences prefs;
    prefs.begin("weather", false);
    prefs.putString("area", areaName);
    prefs.end();
    lastFetch = 0; // fetch for the new area right away
    if (areaLabel)
        lv_label_set_text(areaLabel, areaName);
}

void weatherSetIntervalMin(int minutes)
{
    fetchIntervalMin = constrain(minutes, 5, 60);
    Preferences prefs;
    prefs.begin("weather", false);
    prefs.putInt("mins", fetchIntervalMin);
    prefs.end();
}

void weatherGetContextSummary(char *out, size_t outSize)
{
    if (!weatherEverSucceeded)
    {
        snprintf(out, outSize, "Weather: not loaded yet.");
        return;
    }
    snprintf(out, outSize,
             "Weather for %s: next 2 hours %s. Today %s, %d-%dC (now %dC), humidity %d-%d%%, wind %d-%d km/h. "
             "UV index %d. PSI %d. Sunrise %s, sunset %s.",
             areaName, areaForecast[0] ? areaForecast : "unknown", currentConditionText, todayLow, todayHigh, liveTempC,
             humidityLow, humidityHigh, windLow, windHigh, uvIndex, psiCentral, sunriseStr, sunsetStr);
}

bool weatherRainSoon()
{
    return rainSoon;
}

void weatherHide()
{
    if (showing)
        hideWeather(nullptr);
}

int weatherSunriseMinute()
{
    return sunriseMinuteOfDay;
}

int weatherSunsetMinute()
{
    return sunsetMinuteOfDay;
}

void weatherShow()
{
    if (showing)
        return;
    showing = true;
    lastTouchMs = millis();

    overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, hideWeather, LV_EVENT_CLICKED, nullptr);

    bg = lv_img_create(overlay);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    updateBackgroundForTimeOfDay(); // sets bg's real source (day or night) based on the current time

    // Shown centered at the top, in place of every other piece of content,
    // until the first real NEA fetch succeeds (see renderWeather()'s
    // weatherEverSucceeded gate) - per explicit request, so the screen
    // says something rather than sitting blank while loading.
    bootingLabel = lv_label_create(overlay);
    lv_label_set_text(bootingLabel, WiFi.status() == WL_CONNECTED ? "Loading..." : "Connecting...");
    lv_obj_set_style_text_font(bootingLabel, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(bootingLabel, lv_color_white(), 0);
    lv_obj_clear_flag(bootingLabel, LV_OBJ_FLAG_CLICKABLE);
    // Positioned relative to networkStatusIcon (below, right after its own
    // creation) so it's vertically MID-aligned to the icon - same row, not
    // above or below it, per explicit request.

    // Wi-Fi icon: top-left corner, per explicit request (was top-right,
    // matching main.cpp's status-bar wifiIcon position - moved off that
    // spot on request). lv_obj_align(LV_ALIGN_TOP_LEFT, 8, 14) - x/y offsets
    // here are exactly what to change to nudge it further.
    networkStatusIcon = lv_img_create(overlay);
    lv_obj_align(networkStatusIcon, LV_ALIGN_TOP_LEFT, 8, 4);
    lv_obj_clear_flag(networkStatusIcon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_img_recolor(networkStatusIcon, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(networkStatusIcon, LV_OPA_COVER, 0);

    areaLabel = lv_label_create(overlay);
    lv_obj_set_style_text_color(areaLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(areaLabel, &lv_font_montserrat_20, 0); // same size as the condition text
    lv_obj_clear_flag(areaLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(areaLabel, areaName);
    lv_obj_add_flag(areaLabel, LV_OBJ_FLAG_HIDDEN); // shown/positioned in renderWeather(), once the hero icon and clock are laid out
    // Horizontally centered on screen (like "Booting..." was), but Y
    // matched to networkStatusIcon's own vertical center - same row as the
    // icon, not to its immediate right (that put it in the far top-left
    // corner, not what "same row" meant here). Real bug just fixed:
    // lv_obj_align(..., LV_ALIGN_TOP_MID, 0, someY) treats someY as an
    // OFFSET added to the top-aligned position, not an absolute y - so
    // passing an already-absolute "icon's vertical center" value there
    // double-applied/misread it, and for a short icon (~20px) minus half
    // the label's own height, it could go negative - pushing the label's
    // top above the screen's y=0 and clipping it exactly as seen on-device.
    // Fixed by aligning horizontally first (offset 0 is correct there),
    // then setting the real absolute y directly via lv_obj_set_y().
    lv_obj_update_layout(networkStatusIcon);
    lv_obj_align(bootingLabel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_update_layout(bootingLabel);
    lv_coord_t targetY = lv_obj_get_y(networkStatusIcon) + lv_obj_get_height(networkStatusIcon) / 2 - lv_obj_get_height(bootingLabel) / 2 + 12; // +8px down total, per explicit request (previous +4 wasn't visibly enough)
    if (targetY < 0)
        targetY = 0; // guard against a shorter icon/taller label combo pushing this above the screen
    lv_obj_set_y(bootingLabel, targetY);

    // Hero icon top-left, 120x120, positioned at (0, 0) - flush against
    // the panel's actual top-left corner. conditionIcon's own parent is
    // `overlay`, which has zero padding (lv_obj_remove_style_all() at
    // creation strips LVGL's default padding entirely), so (0,0) here
    // really is the screen's physical corner, not inset by a container pad.
    // Left column below the icon (condition text / wind / haze) is now a
    // single vertical flex container instead of three separately-
    // lv_obj_align_to()-chained rows. Real bug this replaces: each row used
    // to be anchored to the row above it with lv_obj_align_to(), computed
    // once at creation time before that row's icon had a real image source
    // assigned (lv_img_create() starts at zero size until lv_img_set_src()
    // runs) - so every row locked in a position based on a phantom
    // zero-height sibling and they all landed stacked on top of each
    // other / inside the bottom strip instead of spaced out below the
    // icon. A flex column lets LVGL compute each row's position from the
    // *actual current* size of everything above it, every time, so this
    // whole class of stale-position bug can't happen again.
    conditionIcon = lv_img_create(overlay);
    lv_img_set_src(conditionIcon, &weather_icon_cloudy); // renderWeather() swaps this for the real condition
    lv_obj_set_pos(conditionIcon, 10, 5);
    lv_obj_clear_flag(conditionIcon, LV_OBJ_FLAG_CLICKABLE);

    leftCol = lv_obj_create(overlay);
    lv_obj_remove_style_all(leftCol);
    lv_obj_set_size(leftCol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(leftCol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(leftCol, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(leftCol, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(leftCol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(leftCol, 4, 0); // 4px gap between the three strips, per explicit request
    // -10 cancels conditionIcon's own x=10 (leftCol's left edge sits at
    // screen x=0). y offset was -2 (haze row bottomed at y=221, 7px above
    // forecastRow's y=228 - an uneven gap vs. the 4px between the other
    // rows); +3 makes it 1, so: leftCol.y = conditionIcon.y(5) + 120 + 1 =
    // 126 -> conditionRow 126-156, gap 4, windRow 160-190, gap 4, hazeRow
    // 194-224, gap to forecastRow's y=228 (320 screen height - its own 92px
    // height) is exactly 4 - matches the other two row gaps. Recompute if
    // conditionIcon's y/height, row heights, or forecastRow's height ever
    // change.
    lv_obj_align_to(leftCol, conditionIcon, LV_ALIGN_OUT_BOTTOM_LEFT, -10, 4); // was 1 - +3px down per explicit request, moves the whole condition/wind/haze block as one unit

    // Condition/wind/haze each get their own square-cornered (no radius),
    // full-bleed (no side margin - starts flush at leftCol's own left
    // edge), semi-transparent black strip sized to actually contain its
    // icon+text with real padding, not clip it. Real bug fixed here: the
    // previous version used lv_obj_set_size(row, LV_SIZE_CONTENT,
    // LV_SIZE_CONTENT) with only 3px vertical padding and a negative
    // pad_top "nudge" on top of that - LV_SIZE_CONTENT sizes the row to
    // its content's laid-out height, but the negative pad_top then shrank
    // that available height further without the content's own font size
    // shrinking with it, so the strip ended up shorter than the text it
    // was supposed to contain. Fixed by using ordinary (non-negative)
    // padding sized to comfortably fit each row's actual font, and
    // reordering the rows in leftCol itself (not per-row top-padding
    // hacks) to get the "move up/down" spacing actually requested.
    auto makeStripRow = [](lv_obj_t *parent) -> lv_obj_t *
    {
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_width(row, LV_SIZE_CONTENT);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_50, 0); // 50% opaque, per explicit request (was 40%)
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0); // square corners, per explicit request ("no rounding")
        lv_obj_set_style_pad_all(row, 6, 0); // real padding on every side so icon+text is never clipped
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        return row;
    };

    lv_obj_t *conditionRow = makeStripRow(leftCol);
    // Overrides on top of makeStripRow's shared defaults, per explicit
    // request: back to LV_SIZE_CONTENT width (makeStripRow's own default -
    // no override needed) so the strip stops where its text stops, plus a
    // 5px right margin past the text via pad_right; pad_left bumped 6->11
    // (5px more) to shift the icon+text 5px right within the strip; 4px
    // shorter than makeStripRow's normal content-fit height via pad_top/
    // pad_bottom trimmed 6->4 each. The row's "5px up" is done at the
    // leftCol/align_to level above (its y-offset was reduced from 8 to 3,
    // moving the whole leftCol block - condition/wind/haze rows together -
    // up 5px as one unit) rather than translate_y here, which would only
    // shift this row's paint upward into conditionIcon's space without
    // moving errorLabel/windRow/hazeRow below it (see bigClockLabel's
    // comment above for the full reasoning on why translate is wrong here).
    lv_obj_set_style_pad_left(conditionRow, 18, 0); // 6 (makeStripRow default) + 12px right shift
    lv_obj_set_style_pad_right(conditionRow, 11, 0); // 6 (makeStripRow default) + 5px margin past the text
    lv_obj_set_style_pad_top(conditionRow, 4, 0);
    lv_obj_set_style_pad_bottom(conditionRow, 4, 0);
    // Fixed height, matches windRow/hazeRow exactly (font+icon combos gave
    // each row a different natural height otherwise). 30, not 40: leftCol
    // starts at y=123 (conditionIcon's y=5 + its 120px height - 2px
    // align_to offset) and forecastRow starts at y=228 (320 screen height -
    // its own 92px height) - 3 rows at 40px + 2x4px row-gap bottomed out at
    // y=251, thoroughly inside forecastRow. At 30px: 3*30 + 2*4 = 98,
    // leftCol bottoms out at 123+98=221, a real 7px clear of forecastRow.
    lv_obj_set_height(conditionRow, 30);
    conditionTextLabel = lv_label_create(conditionRow);
    lv_obj_set_style_text_font(conditionTextLabel, &lv_font_montserrat_20, 0); // matches the forecast strip's "Today" size
    lv_obj_set_style_text_color(conditionTextLabel, lv_color_hex(0xffeb3b), 0); // yellow, per explicit request (was purple)
    lv_obj_clear_flag(conditionTextLabel, LV_OBJ_FLAG_CLICKABLE);

    errorLabel = lv_label_create(leftCol); // stays outside any strip - only shown on a real fetch error
    lv_obj_set_style_text_color(errorLabel, lv_color_hex(0xffb3b3), 0);
    lv_obj_add_flag(errorLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(errorLabel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *windRow = makeStripRow(leftCol);
    // Same treatment as conditionRow above: pad_left 6->11 (5px right shift
    // for icon+text), pad_right 6->11 (5px margin past the text so the
    // strip stops where the text stops), pad_top/pad_bottom 6->4 (matches
    // conditionRow's height exactly, both 4px shorter than
    // makeStripRow's own default). pad_column (icon-to-label gap) stays a
    // separate, smaller reduction (6->4) so windLabel sits close to
    // windIconObj without overlapping it - unrelated to the row's own
    // left/right margins.
    lv_obj_set_style_pad_left(windRow, 18, 0); // 6 (makeStripRow default) + 12px right shift
    lv_obj_set_style_pad_right(windRow, 11, 0);
    lv_obj_set_style_pad_top(windRow, 4, 0);
    lv_obj_set_style_pad_bottom(windRow, 4, 0);
    lv_obj_set_style_pad_column(windRow, 4, 0);
    lv_obj_set_height(windRow, 30); // fixed, matches conditionRow/hazeRow exactly - see conditionRow's comment for the full y-budget math
    windIconObj = lv_img_create(windRow);
    lv_img_set_src(windIconObj, &weather_icon_wind_speed);
    windLabel = lv_label_create(windRow);
    lv_obj_set_style_text_font(windLabel, &lv_font_montserrat_18, 0); // matches the strip's H/L size
    // Humidity shares the wind strip: <wind icon> 5-15 km/h <humidity icon> 65-95%
    lv_obj_t *humiditySpacer = lv_obj_create(windRow);
    lv_obj_remove_style_all(humiditySpacer);
    lv_obj_set_size(humiditySpacer, 6, 1); // extra gap on top of pad_column, separating the two readings
    humidityIconObj = lv_img_create(windRow);
    lv_img_set_src(humidityIconObj, &weather_icon_humidity);
    lv_obj_set_style_img_recolor(humidityIconObj, lv_color_hex(0x42a5f5), 0); // blue, same as the low-temp reading
    lv_obj_set_style_img_recolor_opa(humidityIconObj, LV_OPA_COVER, 0);
    humidityLabel = lv_label_create(windRow);
    lv_obj_set_style_text_font(humidityLabel, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(humidityLabel, lv_color_hex(0x42a5f5), 0);

    lv_obj_t *hazeRow = makeStripRow(leftCol);
    lv_obj_set_style_pad_left(hazeRow, 18, 0); // same reasoning as windRow above
    lv_obj_set_style_pad_right(hazeRow, 11, 0);
    lv_obj_set_style_pad_top(hazeRow, 4, 0);
    lv_obj_set_style_pad_bottom(hazeRow, 4, 0);
    lv_obj_set_style_pad_column(hazeRow, 4, 0);
    lv_obj_set_height(hazeRow, 30); // fixed, matches conditionRow/windRow exactly - see conditionRow's comment for the full y-budget math
    hazeIconObj = lv_img_create(hazeRow);
    lv_img_set_src(hazeIconObj, &weather_icon_foggy_haze);
    hazeLabel = lv_label_create(hazeRow);
    lv_obj_set_style_text_font(hazeLabel, &lv_font_montserrat_18, 0);

    // Top-right block, also a single vertical flex column for the same
    // reason as leftCol above - clock, day+date, big live temp, H/L with
    // thermometer icons, sunrise/sunset with icons, each one genuinely
    // positioned relative to real content above it instead of a hand-
    // guessed fixed Y offset (which is what silently pushed the H/L and
    // sunrise/sunset rows down into/past the bottom strip before - even
    // though those two specific rows weren't literally chained via
    // align_to(), the fixed offsets were tuned against sizes that changed
    // as other rows above them changed, so they went stale the same way).
    rightCol = lv_obj_create(overlay);
    lv_obj_remove_style_all(rightCol);
    lv_obj_set_size(rightCol, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(rightCol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rightCol, LV_OBJ_FLAG_CLICKABLE);
    // Belt-and-suspenders: LVGL containers clip children to their own box
    // by default. The actual bug that made hotIcon/highLabel and
    // sunriseIcon/sunriseLabel disappear was LV_FLEX_ALIGN_END on
    // hiLoRow/sunRow (see their own comments) pushing those children to
    // negative x, not clipping here - but leaving overflow visible costs
    // nothing and guards against the same class of bug elsewhere.
    lv_obj_add_flag(rightCol, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(rightCol, LV_FLEX_FLOW_COLUMN);
    // CENTER cross-align (was END/right-aligned) per explicit request: every
    // child (clock/date/temp/H-L/sunrise-sunset) is now centered on
    // rightCol's own vertical center axis, instead of each sharing a common
    // right edge. rightCol's own x-position (align() call below) places
    // that center axis where bigClockLabel's own horizontal center used to
    // sit under the old right-aligned layout, so the clock's mid-point is
    // the fixed reference the rest of the column centers under.
    lv_obj_set_flex_align(rightCol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(rightCol, 4, 0);
    lv_obj_align(rightCol, LV_ALIGN_TOP_RIGHT, 3, 0); // clock top edge aligned with conditionIcon's top edge (y=0); x was -12, now 3 (15px right), per explicit request - moves the whole column since everything centers under bigClockLabel's axis

    bigClockLabel = lv_label_create(rightCol);
    lv_obj_set_style_text_font(bigClockLabel, &lv_font_digital_clock_66, 0);
    lv_obj_set_style_text_color(bigClockLabel, lv_color_white(), 0);
    lv_obj_clear_flag(bigClockLabel, LV_OBJ_FLAG_CLICKABLE);
    // Fixed width + centered text alignment, per explicit request ("clock
    // text moves to the right when text of different width appears" - not
    // every digit in this font renders the same pixel width, e.g. "1" vs
    // "8", so "01:23" and "11:11" have different natural label widths;
    // under rightCol's CENTER cross-align, that made the label's own box -
    // and the text inside it - visibly shift side to side as the time
    // changed). 190px comfortably fits "HH:MM" at this font/size with
    // margin; LV_TEXT_ALIGN_CENTER keeps the text centered within that now-
    // fixed box regardless of which digits are showing.
    lv_obj_set_width(bigClockLabel, 190);
    lv_obj_set_style_text_align(bigClockLabel, LV_TEXT_ALIGN_CENTER, 0);
    // 12px down, WITHOUT moving dateLabel/tempLabel/hiLoRow below it and
    // without the clipping/overlap translate_x/translate_y caused
    // previously (translate is paint-only - it shifts pixels but never
    // touches the flex layout box, so rightCol's own SIZE_CONTENT
    // width/position and dateLabel's "stack below the clock" position never
    // moved with it - the clock's paint just slid outside its own reserved
    // box, clipping at rightCol's edge along the way). pad_top is a real
    // per-item flex property instead - it pushes bigClockLabel down
    // *within its own slot* (rightCol's row-gap already separates it from
    // dateLabel below, so this doesn't collide with it) and grows
    // rightCol's own computed content box to fit, so nothing clips and no
    // sibling moves. The earlier "10px left" was done via pad_right, which
    // only made sense under the old END/right-aligned rightCol (it nudged
    // the clock off the shared right edge) - now that rightCol is
    // CENTER-aligned (see its own comment above), every child is centered
    // on the same axis by construction, so that pad_right is gone; dateLabel
    // below no longer needs a matching one either.
    lv_obj_set_style_pad_top(bigClockLabel, 12, 0);

    dateLabel = lv_label_create(rightCol);
    lv_obj_set_style_text_font(dateLabel, &lv_font_montserrat_22, 0); // bumped from 18, per explicit request
    lv_obj_set_style_text_color(dateLabel, lv_color_hex(0xffeb3b), 0); // yellow, per explicit request
    lv_obj_clear_flag(dateLabel, LV_OBJ_FLAG_CLICKABLE);
    // No pad_right needed - rightCol is now CENTER cross-aligned (see its
    // own comment above), so dateLabel is centered under bigClockLabel by
    // construction, not lined up via matched right-edge padding anymore.

    tempLabel = lv_label_create(rightCol);
    lv_obj_set_style_text_font(tempLabel, &lv_font_digital_temp_96, 0);
    lv_obj_set_style_text_color(tempLabel, lv_color_hex(0xff9800), 0);
    lv_obj_clear_flag(tempLabel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_top(tempLabel, 5, 0); // 5px down, per explicit request - pad_top (not translate_y, see bigClockLabel's comment above) so hiLoRow below it isn't overlapped

    // Degree mark is a separate, smaller Montserrat label superimposed over
    // tempLabel's own top-right corner (not a rightCol flex child - would
    // push hiLoRow/sunrise-sunset down by its own height) - the 96px digit
    // font has no degree glyph (only digits/-/: were converted, see its
    // extern decl comment above), and a same-size degree mark looked too
    // heavy against 96px digits per explicit request.
    tempDegLabel = lv_label_create(overlay);
    lv_label_set_text(tempDegLabel, "\xC2\xB0");
    lv_obj_set_style_text_font(tempDegLabel, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(tempDegLabel, lv_color_hex(0xff9800), 0);
    lv_obj_clear_flag(tempDegLabel, LV_OBJ_FLAG_CLICKABLE);

    // 2x2 matrix: row0 = [L, H], row1 = [sunrise, sunset], per explicit
    // request - and a real matrix means all 4 cells share the exact same
    // fixed width AND height, with the background strip covering the whole
    // cell (icon + text together), not just the text label on its own.
    const lv_coord_t kCellW = 80;
    const lv_coord_t kCellH = 24; // sunRow's own cell height - unchanged, so sunRow's bottom edge (and its clearance from forecastRow) never moves
    // iconXNudge: paint-only horizontal shift of just the icon (via
    // translate_x, not a layout property), used below to pull L/H's icon
    // left so it lines up in the same column as sunrise/sunset's icon
    // directly below it - sunrise/sunset themselves are never touched.
    // cellH defaults to kCellH (sunRow's height) - hiLoRow's own cells pass
    // kCellH+3 explicitly instead, so ONLY hiLoRow gets taller (closing its
    // own gap to sunRow) without growing sunRow itself, which would have
    // pushed sunRow's bottom edge - and its clearance from forecastRow -
    // down by the same amount. That clearance must not change.
    auto makeCell = [kCellW, kCellH](lv_obj_t *parent, const lv_img_dsc_t *icon, lv_coord_t iconXNudge = 0, lv_coord_t cellH = 0) -> lv_obj_t *
    {
        lv_obj_t *cell = lv_obj_create(parent);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, kCellW, cellH != 0 ? cellH : kCellH);
        lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_50, 0);
        lv_obj_set_style_radius(cell, 5, 0); // 5px rounded corners, per explicit request
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(cell, 4, 0);
        lv_obj_t *img = lv_img_create(cell);
        lv_img_set_src(img, icon);
        if (iconXNudge != 0)
            lv_obj_set_style_translate_x(img, iconXNudge, 0);
        return cell;
    };

    lv_obj_t *hiLoRow = lv_obj_create(rightCol);
    lv_obj_remove_style_all(hiLoRow);
    lv_obj_set_size(hiLoRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(hiLoRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hiLoRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(hiLoRow, LV_FLEX_FLOW_ROW);
    // LV_FLEX_ALIGN_START (not END): confirmed via serial diag dump that
    // END on a LV_SIZE_CONTENT row anchors the LAST child at the row's
    // right edge and pushes every earlier child into NEGATIVE x (off the
    // left edge of the row's own box) when the row's own computed width
    // hasn't grown to fit all children yet - hotIcon showed x=-106,
    // highLabel x=-78, both entirely off-screen, while the last two
    // children (anchored at the real edge) still rendered.
    lv_obj_set_flex_align(hiLoRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hiLoRow, 4, 0);
    // Real bug history (for the next person who touches this): ANY negative
    // PADDING on hiLoRow (top or bottom) shrinks its own content box from
    // that side, clipping its children's corners on that side - tried and
    // reverted twice. Growing just hiLoRow's own cells was also tried and
    // reverted: hiLoRow's height is LV_SIZE_CONTENT, so taller cells there
    // also grow hiLoRow's OWN computed height, which (via rightCol's COLUMN
    // flex flow) pushes every sibling after it - sunRow included - down by
    // that same amount, moving sunRow's bottom edge/forecastRow clearance,
    // which must not move. It also made hiLoRow's own strip visibly taller
    // than sunRow's, which wasn't asked for either. The fix that actually
    // holds: both rows keep IDENTICAL cell height (24px, the makeCell
    // default), and the gap between them is closed with translate_y on
    // sunRow instead (below, at its own creation) - translate_y is
    // paint-only, so it doesn't touch sunRow's own layout box/children or
    // move its real bottom edge, it only shifts where it's drawn.

    lv_obj_t *lowCell = makeCell(hiLoRow, &weather_icon_thermostat_low, -4); // -4px left: aligns L's icon into the same column as sunrise's icon below it, per explicit request (sunrise itself untouched)
    lowLabel = lv_label_create(lowCell);
    lv_obj_set_style_text_font(lowLabel, &lv_font_montserrat_14, 0); // matches sunrise/sunset's own (default) font size, per explicit request

    lv_obj_t *highCell = makeCell(hiLoRow, &weather_icon_thermostat_high, -4); // -4px left: aligns H's icon into the same column as sunset's icon below it, per explicit request (sunset itself untouched)
    highLabel = lv_label_create(highCell);
    lv_obj_set_style_text_font(highLabel, &lv_font_montserrat_14, 0); // matches sunrise/sunset's own (default) font size, per explicit request

    lv_obj_t *sunRow = lv_obj_create(rightCol);
    lv_obj_remove_style_all(sunRow);
    lv_obj_set_size(sunRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(sunRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sunRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(sunRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sunRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER); // same reasoning as hiLoRow above
    lv_obj_set_style_pad_column(sunRow, 4, 0);
    // LVGL 8.3.6 (this project's version) has no margin style (that's an
    // LVGL 9 API - lv_obj_set_style_margin_top doesn't exist here, confirmed
    // by a real compile error when tried). translate_y is used instead: it
    // shifts sunRow's PAINT position only, without touching its own layout
    // box/children (same proven-safe pattern as bigClockLabel's own
    // translate_y avoidance notes elsewhere in this file - the difference
    // here is translate_y is exactly what's wanted, since sunRow's
    // computed box/children must NOT change, only where it visually sits).
    lv_obj_set_style_translate_y(sunRow, 0, 0); // was -3 (canceling hiLoRow's now-reverted +3px cell height); net effect after the +3px-down request below is 0, i.e. sunRow's natural flex position
    // Real bug just reverted: translate_y only shifts PAINT, never the
    // layout box - sunRow's true occupied space (what actually needs to
    // clear forecastRow) never moved, so translate_y(-5) here was pure
    // cosmetic cover-up while sunRow's real bottom edge kept growing as
    // kCellH increased to 32px, eventually reaching visibly into
    // forecastRow. Reverted; real fix is below at forecastRow's own
    // alignment instead of faking movement here.
    sunRowDiag = sunRow; // temporary, see diagnostic print at the end of this function

    lv_obj_t *sunriseCell = makeCell(sunRow, &weather_icon_sunrise);
    sunriseLabel = lv_label_create(sunriseCell);

    lv_obj_t *sunsetCell = makeCell(sunRow, &weather_icon_sunset);
    sunsetLabel = lv_label_create(sunsetCell);

    // Bottom strip: 60% TRANSPARENT (i.e. 40% opaque) black band holding
    // the 5-day forecast (today included). Forecast text rows nudged down
    // 8px per explicit request; the icon stays where it was.
    forecastRow = lv_obj_create(overlay);
    lv_obj_set_size(forecastRow, LV_PCT(100), 92);
    lv_obj_set_style_bg_color(forecastRow, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(forecastRow, LV_OPA_30, 0); // 30% opaque i.e. 70% transparent, per explicit request (was 50%, before that 40%)
    lv_obj_set_style_border_width(forecastRow, 0, 0);
    lv_obj_set_style_radius(forecastRow, 0, 0);
    lv_obj_set_style_pad_all(forecastRow, 0, 0);
    lv_obj_align(forecastRow, LV_ALIGN_BOTTOM_MID, 0, 3); // +3px down, per explicit request - each forecast cell gets a compensating -3px translate_y in renderForecastRow() so the visible forecast content itself doesn't move
    lv_obj_clear_flag(forecastRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(forecastRow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(forecastRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(forecastRow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    updateClock();
    updateNetworkStatus();
    renderWeather();

    // Temporary: need sunRow's real bottom edge (y+h) to match its gap to
    // forecastRow (fixed at y=228) against hazeRow's own 4px gap there.
    lv_obj_update_layout(overlay);
    Serial.printf("weather diag: sunRow y=%d h=%d bottom=%d forecastRow.y=228\n",
                  lv_obj_get_y(sunRowDiag), lv_obj_get_height(sunRowDiag),
                  lv_obj_get_y(sunRowDiag) + lv_obj_get_height(sunRowDiag));
}

void weatherInit()
{
    lastTouchMs = millis();
    weatherShow(); // shown at boot, per explicit request
    // First fetch comes from weatherTick() once wifiSettled() - fetching
    // here, before Wi-Fi is even up, just burned attempts on DNS failures.
}

void weatherNoteTouch()
{
    lastTouchMs = millis();
}

void weatherTick()
{
    unsigned long now = millis();

    if (showing)
    {
        updateClock();
        updateNetworkStatus();
        static unsigned long lastStripFlipMs = 0;
        if (now - lastStripFlipMs >= 30000UL)
        {
            lastStripFlipMs = now;
            showUvOnStrip = !showUvOnStrip;
            if (weatherEverSucceeded)
                renderWeather();
        }
    }

    if (fetchJustCompleted)
    {
        if (weatherLastError[0] == '\0')
            weatherEverSucceeded = true;
        diagReport(DIAG_WEATHER, weatherLastError[0] == '\0', weatherLastError);
        fetchJustCompleted = false;
        setLoadingVisible(false);
        if (showing)
            renderWeather();
    }

    bool dueForAutoFetch = lastFetch == 0 || now - lastFetch > FETCH_INTERVAL_MS;
    bool dueForInitialRetry = !weatherEverSucceeded && (lastFetch == 0 || now - lastFetch > 5000UL);
    if (!fetchInProgress && wifiSettled() && (dueForAutoFetch || dueForInitialRetry))
    {
        lastFetch = now;
        startFetch();
    }

    // Same shape as bus.cpp's/today.cpp's own fetch watchdogs - the Network
    // Worker itself has no cancel mechanism, so this only clears the UI's
    // own "still loading" state after a fetch has clearly wedged, it
    // doesn't touch the worker task.
    if (fetchInProgress && millis() - fetchStartedAtMs > 65000) // fresh millis(): `now` predates startFetch() above, and now - fetchStartedAtMs would underflow
    {
        Serial.println("weather: fetchInProgress stuck past 65s - clearing UI state only");
        fetchInProgress = false;
        setLoadingVisible(false);
    }

    if (!showing && now - lastTouchMs > AUTO_RESHOW_MS)
        weatherShow();
}
