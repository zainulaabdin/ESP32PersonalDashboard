// Settings tab: left side has two vertical sliders (screen brightness -
// wired to the real PWM backlight; speaker volume - UI only for now, no
// audio hardware yet), right side is a scrollable column of setting rows,
// each a numeric value with a -/+ stepper (a slider is too imprecise for
// picking an exact interval). More rows will be added later, so
// addIntervalRow() is a small reusable helper rather than one-off layout
// code per setting.
#include "settings.h"
#include "backlight.h"
#include "bus.h"
#include "today.h"
#include "power.h"
#include "wifi_manager.h"
#include "ota.h"
#include "firmware_version.h"
#include "icons/settings_icon_sleep.h"
#include "icons/settings_icon_reboot.h"
#include "icons/settings_icon_wifi.h"
#include "icons/settings_icon_ota.h"
#include "icons/settings_icon_dashboard.h"
#include "icons/settings_icon_rotate.h"
#include "night_mode.h"
#include "es8311.h"
#include <Arduino.h>
#include <Preferences.h>
#include "display_rotation.h"
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

#define SETTINGS_NVS_NAMESPACE "settings"

// Screen goes fully dark (and touch becomes invisible/unusable) below this -
// the brightness slider physically can't be dragged lower than this, so the
// screen can never be driven to an unreadable state from this control.
#define MIN_BRIGHTNESS_PCT 10

// Now actually wired to the speaker (es8311.cpp's es8311InitForSpeaker(),
// via settingsGetVolumePct() below) - was genuinely UI-only for a while
// (see this file's header comment, now stale) until a real user report of
// the volume slider having no audible effect traced directly to that gap:
// the speaker code had its own hardcoded register value and never read
// this at all.
static int volumePct = 50;

static lv_obj_t *brightnessSlider = nullptr;
static lv_obj_t *brightnessValueLabel = nullptr;

static void brightnessSliderEvent(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *valueLabel = (lv_obj_t *)lv_event_get_user_data(e);
    int val = lv_slider_get_value(slider);
    backlightSetBrightnessPct((uint8_t)val);
    nightModeSetCurrentBrightness(val); // day or night value, whichever is current
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", val);
    lv_label_set_text(valueLabel, buf);
}

static lv_obj_t *volumeSlider = nullptr;
static lv_obj_t *volumeValueLabel = nullptr;

static void volumeSliderEvent(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *valueLabel = (lv_obj_t *)lv_event_get_user_data(e);
    volumePct = lv_slider_get_value(slider);
    es8311ApplyLiveVolume(); // audible straight away if something is playing
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", volumePct);
    lv_label_set_text(valueLabel, buf);
}

// NVS writes are relatively slow flash operations and wear the flash with
// repeated use, so settings aren't written on every interaction (slider
// release, stepper tap) - they're just marked dirty here, and actually
// flushed once by settingsOnTabLeave() when the user navigates away from
// the tab. Persists every setting together, regardless of which one
// changed, since that's simpler than tracking which single value is dirty.
static bool settingsDirty = false;

static void saveSettings()
{
    Preferences prefs;
    prefs.begin(SETTINGS_NVS_NAMESPACE, false);
    prefs.putUChar("bright", backlightGetBrightnessPct());
    prefs.putInt("vol", volumePct);
    prefs.putInt("busSec", busGetRefreshIntervalSec());
    prefs.putInt("calMin", todayGetRefreshIntervalMin());
    prefs.end();
    nightModeSave();
    settingsDirty = false;
}

void settingsOnTabLeave()
{
    if (settingsDirty)
        saveSettings();
}

int settingsGetVolumePct()
{
    return volumePct;
}

// Only fires on release (not every drag tick).
static void sliderReleasedEvent(lv_event_t *e)
{
    settingsDirty = true;
}

