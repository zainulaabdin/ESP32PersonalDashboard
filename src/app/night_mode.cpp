#include "night_mode.h"
#include "backlight.h"
#include "weather.h"
#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <time.h>

#define NIGHT_NVS_NAMESPACE "night"
#define MIN_BRIGHTNESS 10

static NightModeSetting setting = NIGHT_MODE_AUTO;
static int dayPct = 80;
static int nightPct = 30;
static int period = -1;           // 0 day, 1 night, -1 not decided yet
static bool darkApplied = false;  // dark colours currently applied
static bool forceNight = false;   // serial "night" test

static int clampPct(int pct)
{
    return pct < MIN_BRIGHTNESS ? MIN_BRIGHTNESS : pct > 100 ? 100 : pct;
}

static void save()
{
    Preferences prefs;
    prefs.begin(NIGHT_NVS_NAMESPACE, false);
    prefs.putUChar("mode", (uint8_t)setting);
    prefs.putUChar("day", dayPct);
    prefs.putUChar("night", nightPct);
    prefs.end();
}

// ---- Dark colours --------------------------------------------------------
// Only style colours change - images (profile photo, icons) are never
// touched. The dark default theme covers theme-styled objects; objects with
// their own local colours (most of this app - lv_obj_remove_style_all() +
// hand-set colours) are recoloured by rule: light neutral backgrounds and
// borders become dark, dark neutral text becomes light, anything colourful
// stays. The original colours are remembered so switching back to day
// restores them exactly. Objects on lv_layer_top() (weather screen, QR
// codes, keypad) are left alone.
#define DARKENED_FLAG LV_OBJ_FLAG_USER_1
#define MAX_SAVED_COLORS 4096

struct SavedColor
{
    lv_obj_t *obj;
    uint32_t selector;
    lv_style_prop_t prop;
    lv_color_t original;
};
static SavedColor *entries = nullptr; // PSRAM
static int savedCount = 0;

static bool isNeutral(uint32_t rgb, int *lightness)
{
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    int mx = max(r, max(g, b)), mn = min(r, min(g, b));
    *lightness = (mx + mn) / 2;
    return mx - mn < 40;
}

static uint32_t flip(uint32_t rgb, int floor)
{
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
        out |= (uint32_t)min(255, 255 - (int)((rgb >> shift) & 0xFF) + floor) << shift;
    return out;
}

// Drops entries whose object no longer exists (only flagged objects in the
// tree are kept) - run when the table fills up.
static lv_obj_tree_walk_res_t collectLive(lv_obj_t *obj, void *liveSet)
{
    if (lv_obj_has_flag(obj, DARKENED_FLAG))
        for (int i = 0; i < savedCount; i++)
            if (entries[i].obj == obj)
                entries[i].selector |= 0x80000000u; // mark live
    return LV_OBJ_TREE_WALK_NEXT;
}

static void compactEntries()
{
    lv_obj_tree_walk(lv_scr_act(), collectLive, nullptr);
    int kept = 0;
    for (int i = 0; i < savedCount; i++)
        if (entries[i].selector & 0x80000000u)
        {
            entries[kept] = entries[i];
            entries[kept].selector &= ~0x80000000u;
            kept++;
        }
    savedCount = kept;
}

static void darkenProp(lv_obj_t *obj, uint32_t selector, lv_style_prop_t prop)
{
    lv_style_value_t v;
    if (lv_obj_get_local_style_prop(obj, prop, &v, selector) != LV_RES_OK)
        return;
    uint32_t rgb = lv_color_to32(v.color) & 0xFFFFFF;
    int lightness;
    uint32_t dark;
    bool accent = prop == LV_STYLE_TEXT_COLOR || prop == LV_STYLE_IMG_RECOLOR ||
                  (prop == LV_STYLE_BG_COLOR && (selector & 0xFF0000) == LV_PART_INDICATOR);
    if (!isNeutral(rgb, &lightness))
    {
        if (!accent || lightness >= 110)
            return;
        // Text/icons -> white; slider fill -> a softer light grey.
        dark = prop == LV_STYLE_BG_COLOR ? 0xBDBDBD : 0xFFFFFF;
    }
    else if (prop == LV_STYLE_IMG_RECOLOR)
        return;
    else if (prop == LV_STYLE_TEXT_COLOR)
    {
        if (lightness >= 160)
            return; // light text already sits on a dark/coloured background
        dark = flip(rgb, 0x30);
        if ((dark & 0xFF) < 0xDA)
            dark = 0xDADADA; // grey/secondary text all at one readable level
    }
    else
    {
        if (lightness < 128)
            return;
        // White surfaces -> #121212; grey ones (slider tracks, +/- buttons)
        // -> a mid grey so they still stand out from the background.
        if (rgb == (lv_color_to32(lv_color_hex(0xF1F3F5)) & 0xFFFFFF)) // compare after RGB565 rounding
            dark = 0x282B30; // top bar -> same as the dark theme's tab bar
        else
            // Tinted list stripes (Bus 0xE8F5E9, Today's pale purple) -> a
            // dark stripe so white text stays readable; other greys (slider
            // tracks, +/- buttons) -> mid grey.
            dark = lightness >= 245 ? flip(rgb, 0x12) : (lightness >= 225 && rgb != (lv_color_to32(lv_color_hex(0xE0E0E0)) & 0xFFFFFF)) ? 0x3A3A3A : 0x9A9A9A;
    }
    if (savedCount >= MAX_SAVED_COLORS)
        compactEntries();
    if (savedCount >= MAX_SAVED_COLORS)
        return;
    entries[savedCount++] = {obj, selector, prop, v.color};
    v.color = lv_color_hex(dark);
    lv_obj_set_local_style_prop(obj, prop, v, selector);
}

