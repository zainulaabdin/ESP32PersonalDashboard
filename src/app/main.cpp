// App shell: top status strip + bottom tab bar (Profile / Today / Bus / Ask).
// Rendering via LVGL, driven through TFT_eSPI, with FT6336 touch input so
// tabs are switchable by hand. Tab content is still placeholder text.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_freertos_hooks.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <SPIFFS.h>
#include <stdio.h>
#include <errno.h>
#include "touch.h"
#include "bus.h"
#include "bus_stops.h"
#include "secrets_select.h"
#include "profile.h"
#include "today.h"
#include "ask.h"
#include "status_bar.h"
#include "power.h"
#include "net_lock.h"
#include "network_worker.h"
#include "backlight.h"
#include "settings.h"
#include "display_rotation.h"
#include "weather.h"
#include "wifi_manager.h"
#include "ota.h"
#include "status_led.h"
#include "app_config.h"
#include "web_config.h"
#include "night_mode.h"
#include "wake_word.h"
#include "firmware_version.h"
#include "icons/wifi_connected_icon.h"
#include "icons/wifi_disconnected_icon.h"
#include "speaker.h"
#include "icons/tab_icon_person.h"
#include "icons/tab_icon_calendar.h"
#include "icons/tab_icon_bus.h"
#include "icons/tab_icon_mic.h"
#include "icons/tab_icon_settings.h"

static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 320;
static const uint16_t topBarHeight = 28;
static const uint16_t tabBarHeight = 50;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 40];

TFT_eSPI tft = TFT_eSPI();
lv_indev_t *touchIndev;
lv_obj_t *clockLabel;
lv_obj_t *statsLabel;
static lv_obj_t *wifiIcon;
static lv_obj_t *loadingSpinner;
// Real screen-update count (incremented on each actual flush to the panel,
// not on each loop() iteration - those aren't the same thing: loop() can
// spin faster than anything visibly changes).
uint32_t flushCount = 0;
uint32_t lastFps = 0;

// Per-core CPU%, one number per core rather than a single figure, since
// this chip's two cores do genuinely different jobs here: Arduino's
// loop() (LVGL, touch, this app's own tick functions) is pinned to core 1
// by this SDK's default config (CONFIG_ARDUINO_RUNNING_CORE=1, confirmed
// by reading the framework's sdkconfig - not assumed), while core 0 runs
// the Wi-Fi/BT stack plus the background HTTP fetch tasks this app spawns
// there (bus.cpp/today.cpp) - a single combined percentage would hide
// which core is actually doing the work.
//
// Measured via FreeRTOS's per-core idle hooks
// (esp_register_freertos_idle_hook_for_cpu), not a hand-rolled busy-timer
// like the previous single-number version: this chip's precompiled SDK
// ships with CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS and
// CONFIG_FREERTOS_USE_TRACE_FACILITY both disabled (confirmed in
// sdkconfig), which is what vTaskGetRunTimeStats()/uxTaskGetSystemState()
// need to report real per-task CPU time - not available here without
// rebuilding the framework from source, well out of scope. Idle hooks are
// a separate, always-available mechanism: each hook is throttled to fire
// at most once per FreeRTOS tick (CONFIG_FREERTOS_HZ=1000, i.e. up to
// 1000 calls/sec while a core is genuinely idle every tick), so
// (idle hook calls in the last window) / (ticks in that window) is real
// idle time, not an approximation from timing our own code - it correctly
// reflects everything on that core, including Wi-Fi/BT driver activity we
// never instrumented ourselves.
static volatile uint32_t idleHookCount[2] = {0, 0};
static uint32_t lastCpuPct[2] = {0, 0};

static bool idleHookCore0()
{
    idleHookCount[0]++;
    return true; // "call me once per tick", not "as fast as possible" - see .h comment's warning about blocking
}

static bool idleHookCore1()
{
    idleHookCount[1]++;
    return true;
}

// Network throughput: no single built-in "total Wi-Fi bytes" counter in
// the Arduino API, so every fetch (bus.cpp/bus_stops.cpp/today.cpp)
// reports its own byte count here via addNetworkBytes().
static volatile uint32_t networkBytesSinceCheck = 0;

void addNetworkBytes(uint32_t bytes)
{
    networkBytesSinceCheck += bytes;
}

// Tabview, for uiShowTab() (voice commands / Ask shortcuts).
static lv_obj_t *tabview = nullptr;

void uiShowTab(uint32_t index)
{
    if (!tabview)
        return;
    lv_tabview_set_act(tabview, index, LV_ANIM_OFF);
    lv_event_send(tabview, LV_EVENT_VALUE_CHANGED, NULL); // same handlers as a real tap (tab-active flags, Today fetch...)
}

