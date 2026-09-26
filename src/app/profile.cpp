// Profile tab: static personal/contact card. Content sourced from the
// user's own business card design, simplified for a 480x292 touchscreen.
// Photo stays at its native 160x160 on the left, with a small QR code
// beneath it that enlarges to a full-screen overlay on tap (for scanning);
// info stacks beside them on the right.
#include "profile.h"
#include "profile_data.h"
#include "icons/profile_pic.h"
#include "icons/qr_small.h"
#include "icons/qr_large.h"
#include "icons/profile_bg.h"
#include <Preferences.h>
#include <SPIFFS.h>

static const lv_img_dsc_t *qrLargeImg = &qr_large; // set by loadProfile()

static void closeQrOverlay(lv_event_t *e)
{
    lv_obj_t *overlay = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_del(overlay);
}

// Full-screen dark backdrop with the QR code large enough to scan easily.
// Placed on lv_layer_top() so it renders above the tab bar/status strip
// regardless of which tab is active, and a tap anywhere closes it (the QR
// image itself isn't clickable, so taps on it fall through to the backdrop).
static void showQrOverlay(lv_event_t *e)
{
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, closeQrOverlay, LV_EVENT_CLICKED, overlay);

    lv_obj_t *qrBig = lv_img_create(overlay);
    lv_img_set_src(qrBig, qrLargeImg);
    lv_obj_set_style_radius(qrBig, 5, 0);
    lv_obj_set_style_clip_corner(qrBig, true, 0);
    lv_obj_clear_flag(qrBig, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(qrBig, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *hint = lv_label_create(overlay);
    lv_label_set_text(hint, "Tap anywhere to close");
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void addContactRow(lv_obj_t *parent, const char *symbol, const char *text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x1976d2), 0);
    lv_obj_set_width(icon, 18);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
}

static void addSpecialtyLine(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
}

// The 3 specialty lines read as one group, so they get their own tightly
// spaced sub-column instead of using `info`'s normal (larger) row gap,
// which is meant for spacing between visually distinct fields.
static lv_obj_t *createSpecialtyGroup(lv_obj_t *parent)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(group, 1, 0);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    return group;
}

// Profile content lives on the board, so OTA (release builds, which only
// carry placeholders) keeps it: text in NVS "profile", images in SPIFFS.
// A local build with src/app/profile_private/ writes its content there
// whenever it differs from what's stored; every build then reads it back.
enum
{
    P_NAME,
    P_TITLE,
    P_ORG,
    P_LINE1,
    P_LINE2,
    P_LINE3,
    P_PHONE,
    P_EMAIL,
    P_WEB,
    P_COUNT
};
static const char *textKeys[P_COUNT] = {"name", "title", "org", "l1", "l2", "l3", "phone", "email", "web"};
static const char *textBuiltIn[P_COUNT] = {PROFILE_NAME, PROFILE_TITLE, PROFILE_ORG, PROFILE_LINE1, PROFILE_LINE2,
                                           PROFILE_LINE3, PROFILE_PHONE, PROFILE_EMAIL, PROFILE_WEB};
static String text[P_COUNT];

struct ProfileImage
{
    const char *path;
    const lv_img_dsc_t *builtIn;
    lv_img_dsc_t stored; // data in PSRAM when loaded from SPIFFS
    const lv_img_dsc_t *use;
};
static ProfileImage images[] = {
    {"/profile_pic.bin", &profile_pic, {}, &profile_pic},
    {"/qr_small.bin", &qr_small, {}, &qr_small},
    {"/qr_large.bin", &qr_large, {}, &qr_large},
};

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

#if PROFILE_PRIVATE
static void saveBuiltInProfile(Preferences &prefs)
{
    uint32_t sum = 2166136261u;
    for (auto t : textBuiltIn)
        sum = fnv1a(sum, (const uint8_t *)t, strlen(t) + 1);
    for (auto &img : images)
        sum = fnv1a(sum, img.builtIn->data, img.builtIn->data_size);
    if (prefs.getUInt("sum", 0) == sum)
        return;
    for (int i = 0; i < P_COUNT; i++)
        prefs.putString(textKeys[i], textBuiltIn[i]);
    for (auto &img : images)
    {
        File f = SPIFFS.open(img.path, "w");
        if (!f || f.write(img.builtIn->data, img.builtIn->data_size) != img.builtIn->data_size)
        {
            Serial.printf("profile: could not write %s\n", img.path);
            return; // sum not stored - retried next boot
        }
    }
    prefs.putUInt("sum", sum);
    Serial.println("profile: saved to the board");
}
#endif

static void loadProfile()
{
    bool fs = SPIFFS.begin(true);
    Preferences prefs;
    prefs.begin("profile", false);
#if PROFILE_PRIVATE
    if (fs)
        saveBuiltInProfile(prefs);
#endif
    for (int i = 0; i < P_COUNT; i++)
        text[i] = prefs.getString(textKeys[i], textBuiltIn[i]);
    prefs.end();
    if (!fs)
        return;
    for (auto &img : images)
    {
        File f = SPIFFS.open(img.path, "r");
        if (!f || f.size() != img.builtIn->data_size) // same dimensions as the placeholder
            continue;
        uint8_t *data = (uint8_t *)ps_malloc(f.size());
        if (data && f.read(data, f.size()) == f.size())
        {
            img.stored = *img.builtIn;
            img.stored.data = data;
            img.use = &img.stored;
        }
        else
            free(data);
    }
    qrLargeImg = images[2].use;
}

