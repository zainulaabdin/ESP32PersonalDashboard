#include "web_config.h"
#include "app_config.h"
#include "diag.h"
#include "bus.h"
#include "bus_stops.h"
#include "today.h"
#include "settings.h"
#include "ota.h"
#include "wifi_manager.h"
#include "network_worker.h"
#include "firmware_version.h"
#include "power.h"
#include "night_mode.h"
#include "ask.h"
#include "web_icons.h"
#include "weather.h"
#include "wake_word.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#define WEB_CONFIG_USER "admin"

static WebServer server(80);
static bool serverRunning = false;
// Restart / sleep / provisioning requested from the web page, run a second
// later from webConfigTick() so the reply reaches the browser first.
enum DeferredAction
{
    ACTION_NONE,
    ACTION_RESTART,
    ACTION_SLEEP,
    ACTION_PROVISION,
};
static DeferredAction deferredAction = ACTION_NONE;
static unsigned long deferredAtMs = 0;

static void deferAction(DeferredAction action)
{
    deferredAction = action;
    deferredAtMs = millis() + 1000;
}

static const char PAGE_HEAD[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:sans-serif;margin:0;padding:16px 16px 76px;background:#121212;color:#e0e0e0;max-width:560px}"
    "h1{font-size:20px;margin:0 0 4px}h2{font-size:15px;color:#4dd0e1;margin:22px 0 8px}"
    "a{color:#4dd0e1}label{display:block;margin:10px 0 4px;font-size:13px;color:#9e9e9e}"
    "input{width:100%;box-sizing:border-box;padding:9px;border-radius:6px;border:1px solid #444;background:#1e1e1e;color:#fff;font-size:15px}"
    "button{padding:10px 16px;border:0;border-radius:6px;background:#27488f;color:#fff;font-size:15px;margin:14px 8px 0 0}"
    "button.alt{background:#424242}table{width:100%;border-collapse:collapse;font-size:14px}"
    "td{padding:6px 4px;border-bottom:1px solid #2a2a2a}td:first-child{color:#9e9e9e;width:42%}"
    ".ok{color:#66bb6a}.fail{color:#ef5350}.dim{color:#757575}.note{font-size:12px;color:#757575}"
    "nav{position:fixed;left:0;right:0;bottom:0;display:flex;background:#1e1e1e;border-top:1px solid #333}"
    "nav a{flex:1;text-align:center;padding:16px 0;color:#9e9e9e;text-decoration:none;font-size:15px}"
    "nav a.on{color:#4dd0e1;box-shadow:inset 0 3px #4dd0e1}"
    ".actions button{display:block;width:100%;margin:12px 0 0}"
    ".volrow{display:flex;align-items:stretch;gap:12px;margin-top:10px}input[type=range]{width:100%;padding:0;border:0;background:none;accent-color:#27488f;height:32px}"
    ".volrow .seg{flex:1}.seg button{flex:1;margin:0;border-radius:0;background:#2a2a2a;color:#bdbdbd}"
    ".seg button:first-child{border-radius:6px 0 0 6px}.seg button:last-child{border-radius:0 6px 6px 0}.seg button.on{background:#3949ab;color:#fff}"
    "button.rp{background:#2a2a2a;margin:0;padding:6px 14px;line-height:0}"
    // Same colours as the board's Settings icons (wifi_button, ota_update, sleeping, reboot).
    "button.prov{background:#35a5f9}button.ota{background:#8c288e}button.sleep{background:#ffd93b;color:#222}button.rot{background:#9b00ff}"
    ".seg{display:flex;gap:0}.actions .seg button{flex:1;margin:0;border-radius:0;background:#2a2a2a;color:#bdbdbd}"
    ".actions .seg button:first-child{border-radius:6px 0 0 6px}.actions .seg button:last-child{border-radius:0 6px 6px 0}"
    ".actions .seg button.on{background:#3949ab;color:#fff}"
    "button.restart{background:#f44336}button:disabled{opacity:.5}"
    "#report{font-size:15px;margin:18px 0 0}"
    "select{width:100%;padding:9px;border-radius:6px;border:1px solid #444;background:#1e1e1e;color:#fff;font-size:15px}"
    "</style>";

// Settings tab: live volume slider + replay-last-answer button.
static const char AUDIO_SCRIPT[] PROGMEM = R"(<script>
function el(id) { return document.getElementById(id); }
var sending = false, pending = null;
function setVol(v) { el('vv').textContent = v; pending = v; if (!sending) sendVol(); }
function sendVol() {
  if (pending === null) return;
  sending = true; var v = pending; pending = null;
  fetch('/volume', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: 'v=' + v})
    .finally(() => { sending = false; sendVol(); });
}
function sound(on) {
  el('snd1').className = on ? 'on' : ''; el('snd0').className = on ? '' : 'on';
  fetch('/sound', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: 'on=' + on});
}
function wake(on) {
  el('wk1').className = on ? 'on' : ''; el('wk0').className = on ? '' : 'on';
  fetch('/wake', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body: 'on=' + on});
}
function replay() {
  fetch('/replay', {method: 'POST'}).then(r => r.text()).then(t => {
    if (t == 'busy') alert('The board is busy (answering or already playing) - try again in a moment');
  });
}
function audio() {
  fetch('/audio').then(r => r.json()).then(s => {
    el('rpi').src = s.replay ? '/icon/replay.png' : '/icon/replay_empty.png';
    el('rp').disabled = !s.replay;
    if (document.activeElement != el('vol') && pending === null && !sending) { el('vol').value = s.volume; el('vv').textContent = s.volume; }
  });
}
audio(); setInterval(audio, 3000);
</script>)";

