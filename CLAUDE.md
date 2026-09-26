# ESP32Bus

LTA (Singapore) bus-arrival display on an ESP32-S3 + 4.0" TFT (TFT_eSPI) + FT6336
capacitive touch, UI built with LVGL 8.3.6. Shows a bottom tab bar (Profile /
Today / Bus / Ask) with a top status strip (clock, Wi-Fi RSSI, RAM, FPS).

## Board identity (important - don't guess this again)

- Board: LCDWiki 4.0" ESP32-S3 Display. Vendor product page (confirmed,
  don't re-derive this from scratch - it also has the full GPIO table used
  throughout this doc): https://www.lcdwiki.com/4.0inch_ESP32-S3_Display
- Chip: ESP32-S3 (QFN56, rev v0.2), **Embedded Octal PSRAM 8MB** (in-package,
  not an external chip), 16MB flash, Quad SPI flash mode (per eFuse).
- USB: native **USB-Serial/JTAG** (built into the chip, not an external
  CP210x/CH340 bridge). This matters a lot for debugging - see below.
- Vendor reference: lcdwiki.com's "ESP32 Arduino IDE development environment"
  PDF for this display explicitly specifies **PSRAM: OPI PSRAM** and shows a
  dedicated "ESP32S3 Dev Module Octal (WROOM2)" board entry in Arduino IDE.
  PlatformIO's plain `esp32-s3-devkitc-1` board profile defaults to "No
  PSRAM" - always override this (see `platformio.ini`).