void profileInit(lv_obj_t *tab)
{
    loadProfile();
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(tab, 14, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_ROW);
    // Cross-axis CENTER so the (shorter) photo+QR column is vertically
    // centered against the (taller) info column, not pinned to its top.
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    // +20px beyond the normal 14px gap, to shift the whole info column
    // right as its own adjustment (independent of the photo's own size).
    lv_obj_set_style_pad_column(tab, 34, 0);

    // Decorative background strip, flush to the tab's right edge, spanning
    // the full content area from the top status strip down to the tab bar
    // (screenWidth x (screenHeight - topBarHeight - tabBarHeight) = 480x242,
    // matching the image's own aspect). Created first so it renders behind
    // the photo/QR/text added after it, and excluded from the row flex
    // layout (IGNORE_LAYOUT) since it's positioned absolutely, not flowed
    // alongside the other two columns.
    lv_obj_t *bg = lv_img_create(tab);
    lv_img_set_src(bg, &profile_bg);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    // lv_obj_align() positions relative to the parent's *content* area
    // (i.e. already inset by tab's own 14px padding on every side) - the
    // +14/-14 offsets push past that inset so the image is flush with the
    // tab's actual top/right edges instead of sitting 14px in from them.
    lv_obj_align(bg, LV_ALIGN_TOP_RIGHT, 14, -14);

    lv_obj_t *photoCol = lv_obj_create(tab);
    lv_obj_remove_style_all(photoCol);
    lv_obj_set_size(photoCol, 160, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(photoCol, LV_FLEX_FLOW_COLUMN);
    // 2nd param is cross-place (horizontal, for a COLUMN flow) - CENTER
    // here is what actually centers the photo/QR; a prior version passed
    // START here and CENTER as the 3rd (track-place, irrelevant with one
    // column) param by mistake, which is why the QR sat at the left edge
    // instead of centered under the photo.
    lv_obj_set_flex_align(photoCol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(photoCol, 8, 0);
    lv_obj_clear_flag(photoCol, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *photo = lv_img_create(photoCol);
    lv_img_set_src(photo, images[0].use);
    lv_obj_set_style_radius(photo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(photo, true, 0);
    // Now 180x180 (20px larger than photoCol's 160px column - it overflows
    // photoCol's declared bounds slightly on both sides, which is fine
    // since nothing clips it) shifted up 10px. translate is a paint-time
    // transform, not a layout change, so it doesn't disturb the QR's
    // position below it.
    lv_obj_set_style_translate_y(photo, -10, 0);
    lv_obj_set_style_translate_x(photo, -10, 0);

    lv_obj_t *qrSmall = lv_img_create(photoCol);
    lv_img_set_src(qrSmall, images[1].use);
    lv_obj_add_flag(qrSmall, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(qrSmall, showQrOverlay, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_translate_y(qrSmall, -25, 0);
    lv_obj_set_style_translate_x(qrSmall, -10, 0);

    lv_obj_t *info = lv_obj_create(tab);
    lv_obj_remove_style_all(info);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_height(info, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 6, 0);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_translate_x(info, -15, 0);
    lv_obj_set_style_translate_y(info, -5, 0);

    lv_obj_t *nameLabel = lv_label_create(info);
    lv_label_set_text(nameLabel, text[P_NAME].c_str());
    lv_obj_set_style_text_font(nameLabel, &lv_font_montserrat_18, 0);

    lv_obj_t *titleLabel = lv_label_create(info);
    lv_label_set_text(titleLabel, text[P_TITLE].c_str());
    lv_obj_set_style_text_color(titleLabel, lv_color_hex(0x555555), 0);

    lv_obj_t *orgLabel = lv_label_create(info);
    lv_label_set_text(orgLabel, text[P_ORG].c_str());
    lv_obj_set_style_text_color(orgLabel, lv_color_hex(0x555555), 0);

    lv_obj_t *specialtyGroup = createSpecialtyGroup(info);
    addSpecialtyLine(specialtyGroup, text[P_LINE1].c_str());
    addSpecialtyLine(specialtyGroup, text[P_LINE2].c_str());
    addSpecialtyLine(specialtyGroup, text[P_LINE3].c_str());

    // LVGL styles have no "margin" concept, only padding (which doesn't add
    // flex-gap space around an object) - so extra space above/below the
    // divider line is a slightly taller invisible box with the actual 1px
    // line centered inside it, rather than a style property on the line.
    lv_obj_t *dividerBox = lv_obj_create(info);
    lv_obj_remove_style_all(dividerBox);
    lv_obj_set_size(dividerBox, LV_PCT(100), 13);
    // Left edge flush with the text above/below it (no left inset); all
    // of the shortening comes off the right side only.
    lv_obj_set_style_pad_right(dividerBox, 70, 0);
    lv_obj_clear_flag(dividerBox, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *divider = lv_obj_create(dividerBox);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0xdddddd), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_align(divider, LV_ALIGN_CENTER, 0, 0);

    addContactRow(info, LV_SYMBOL_CALL, text[P_PHONE].c_str());
    addContactRow(info, LV_SYMBOL_ENVELOPE, text[P_EMAIL].c_str());
    addContactRow(info, LV_SYMBOL_HOME, text[P_WEB].c_str());
}