void settingsLoadPersisted()
{
    Preferences prefs;
    prefs.begin(SETTINGS_NVS_NAMESPACE, true); // read-only
    uint8_t bright = prefs.getUChar("bright", backlightGetBrightnessPct());
    int vol = prefs.getInt("vol", volumePct);
    int busSec = prefs.getInt("busSec", busGetRefreshIntervalSec());
    int calMin = prefs.getInt("calMin", todayGetRefreshIntervalMin());
    bool flip = prefs.getBool("flip", false);
    prefs.end();

    displaySetFlipped(flip);
    backlightSetBrightnessPct(bright);
    volumePct = vol;
    busSetRefreshIntervalSec(busSec);
    todaySetRefreshIntervalMin(calMin);
}

// Settings tab tint - applied to the tab bar icon (main.cpp) and reused
// here for the slider fill/labels so the tab reads as one consistent color,
// same pattern as Bus (green) and Today (purple).
#define SETTINGS_TINT 0x27488F
// Track color matches the other tabs' toggle switches in their unchecked
// state - the actual LVGL default theme grey, lv_palette_lighten(GREY, 2).
// Confirmed from lib/lvgl/src/misc/lv_color.c's own palette table (not
// guessed): the grey row is {0xBDBDBD, 0xE0E0E0, 0xEEEEEE, 0xF5F5F5,
// 0xFAFAFA} for lighten levels 1-5, so level 2 is 0xE0E0E0 - an earlier
// version of this used 0xBDBDBD (level 1) by mistake, which is visibly
// darker than the real thing.
#define SETTINGS_TRACK_GREY 0xE0E0E0
// Bus tab's sidebar history rows' own colors (bus.cpp renderHistoryList()),
// reused here so the interval value box reads as the same kind of button.
#define SETTINGS_VALUE_BOX_BORDER 0xd8dbde
#define SETTINGS_VALUE_BOX_TEXT 0x222222

// A wide rectangular slider that fills bottom-to-top, with the live
// %-value above it and the title below - shared layout for brightness and
// volume. LVGL auto-detects vertical orientation when a slider's height
// exceeds its width, and vertical sliders fill from the bottom by default.
// The round knob is hidden (zero padding + transparent) so it reads as a
// solid bar rather than a thin track with a handle - dragging still works,
// since the whole bar (not just the knob) responds to touch.
//
// The column's height used to be forced to LV_PCT(100) of its (fixed-height)
// parent, which was a few pixels shorter than the label+slider+label
// content actually needed, so LVGL centered the overflow and clipped it
// top and bottom. Sizing the column to LV_SIZE_CONTENT instead removes the
// fixed box entirely - it's exactly as tall as its content, and the parent
// row's own CENTER cross-alignment still centers it vertically.
static lv_obj_t *createVerticalSlider(lv_obj_t *parent, const char *title, int minValue, int startValue, lv_event_cb_t cb)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 90, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *valueLabel = lv_label_create(col);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", startValue);
    lv_label_set_text(valueLabel, buf);
    lv_obj_set_style_text_color(valueLabel, lv_color_hex(SETTINGS_TINT), 0);

    lv_obj_t *slider = lv_slider_create(col);
    lv_obj_set_size(slider, 48, 150);
    lv_slider_set_range(slider, minValue, 100);
    lv_slider_set_value(slider, startValue, LV_ANIM_OFF);
    lv_obj_set_style_radius(slider, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_TRACK_GREY), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_TINT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);

    lv_obj_t *titleLabel = lv_label_create(col);
    lv_label_set_text(titleLabel, title);

    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, valueLabel);
    if (cb == brightnessSliderEvent)
    {
        brightnessSlider = slider;
        brightnessValueLabel = valueLabel;
    }
    else if (cb == volumeSliderEvent)
    {
        volumeSlider = slider;
        volumeValueLabel = valueLabel;
    }
    lv_obj_add_event_cb(slider, sliderReleasedEvent, LV_EVENT_RELEASED, NULL);
    return titleLabel;
}

// Mutable state for one stepper row - a plain static array (not heap
// allocated) since the row count is small and fixed at compile time, same
// style as this project's other small fixed-size tables (e.g. bus.cpp's
// service rows).
struct IntervalRowState
{
    int value;
    int minVal;
    int maxVal;
    int step;
    void (*apply)(int);
    const char *unitSuffix;
    lv_obj_t *valueLabel;
};
static IntervalRowState intervalRowStates[4];
static int intervalRowStateCount = 0;