// Actions tab: update check/install in place, reported under the buttons.
static const char ACTIONS_SCRIPT[] PROGMEM = R"(<script>
var base = 0;
function el(id) { return document.getElementById(id); }
function st() { return fetch('/ota/status').then(r => r.json()); }
function show(t) { el('report').textContent = t; }
function check() {
  el('chk').disabled = true; el('inst').style.display = 'none'; show('Checking for update...');
  st().then(s => { base = s.count; return fetch('/ota/check', {method: 'POST'}); })
      .then(poll).catch(() => { show('Board not reachable'); el('chk').disabled = false; });
}
function poll() {
  st().then(s => {
    if (s.count == base) { setTimeout(poll, 1000); return; }
    el('chk').disabled = false;
    if (s.state == 'uptodate') show('No update available (Version ' + s.current + ')');
    else if (s.state == 'available') {
      show('Update available (Version ' + s.current + ' -> ' + s.latest + ')');
      el('inst').style.display = 'block';
    } else show('Update check failed (Version ' + s.current + ') - try again later');
  }).catch(() => setTimeout(poll, 2000));
}
function install() {
  if (!confirm('Install the update? The board restarts when done.')) return;
  el('inst').style.display = 'none'; el('chk').disabled = true; show('Starting update...');
  fetch('/ota/install', {method: 'POST'}).then(() => setTimeout(progress, 1000));
}
function progress() {
  st().then(s => { if (s.updating) show('Installing update... ' + s.pct + '%'); setTimeout(progress, 1000); })
      .catch(() => show('Board is restarting with the new version - reload this page in a minute'));
}
</script>)";

static bool authorized()
{
    if (server.authenticate(WEB_CONFIG_USER, cfgWebPassword()))
        return true;
    server.requestAuthentication(BASIC_AUTH, "ESP32 Dashboard");
    return false;
}

static void beginPage(String &html, const char *title, int refreshSec = 0)
{
    html.reserve(8192); // > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, so it lands in PSRAM
    html += FPSTR(PAGE_HEAD);
    if (refreshSec)
        html += "<meta http-equiv='refresh' content='" + String(refreshSec) + "'>";
    html += "<title>";
    html += title;
    html += "</title></head><body>";
}