static lv_obj_tree_walk_res_t darkenObject(lv_obj_t *obj, void *)
{
    if (lv_obj_has_flag(obj, DARKENED_FLAG))
        return LV_OBJ_TREE_WALK_NEXT;
    // A new object at the address of a deleted one - forget the old entries.
    for (int i = 0; i < savedCount; i++)
        if (entries[i].obj == obj)
            entries[i--] = entries[--savedCount];
    lv_obj_add_flag(obj, DARKENED_FLAG);

    uint32_t selectors[16];
    int selectorCount = 0;
    for (uint32_t i = 0; i < obj->style_cnt && selectorCount < 16; i++)
        if (obj->styles[i].is_local)
            selectors[selectorCount++] = obj->styles[i].selector;
    for (int i = 0; i < selectorCount; i++)
    {
        darkenProp(obj, selectors[i], LV_STYLE_BG_COLOR);
        darkenProp(obj, selectors[i], LV_STYLE_BORDER_COLOR);
        darkenProp(obj, selectors[i], LV_STYLE_TEXT_COLOR);
        darkenProp(obj, selectors[i], LV_STYLE_IMG_RECOLOR);
    }
    return LV_OBJ_TREE_WALK_NEXT;
}

static lv_obj_tree_walk_res_t restoreObject(lv_obj_t *obj, void *)
{
    if (!lv_obj_has_flag(obj, DARKENED_FLAG))
        return LV_OBJ_TREE_WALK_NEXT;
    lv_obj_clear_flag(obj, DARKENED_FLAG);
    for (int i = 0; i < savedCount; i++)
        if (entries[i].obj == obj)
        {
            lv_style_value_t v;
            v.color = entries[i].original;
            lv_obj_set_local_style_prop(obj, entries[i].prop, v, entries[i].selector);
        }
    return LV_OBJ_TREE_WALK_NEXT;
}
void nightModeInit()
{
    Preferences prefs;
    prefs.begin(NIGHT_NVS_NAMESPACE, true);
    uint8_t mode = prefs.getUChar("mode", NIGHT_MODE_AUTO);
    dayPct = clampPct(prefs.getUChar("day", backlightGetBrightnessPct()));
    nightPct = clampPct(prefs.getUChar("night", 30));
    prefs.end();
    setting = mode <= NIGHT_MODE_DAY ? (NightModeSetting)mode : NIGHT_MODE_AUTO;
    entries = (SavedColor *)heap_caps_malloc(MAX_SAVED_COLORS * sizeof(SavedColor), MALLOC_CAP_SPIRAM);
}

static void applyBrightness()
{
    backlightSetBrightnessPct(period == 1 ? nightPct : dayPct);
    settingsSyncBrightnessSlider();
}

static int currentPeriod()
{
    if (forceNight || setting == NIGHT_MODE_NIGHT)
        return 1;
    if (setting == NIGHT_MODE_DAY)
        return 0;
    time_t t = time(nullptr);
    struct tm local;
    localtime_r(&t, &local);
    if (local.tm_year + 1900 < 2024)
        return 0; // clock not synced yet
    int minute = local.tm_hour * 60 + local.tm_min;
    return (minute < weatherSunriseMinute() || minute >= weatherSunsetMinute()) ? 1 : 0;
}

void nightModeTick()
{
    static unsigned long lastCheckMs = 0;
    unsigned long now = millis();
    if (period == -1 || now - lastCheckMs >= 1000)
    {
        lastCheckMs = now;
        int newPeriod = currentPeriod();
        if (newPeriod != period)
        {
            period = newPeriod;
            Serial.printf("night: %s\n", period == 1 ? "night" : "day");
            applyBrightness();
        }
    }

    // Keep the tabs' colours matching the period - also catches objects
    // created since the last pass (bus rows, calendar entries...), before
    // they're ever drawn, since this runs right before the refresh.
    bool wantDark = period == 1;
    if (wantDark != darkApplied)
    {
        darkApplied = wantDark;
        lv_disp_t *disp = lv_disp_get_default();
        lv_theme_t *th = lv_disp_get_theme(disp);
        lv_theme_default_init(disp, th->color_primary, th->color_secondary, darkApplied, th->font_normal);
        if (!darkApplied)
        {
            lv_obj_tree_walk(lv_scr_act(), restoreObject, nullptr);
            savedCount = 0;
        }
        Serial.printf("night: %s colours\n",darkApplied ? "dark" : "light");
    }
    if (darkApplied && entries)
        lv_obj_tree_walk(lv_scr_act(), darkenObject, nullptr);
}

NightModeSetting nightModeGetSetting()
{
    return setting;
}

void nightModeSetSetting(NightModeSetting newSetting)
{
    setting = newSetting;
    save();
    period = -1; // re-evaluated (and brightness re-applied) on the next tick
}

bool nightModeIsNight()
{
    return period == 1;
}

int nightModeDayBrightness()
{
    return dayPct;
}

int nightModeNightBrightness()
{
    return nightPct;
}

void nightModeSetBrightness(int day, int night)
{
    dayPct = clampPct(day);
    nightPct = clampPct(night);
    save();
    if (period != -1)
        applyBrightness();
}

void nightModeSetCurrentBrightness(int pct)
{
    if (period == 1)
        nightPct = clampPct(pct);
    else
        dayPct = clampPct(pct);
}

void nightModeToggleTest()
{
    forceNight = !forceNight;
    period = -1;
    Serial.printf("night: test %s\n", forceNight ? "on" : "off");
}

void nightModeSave()
{
    save();
}