- **Onboard audio hardware exists** (real mic + codec + speaker amp - don't
  ask whether this board has a microphone, it does, this was confirmed
  directly from the vendor page above): ES8311 codec over I2C (address
  `0x18`, CE pin low), MEMS mic (`LMA2718B381-OA7`) feeding the codec's ADC,
  speaker amp (`FM8002E`, unused - no TTS/voice-output feature exists).
  I2C is shared with the FT6336 touch controller (SDA=IO16, SCL=IO15,
  already brought up by `lib/FT6336-arduino/FT6336.cpp`'s `Wire.begin()` -
  don't call `Wire.begin()` again). I2S: MCLK=IO4, BCLK=IO5, WS=IO7,
  **DIN(mic)=IO6, DOUT(speaker)=IO8** - the vendor's own PDF has these
  backwards (says DO=IO6/DI=IO8); trust this line, not that PDF, and see
  the Ask tab's debugging postmortem below for how that was found and
  confirmed against the vendor's actual working reference firmware. MCLK
  runs at 384x the 16kHz sample rate (6.144MHz), also confirmed against
  that same reference firmware. Codec enable pin: IO1,
  **active-low** (low = enabled, high = disabled). See `src/app/es8311.*`
  (ported directly from Espressif's own official open-source ES8311 driver,
  not a third-party wrapper - see that file's header comment for why) and
  `src/app/mic_capture.*` for the actual bring-up code, used by the Ask tab.

## Required non-default build config (platformio.ini `[env]`)

These are all load-bearing. If a future change strips them out while
"cleaning up" the ini, expect the exact symptoms described in the postmortem
below to come back.

- `monitor_dtr = 1` - **must be 1, not 0.** With DTR forced inactive, the
  ROM's very first raw boot bytes still print (they ignore DTR), but every
  byte from the app or the 2nd-stage bootloader's own log gets silently
  dropped by the native USB-Serial/JTAG console driver, which gates on a
  connected host. This looks exactly like a boot hang if you only judge by
  serial silence. Don't be fooled again - see postmortem.
- `-D ARDUINO_USB_CDC_ON_BOOT=1` - brings the application-level USB-CDC
  console up at boot instead of leaving it in a state where the port
  enumerates and `esptool` works fine, but `Serial.print()` from your sketch
  is silently dropped. This was the actual fix that made `Serial` output
  appear at all.
- `board_build.arduino.memory_type = qio_opi` + `-D BOARD_HAS_PSRAM` -
  correct config for this chip's Quad-flash + Octal-PSRAM combo package.
  Objectively correct per the vendor doc, but turned out NOT to be the cause
  of the "hang" scare (see postmortem) - keep it anyway, it's still right.
- `board_build.flash_size = 16MB` / `board_upload.flash_size = 16MB` - actual
  flash size; the board profile defaults to 8MB.

## Debugging postmortem: the "dead board" false alarm

**tl;dr: the board was never broken. Don't jump to "hardware fault" just
because Serial is silent on this specific chip - its USB-Serial/JTAG console
has real, easy-to-hit gotchas that look identical to a dead chip.**

What happened, for ~2 hours of a session: `Serial.begin()` was missing from
the sketch entirely, so the user saw no serial output and a blank Bus tab.
While adding diagnostics, every reset (RTS-pulse, esptool's own reset, and
even a genuine physical power-cycle) showed the ROM's boot banner and then
**total silence**, always stopping at the exact same instruction
(`entry 0x403c9...`). This was misread as the chip hanging immediately after
the ROM-to-bootloader handoff. Ruled out (all genuinely, all necessary,
none of it was the actual bug): QIO vs DIO flash mode, different USB
cable, different USB port, full chip erase + reflash of a factory backup
image, default vs Octal-PSRAM memory_type. Conclusion at the time: hardware
failure, recommended replacement.

**That conclusion was wrong.** JTAG (via the chip's built-in USB-JTAG
interface - needed a one-time Zadig WinUSB driver bind on the JTAG
sub-interface, see below) proved the CPU was fine the whole time: halting it
showed `PC` sitting in `esp_pm_impl_waiti` (FreeRTOS's normal idle-task wait
loop) with the cycle counter (`ccount`) actively incrementing across
halt/resume samples. The "`Saved PC`" field the ROM prints on every boot is
*not* the current boot's entry point - it's where the CPU was executing at
the moment of the *previous* reset, which is exactly why it kept matching
the idle-loop address. The chip was booting, running, and idling completely
normally the entire time; only the serial console output was broken (root
cause: `monitor_dtr`/`ARDUINO_USB_CDC_ON_BOOT`, above). Confirmed once fixed:
touch, Wi-Fi, NTP clock, and general LVGL rendering all worked fine even
before any of the serial fixes landed - the screen was reacting correctly
the whole time and nobody had checked.

**Lesson for next time:** on this board, silence on Serial is *not*
sufficient evidence of a boot failure. Before concluding hardware fault,
check independent signals that don't route through the USB-CDC console -
does the display draw anything, does touch respond, does Wi-Fi status
change - or attach JTAG and check if `ccount`/`PC` are actually moving.

### JTAG setup notes (if needed again)

- `openocd` from `tool-openocd-esp32` works against
  `board/esp32s3-builtin.cfg` (the chip's own USB-JTAG, no external
  probe needed).
- On Windows, the JTAG sub-interface of the composite USB device needs a
  WinUSB driver bound via **Zadig** (Options -> List All Devices, then pick
  the specific `Interface 2` entry - *not* the CDC/serial interface, which
  must stay on its existing driver or COM-port flashing breaks). VID/PID is
  `303A:1001` for this chip family.
- `openocd -f board/esp32s3-builtin.cfg`, then telnet to port 4444
  (`reset` / `halt` / `reg pc` / `reg ccount` / `resume`) is enough to sanity
  check "is the CPU actually alive" independent of Serial.

## Progress / status

- [x] Base LVGL shell: top status bar + bottom tab bar (Profile / Today /
      Bus / Ask), touch via FT6336. Working.
- [x] Profile tab: fully built out (`src/app/profile.cpp`), real content from
      the user's own business card, **user-confirmed working and liked**
      ("Perfect!!"). Layout: a `photoCol` (photo + QR, flex COLUMN) on the
      left and an `info` column (name/title/org/specialty/divider/contact
      rows, flex COLUMN) on the right, both children of a ROW-flow tab, plus
      a decorative background image positioned absolutely behind everything.
      - Photo: `profile_pic.c/.h`, 180x180 (20px larger than the original
        160x160 - explicitly requested), circular (`LV_RADIUS_CIRCLE` +
        `clip_corner`), converted from `icons/zainul_profile_pic.png` via
        LANCZOS resize.
      - QR code: tap-to-enlarge. Small version (`qr_small.c/.h`, 56x56)
        sits under the photo; tapping it opens a full-screen overlay
        (`showQrOverlay()`) on `lv_layer_top()` (renders above the tab
        bar/status strip regardless of active tab) showing the large
        version (`qr_large.c/.h`, 230x230, corners rounded 5px), with a
        "Tap anywhere to close" hint; tapping the backdrop calls
        `lv_obj_del()` on the overlay. Both generated from
        `icons/qrcode_v2.png` (782x782 source - the original `qrcode.png`
        was only 49x49 and visibly pixelated once enlarged, so the user
        supplied a high-res replacement). **Real bug fixed here**:
        `lv_obj_set_flex_align(photoCol, ...)` had `LV_FLEX_ALIGN_START` in
        the cross-place (2nd) argument and `LV_FLEX_ALIGN_CENTER` in the
        track-place (3rd, irrelevant for a single column) - for a COLUMN
        flow the 2nd argument is what actually controls horizontal
        centering, so the QR sat at the left edge instead of centered under
        the photo until the two were swapped.
      - Specialty lines ("Material characterization" / "AI-driven image
        analytics" / "Laboratory automation") live in their own nested
        flex-COLUMN sub-group (`createSpecialtyGroup()`) with a tight 1px
        row gap, separate from `info`'s normal 6px row gap, so they read
        visually as one block rather than three independent fields.
      - Divider line: LVGL has no "margin" concept (only padding, which
        doesn't create flex-gap space), so extra space above/below the 1px
        line is a slightly taller (13px) invisible wrapper box with the
        line centered inside it, not a style property on the line itself.
        Shortened over two rounds (50px, then another 20px, all trimmed
        from the right only via `pad_right` on the wrapper) to stay
        left-aligned with the text above/below it rather than centered.
      - Background image (`profile_bg.c/.h`, from `icons/background.png`):
        a plain `lv_img` child of the tab with `LV_OBJ_FLAG_IGNORE_LAYOUT`
        (so it's excluded from the tab's row-flex layout and positioned
        absolutely instead) and `lv_obj_align(bg, LV_ALIGN_TOP_RIGHT, 14,
        -14)` - the +14/-14 compensates for the tab's own 14px padding so
        it reaches the tab's true outer edges rather than sitting inset
        within the padded content area. **Real bug hit and fixed here**:
        the first version resized the 90x292 source down to 90x242 (to fit
        the tab's content height) using a plain non-uniform resize, which
        squished the image vertically and visibly distorted it - "did not
        maintain the aspect ratio". Fixed (twice) by not stretching at
        all: first by cropping the source instead of resizing it (crop
        preserves 1:1 pixel scale), then simplified further when the user
        cropped/resized their own source image to the exact target
        80x242px and re-supplied it, so the conversion script now just
        converts that file directly with no resize/crop step at all.
      - Contact rows (`addContactRow()`): phone/email/website as
        icon+label flex ROWs, icon color blue (`0x1976d2`) per explicit
        request (originally default color). A GPS/location row was
        included in an early draft and then explicitly removed.
      - Tab content geometry, for future reference: the tab's outer box is
        `screenWidth x (screenHeight - topBarHeight - tabBarHeight)` =
        480x242; `profileInit()` then adds `pad_all(tab, 14)`, so the
        actual usable content area inside a tab is 452x214.
- [x] Top status strip rework: removed LVGL's built-in bottom-left perf
      monitor overlay (`LV_USE_PERF_MONITOR` now `0` in `lv_conf.h`).
      `statsLabel` now shows real `CPU %  RAM %  FPS` (FPS = actual
      `my_disp_flush()` calls/sec via `flushCount`, not loop() iterations;
      CPU% = measured busy time vs the loop's `delay(5)` idle time via
      `busyMicros`, genuinely meaningful since it now yields). Numeric dBm
      replaced with an actual Wi-Fi icon (`wifi_connected_icon` /
      `wifi_disconnected_icon`, rasterized from the user's own
      `icons/wifi_4_bar_*.svg` - see "SVG icon pipeline" below; note these
      are single wifi-glyph icons, NOT segmented bars, so there is no
      granular signal-strength level, just connected/disconnected).
      **User-confirmed both of these fixed and working**, including a real
      bug fix along the way: `statsLabel` was being positioned with
      `lv_obj_align_to()` only once at creation (while still empty), so as
      its text grew it kept extending off-screen instead of re-anchoring -
      fixed by calling `repositionStatsLabel()` after every text update, and
      documented as a pattern to watch for anywhere else dynamic-width
      labels are aligned relative to a sibling.
- [x] Bottom tab bar icons. **User-confirmed working** ("LOoks good now")
      after 4 attempts - kept here in full since three of the four were
      wrong in ways worth not repeating. **Attempt 1** (session 1): fetched Material
      Symbols SVGs, rasterized to white, recolored via LVGL `img_recolor` to
      cyan/teal/orange/purple, positioned *above* each button's text using a
      formula that assumed 4 equal-width columns spanning `screenWidth`.
      User called the on-device result "a complete mess" - never debugged,
      abandoned. **Attempt 2** (session 2, current): user supplied their own
      final 24x24 PNG icons instead (`icons/profile_icon_24x24.png`,
      `calendar_24x24.png`, `bus_single_24x24.png` - note there's also an
      unused `bus_double_24x24.png` in that folder, single was the one
      requested - and `microphone_24x24.png`), already colored, with
      explicit instructions: use their colors as-is (no `img_recolor`), and
      place icons to the *left* of each button's text, not above it.
      Rewrote `addTabIcon()` in `main.cpp` to read the button matrix's own
      real per-button layout directly
      (`((lv_btnmatrix_t *)tabBtns)->button_areas[index]`, forcing
      `lv_obj_update_layout()` first) instead of the guessed equal-column
      formula - but this made icons **completely invisible**, not just
      misplaced (user: "where is tab icons I do not see any all gone?").
      Root cause (near-certain, not 100% proven since it wasn't
      instrumented before being reverted): `button_areas` is populated by
      the button matrix's own internal layout pass, and
      `lv_obj_update_layout()` (which handles LVGL's generic flex/grid
      layout system) was never verified to actually trigger *that*, so the
      array was likely still zeroed/stale, producing garbage/off-screen
      coordinates that got clipped away entirely. **Fixed by removing that
      unverified internal-struct read and going back to plain column
      arithmetic** - but this time actually confirmed against
      `lv_tabview.c` itself (`lv_obj_set_size(btnm, LV_PCT(100), tabsize_create)`
      for a bottom tab bar, i.e. the button matrix is provably always
      exactly `screenWidth` wide with `totalTabs` equal columns, not merely
      assumed) rather than trusted on faith like the first attempt's
      formula was. Kept the one part of the discarded approach that *was*
      built on solid ground: `lv_txt_get_size()` (a plain documented
      text-measurement call, no layout-timing dependency) to find where
      each button's centered label text actually starts, so the icon sits
      immediately left of it with a small gap rather than at a fixed
      column offset that wouldn't account for each label's differing
      width. `tabBarHeight` (was an inline `50` literal passed to
      `lv_tabview_create`) pulled out to a named constant since the icon
      vertical-centering math needs it too. Icon PNGs converted straight to
      `lv_img_dsc_t` C arrays preserving their original RGBA colors exactly
      (no recolor step, per the user's instruction). **Attempt 3** (same
      session): user rightly called out that positioning the icon just to
      the left of the text left the icon+text *pair* looking off-center as
      a group, since `lv_btnmatrix` still centers the text alone across the
      button's full width regardless of where the icon sits next to it.
      Fix: `lv_obj_set_style_pad_left(tabBtns, kTabIconSize + kTabIconGap, LV_PART_ITEMS)`
      once on the whole button matrix, which shifts lv_btnmatrix's *own*
      text-centering rightward by exactly the icon's footprint (working
      with its layout instead of overriding it), then the icon's x is
      derived from that same padding value so icon+gap+text works out
      centered as one group. **This padding trick did not work** - user
      reported the label ended up hidden entirely underneath the icon.
      Root cause, this time actually confirmed by reading
      `lv_btnmatrix.c`'s draw function line by line (not just its header,
      which is what led to the wrong assumption before): the text-centering
      code there (`btn_area.x1 += (lv_area_get_width(&btn_area) - txt_size.x) / 2`)
      operates on the button's raw width from `button_areas[]` and never
      once consults `LV_PART_ITEMS` padding - so the padding call did
      *nothing*, the text never moved, and the icon (added afterward, drawn
      on top as a later child) simply landed directly over the
      still-centered, unmoved text.
      **Attempt 4** (same session, current): stopped trying to influence
      `lv_btnmatrix`'s own text rendering at all. `buildUi()` now hides the
      button matrix's built-in text completely
      (`lv_obj_set_style_text_opa(tabBtns, LV_OPA_TRANSP, LV_PART_ITEMS)`,
      not removed from the map, so tab identity/click handling - which is
      index-based - is untouched), and `addTabIcon()` creates its own
      `lv_label` for the button text alongside the icon, measuring the
      label's real rendered width via `lv_obj_update_layout()` +
      `lv_obj_get_width()` (both fully controlled, ordinary LVGL objects,
      not reading any of lv_btnmatrix's internal state) to center the
      icon+label pair as one group. **This is the fix that stuck** -
      confirmed correct on-device. Lesson that generalized well to the Bus
      tab ETA bug below: after three iterations of "reason from source,
      flash, guess wrong," the thing that actually ended the loop was
      verifying against the running device directly (`Serial.printf` of
      real computed values) instead of reasoning from source code alone -
      do that *first* for any future rendering/timing bug on this project,
      not as a last resort.
- [x] SVG icon pipeline (new capability, reusable going forward): this repo
      has no SVG renderer, so icons go SVG -> rasterized PNG -> LVGL C image
      array. Rasterization does NOT use cairosvg/svglib+reportlab's PM
      backend (both need a system libcairo-2.dll that isn't present on this
      Windows machine and isn't easily installable) - instead: parse the SVG
      with `svgelements` (pure Python), flatten each subpath's segments to
      line points via `.npoint()`, build a combined `matplotlib.path.Path`
      (multiple subpaths + MOVETO/LINETO/CLOSEPOLY codes), and rasterize
      with `Path.contains_points()` over a pixel grid - this correctly
      implements the nonzero winding fill rule these icons need (so rings/
      arcs render as rings, not solid blobs). Output as flat-color PNG
      (white, to be recolored later via LVGL's `img_recolor` style, or
      baked directly to a final color), then converted to an
      `lv_img_dsc_t` C array (RGB565 + alpha, matching `LV_IMG_CF_TRUE_COLOR_ALPHA`
      and this project's `LV_COLOR_DEPTH 16`) the same way `profile_pic.c`
      was generated. Material Symbols icons not already in `icons/` can be
      fetched directly (confirmed working) from
      `https://fonts.gstatic.com/s/i/short-term/release/materialsymbolsoutlined/<name>/default/24px.svg`.
      A real, previously-latent build bug was found and fixed while adding
      non-default font sizes for the Profile tab: `lib/lvgl`'s own sources
      (e.g. font files) don't automatically get the project's `include/`
      dir the way `src/` files do, so `lv_conf_internal.h`'s auto-detect
      silently fell back to conservative defaults there (only the default
      font compiled in) even though `-D LV_CONF_INCLUDE_SIMPLE` was set -
      fixed by adding `-Iinclude` to the global `build_flags`.
- [x] Wi-Fi + NTP time sync. Working.
- [x] `Serial` / USB-CDC console output. Fixed (`monitor_dtr = 1` +
      `ARDUINO_USB_CDC_ON_BOOT=1`).
- [x] Bus tab data fetch (`bus.cpp`, LTA DataMall v3 BusArrival API). Fetch
      itself works (confirmed via serial: HTTP 200, services parsed).
- [x] Bus tab ETA countdown - was permanently showing "Arr" for every bus
      regardless of real arrival time ("173 Arr(s) Arr(S)... every bus can
      not be just ARR all the time"). **Root cause, verified on-device by
      printing `time(nullptr)` and the parsed ETA epoch side by side (not
      guessed from reading the code)**: `parseIso8601ToUtcEpoch()` parsed
      LTA's ISO8601 timestamp (e.g. `"2026-09-17T00:06:24+08:00"`) via
      `mktime()`, on the documented assumption that "mktime with no TZ set
      treats the fields as already UTC" - true in isolation, but this
      project's own `setup()` calls
      `configTime(8 * 3600, 0, "pool.ntp.org")`, which sets the device's TZ
      to Singapore (UTC+8). `mktime()` honors that TZ, so it was already
      silently converting the parsed fields from SGT to UTC on its own;
      the code then subtracted the ISO string's own `+08:00` offset
      *again*, applying the same 8h shift twice. Every ETA landed ~8h in
      the past, `mins` permanently clamped to 0, hence "Arr" forever.
      Confirmed with real numbers: raw ETA "2026-09-17T00:06:24+08:00"
      (~8 minutes away) was parsing to an epoch 7h52m *before*
      `time(nullptr)`. Fix: added `daysFromCivil()` (Howard Hinnant's
      "days from civil" calendar-arithmetic algorithm) as a genuinely
      TZ-independent replacement for the UTC half of the conversion -
      `timegm()` would be the standard fix but isn't available in this
      toolchain's newlib (`'timegm' was not declared in this scope`).
      Verified fixed on-device: bus 173's two ETAs now compute to 274s
      (4.6 min) and 1590s (26.5 min) ahead of `time(nullptr)`, matching the
      raw JSON exactly. **General lesson, same as the tab-icon saga
      above**: a comment explaining *why* code does something can go stale
      the moment something elsewhere in the file changes (here,
      `configTime`'s TZ argument) without the comment being revisited -
      don't trust an existing "why" comment as proof of correctness, verify
      against real device output when a symptom shows up. Debug technique
      worth reusing: `bus.cpp`'s `fetchBusData()` now does
      `String body = http.getString(); Serial.println(body);` before
      `deserializeJson()`, so the exact raw API response is always in the
      serial log - added because the fastest way to root-cause this kind
      of bug is comparing real API output against real parsed values, not
      re-reading the parsing code under suspicion.
- [x] Bus tab *display* + tab switching in general. Fixed and confirmed
      working (both tap-to-switch and swipe now jump to the correct page and
      show live content, including real bus arrival rows on the Bus tab).
      Root cause (found by reading `lib/lvgl/src/extra/widgets/tabview/lv_tabview.c`
      directly): LVGL's tabview switches tabs via an *animated*
      (`LV_ANIM_ON`) scroll for both the tab-button click handler
      (`btns_value_changed_event_cb`) and the swipe-release snap
      (`cont_scroll_end_event_cb`). This project's `loop()` drives LVGL with
      a manual `lv_timer_handler()` + `lv_indev_read_timer_cb()` +
      `lv_refr_now(NULL)` sequence instead of leaning on LVGL's own internal
      refresh timer, and that doesn't service LVGL's animation timer the way
      `lv_tabview` expects - so the animated scroll never visibly completed.
      A tap (whose entire scroll distance depends on that animation) did
      nothing visible; a swipe *looked* like it mostly worked only because
      direct finger-drag movement (not the animation) already carried most
      of the distance before the broken animated "snap" at the end.
      Fix: added `LV_EVENT_VALUE_CHANGED` handlers on both
      `lv_tabview_get_tab_btns(tabview)` (fires on button click) and the
      tabview itself (fires on swipe completion) that re-apply
      `lv_tabview_set_act(tv, lv_tabview_get_tab_act(tv), LV_ANIM_OFF)` -
      i.e. instantly re-snap to the already-correct target tab - followed by
      `lv_obj_invalidate(lv_scr_act())` + `lv_refr_now(NULL)`. Trade-off:
      tab switches now jump instantly instead of sliding smoothly: fine for
      this project, but worth knowing if a future ask is "make tab
      switching animate smoothly" - that would mean fixing the underlying
      animation-timer starvation instead of working around it like this.
- [x] Bus tab full redesign (`src/app/bus.cpp`, `src/app/bus_stops.cpp/.h`).
      **User-confirmed complete** ("bus tab is complete now"). Replaced the
      original single-column list with: a left sidebar (fixed green search
      box + scrollable recent-stop history) and a right-side scrollable
      table of services with a real capacity gauge, in a `ROW`-flow tab
      (`sidebar` fixed-width + `content` flex-grow).
      - **Stop-name resolution** (`bus_stops.cpp/.h`, new subsystem): the
        `v3/BusArrival` endpoint never returns a human-readable stop name
        (confirmed by inspecting its raw JSON - only `BusStopCode` at the
        top level), so this fetches LTA's separate static `BusStops`
        dataset once (paginated 500/page, confirmed via a direct `curl`
        test against the real API rather than assumed: `value[]` array,
        `BusStopCode`/`RoadName`/`Description`/`Latitude`/`Longitude`
        fields, 5,208 stops total, `Description` max 36 chars). Cached to
        the SPIFFS partition (`/busstops.bin`, ~291KB, well inside the
        existing 1.5MB `spiffs` partition - no partition table changes
        needed) so it only re-fetches over the network once ever, not on
        every boot; the in-memory array itself is allocated via
        `ps_malloc()` into PSRAM, not the 327KB internal heap. Lookup is a
        linear scan (trivial for 5,208 entries at 240MHz, called only on a
        stop-change, not per-frame). `busStopsTick()` drives a one-time
        cache-load-or-fetch state machine from `loop()`, non-blocking
        except for the genuine one-time fetch itself (~11 sequential HTTP
        requests, several seconds, acceptable since it happens once).
      - **Capacity/load gauge**: LTA's `Load` field (`SEA`/`SDA`/`LSD` =
        seats available / standing available / limited standing, confirmed
        against the live API) drives a small `lv_bar` per time cell,
        colored green/orange/red and filled to 33/66/100% as a rough
        "how full" cue - this directly answers "does the JSON have bus
        capacity" (yes, it was already being fetched and just not
        displayed). Each cell shows: value (minutes, or "NA" if that
        arrival slot is empty), the gauge, and a "DOUBLE" label (no icon,
        no "min" text, no "SINGLE" label - all per explicit request) shown
        only when `Type` starts with `D`; bendy buses (`BD`) fall back to
        treating them as not-double since only a double/single distinction
        was asked for.
      - **Search**: a fixed, green, tap-to-open box at the top of the
        sidebar; tapping it raises a full-screen numeric keypad overlay
        (`lv_keyboard` in `LV_KEYBOARD_MODE_NUMBER` bound to a fresh
        `lv_textarea`) on `lv_layer_top()`, same overlay pattern as the
        Profile tab's QR modal.
      - **History**: persisted across reboots via NVS (`Preferences`,
        namespace `"bushist"`, codes joined as one comma-separated string
        under key `"codes"`). Seeded once with `18101, 11261, 43239,
        43231, 18071` - a `"seeded"` bool flag in the same NVS namespace
        makes this a genuine one-time migration (overwrites whatever was
        there - including this feature's own earlier single-entry
        `BUS_STOP`-only seed - exactly once, never again), so real usage
        built up afterward survives later reboots instead of being wiped
        every time. **Real behavior bug fixed**: selecting an
        already-listed stop used to move it to the front of the list
        (`pushHistory()`), which kept reshuffling the user's deliberately
        chosen seed order on every tap - user: "Do not alter list on
        selection". Fixed by splitting the two cases: selecting an
        existing entry now only changes which one is highlighted active
        (no reorder), while a genuinely new code (typed via the search
        keypad) still gets inserted at the front, since there's no
        existing position for it to disturb. Rows are two lines (stop code
        bigger/14px, stop name smaller/10px/ellipsized) styled like tabs -
        rounded, bordered, white by default, solid green with white text
        for whichever stop is currently active - rebuilt from scratch via
        `lv_obj_clean()` + recreate on every change rather than a
        show/hide pool, since it's infrequent (stop-change only) and the
        list length varies.
      - **No standalone refresh button** - removed per explicit request;
        tapping any stop (including re-tapping the active one) already
        forces a fetch via `selectStop()`, which doubles as refresh.
        Auto-refresh is a separate toggle switch (green when on, **off by
        default**), gated by its own `autoRefreshEnabled` flag independent
        of the manual `forceFetch` flag so toggling auto-refresh off
        doesn't block a manual re-select from still fetching immediately.
      - **Scrollbars real bug fixed twice**: both the table and the
        history list are built on hand-styled containers via
        `lv_obj_remove_style_all()` (this project's standard pattern for
        full manual control) - but that call also wipes the scrollbar
        part's default styling, which is why the scrollbar was invisible
        even though the content was genuinely scrollable; not a layout
        bug, a styling one. Fixed by explicitly restyling
        `LV_PART_SCROLLBAR` (width/color/opacity/radius) after the
        `remove_style_all()` call. Mode went through two rounds:
        `LV_SCROLLBAR_MODE_AUTO` (shows persistently whenever content
        overflows) first, then changed to `LV_SCROLLBAR_MODE_ACTIVE`
        (shows only while actively being scrolled/dragged, fades
        otherwise) per explicit request - these are genuinely different
        LVGL enum values, not a naming quirk, confirmed by reading
        `lv_obj_scroll.h`'s own doc comments rather than assumed. Color
        grey (`0x9e9e9e`), not the green used elsewhere in this tab.
      - **Bottom tab bar icon**: regenerated `tab_icon_bus.c/.h` from
        `icons/bus_24x24.png` (was `bus_single_24x24.png`) - same output
        symbol name, so no other file needed to change.
      - **Real corruption bug fixed**: a chat message got accidentally
        pasted directly into `main.cpp` mid-statement
        (`screenWidth / totalTabs;bus_24x24.png`), which would not have
        compiled - caught and fixed while making an unrelated change to
        that file, worth remembering that paste-into-open-editor is a
        real failure mode on this project, not just a hypothetical.
- [x] Today tab (`src/app/today.cpp`) - calendar agenda view, fetched from
      `OUTLOOK_ICS_URL` on tab-show with a background FreeRTOS fetch task
      (same pattern as Bus, see below), auto-refresh interval controllable
      from the Settings tab. Implemented in an earlier session; see git
      history/`today.cpp` itself for the full story rather than this doc.
- [x] Ask tab (`src/app/ask.cpp`, `src/app/mic_capture.*`,
      `src/app/es8311.*`) - tap the mic icon, speak a short question, get a
      short answer, entirely on-device (no server backend). Two OpenAI
      calls per question over plain HTTPS (`WiFiClientSecure` +
      `setInsecure()`, same convention as every other HTTPS call in this
      project): `gpt-transcribe` (cheapest current OpenAI transcription
      model, $0.0045/min) for speech-to-text, then `gpt-5.6-luna` (the
      low-cost GPT-5.6 tier - confirmed text-only, no audio input, hence
      the separate transcription step) via `POST /v1/responses` for a short
      answer sized for this display. API key lives in `include/secrets.h`
      as `OPENAI_API_KEY`, currently still the placeholder string
      `"SET_ME_OPENAI_API_KEY"` - `ask.cpp` checks for that exact string and
      shows a clear "No OpenAI API key set yet" error instead of sending it
      and getting a confusing 401.
      - **Recording**: `mic_capture.cpp` drives the onboard ES8311 codec's
        active-low enable pin (IO1), brings up the codec (`es8311.cpp` -
        register init ported directly from Espressif's own official
        open-source ES8311 driver, not a third-party wrapper - see that
        file's header comment for why: the obvious wrapper candidate,
        `pschatzmann/arduino-audio-driver`, has an open unresolved GitHub
        issue #27 showing the exact class of `LoadProhibited`/null-pointer
        crash this project already burned hours root-causing once, see the
        `LV_MEM_SIZE` postmortem elsewhere in this doc), then records via
        the ESP-IDF legacy `driver/i2s.h` API (confirmed present in this
        project's exact installed toolchain version) at 16kHz mono 16-bit
        into a `ps_malloc()`'d PSRAM buffer, wrapped in a WAV header. Capped
        at 8 seconds (`MAX_RECORD_SECONDS_HARD_CAP`), with manual
        tap-again-to-stop-early - no silence/VAD auto-stop, kept simple for
        a first version. Codec + I2S are fully torn down and the enable pin
        driven back high after every recording (matching this project's
        general power-consciousness, see `power.cpp`), not left running
        between questions.
      - **Orchestration**: one FreeRTOS task on core 0
        (`xTaskCreatePinnedToCore`, same `bus.cpp`/`today.cpp` convention -
        `WiFiClientSecure`/`HTTPClient` locals scoped inside
        `transcribeAudio()`/`askChatModel()` so they destruct before
        `vTaskDelete(NULL)` runs) drives record -> transcribe -> chat in
        sequence. Unlike Bus/Today's single `fetchInProgress`/
        `fetchJustCompleted` bool pair, this tab has several distinct
        visible states, so the task writes directly to a shared
        `AskState` enum (`LISTENING`/`TRANSCRIBING`/`THINKING`/`DONE`/
        `ERROR`) as it progresses, and `askTick()` (polled from `loop()`)
        just re-renders whenever that value changes - safe without a mutex
        since only the task ever writes `state`/`answerText`/`statusError`,
        and the UI only reads the text buffers once `state` has already
        reached `DONE`/`ERROR`. Same 40s watchdog pattern as Bus/Today's
        fetch watchdogs. Checks `WiFi.status()` and the API-key placeholder
        before even starting a recording, so "offline"/"no key" fail fast
        with a clear message instead of recording audio nobody can send.
      - **UI**: mic icon (`icons/microphone_32x32.png`) + "Tap to speak"
        centered at the top, a status line below it, and a scrollable
        answer label filling the rest of the tab (same scrollable-box +
        restyled `LV_PART_SCROLLBAR` pattern as `settings.cpp`'s
        `rightList`). Both mic icons (this one and the bottom tab-bar icon,
        `icons/microphone_24x24_2.png`) are shown in their own native PNG
        colors, no `img_recolor` - per explicit request, matching the
        no-recolor convention every tab icon except Settings' already
        follows (see `addTabIcon()`'s own comment in `main.cpp`). Tint
        `0xE91D65` (pink) is still used for the "Tap to speak" text label.
      - **User-confirmed working end-to-end on real hardware**: tap mic ->
        speak -> real transcription -> real contextual answer, verified over
        several rounds with the real `OPENAI_API_KEY` in `secrets.h`.
      - **Debugging postmortem - the vendor's own PDF pin table is wrong.**
        Recording ran (no crash, no I2C/config errors) but every clip came
        back from OpenAI as an empty transcript with no detected language -
        genuinely near-silent audio, not a network/API bug. Real root
        cause, found only after downloading the vendor's actual example
        firmware package (not just their wiki page/PDF) and reading their
        working `Example_17_echo` demo's `ESP_Panel_Board_Custom.h`
        directly: **the PDF's pin table has I2S DIN/DOUT backwards from
        their own working code.** The PDF says `I2S_DO=IO6` (speaker out)
        / `I2S_DI=IO8` (mic in) - this project's audio hardware section
        above used to say the same thing, copied from that PDF. Their real
        firmware defines `I2S_DINT=6` / `I2S_DOUT=8` - the *opposite*. Real
        mic data was on GPIO6 the entire time; `mic_capture.cpp` was
        reading GPIO8 (the unused speaker pin) instead, hence near-total
        silence with only tiny amplitude glitches, not zero (confirmed via
        this project's own added per-sample amplitude diagnostics - peak
        briefly nonzero, average ~0, before vs. after the fix: ~20-280
        peak/~0 average before, 1700-3400 peak/~200-250 average after,
        genuinely speech-shaped). Also switched MCLK from 256x (4.096MHz,
        never actually confirmed working) to 384x (6.144MHz) and I2S port
        0 -> port 1, both matching that same reference firmware exactly,
        to remove every remaining discrepancy at once rather than
        re-testing one variable at a time. **Lesson for this project,
        generalizing the tab-icon and ETA-countdown sagas above**: a
        vendor's own PDF documentation is not guaranteed more accurate than
        their actual shipped example code - when a hardware pin assignment
        doesn't behave as documented, check for real reference firmware
        (not just the datasheet-style doc) before spending more time on
        register-level software debugging. Also fixed along the way: a
        buffer-overflow bug in the multipart HTTP request's `authHeader`
        (was `char[96]`, too small for a real ~164-char `sk-proj-...` key +
        `"Bearer "` prefix - `snprintf`'s return value is the *untruncated*
        length, so a too-small buffer here wasn't just wrong, it was a real
        stack over-read); and an ESP32 mbedTLS quirk where opening a second
        `WiFiClientSecure` HTTPS connection immediately after the first one
        closes can fail with "SSL - Memory allocation failed" even with
        plenty of free heap - fixed with a short `delay(300)` between the
        two OpenAI calls, giving the TLS stack's own internal cleanup time
        to actually finish.
      - **Bus/calendar context-injection** (added after the basic voice
        Q&A loop was confirmed working, so questions like "when's the next
        173" or "what's my next meeting" have real answers instead of the
        model just guessing/declining). `askWork()` calls
        `buildAskContext()` before the chat call, which pulls a short
        plain-text summary from `bus.cpp`'s `busGetContextSummary()`
        (active stop's live arrivals) and `today.cpp`'s
        `todayGetContextSummary()` (next few upcoming events, not just
        "today" - filtered by real end-time, not day equality, so "next
        meeting" still works correctly near midnight) plus current
        SGT time and Wi-Fi status, and prepends it to the Responses API's
        `instructions` field. Both accessors are safe to call any time
        (same "only the fetch task writes these arrays" invariant as the
        rest of each file) and just return a "not loaded yet" sentence if
        nothing has been fetched.
        - **Data is now loaded once at boot regardless of which tab is
          active**, not just on tab-show: `busInit()` already forced an
          initial fetch (`forceFetch = true`) before this change;
          `todayInit()` didn't, so the calendar previously sat empty until
          the user happened to open the Today tab at least once - fixed by
          adding the same `forceFetch = true` there. This only changes
          when the *first* fetch happens; the existing power-conscious
          "only auto-refresh on a timer while that tab is actually being
          viewed" policy (`tabIsActive` gating in both files) is
          unchanged, so data can be a few minutes stale if the user's been
          on the Ask tab a while - same staleness a human glancing at a
          slightly-old screen would see, and an explicit, deliberate
          trade-off against polling both APIs continuously in the
          background regardless of tab (which would cost meaningfully
          more Wi-Fi/battery for a benefit this project didn't need).
        - **Bus stop by name**: `bus.h`'s `busFindStopInHistoryByName()`
          lets a spoken question like "bus to Yishun" switch the active
          stop before the chat call, by case-insensitively matching the
          question text against the *resolved names of the user's saved
          history stops only* - deliberately not the full ~5200-stop LTA
          dataset, since many real stop descriptions are generic ("Blk
          123", "Opp ... Stn") and would produce false-positive matches
          against arbitrary free-form speech with no distance/ranking
          signal to disambiguate. If a different saved stop is named,
          `askWork()` calls `busSelectStopByCode()` (same `forceFetch`
          path a sidebar tap uses) and polls `busIsFetchInProgress()` for
          up to 6s before building context - this does add real latency to
          that specific question, and there's a documented race in the
          polling itself (a fixed `delay(150)` before polling starts,
          since `busTick()` on a different core/task is what actually
          flips the in-progress flag, so checking too early would
          otherwise see "not in progress" from *before* the new fetch
          started rather than *after* it finished).
        - **Deliberately did not** offload any of this to OpenAI directly
          (e.g. giving the model a tool/function to call the LTA API or the
          ICS feed itself) - OpenAI has no way to reach either endpoint
          without this project standing up a public server for it to call,
          which the original Ask tab requirements explicitly ruled out
          ("Do not create a Python/server backend"). All data fetching
          stays on-device; the model only ever reasons over text this
          project hands it.
      - **Bug found and fixed after the context-injection change above: a
        failed fetch could get permanently stuck with no retry.** Symptom
        reported by the user: "HTTP -1" appears once, and after that
        "network things never update... it never comes out of it" - the
        display keeps working (LVGL/touch are unaffected) but every tab's
        data stays frozen forever, only fixable by a reboot. Two distinct
        real bugs contributed, both fixed:
        1. **Socket leak on a watchdog-forced task kill.** Every fetch
           (`bus.cpp`/`today.cpp`/`ask.cpp`) runs on its own FreeRTOS task,
           and a watchdog in each tab's `*Tick()` calls
           `vTaskDelete(taskHandle)` if that task ever wedges past its
           timeout. `vTaskDelete()` on another task does **not** unwind
           that task's C++ stack, so the `WiFiClientSecure`/`HTTPClient`
           still alive on it at the moment of the kill never gets its
           destructor run - its TCP socket/TLS context leaks. This exact
           mechanism was already found and fixed for the *normal*
           completion path (see `today.cpp`'s own comment on it, from an
           earlier session) but the watchdog's forced-kill path never got
           the equivalent fix. This board only has a handful of concurrent
           TLS-capable sockets, so a couple of these leaks permanently
           exhausts the pool - every future HTTPS call, on every tab,
           starts returning `-1` (connection refused) with no recovery
           short of a reboot. Fixed: each of the three watchdogs now calls
           `WiFi.disconnect()` right after the forced `vTaskDelete()`,
           which tears down the whole Wi-Fi netif (reclaiming any leaked
           sockets with it); `main.cpp`'s existing `wifiTask` already
           retries `wifiMulti.run()` every 5s once disconnected, so
           reconnection is automatic.
        2. **A failed *first* fetch was never retried unless its own tab
           was the one on screen.** `busInit()`/`todayInit()` force one
           fetch at boot regardless of the active tab (see the
           context-injection entry above) - but if that attempt failed for
           *any* reason (including, plausibly, `WiFiMulti` reporting
           "connected" a moment before DNS/routing is actually usable -
           the user's own hypothesis when this was reported, and a
           reasonable one), nothing ever tried again: normal periodic
           auto-refresh is gated on `tabIsActive` (by design, to not burn
           Wi-Fi/battery polling a tab nobody's looking at), and the
           one-shot `forceFetch` flag had already been consumed by the
           failed attempt. A user sitting on the Ask tab would see stale
           "no data" forever with zero indication anything was wrong,
           since nothing was actually stuck - it just stopped trying.
           Fixed: both files now track `everSucceeded` (set the first time
           a fetch completes with `lastError[0] == '\0'`) and, until it's
           true, retry regularly regardless of `tabIsActive`/
           `autoRefreshEnabled` (currently every 5s, shortened from an
           initial 20s once real serial evidence showed the remaining
           residual failure mode - see below - fails fast and recovers
           fast, so a short retry interval clears it sooner). Once the
           first real success lands, this reverts to the original power-
           conscious "only refresh on a timer while that tab is actually
           visible" behavior - unchanged from before.
        Confirmed via real captured serial output (not just inferred):
        `bus: fetch watchdog fired - force-recovering` did appear, and
        real internal-heap numbers were captured too - see the next entry.
        A genuinely separate residual issue was found this way: a single,
        uncontended HTTPS attempt could still fail with "SSL - Memory
        allocation failed" even with ~70KB free internal heap - real
        fragmentation (mbedTLS needs one large *contiguous* block for its
        TLS record buffers), not scarcity, most likely from the Wi-Fi
        stack's own post-connect churn plus two ~20KB fetch-task stacks
        both being reserved at the same boot-time moment. This project's
        framework doesn't expose a way to shrink mbedTLS's buffer sizes
        from sketch code, so this isn't eliminated outright - mitigated by
        staggering `bus.cpp`'s and `today.cpp`'s boot-time fetches ~5s
        apart and retrying fast (5s) rather than slow (20s), since the
        failure is fast and usually transient (the very next attempt
        normally succeeds). `net_lock.h`'s `wifiSettled()` (a ~3s grace
        period after `WiFi.status()==WL_CONNECTED` before any fetch is
        allowed to start) also exists for this same investigation, though
        on its own it didn't eliminate the very-first-attempt failure -
        kept anyway since it's cheap and still theoretically reduces risk.
- [x] Speaker output / TTS (`src/app/speaker.*`, `es8311InitForSpeaker()`
      in `src/app/es8311.*`) - the Ask tab's answer is now also spoken
      aloud through the board's onboard FM8002E amp, not just shown as
      text. A third OpenAI call per question, `POST /v1/audio/speech`
      (`gpt-4o-mini-tts`, OpenAI's current lower-cost TTS-capable model,
      same "low-cost" intent as `gpt-transcribe`/`gpt-5.6-luna` elsewhere
      in this file), requesting `response_format: "pcm"` specifically so
      the response is raw signed 16-bit mono samples at a fixed 24kHz with
      no header - deliberately avoiding any MP3/AAC decoder dependency,
      matching this project's preference for the smallest dependency
      footprint that works (see the mic-capture wrapper-library decision
      above). `speaker.cpp` mirrors `mic_capture.cpp`'s exact
      install/play/uninstall-per-call pattern on the same shared I2S
      peripheral (this board only has one, wired to the ES8311) - full
      teardown between calls, not left running, matching this project's
      general power-consciousness.
      - **Built the "vendor's way" on purpose, not a third-party library.**
        The user explicitly asked to follow the vendor's own demonstrated
        approach rather than adopt a library, after a pasted TTS example
        using the `ESP32-audioI2S`/`Audio.h` library turned out to *also*
        be bundled (unused/dead code) in the vendor's own
        `Example_17_echo` package alongside a `demo_music.cpp` that uses
        it for MP3 playback - real evidence that library is intended for
        this hardware family, but not a directly wired, provably-working
        reference for *this* exact use case. What *is* a directly wired,
        provably-working reference in that same package: `echo.ino`'s raw
        `i2s_std`-API full-duplex mic+speaker loop, on the exact same
        pins already confirmed for mic capture (MCLK=4, BCLK=5, WS=7,
        DIN=6, DOUT=8) - `speaker.cpp` follows that pattern directly (via
        the legacy `driver/i2s.h` API instead, matching `mic_capture.cpp`'s
        own existing choice, not the newer `i2s_std.h` API `echo.ino`
        happens to use - same hardware behavior either way).
      - **24kHz clock coefficients taken verbatim from the vendor's own
        `es8311.cpp` coefficient table** (`Example_17_echo`'s copy, which
        matches Espressif's official driver this project's own
        `es8311.cpp` was already ported from), not derived/guessed: the
        `{6144000, 24000, ...}` row happens to need the exact same 6.144MHz
        MCLK this project's mic path already generates for 16kHz (at a
        384x multiple there vs. 256x here) - worked out by hand that every
        clock register except REG02 (`pre_div`/`pre_multi`) is byte-
        identical between the two configs, so `es8311InitForSpeaker()`
        only actually differs from `es8311InitForMicCapture()` in that one
        register's value (0x00 vs 0x48).
      - **Both TX and RX enabled together during playback, not TX-only**,
        deliberately matching `echo.ino`'s own always-both-directions
        config and `mic_capture.cpp`'s own prior finding that this legacy
        I2S driver only reliably generates a clean MCLK when both
        directions are active - RX data is never read during playback,
        wired up for clock-generation reliability only, same reasoning
        `mic_capture.cpp` already uses in reverse (TX wired up during
        capture for the same reason).
      - **Correction to an earlier claim in this file's own history**: an
        earlier version of the mic-capture writeup said the third-party
        `pschatzmann/arduino-audio-driver` wrapper's ES8311 support had a
        "still-open" crash issue - checked directly (not left on recall)
        and that's wrong: issue #27 is closed, and the crash in it traced
        to the reporter's own double-initialization and wrong
        `MCLK_SOURCE` value, not a confirmed library defect. Doesn't
        change the decision to keep the ported driver (no reason to
        rip out already-working, hard-won code over a corrected detail),
        but the original justification for *avoiding* that library was
        overstated and is corrected here rather than left standing.
      - Not yet confirmed on real hardware (audio played through the
        speaker and actually heard) - built and flashed, verification
        pending.

## Source layout

- `src/app/` - the real app (`main.cpp`, `bus.cpp`/`.h`, `touch.h`). Builds as
  the `app` PlatformIO environment (default).
- `src/led_test/`, `src/tft_test/` - standalone hardware bring-up sketches,
  each its own PlatformIO environment. Not part of the real app; used during
  this session as minimal-firmware isolation tests while debugging the false
  "dead board" alarm above.
- `include/secrets.h` - Wi-Fi + LTA DataMall API key + bus stop code. Not
  committed to a public remote (check `.gitignore` before pushing anywhere).
- `factory_backup.bin` - a full 16MB flash dump taken via
  `esptool read-flash 0 ALL`, hash `3edb10b9...b133c564`. Note: based on a
  stray `--environment esp32s3` line in the terminal history this was copied
  from into `readme.txt`, this backup may have already been taken *after*
  this project's own firmware was flashed at least once - i.e. it is
  probably **not** a pristine vendor image, just a snapshot of whatever was
  in flash at the time. Don't treat it as a known-good factory reset image
  without verifying first.