enum WebTab
{
    TAB_SETTINGS,
    TAB_DIAG,
    TAB_ACTIONS,
};

// Bottom tab bar shared by all three pages.
static void endPage(String &html, WebTab active)
{
    static const char *hrefs[] = {"/", "/diag", "/actions"};
    static const char *names[] = {"Settings", "Diagnostics", "Actions"};
    html += "<nav>";
    for (int i = 0; i < 3; i++)
    {
        html += "<a href='";
        html += hrefs[i];
        html += i == active ? "' class='on'>" : "'>";
        html += names[i];
        html += "</a>";
    }
    html += "</nav></body></html>";
}

static void row(String &html, const char *name, const String &value, const char *cls = nullptr)
{
    html += "<tr><td>";
    html += name;
    html += cls ? String("</td><td class='") + cls + "'>" : String("</td><td>");
    html += value;
    html += "</td></tr>";
}

// Keys are write-only: the page only ever shows the last 4 characters.
static String maskedHint(const char *field, const char *value)
{
    size_t len = strlen(value);
    if (len == 0 || strncmp(value, "SET_ME", 6) == 0)
        return "not set";
    String hint = String(appConfigIsOverridden(field) ? "set on web page" : "from secrets.h") + ", ends ...";
    hint += (len > 4) ? value + len - 4 : value;
    return hint;
}

static void favoriteField(String &html, const char *label, const char *field, const char *code)
{
    const char *name = busStopsLookupName(code);
    html += "<label>";
    html += label;
    if (name && name[0])
    {
        html += " <span class='dim'>(";
        html += name;
        html += ")</span>";
    }
    html += "</label><input name='";
    html += field;
    html += "' inputmode='numeric' pattern='[0-9]{5}' maxlength='5' value='";
    html += code;
    html += "'>";
}

static void keyField(String &html, const char *label, const char *field, const char *value)
{
    html += "<label>";
    html += label;
    html += " <span class='dim'>(";
    html += maskedHint(field, value);
    html += ")</span></label><input name='";
    html += field;
    html += "' type='password' autocomplete='off' placeholder='leave empty to keep'>";
}