static void updateIntervalRowLabel(IntervalRowState *st)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%s", st->value, st->unitSuffix);
    lv_label_set_text(st->valueLabel, buf);
}

static void intervalMinusEvent(lv_event_t *e)
{
    IntervalRowState *st = (IntervalRowState *)lv_event_get_user_data(e);
    st->value -= st->step;
    if (st->value < st->minVal)
        st->value = st->minVal;
    st->apply(st->value);
    updateIntervalRowLabel(st);
    settingsDirty = true;
}

static void intervalPlusEvent(lv_event_t *e)
{
    IntervalRowState *st = (IntervalRowState *)lv_event_get_user_data(e);
    st->value += st->step;
    if (st->value > st->maxVal)
        st->value = st->maxVal;
    st->apply(st->value);
    updateIntervalRowLabel(st);
    settingsDirty = true;
}

static lv_obj_t *addStepperButton(lv_obj_t *parent, const char *symbol)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 32, 32);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(SETTINGS_TRACK_GREY), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_TINT), 0);
    lv_obj_center(label);
    return btn;
}

// Styled like the Bus tab's sidebar history rows (bus.cpp
// renderHistoryList(), inactive state: white background, thin light-grey
// border, rounded corners, dark text) so the value reads as that same kind
// of button rather than matching the grey stepper buttons either side of
// it. Returns the inner label (what addIntervalRow() needs to update on
// each step), not the box itself.
static lv_obj_t *addValueBox(lv_obj_t *parent, int width, const char *initialText)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, width, 32);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(SETTINGS_VALUE_BOX_BORDER), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, initialText);
    lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_VALUE_BOX_TEXT), 0);
    lv_obj_center(label);
    return label;
}