// Screenshot capture (serial "shot", used by docs/screenshot.py): while
// set, every flushed area is also copied into this full-frame RGB565 buffer.
static uint16_t *shotBuf = nullptr;

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    if (shotBuf)
        for (uint32_t y = 0; y < h; y++)
            memcpy(&shotBuf[(area->y1 + y) * screenWidth + area->x1], &color_p[y * w], w * 2);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
    flushCount++;
}

// Swipe-down-from-top-edge detection for the weather screen (weather.cpp) -
// not done via LVGL's own gesture recognizer because this project's manual
// lv_indev_read_timer_cb()-driven loop already doesn't service LVGL's
// animation timer correctly for other gestures (see buildUi()'s tabview
// comment on the same root cause), so this tracks the raw press start/
// current position by hand instead, the same way touch.h itself already
// hands back a single latest point rather than a full gesture history.
// Armed only when a press *starts* within kSwipeTopEdgePx of the top, so a
// downward drag starting anywhere else on screen (e.g. scrolling a list)
// never triggers it.
static const int kSwipeTopEdgePx = 20;
static const int kSwipeDownThresholdPx = 40;
static bool swipeArmed = false;
static bool swipeFired = false;
static int swipeStartY = 0;

static bool screenFlipped = false;

void displaySetFlipped(bool flipped)
{
    screenFlipped = flipped;
    tft.setRotation(flipped ? 3 : 1);
    if (lv_disp_get_default()) // also called from setup() before lv_init()
    {
        lv_obj_invalidate(lv_scr_act());
        lv_obj_invalidate(lv_layer_top());
        lv_obj_invalidate(lv_layer_sys());
    }
}

bool displayIsFlipped()
{
    return screenFlipped;
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    if (touch_touched())
    {
        if (screenFlipped)
        {
            touch_last_x = screenWidth - 1 - touch_last_x;
            touch_last_y = screenHeight - 1 - touch_last_y;
        }
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touch_last_x;
        data->point.y = touch_last_y;
        powerNoteTouch();
        weatherNoteTouch();

        if (!swipeArmed && !swipeFired)
        {
            swipeArmed = touch_last_y <= kSwipeTopEdgePx;
            swipeStartY = touch_last_y;
        }
        if (swipeArmed && !swipeFired && touch_last_y - swipeStartY >= kSwipeDownThresholdPx)
        {
            swipeFired = true;
            weatherShow();
        }
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
        swipeArmed = false;
        swipeFired = false;
    }
}