static void handleRoot()
{
    if (!authorized())
        return;
    String html;
    beginPage(html, "Settings");
    html += "<h1>Settings</h1>";

    // Outside the form: applied live, not on Save.
    int vol = settingsGetVolumePct();
    bool soundOn = askSoundEnabled();
    html += "<h2>Voice</h2><label>Volume <span id='vv'>";
    html += vol;
    html += "</span>%</label><input type='range' id='vol' min='0' max='100' value='";
    html += vol;
    html += "' oninput='setVol(this.value)'><div class='volrow'><div class='seg'>";
    html += soundOn ? "<button type='button' id='snd1' class='on' onclick='sound(1)'>Unmute</button><button type='button' id='snd0' onclick='sound(0)'>Mute</button>"
                    : "<button type='button' id='snd1' onclick='sound(1)'>Unmute</button><button type='button' id='snd0' class='on' onclick='sound(0)'>Mute</button>";
    html += "</div><button type='button' id='rp' class='rp' onclick='replay()' title='Replay last answer'";
    html += askHasReplay() ? "><img id='rpi' src='/icon/replay.png'" : " disabled><img id='rpi' src='/icon/replay_empty.png'";
    html += " width='24' height='24' alt='replay'></button></div>";
    if (wakeWordAvailable())
    {
        html += "<label>Wake word \"Jarvis\" <span class='dim'>(say it to start a question)</span></label><div class='volrow'><div class='seg'>";
        html += wakeWordEnabled() ? "<button type='button' id='wk1' class='on' onclick='wake(1)'>On</button><button type='button' id='wk0' onclick='wake(0)'>Off</button>"
                                  : "<button type='button' id='wk1' onclick='wake(1)'>On</button><button type='button' id='wk0' class='on' onclick='wake(0)'>Off</button>";
        html += "</div></div>";
    }
    html += FPSTR(AUDIO_SCRIPT);

    html += "<form method='post' action='/save'><h2>Bus</h2>";
    favoriteField(html, "Favourite stop, midnight to noon", "favAm", busGetFavorite(false));
    favoriteField(html, "Favourite stop, noon to midnight", "favPm", busGetFavorite(true));
    const char *stop = busGetActiveStopCode();
    const char *stopName = busStopsLookupName(stop);
    html += "<label>Show another stop now <span class='dim'>(showing ";
    html += stop;
    if (stopName && stopName[0])
    {
        html += " - ";
        html += stopName;
    }
    html += ")</span></label><input name='stop' inputmode='numeric' maxlength='5' placeholder='e.g. 18101'>";
    html += "<label>Bus auto-refresh interval (seconds, 5-120)</label><input name='busSec' type='number' min='5' max='120' value='";
    html += busGetRefreshIntervalSec();
    html += "'><h2>Calendar</h2><label>Calendar auto-refresh interval (minutes, 1-180)</label><input name='calMin' type='number' min='1' max='180' value='";
    html += todayGetRefreshIntervalMin();
    html += "'>";
    keyField(html, "Outlook calendar ICS URL", "ics", cfgIcsUrl());

    html += "<h2>Weather</h2><label>Area <span class='dim'>(NEA 2-hour forecast / rain warning, nearest temperature station)</span></label><select name='area'>";
    for (int i = 0; i < weatherAreaCount(); i++)
    {
        const char *name = weatherAreaNameAt(i);
        html += "<option";
        if (strcmp(name, weatherGetArea()) == 0)
            html += " selected";
        html += ">";
        html += name;
        html += "</option>";
    }
    html += "</select><label>Weather update interval (minutes, 5-60)</label><input name='wxMin' type='number' min='5' max='60' value='";
    html += weatherGetIntervalMin();
    html += "'>";

    html += "<h2>Screen</h2><label>Day brightness, sunrise to sunset (%, 10-100)</label><input name='dayBr' type='number' min='10' max='100' value='";
    html += nightModeDayBrightness();
    html += "'><label>Night brightness, sunset to sunrise (%, 10-100)</label><input name='nightBr' type='number' min='10' max='100' value='";
    html += nightModeNightBrightness();
    html += "'><div class='note'>Screen mode (Auto / Night / Day) is on the Actions tab.</div>";

    html += "<h2>API keys</h2>";
    keyField(html, "LTA DataMall key", "lta", cfgLtaKey());
    keyField(html, "OpenAI API key", "openai", cfgOpenAiKey());
    keyField(html, "GitHub OTA token (private repo only)", "gh", cfgGithubToken());
    keyField(html, "Web page password", "webpw", cfgWebPassword());
    html += "<div class='note'>Changing a key or the calendar URL restarts the board.</div>";
    html += "<button type='submit'>Save</button></form>";
    endPage(html, TAB_SETTINGS);
    server.send(200, "text/html", html);
}