// One settings row: a title label, then a numeric value with a -/+ stepper
// next to it (a slider is too imprecise for picking an exact interval).
static void addIntervalRow(lv_obj_t *parent, const char *title, int minVal, int maxVal, int step, int startVal,
                            const char *unitSuffix, void (*applyFn)(int))
{
    IntervalRowState *st = &intervalRowStates[intervalRowStateCount++];
    st->value = startVal;
    st->minVal = minVal;
    st->maxVal = maxVal;
    st->step = step;
    st->apply = applyFn;
    st->unitSuffix = unitSuffix;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, title);

    lv_obj_t *stepperRow = lv_obj_create(row);
    lv_obj_remove_style_all(stepperRow);
    lv_obj_set_size(stepperRow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stepperRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stepperRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(stepperRow, 12, 0);
    lv_obj_clear_flag(stepperRow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *minusBtn = addStepperButton(stepperRow, LV_SYMBOL_MINUS);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d%s", startVal, unitSuffix);
    st->valueLabel = addValueBox(stepperRow, 112, buf);

    lv_obj_t *plusBtn = addStepperButton(stepperRow, LV_SYMBOL_PLUS);

    lv_obj_add_event_cb(minusBtn, intervalMinusEvent, LV_EVENT_CLICKED, st);
    lv_obj_add_event_cb(plusBtn, intervalPlusEvent, LV_EVENT_CLICKED, st);
}

// Web dashboard volume slider: applied live, saved once it stops moving
// (settingsTick()) so dragging doesn't hammer NVS.
static unsigned long volumeSaveAtMs = 0;

void settingsSetVolumePct(int pct)
{
    volumePct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    es8311ApplyLiveVolume();
    if (volumeSlider)
    {
        lv_slider_set_value(volumeSlider, volumePct, LV_ANIM_OFF);
        lv_label_set_text_fmt(volumeValueLabel, "%d%%", volumePct);
    }
    volumeSaveAtMs = millis() + 2000;
}

void settingsSyncBrightnessSlider()
{
    if (!brightnessSlider)
        return;
    int pct = backlightGetBrightnessPct();
    lv_slider_set_value(brightnessSlider, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(brightnessValueLabel, "%d%%", pct);
}

void settingsSetIntervals(int busSec, int calMin)
{
    busSetRefreshIntervalSec(busSec);
    todaySetRefreshIntervalMin(calMin);
    for (int i = 0; i < intervalRowStateCount; i++)
    {
        IntervalRowState *st = &intervalRowStates[i];
        if (st->apply == busSetRefreshIntervalSec)
            st->value = busGetRefreshIntervalSec();
        else if (st->apply == todaySetRefreshIntervalMin)
            st->value = todayGetRefreshIntervalMin();
        updateIntervalRowLabel(st);
    }
    saveSettings();
}

static void sleepNowButtonEvent(lv_event_t *e)
{
    // Flush pending settings first, same as leaving the tab normally -
    // sleeping is another way of "leaving" without a tab switch event.
    settingsOnTabLeave();
    powerSleepNow();
}

static void rebootButtonEvent(lv_event_t *e)
{
    settingsOnTabLeave();
    delay(50); // let the NVS write above actually land before the reset
    ESP.restart();
}

// Wi-Fi provisioning overlay, same lv_layer_top() pattern as profile.cpp's
// QR overlay - shown for as long as wifiProvisioningInProgress() is true
// (polled from settingsTick()), so it renders above whatever tab is active
// even if the user leaves Settings while still waiting for the phone app.
// Unlike profile.cpp's QR (a pre-baked static PNG), this QR's content
// (service name/PIN) is only known at runtime, so it's generated live via
// LVGL's built-in lv_qrcode widget (LV_USE_QRCODE, enabled in lv_conf.h)
// instead of an offline-converted image.
#define OVERLAY_BG_COLOR 0x121212 // solid dark background for the provisioning/OTA screens
static lv_obj_t *provisioningOverlay = nullptr;

// On-demand GitHub release check (ota.cpp) - the status LED blinks cyan
// while checking, and the update overlay takes over if one is installing.
// Version line under the action icons; doubles as the OTA button's
// feedback line ("Checking...", then "No update available" / failure for
// OTA_MESSAGE_MS, then back to the version).
#define OTA_MESSAGE_MS 30000UL
static lv_obj_t *versionLabel = nullptr;
// The version line sits on the same baseline as the "Volume" slider label,
// which lives in a different (vertically centred) container - so it's
// positioned from that label's real coordinates once layout has run.
static lv_obj_t *volumeTitleLabel = nullptr;
static bool versionLabelPlaced = false;
static bool otaManualCheckPending = false;
static unsigned otaManualCheckStartCount = 0;
static unsigned long otaMessageUntilMs = 0;

// Also shows the web settings page address (web_config.cpp) once Wi-Fi is up.
static uint32_t shownIp = 0;

static void showVersion()
{
    shownIp = WiFi.status() == WL_CONNECTED ? (uint32_t)WiFi.localIP() : 0;
    if (shownIp)
        lv_label_set_text_fmt(versionLabel, "Version " FIRMWARE_VERSION "   Web: %s", WiFi.localIP().toString().c_str());
    else
        lv_label_set_text(versionLabel, "Version " FIRMWARE_VERSION);
}

static void otaButtonEvent(lv_event_t *e)
{
    otaManualCheckPending = true;
    otaManualCheckStartCount = otaCheckCount();
    otaMessageUntilMs = 0;
    lv_label_set_text(versionLabel, "Checking for updates...");
    otaCheckNow(); // an install, if found, takes over with its own overlay and restarts
}

static void updateVersionLabel()
{
    if (!versionLabel)
        return;
    if (!versionLabelPlaced && lv_obj_get_height(volumeTitleLabel) > 0)
    {
        lv_area_t vol, area;
        lv_obj_get_coords(volumeTitleLabel, &vol);
        lv_obj_get_coords(lv_obj_get_parent(versionLabel), &area);
        lv_obj_set_pos(versionLabel, 12, vol.y1 - area.y1); // x 12 = rightList's pad_left, lines up with the icons
        versionLabelPlaced = true;
    }
    if (otaManualCheckPending && otaCheckCount() != otaManualCheckStartCount)
    {
        otaManualCheckPending = false;
        OtaCheckState result = otaCheckState();
        lv_label_set_text(versionLabel, result == OTA_UP_TO_DATE          ? "No update available"
                                        : result == OTA_UPDATE_AVAILABLE ? "Update available"
                                                                         : "Update check failed - try again later");
        otaMessageUntilMs = millis() + OTA_MESSAGE_MS;
    }
    if (otaMessageUntilMs && (long)(millis() - otaMessageUntilMs) >= 0)
    {
        otaMessageUntilMs = 0;
        showVersion();
    }
    uint32_t ip = WiFi.status() == WL_CONNECTED ? (uint32_t)WiFi.localIP() : 0;
    if (!otaMessageUntilMs && !otaManualCheckPending && ip != shownIp)
        showVersion();
}

static void wifiButtonEvent(lv_event_t *e)
{
    wifiProvisioningStart();
}

static void provisioningOverlayClicked(lv_event_t *e)
{
    wifiProvisioningCancel(); // overlay itself disappears via settingsTick() once the session has actually ended
}

static void showProvisioningOverlay()
{
    if (provisioningOverlay)
        return;
    provisioningOverlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(provisioningOverlay);
    lv_obj_set_size(provisioningOverlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(provisioningOverlay, lv_color_hex(OVERLAY_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(provisioningOverlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(provisioningOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(provisioningOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(provisioningOverlay, provisioningOverlayClicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *qr = lv_qrcode_create(provisioningOverlay, 150, lv_color_black(), lv_color_white());
    const char *payload = wifiProvisioningQrPayload();
    lv_qrcode_update(qr, payload, strlen(payload));
    // A white quiet-zone border is required for a QR code to scan reliably -
    // without it, a dark app background right up to the code's own edge can
    // confuse a scanner's finder-pattern detection.
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 5, 0);
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -30); // centred like profile.cpp's QR overlay, nudged up to leave room for the 3-line hint below

    lv_obj_t *label = lv_label_create(provisioningOverlay);
    lv_label_set_text_fmt(label, "Scan with the ESP SoftAP Provisioning app\n(iPhone: first join Wi-Fi \"%s\")\nTap anywhere to cancel",
                          wifiProvisioningServiceName());
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, 360);
    lv_obj_align_to(label, qr, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
}

static void hideProvisioningOverlay()
{
    if (!provisioningOverlay)
        return;
    lv_obj_del(provisioningOverlay);
    provisioningOverlay = nullptr;
}

// Web dashboard QR (web_config.cpp): scanning it opens http://<board IP>/
// on the phone. Tap anywhere to close.
static lv_obj_t *dashboardOverlay = nullptr;

static void dashboardOverlayClicked(lv_event_t *e)
{
    lv_obj_del(dashboardOverlay);
    dashboardOverlay = nullptr;
}

static void dashboardButtonEvent(lv_event_t *e)
{
    if (dashboardOverlay)
        return;
    dashboardOverlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(dashboardOverlay);
    lv_obj_set_size(dashboardOverlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(dashboardOverlay, lv_color_hex(OVERLAY_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(dashboardOverlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dashboardOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dashboardOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dashboardOverlay, dashboardOverlayClicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(dashboardOverlay);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, 360);

    if (WiFi.status() != WL_CONNECTED)
    {
        lv_label_set_text(label, "Not connected to Wi-Fi\nTap anywhere to close");
        lv_obj_center(label);
        return;
    }
    char url[32];
    snprintf(url, sizeof(url), "http://%s/", WiFi.localIP().toString().c_str());
    lv_obj_t *qr = lv_qrcode_create(dashboardOverlay, 150, lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0); // quiet zone, see showProvisioningOverlay()
    lv_obj_set_style_border_width(qr, 5, 0);
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -30);

    lv_label_set_text_fmt(label, "Scan to open the dashboard (same Wi-Fi)\n%s   user: admin\nTap anywhere to close", url);
    lv_obj_align_to(label, qr, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
}

// Firmware-update overlay (ota.cpp) - same lv_layer_top() pattern, not
// dismissable: the board restarts itself once the image is written.
static lv_obj_t *otaOverlay = nullptr;
static lv_obj_t *otaLabel = nullptr;
static int otaShownPct = -1;

static lv_obj_t *otaBar = nullptr;

static void updateOtaOverlay()
{
    if (!otaInProgress())
        return;
    if (!otaOverlay)
    {
        otaOverlay = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(otaOverlay);
        lv_obj_set_size(otaOverlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(otaOverlay, lv_color_hex(OVERLAY_BG_COLOR), 0);
        lv_obj_set_style_bg_opa(otaOverlay, LV_OPA_COVER, 0);
        lv_obj_add_flag(otaOverlay, LV_OBJ_FLAG_CLICKABLE); // swallow taps while updating

        lv_obj_t *title = lv_label_create(otaOverlay);
        lv_label_set_text(title, "Updating firmware");
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -50);

        otaBar = lv_bar_create(otaOverlay);
        lv_obj_set_size(otaBar, 320, 22);
        lv_bar_set_range(otaBar, 0, 100);
        lv_obj_set_style_bg_color(otaBar, lv_color_hex(0x37474f), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(otaBar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(otaBar, lv_color_hex(0x00bcd4), LV_PART_INDICATOR); // cyan, matches the status LED's OTA colour
        lv_obj_set_style_radius(otaBar, 11, LV_PART_MAIN);
        lv_obj_set_style_radius(otaBar, 11, LV_PART_INDICATOR);
        lv_obj_align(otaBar, LV_ALIGN_CENTER, 0, 0);

        otaLabel = lv_label_create(otaOverlay);
        lv_obj_set_style_text_color(otaLabel, lv_color_white(), 0);
        lv_obj_set_style_text_font(otaLabel, &lv_font_montserrat_20, 0);
        lv_obj_align(otaLabel, LV_ALIGN_CENTER, 0, 36);

        lv_obj_t *hint = lv_label_create(otaOverlay);
        lv_label_set_text(hint, "Don't power off - the board restarts when done");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x9e9e9e), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    }
    int pct = otaProgressPct();
    if (pct != otaShownPct)
    {
        otaShownPct = pct;
        lv_bar_set_value(otaBar, pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(otaLabel, "%d%%", pct);
    }
    lv_obj_move_foreground(otaOverlay);
}

void settingsTick()
{
    if (volumeSaveAtMs && (long)(millis() - volumeSaveAtMs) >= 0)
    {
        volumeSaveAtMs = 0;
        saveSettings();
    }
    updateVersionLabel();
    updateOtaOverlay();
    if (wifiRestartRequested())
    {
        rebootButtonEvent(nullptr); // same flush-then-restart path as the reboot icon
        return;
    }
    if (wifiProvisioningInProgress())
    {
        showProvisioningOverlay();
        // weather.cpp's full-screen overlay also lives on lv_layer_top() and
        // re-appears on its own after inactivity - keep the QR above it.
        lv_obj_t *top = lv_obj_get_parent(provisioningOverlay);
        if (lv_obj_get_index(provisioningOverlay) != (int32_t)lv_obj_get_child_cnt(top) - 1)
            lv_obj_move_foreground(provisioningOverlay);
    }
    else
    {
        hideProvisioningOverlay();
    }
}

// Rotate icon: flips the whole UI 180 degrees (display + touch, main.cpp)
// and saves it straight away - a rare tap, so no batching needed.
void settingsToggleRotation()
{
    bool flip = !displayIsFlipped();
    displaySetFlipped(flip);
    Preferences prefs;
    prefs.begin(SETTINGS_NVS_NAMESPACE, false);
    prefs.putBool("flip", flip);
    prefs.end();
}

static void rotateButtonEvent(lv_event_t *e)
{
    settingsToggleRotation();
}

// The icon itself is the tappable control - no button chrome, no label, and
// no recolor, so it shows in the exact native colors supplied
// (icons/sleeping_24x24.png, icons/reboot_24x24.png).
static lv_obj_t *addActionButton(lv_obj_t *parent, const lv_img_dsc_t *icon, lv_event_cb_t cb)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, icon);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, cb, LV_EVENT_CLICKED, NULL);
    return img;
}

void settingsInit(lv_obj_t *tab)
{
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tab, 16, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tab, 10, 0);

    lv_obj_t *leftCol = lv_obj_create(tab);
    lv_obj_remove_style_all(leftCol);
    lv_obj_set_size(leftCol, 180, LV_PCT(100));
    lv_obj_set_flex_flow(leftCol, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(leftCol, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(leftCol, LV_OBJ_FLAG_SCROLLABLE);

    createVerticalSlider(leftCol, "Brightness", MIN_BRIGHTNESS_PCT, backlightGetBrightnessPct(), brightnessSliderEvent);
    volumeTitleLabel = createVerticalSlider(leftCol, "Volume", 0, volumePct, volumeSliderEvent);

    lv_obj_t *divider = lv_obj_create(tab);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 1, LV_PCT(100));
    lv_obj_set_style_bg_color(divider, lv_color_hex(0xd0d0d0), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    lv_obj_t *rightArea = lv_obj_create(tab);
    lv_obj_remove_style_all(rightArea);
    lv_obj_set_flex_grow(rightArea, 1);
    lv_obj_set_height(rightArea, LV_PCT(100));
    lv_obj_set_flex_flow(rightArea, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(rightArea, LV_OBJ_FLAG_SCROLLABLE);

    // Root cause of the earlier crash was LVGL's 64KB memory pool running
    // out (see lv_conf.h LV_MEM_SIZE), not this layout - back to the
    // straightforward version, scrollbar restyled and active as intended.
    lv_obj_t *rightList = lv_obj_create(rightArea);
    lv_obj_remove_style_all(rightList);
    lv_obj_set_size(rightList, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(rightList, 1);
    lv_obj_set_flex_flow(rightList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(rightList, 12, 0);
    lv_obj_set_style_pad_row(rightList, 16, 0);
    lv_obj_set_scrollbar_mode(rightList, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(rightList, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(rightList, lv_color_hex(0x9e9e9e), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(rightList, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(rightList, 2, LV_PART_SCROLLBAR);

    addIntervalRow(rightList, "Bus auto-refresh interval", 5, 120, 5, busGetRefreshIntervalSec(), "s", busSetRefreshIntervalSec);
    addIntervalRow(rightList, "Calendar auto-refresh interval", 1, 180, 10, todayGetRefreshIntervalMin(), "min", todaySetRefreshIntervalMin);

    lv_obj_t *actionsRow = lv_obj_create(rightList);
    lv_obj_remove_style_all(actionsRow);
    lv_obj_set_size(actionsRow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actionsRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actionsRow, 8, 0); // 6 x 32px icons must fit the ~235px column
    lv_obj_clear_flag(actionsRow, LV_OBJ_FLAG_SCROLLABLE);

    addActionButton(actionsRow, &settings_icon_rotate, rotateButtonEvent);
    addActionButton(actionsRow, &settings_icon_wifi, wifiButtonEvent);
    addActionButton(actionsRow, &settings_icon_dashboard, dashboardButtonEvent);
    addActionButton(actionsRow, &settings_icon_ota, otaButtonEvent);
    addActionButton(actionsRow, &settings_icon_sleep, sleepNowButtonEvent);
    addActionButton(actionsRow, &settings_icon_reboot, rebootButtonEvent);

    versionLabel = lv_label_create(rightArea);
    lv_obj_add_flag(versionLabel, LV_OBJ_FLAG_IGNORE_LAYOUT); // placed by updateVersionLabel(), not the column flex
    lv_obj_set_style_text_color(versionLabel, lv_color_hex(0x757575), 0);
    showVersion();
}