// Re-anchors statsLabel to sit just left of the wifi icon. Must be called
// after every lv_label_set_text() on it: lv_obj_align_to() computes an
// absolute position once, so if it were only called at creation time (when
// the label was still empty), the label would keep growing rightward off
// the visible area as its text got longer instead of staying anchored.
void repositionStatsLabel()
{
    lv_obj_align_to(statsLabel, wifiIcon, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

// Shown while any tab is doing a slow background fetch (currently just the
// Today tab's calendar) - added after that fetch turned out to block the
// whole UI with no indication anything was happening. The fetch itself now
// runs on a background FreeRTOS task (see today.cpp) so this spinner
// animating is also a real, working signal that the UI thread itself is
// still alive and responsive, not just decoration.
//
// Lives next to the clock (leftmost) rather than in the already-crowded
// stats/wifi cluster on the right, and nothing else is anchored to its
// position - hiding it just stops it drawing, it doesn't leave a gap for
// anything to fill or avoid.
void setLoadingVisible(bool visible)
{
    if (visible)
        lv_obj_clear_flag(loadingSpinner, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(loadingSpinner, LV_OBJ_FLAG_HIDDEN);
}

void setLoadingSpinnerColor(uint32_t hexColor)
{
    lv_obj_set_style_arc_color(loadingSpinner, lv_color_hex(hexColor), LV_PART_INDICATOR);
}

// Gap between icon and text, and the shared icon size (all 4 tab icons are
// 24x24) - shared so the label-width math below can't drift from what's
// actually drawn.
static const lv_coord_t kTabIconGap = 4;
static const lv_coord_t kTabIconSize = 24;

// Matches the lv_tabview_add_tab() call order in buildUi() (Profile, Today,
// Bus, Ask) - needed to tell which tab just became active from a plain
// index, e.g. for Today's "load on tab-show" hook and both tabs'
// active-tab-gated auto-refresh.
static const uint32_t kTodayTabIndex = 1;
static const uint32_t kBusTabIndex = 2;
static const uint32_t kSettingsTabIndex = 4;

// Today's own tint (today.cpp's TODAY_TINT) - the loading spinner's default
// color at boot, before any real fetch has set it at its own trigger site
// (see bus.cpp's/today.cpp's startFetch(), which now set the spinner's
// color themselves - it identifies which subsystem is fetching, not which
// tab is on screen).
static const uint32_t kTodayTintColor = 0x8F13FD;

// Draws an icon (+ optional label) for one tab button, centered as a single
// group within that button's column. columnLeftX/columnWidth describe the
// column in pixels rather than assuming equal-width columns, so unequal
// button widths (e.g. the narrower icon-only Settings tab, set via
// lv_btnmatrix_set_btn_width() in buildUi()) work the same way - the caller
// computes those pixel columns from the same width-unit numbers passed to
// lv_btnmatrix_set_btn_width(), so the two can't drift apart. buttonText
// may be "" for an icon-only button (no label created at all).
//
// lv_btnmatrix draws its own button text directly (lv_btnmatrix.c line
// ~788: `btn_area.x1 += (lv_area_get_width(&btn_area) - txt_size.x) / 2`) -
// centered across the button's *full* width, and this ignores LV_PART_ITEMS
// padding entirely (verified by reading that function - an earlier attempt
// assumed padding would shift it, which silently did nothing, so the
// now-unmoved centered text ended up directly underneath the icon, which is
// drawn after/on top of it as a later child). Rather than keep fighting
// lv_btnmatrix's built-in text rendering, buildUi() hides it completely
// (LV_PART_ITEMS text opacity 0) and this function draws both the icon and
// the label itself, as two ordinary child objects it fully controls.
lv_obj_t *addTabIcon(lv_obj_t *tabBtns, lv_coord_t columnLeftX, lv_coord_t columnWidth, const char *buttonText, const lv_img_dsc_t *img)
{
    lv_coord_t textWidth = 0;
    lv_coord_t textHeight = 0;
    lv_obj_t *label = NULL;
    if (buttonText && buttonText[0])
    {
        label = lv_label_create(tabBtns);
        lv_label_set_text(label, buttonText);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_update_layout(label);
        textWidth = lv_obj_get_width(label);
        textHeight = lv_obj_get_height(label);
    }

    lv_coord_t groupWidth = kTabIconSize + (label ? kTabIconGap + textWidth : 0);
    lv_coord_t groupLeftX = columnLeftX + (columnWidth - groupWidth) / 2;

    lv_obj_t *icon = lv_img_create(tabBtns);
    lv_img_set_src(icon, img);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(icon, groupLeftX, (tabBarHeight - img->header.h) / 2);

    if (label)
        lv_obj_set_pos(label, groupLeftX + kTabIconSize + kTabIconGap, (tabBarHeight - textHeight) / 2);

    return icon;
}

void buildUi()
{
    lv_obj_t *scr = lv_scr_act();

    lv_obj_t *topBar = lv_obj_create(scr);
    lv_obj_remove_style_all(topBar);
    lv_obj_set_size(topBar, screenWidth, topBarHeight);
    lv_obj_set_pos(topBar, 0, 0);
    lv_obj_set_style_bg_color(topBar, lv_color_hex(0xf1f3f5), 0);
    lv_obj_set_style_bg_opa(topBar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(topBar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(topBar, lv_color_hex(0xd8dbde), 0);
    lv_obj_set_style_border_width(topBar, 1, 0);
    lv_obj_set_style_pad_hor(topBar, 8, 0);
    lv_obj_clear_flag(topBar, LV_OBJ_FLAG_SCROLLABLE);

    clockLabel = lv_label_create(topBar);
    lv_label_set_text(clockLabel, "00:00 AM");
    lv_obj_align(clockLabel, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(clockLabel, lv_color_hex(0x27488F), 0);

    // Simple shift, not a repositioning relative to the top bar's edge:
    // was too close to the clock at offset -1, moved 3px right per
    // explicit request. Clock stays exactly where it always was.
    loadingSpinner = lv_spinner_create(topBar, 1000, 90);
    lv_obj_set_size(loadingSpinner, 14, 14);
    lv_obj_set_style_arc_width(loadingSpinner, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(loadingSpinner, 2, LV_PART_MAIN);
    lv_obj_align_to(loadingSpinner, clockLabel, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_add_flag(loadingSpinner, LV_OBJ_FLAG_HIDDEN);
    // Default to Today's tint - Today's own boot-time fetch (todayInit()'s
    // forceFetch=true) can show this spinner before the tab-switch handler
    // below ever fires (Profile is the default active tab at boot), so it
    // needs a sane initial color rather than LVGL's own default.
    setLoadingSpinnerColor(kTodayTintColor);

    wifiIcon = lv_img_create(topBar);
    lv_img_set_src(wifiIcon, &wifi_disconnected_icon);
    // Same blue as the icon's own pixels - makes it a style colour, so night
    // mode (night_mode.cpp) can lighten it.
    lv_obj_set_style_img_recolor(wifiIcon, lv_color_hex(0x27488F), 0);
    lv_obj_set_style_img_recolor_opa(wifiIcon, LV_OPA_COVER, 0);
    lv_obj_align(wifiIcon, LV_ALIGN_RIGHT_MID, 0, 0);

    statsLabel = lv_label_create(topBar);
    lv_label_set_text(statsLabel, "");
    // Back to 14 (LV_FONT_DEFAULT, same as before any of this) - 13 isn't
    // available without generating a custom font asset, and 12 wasn't
    // wanted either.
    lv_obj_set_style_text_font(statsLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(statsLabel, lv_color_hex(0x27488F), 0);
    repositionStatsLabel();

    tabview = lv_tabview_create(scr, LV_DIR_BOTTOM, tabBarHeight);
    lv_obj_set_pos(tabview, 0, topBarHeight);
    lv_obj_set_size(tabview, screenWidth, screenHeight - topBarHeight);

    // lv_tabview's default tab switch (both the tab-button click handler and
    // the swipe-release snap) scrolls with LV_ANIM_ON. The manual
    // lv_refr_now()-driven loop (see loop()) doesn't service LVGL's
    // animation timer the way it expects, so that animated scroll never
    // visibly completes: a button tap silently did nothing (its entire
    // scroll distance relies on the animation), while a swipe *looked*
    // mostly fine only because direct finger-drag movement - not the
    // animation - already carried most of the distance before the broken
    // animated "snap" was even attempted. Fix: re-apply the already-correct
    // active tab index with LV_ANIM_OFF (instant) right after either
    // trigger, then force a redraw so it's actually drawn.
    lv_obj_add_event_cb(
        lv_tabview_get_tab_btns(tabview), [](lv_event_t *e)
        {
            lv_obj_t *tv = (lv_obj_t *)lv_event_get_user_data(e);
            lv_tabview_set_act(tv, lv_tabview_get_tab_act(tv), LV_ANIM_OFF);
            lv_obj_invalidate(lv_scr_act());
            lv_refr_now(NULL); },
        LV_EVENT_VALUE_CHANGED, tabview);
    lv_obj_add_event_cb(
        tabview, [](lv_event_t *e)
        {
            lv_obj_t *tv = (lv_obj_t *)lv_event_get_user_data(e);
            lv_tabview_set_act(tv, lv_tabview_get_tab_act(tv), LV_ANIM_OFF);
            lv_obj_invalidate(lv_scr_act());
            lv_refr_now(NULL); },
        LV_EVENT_VALUE_CHANGED, tabview);

    // Today's agenda is fetched on tab-show rather than at boot (its own
    // explicit request), so something needs to notice when it *becomes*
    // the active tab - tab index 1 in the add-tab order just below.
    lv_obj_add_event_cb(
        tabview, [](lv_event_t *e)
        {
            lv_obj_t *tv = (lv_obj_t *)lv_event_get_user_data(e);
            if (lv_tabview_get_tab_act(tv) == kTodayTabIndex)
                todayOnTabShown(); },
        LV_EVENT_VALUE_CHANGED, tabview);

    // Auto-refresh on both tabs is pointless while the user is looking at
    // a different tab - tell each tab whether it's the one currently
    // shown, so their own periodic timers can skip firing otherwise.
    lv_obj_add_event_cb(
        tabview, [](lv_event_t *e)
        {
            lv_obj_t *tv = (lv_obj_t *)lv_event_get_user_data(e);
            uint32_t act = lv_tabview_get_tab_act(tv);
            todaySetTabActive(act == kTodayTabIndex);
            busSetTabActive(act == kBusTabIndex); },
        LV_EVENT_VALUE_CHANGED, tabview);

    // Settings are batched in NVS rather than written on every slider/
    // stepper interaction (flash wear) - flush them once, here, the moment
    // the user navigates away from the Settings tab. Tracks the previously
    // active tab in a static local so "just left Settings" can be detected
    // from a single VALUE_CHANGED event (which only reports the new tab).
    lv_obj_add_event_cb(
        tabview, [](lv_event_t *e)
        {
            static uint32_t prevAct = kSettingsTabIndex + 1; // any non-Settings start value
            lv_obj_t *tv = (lv_obj_t *)lv_event_get_user_data(e);
            uint32_t act = lv_tabview_get_tab_act(tv);
            if (prevAct == kSettingsTabIndex && act != kSettingsTabIndex)
                settingsOnTabLeave();
            prevAct = act; },
        LV_EVENT_VALUE_CHANGED, tabview);

    profileInit(lv_tabview_add_tab(tabview, "Profile"));
    todayInit(lv_tabview_add_tab(tabview, "Today"));
    busInit(lv_tabview_add_tab(tabview, "Bus"));
    askInit(lv_tabview_add_tab(tabview, "Ask"));
    // A genuinely empty "" name collides with lv_btnmatrix's own map-parsing
    // convention, which uses an empty string as an end-of-array terminator
    // (lv_btnmatrix.c: allocate_btn_areas_and_controls() stops counting
    // buttons at the first map[i][0]=='\0') - lv_tabview_add_tab() always
    // appends its own trailing "" terminator after this name, so a "" name
    // here creates a second, premature one and undercounts the tab buttons.
    // A single space sidesteps that while staying invisible, since the
    // built-in button text is already hidden globally below.
    settingsInit(lv_tabview_add_tab(tabview, " "));

    lv_obj_t *tabBtns = lv_tabview_get_tab_btns(tabview);
    // lv_btnmatrix's own built-in button text is replaced by addTabIcon()'s
    // own icon+label pair (see its comment for why) - hide the built-in
    // text rather than removing it from the map, so tab identity/click
    // handling (which is index-based) is untouched.
    lv_obj_set_style_text_opa(tabBtns, LV_OPA_TRANSP, LV_PART_ITEMS);

    // Settings gets a narrower, icon-only button - the other 4 share the
    // rest of the bar equally. Widths are relative units consumed by both
    // lv_btnmatrix_set_btn_width() (actual button hit-area) and the column
    // math below (icon position) so the two can't disagree.
    static const int kTabWidthUnits[5] = {2, 2, 2, 2, 1};
    static const int kTabCount = 5;
    int totalUnits = 0;
    for (int i = 0; i < kTabCount; i++)
        totalUnits += kTabWidthUnits[i];
    for (int i = 0; i < kTabCount; i++)
        lv_btnmatrix_set_btn_width(tabBtns, i, kTabWidthUnits[i]);

    int cumulativeUnits = 0;
    lv_coord_t columnLeftX[kTabCount];
    lv_coord_t columnWidth[kTabCount];
    for (int i = 0; i < kTabCount; i++)
    {
        columnLeftX[i] = (lv_coord_t)((long)screenWidth * cumulativeUnits / totalUnits);
        cumulativeUnits += kTabWidthUnits[i];
        // Last column absorbs any rounding remainder so the columns always
        // sum to exactly screenWidth with no gap at the right edge.
        lv_coord_t rightEdge = (i == kTabCount - 1) ? screenWidth : (lv_coord_t)((long)screenWidth * cumulativeUnits / totalUnits);
        columnWidth[i] = rightEdge - columnLeftX[i];
    }

    addTabIcon(tabBtns, columnLeftX[0], columnWidth[0], "Profile", &tab_icon_person);
    addTabIcon(tabBtns, columnLeftX[1], columnWidth[1], "Today", &tab_icon_calendar);
    addTabIcon(tabBtns, columnLeftX[2], columnWidth[2], "Bus", &tab_icon_bus);
    addTabIcon(tabBtns, columnLeftX[3], columnWidth[3], "Ask", &tab_icon_mic);
    lv_obj_t *settingsIcon = addTabIcon(tabBtns, columnLeftX[4], columnWidth[4], "", &tab_icon_settings);
    // This tab's own tint, per explicit request - the other icons
    // (including Ask's) keep their original colors (no recolor) as
    // established, this one is the deliberate exception.
    lv_obj_set_style_img_recolor(settingsIcon, lv_color_hex(0x27488F), 0);
    lv_obj_set_style_img_recolor_opa(settingsIcon, LV_OPA_COVER, 0);
}

// mbedTLS allocator. This prebuilt SDK has CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC,
// so every TLS connection needed two separate ~16.7KB record buffers from
// internal RAM. With ~64KB free but the largest block ~32.7KB, the second
// buffer didn't fit - real log: every HTTPS call on every tab failing with
// "SSL - Memory allocation failed" right after a fresh boot. Large
// allocations (the record buffers) now go to PSRAM, which ESP-IDF supports
// for mbedTLS (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC); small ones stay internal.
static void *tlsCalloc(size_t n, size_t size)
{
    void *p = nullptr;
    if (n * size >= 2048)
        p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
    if (!p)
        p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

static void tlsFree(void *p)
{
    heap_caps_free(p); // handles both internal and PSRAM blocks
}

void setup()
{
    Serial.begin(115200);
    delay(300); // let the USB-CDC host enumerate before we print anything
    Serial.println("\n=== ESP32Bus boot v" FIRMWARE_VERSION ", built " __DATE__ " " __TIME__ " ===");

    // Before any TLS connection exists - see tlsCalloc().
    mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);

    // Must exist before any fetch task could possibly start (bus/today/
    // bus_stops all spin up background tasks later in this function) - see
    // net_lock.h for why this serialization exists.
    appConfigLoad(); // runtime API keys/URLs (web settings page) before anything fetches
    statusLedInit();
    netLockInit();
    // Single serialized Network Worker (network_worker.h) - must also exist
    // before buildUi() below, since todayInit()/busInit() can trigger an
    // immediate boot-time fetch that submits a job to it right away.
    networkWorkerInit();

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    backlightInit();
    settingsLoadPersisted();
    nightModeInit();
    weatherLoadSettings();
    wakeWordInit(); // "Jarvis" listener - needs the model partition (readme.txt)

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // Always the unflipped rotation (1): a saved 180-degree flip already set the
    // TFT to 3, and my_touchpad_read() mirrors touch itself - passing 3 here
    // would flip touch twice.
    touch_init(screenWidth, screenHeight, 1);
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    touchIndev = lv_indev_drv_register(&indev_drv);

    buildUi();
    lv_timer_handler();

    backlightSetOn(true);

    // wifi_manager.cpp: single saved network (the SoftAP-provisioned one if
    // the Settings tab's Wi-Fi button has ever been used, else secrets.h's
    // default), reconnected on its own task so it never blocks LVGL.
    wifiManagerInit();

    // Singapore: GMT+8, no daylight saving.
    configTime(8 * 3600, 0, "pool.ntp.org");

    busStopsInit();

    esp_register_freertos_idle_hook_for_cpu(idleHookCore0, 0);
    esp_register_freertos_idle_hook_for_cpu(idleHookCore1, 1);

    powerInit();

    // Shown immediately, covering the Profile tab buildUi() just built -
    // per explicit request, this is the very first thing seen at boot.
    weatherInit();

    Serial.printf("PSRAM: found=%d, total=%u bytes, free=%u bytes\n", psramFound(), ESP.getPsramSize(), ESP.getFreePsram());
    Serial.println("setup() done");
}

// Debug-only Serial command handler. 'replay' and 'dump' (the original
// reasons this existed - see git history) were removed once the Ask tab
// got its own in-UI replay button (ask.cpp's replayIconClicked()), which
// covers the same "hear the last answer again with no network call" need
// without needing a serial connection. 'tone' is kept - it's a pure,
// mathematically-known-correct signal (no network/SPIFFS/TTS involved at
// all) for isolating playback-code bugs from downloaded-audio bugs, which
// the UI has no equivalent for.
static void handleSerialDebugCommands()
{
    if (!Serial.available())
        return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "heap")
    {
        Serial.printf("heap: internal free %u, largest %u, min-ever %u\n",
                      heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        return;
    }
    // Drive Wi-Fi provisioning without the touchscreen, for leak testing.
    if (cmd == "prov")
    {
        wifiProvisioningStart();
        return;
    }
    if (cmd == "ota")
    {
        otaCheckNow();
        return;
    }
    if (cmd == "night") // preview the night look/brightness until the next "night"
    {
        nightModeToggleTest();
        return;
    }
    // Screenshot: full redraw into shotBuf, then "SHOT <w> <h>\n" + raw
    // little-endian RGB565 over serial. See docs/screenshot.py.
    if (cmd == "shot")
    {
        shotBuf = (uint16_t *)ps_malloc(screenWidth * screenHeight * 2);
        if (!shotBuf)
            return;
        lv_obj_invalidate(lv_scr_act());
        lv_obj_invalidate(lv_layer_top());
        lv_refr_now(NULL);
        uint16_t *frame = shotBuf;
        shotBuf = nullptr;
        Serial.printf("SHOT %d %d\n",screenWidth, screenHeight);
        Serial.write((const uint8_t *)frame, screenWidth * screenHeight * 2);
        Serial.flush();
        free(frame);
        return;
    }
    if (cmd.startsWith("tab ")) // tab <index>: switch tab (closes the weather screen)
    {
        weatherHide();
        uiShowTab(cmd.substring(4).toInt());
        return;
    }
    // mode auto|night|day: screen mode (persisted, like the web page);
    // "mode" alone prints the current one.
    if (cmd == "mode" || cmd.startsWith("mode "))
    {
        static const char *names[] = {"auto", "night", "day"};
        String arg = cmd.substring(5);
        for (int i = 0; i < 3; i++)
            if (arg == names[i])
                nightModeSetSetting((NightModeSetting)i);
        Serial.printf("MODE %s\n", names[nightModeGetSetting()]);
        return;
    }
    if (cmd == "weather")
    {
        weatherShow();
        return;
    }
    if (cmd == "version")
    {
        Serial.println("firmware " FIRMWARE_VERSION);
        return;
    }
    if (cmd == "provcancel")
    {
        wifiProvisioningCancel();
        return;
    }
    // One TLS round-trip to the same host the Ask tab uses (401 expected, no
    // key sent) - checks reachability without needing the mic.
    if (cmd == "nettest")
    {
        WiFiClientSecure client;
        client.setInsecure();
        unsigned long t0 = millis();
        bool ok = client.connect("api.openai.com", 443, 8000);
        unsigned long tConn = millis() - t0;
        int status = -1;
        if (ok)
        {
            client.print("GET /v1/models HTTP/1.1\r\nHost: api.openai.com\r\nConnection: close\r\n\r\n");
            String line = client.readStringUntil('\n');
            if (line.startsWith("HTTP/1.1 "))
                status = line.substring(9, 12).toInt();
        }
        client.stop();
        Serial.printf("nettest: connect %s in %lums, HTTP %d, total %lums, local %s gw %s dns %s mode %d\n",
                      ok ? "ok" : "FAILED", tConn, status, millis() - t0, WiFi.localIP().toString().c_str(),
                      WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(), (int)WiFi.getMode());
        return;
    }
    if (cmd == "spifftest")
    {
        // Isolated SPIFFS write test - no network, no OpenAI call, no
        // ask.cpp involvement at all, so this can be run repeatedly for
        // free while chasing the real "wrote 0 of N bytes" bug (which has
        // survived return-value checking, chunking, a settle delay, and an
        // explicit remove() first - none of those actually fixed it).
        // Uses raw POSIX open()/write()/close() against the real mount
        // path (/spiffs/..., confirmed from the installed SPIFFS.cpp's
        // default basePath) instead of the Arduino File wrapper, which
        // swallows the real errno and just reports "0 bytes written" with
        // no explanation. errno after a failed write() tells us definitively
        // what's actually happening (ENOSPC = truly out of usable space,
        // EIO = a real I/O-level failure, etc.) instead of guessing again.
        const size_t testSizes[] = {100, 4096, 50000, 208800};
        for (size_t sz : testSizes)
        {
            uint8_t *buf = (uint8_t *)ps_malloc(sz);
            if (!buf)
            {
                Serial.printf("spifftest: ps_malloc(%u) failed, skipping\n", (unsigned)sz);
                continue;
            }
            memset(buf, 0xAA, sz);

            errno = 0;
            FILE *f = fopen("/spiffs/spifftest.bin", "w");
            if (!f)
            {
                Serial.printf("spifftest: size=%u fopen() failed, errno=%d (%s)\n", (unsigned)sz, errno, strerror(errno));
                free(buf);
                continue;
            }
            errno = 0;
            size_t written = fwrite(buf, 1, sz, f);
            int writeErrno = errno;
            int closeResult = fclose(f);
            free(buf);

            Serial.printf("spifftest: size=%u wrote=%u errno=%d (%s) fclose=%d SPIFFS_used=%u SPIFFS_total=%u heap=%u\n",
                          (unsigned)sz, (unsigned)written, writeErrno, strerror(writeErrno), closeResult,
                          (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes(), (unsigned)ESP.getFreeHeap());
        }
        remove("/spiffs/spifftest.bin");
    }
    else if (cmd == "tone")
    {
        // Pure, mathematically-known-correct 1kHz sine wave, 2 seconds at
        // 24kHz - no network, no SPIFFS, no TTS involved at all. Isolates
        // whether speakerPlayPcm() itself (codec init, I2S config, stereo
        // duplication) can faithfully reproduce known-good audio,
        // independent of whether the downloaded TTS data is good or bad.
        // If this also sounds distorted, the bug is in our playback code,
        // not the downloaded audio - added because a real user report
        // showed the SAME saved file sounding bad on every replay
        // (deterministic, not random), which could mean either source.
        const uint32_t sr = 24000;
        const float freqHz = 1000.0f;
        const uint32_t durationSec = 2;
        size_t sampleCount = sr * durationSec;
        int16_t *tone = (int16_t *)ps_malloc(sampleCount * sizeof(int16_t));
        if (!tone)
        {
            Serial.println("debug: tone ps_malloc failed");
            return;
        }
        for (size_t i = 0; i < sampleCount; i++)
        {
            float t = (float)i / (float)sr;
            tone[i] = (int16_t)(10000.0f * sinf(2.0f * PI * freqHz * t)); // ~30% of full scale, avoids clipping
        }
        Serial.println("debug: playing 1kHz test tone (2s, pure sine, no network/SPIFFS involved)");
        speakerPlayPcm(tone, sampleCount, sr);
        free(tone);
    }
}

void loop()
{
    powerTick();
    handleSerialDebugCommands();

    lv_timer_handler();
    // LVGL's own timers (display refresh, indev read) don't fire reliably
    // driven this way on this board, so force both every iteration instead
    // of waiting on the internal timer schedule.
    lv_indev_read_timer_cb(touchIndev->driver->read_timer);

    busTick();
    busStopsTick();
    todayTick();
    askTick();
    weatherTick();
    settingsTick();
    otaTick();
    statusLedTick();
    webConfigTick();
    nightModeTick(); // last before the refresh - see its comment

    lv_refr_now(NULL);

    static bool wifiLoggedConnected = false;
    if (!wifiLoggedConnected && WiFi.status() == WL_CONNECTED)
    {
        wifiLoggedConnected = true;
        Serial.printf("Wi-Fi: connected, IP %s, RSSI %ddBm\n",
                       WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }

    static unsigned long lastStatsCheck = 0;
    if (millis() - lastStatsCheck > 1000)
    {
        unsigned long windowMs = millis() - lastStatsCheck;
        lastStatsCheck = millis();
        lastFps = flushCount;
        flushCount = 0;

        // Expected idle-hook ceiling for this window if that core were
        // 100% idle the whole time - the hook fires at most once per
        // tick (CONFIG_FREERTOS_HZ=1000, i.e. 1 tick/ms).
        uint32_t ticksInWindow = (uint32_t)windowMs;
        for (int core = 0; core < 2; core++)
        {
            uint32_t idleCalls = idleHookCount[core];
            idleHookCount[core] = 0;
            uint32_t idlePct = (ticksInWindow > 0) ? (uint32_t)min(100UL, (idleCalls * 100UL) / ticksInWindow) : 100;
            lastCpuPct[core] = 100 - idlePct;
        }

        // "Used%", not "free%" - matches how every desktop task manager
        // reports it, unlike the raw ESP.getFreeHeap()-based figure this
        // used to show.
        unsigned heapUsedPct = (unsigned)(((ESP.getHeapSize() - ESP.getFreeHeap()) * 100UL) / ESP.getHeapSize());
        unsigned psramTotal = ESP.getPsramSize();
        unsigned psramUsedPct = psramTotal > 0
                                     ? (unsigned)(((psramTotal - ESP.getFreePsram()) * 100UL) / psramTotal)
                                     : 0;

        uint32_t bytesThisWindow = networkBytesSinceCheck;
        networkBytesSinceCheck = 0;
        float bytesPerSec = bytesThisWindow / (windowMs / 1000.0f);

        char rateBuf[16];
        if (bytesPerSec < 1024.0f)
            snprintf(rateBuf, sizeof(rateBuf), "%.0fB/s", bytesPerSec);
        else
            snprintf(rateBuf, sizeof(rateBuf), "%.1fKB/s", bytesPerSec / 1024.0f);

        char statsBuf[48];
        snprintf(statsBuf, sizeof(statsBuf), "CPU %lu%% %lu%%  RAM %u%% %u%%  %luFPS  %s",
                 (unsigned long)lastCpuPct[0], (unsigned long)lastCpuPct[1],
                 heapUsedPct, psramUsedPct, (unsigned long)lastFps, rateBuf);
        lv_label_set_text(statsLabel, statsBuf);
        repositionStatsLabel();

        lv_img_set_src(wifiIcon, WiFi.status() == WL_CONNECTED
                                      ? &wifi_connected_icon
                                      : &wifi_disconnected_icon);

        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0))
        {
            char timeBuf[16];
            strftime(timeBuf, sizeof(timeBuf), "%I:%M %p", &timeinfo);
            lv_label_set_text(clockLabel, timeBuf);
        }
    }

    delay(5);
}