static void handleActions()
{
    if (!authorized())
        return;
    String html;
    beginPage(html, "Actions");
    html += "<h1>Actions</h1><div class='note'>Version " FIRMWARE_VERSION "</div><div class='actions'>";
    // Auto / Night / Day - the current one is highlighted.
    static const char *modeNames[] = {"Auto", "Night", "Day"};
    html += "<label>Screen mode <span class='dim'>(Auto = night from sunset to sunrise)</span></label><form method='post' action='/night' class='seg'>";
    for (int i = 0; i < 3; i++)
    {
        html += "<button name='mode' value='";
        html += i;
        html += i == (int)nightModeGetSetting() ? "' class='on'>" : "'>";
        html += modeNames[i];
        html += "</button>";
    }
    html += "</form>";
    html += "<form method='post' action='/rotate'><button class='rot'>Rotate screen</button></form>"
            "<form method='post' action='/provision' onsubmit=\"return confirm('Show the Wi-Fi setup QR code on the board? This page goes offline until setup is done or cancelled on the board.')\"><button class='prov'>Provision Wi-Fi</button></form>"
            "<button id='chk' class='ota' onclick='check()'>Check for update</button>"
            "<form method='post' action='/sleep' onsubmit=\"return confirm('Put the board to sleep? Touch the screen to wake it.')\"><button class='sleep'>Sleep</button></form>"
            "<form method='post' action='/restart' onsubmit=\"return confirm('Restart the board?')\"><button class='restart'>Restart</button></form>"
            "</div><p id='report'></p><div class='actions'><button id='inst' class='ota' style='display:none' onclick='install()'>Install update</button></div>";
    html += FPSTR(ACTIONS_SCRIPT);
    endPage(html, TAB_ACTIONS);
    server.send(200, "text/html", html);
}

static void sendMessage(const char *message, const char *backTo, int backAfterSec, WebTab tab)
{
    String html;
    beginPage(html, "Dashboard");
    html += "<meta http-equiv='refresh' content='" + String(backAfterSec) + ";url=" + backTo + "'><h1>";
    html += message;
    html += "</h1>";
    endPage(html, tab);
    server.send(200, "text/html", html);
}

static void handleSave()
{
    if (!authorized())
        return;

    String favAm = server.arg("favAm");
    String favPm = server.arg("favPm");
    favAm.trim();
    favPm.trim();
    if (favAm != busGetFavorite(false) || favPm != busGetFavorite(true))
        busSetFavorites(favAm.c_str(), favPm.c_str()); // switches to the matching one

    String stop = server.arg("stop");
    stop.trim();
    if (stop.length() == 5)
        busSelectStopByCode(stop.c_str()); // ignored if a bus fetch is running right now

    if (server.hasArg("busSec") && server.hasArg("calMin"))
        settingsSetIntervals(server.arg("busSec").toInt(), server.arg("calMin").toInt());
    if (server.hasArg("area"))
        weatherSetArea(server.arg("area").c_str());
    if (server.hasArg("wxMin"))
        weatherSetIntervalMin(server.arg("wxMin").toInt());
    if (server.hasArg("dayBr") && server.hasArg("nightBr"))
        nightModeSetBrightness(server.arg("dayBr").toInt(), server.arg("nightBr").toInt());

    bool needsRestart = false;
    static const char *keyFields[] = {"lta", "openai", "ics", "gh", "webpw"};
    for (const char *field : keyFields)
    {
        String value = server.arg(field);
        value.trim();
        if (value.length() == 0)
            continue;
        appConfigSave(field, value.c_str());
        needsRestart = true;
    }

    if (needsRestart)
    {
        sendMessage("Saved - restarting...", "/", 15, TAB_SETTINGS);
        deferAction(ACTION_RESTART);
    }
    else
        sendMessage("Saved", "/", 2, TAB_SETTINGS);
}

static void handleRestart()
{
    if (!authorized())
        return;
    sendMessage("Restarting...", "/actions", 15, TAB_ACTIONS);
    deferAction(ACTION_RESTART);
}

static void handleVolume()
{
    if (!authorized())
        return;
    if (server.hasArg("v"))
        settingsSetVolumePct(server.arg("v").toInt());
    server.send(200, "text/plain", "ok");
}

static void handleSound()
{
    if (!authorized())
        return;
    askSetSoundEnabled(server.arg("on").toInt() != 0);
    server.send(200, "text/plain", "ok");
}

static void handleWake()
{
    if (!authorized())
        return;
    wakeWordSetEnabled(server.arg("on").toInt() != 0);
    server.send(200, "text/plain", "ok");
}

static void handleReplay()
{
    if (!authorized())
        return;
    if (!askHasReplay())
        server.send(200, "text/plain", "none");
    else
        server.send(200, "text/plain", askReplayLast() ? "ok" : "busy");
}

static void handleAudio()
{
    if (!authorized())
        return;
    char json[48];
    snprintf(json, sizeof(json), "{\"volume\":%d,\"replay\":%s}", settingsGetVolumePct(), askHasReplay() ? "true" : "false");
    server.send(200, "application/json", json);
}

static void sendIcon(const uint8_t *png, size_t len)
{
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send_P(200, "image/png", (const char *)png, len);
}

static void handleNight()
{
    if (!authorized())
        return;
    int mode = server.arg("mode").toInt();
    if (mode >= NIGHT_MODE_AUTO && mode <= NIGHT_MODE_DAY)
        nightModeSetSetting((NightModeSetting)mode);
    server.sendHeader("Location", "/actions");
    server.send(303);
}

static void handleRotate()
{
    if (!authorized())
        return;
    settingsToggleRotation();
    server.sendHeader("Location", "/actions");
    server.send(303);
}

static void handleSleep()
{
    if (!authorized())
        return;
    sendMessage("Going to sleep - touch the screen to wake the board", "/actions", 30, TAB_ACTIONS);
    deferAction(ACTION_SLEEP);
}

static void handleProvision()
{
    if (!authorized())
        return;
    sendMessage("Wi-Fi setup QR code is on the board - scan it with the ESP SoftAP Provisioning app", "/actions", 60, TAB_ACTIONS);
    deferAction(ACTION_PROVISION);
}

// Check / install without leaving the Actions page: the page's script
// POSTs here, then polls /ota/status.
static void handleOtaCheck()
{
    if (!authorized())
        return;
    otaCheckOnly();
    server.send(200, "text/plain", "ok");
}

static void handleOtaInstall()
{
    if (!authorized())
        return;
    otaCheckNow(); // installs the newer release, then the board restarts
    server.send(200, "text/plain", "ok");
}

static void handleOtaStatus()
{
    if (!authorized())
        return;
    static const char *states[] = {"idle", "checking", "uptodate", "available", "failed"};
    char json[160];
    snprintf(json, sizeof(json), "{\"count\":%u,\"state\":\"%s\",\"current\":\"%s\",\"latest\":\"%s\",\"updating\":%s,\"pct\":%d}",
             otaCheckCount(), states[otaCheckState()], FIRMWARE_VERSION, otaLatestVersion(),
             otaInProgress() ? "true" : "false", otaProgressPct());
    server.send(200, "application/json", json);
}

static String formatUptime()
{
    unsigned long s = millis() / 1000UL; // wraps after 49 days
    char buf[32];
    snprintf(buf, sizeof(buf), "%lud %luh %lum", s / 86400UL, (s / 3600UL) % 24UL, (s / 60UL) % 60UL);
    return buf;
}

static String kb(size_t bytes)
{
    return String((unsigned)(bytes / 1024)) + " KB";
}

static const char *resetReasonText()
{
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT: return "reset button";
    default: return "USB reset or unknown";
    }
}

static void handleDiag()
{
    if (!authorized())
        return;
    String html;
    beginPage(html, "Diagnostics", 5);
    html += "<h1>Diagnostics</h1><div class='note'>Refreshes every 5s</div>";

    html += "<h2>System</h2><table>";
    row(html, "Firmware", FIRMWARE_VERSION);
    row(html, "Uptime", formatUptime());
    row(html, "Last reset", resetReasonText());
    row(html, "Free RAM", kb(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    row(html, "Largest block", kb(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    row(html, "Lowest free RAM", kb(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    row(html, "Free PSRAM", String(ESP.getFreePsram() / 1048576.0f, 1) + " MB");
    row(html, "Storage used", kb(SPIFFS.usedBytes()) + " of " + kb(SPIFFS.totalBytes()));
    row(html, "Chip temp", String(temperatureRead(), 0) + " &deg;C");
    const char *job = networkWorkerRunningJob();
    row(html, "Network job", job ? job : "idle");
    html += "</table><h2>Wi-Fi</h2><table>";
    row(html, "Network", WiFi.SSID());
    row(html, "Signal", String(WiFi.RSSI()) + " dBm");
    row(html, "IP", WiFi.localIP().toString());
    row(html, "DNS", WiFi.dnsIP().toString());

    html += "</table><h2>Services</h2><table>";
    for (int i = 0; i < DIAG_COUNT; i++)
    {
        bool ok;
        const char *detail;
        long ageSec;
        if (!diagGet((DiagService)i, &ok, &detail, &ageSec))
        {
            row(html, diagServiceName((DiagService)i), "not used yet", "dim");
            continue;
        }
        String value = ok ? "OK" : "FAIL";
        if (!ok && detail[0])
            value += String(": ") + detail;
        value += "<span class='dim'> &middot; ";
        value += ageSec < 60 ? String(ageSec) + "s ago" : String(ageSec / 60) + "m ago";
        value += "</span>";
        row(html, diagServiceName((DiagService)i), value, ok ? "ok" : "fail");
    }
    html += "</table>";
    endPage(html, TAB_DIAG);
    server.send(200, "text/html", html);
}

void webConfigTick()
{
    if (deferredAction != ACTION_NONE && (long)(millis() - deferredAtMs) >= 0)
    {
        DeferredAction action = deferredAction;
        deferredAction = ACTION_NONE;
        settingsOnTabLeave(); // flush any unsaved Settings-tab changes first
        if (action == ACTION_RESTART)
        {
            delay(50);
            ESP.restart();
        }
        else if (action == ACTION_SLEEP)
            powerSleepNow();
        else if (action == ACTION_PROVISION)
            wifiProvisioningStart(); // stops this server below; the board shows the QR
    }

    bool wanted = WiFi.status() == WL_CONNECTED && !wifiProvisioningInProgress();
    if (wanted && !serverRunning)
    {
        static bool routesAdded = false;
        if (!routesAdded)
        {
            server.on("/", HTTP_GET, handleRoot);
            server.on("/save", HTTP_POST, handleSave);
            server.on("/restart", HTTP_POST, handleRestart);
            server.on("/ota/check", HTTP_POST, handleOtaCheck);
            server.on("/ota/install", HTTP_POST, handleOtaInstall);
            server.on("/ota/status", HTTP_GET, handleOtaStatus);
            server.on("/sleep", HTTP_POST, handleSleep);
            server.on("/rotate", HTTP_POST, handleRotate);
            server.on("/night", HTTP_POST, handleNight);
            server.on("/volume", HTTP_POST, handleVolume);
            server.on("/replay", HTTP_POST, handleReplay);
            server.on("/sound", HTTP_POST, handleSound);
            server.on("/wake", HTTP_POST, handleWake);
            server.on("/audio", HTTP_GET, handleAudio);
            server.on("/icon/replay.png", HTTP_GET, []() { sendIcon(web_icon_replay, web_icon_replay_len); });
            server.on("/icon/replay_empty.png", HTTP_GET, []() { sendIcon(web_icon_replay_empty, web_icon_replay_empty_len); });
            server.on("/provision", HTTP_POST, handleProvision);
            server.on("/actions", HTTP_GET, handleActions);
            server.on("/diag", HTTP_GET, handleDiag);
            server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
            routesAdded = true;
        }
        server.begin();
        serverRunning = true;
        Serial.printf("web: settings page at http://%s/\n", WiFi.localIP().toString().c_str());
    }
    else if (!wanted && serverRunning)
    {
        server.stop();
        serverRunning = false;
        Serial.println("web: server stopped");
    }

    if (serverRunning)
        server.handleClient();
}
