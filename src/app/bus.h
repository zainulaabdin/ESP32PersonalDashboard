#pragma once
#include <lvgl.h>

void busInit(lv_obj_t *tab);
void busTick();
// Call whenever the active tab changes, so periodic auto-refresh only
// fires while the Bus tab is actually the one being looked at.
void busSetTabActive(bool active);
// Settings tab controls - clamped to [5, 600] seconds.
void busSetRefreshIntervalSec(int seconds);
int busGetRefreshIntervalSec();
// Favourite stops (5-digit codes): morning = 00:00-11:59, afternoon =
// 12:00-23:59. Persisted; the matching one becomes active right away and
// again at each switch time. A code that isn't 5 digits is ignored.
void busSetFavorites(const char *morning, const char *afternoon);
const char *busGetFavorite(bool afternoon);

// ---- Exposed for the Ask tab's context-injection (see ask.cpp) ----------
// Switch the active stop by code and force a fetch - same effect as
// tapping it in the sidebar/search box.
void busSelectStopByCode(const char *code);
bool busIsFetchInProgress();
const char *busGetActiveStopCode();
// Fills `out` with a short plain-text summary of the active stop's live
// arrivals, safe to read any time (returns a "not loaded yet" sentence
// instead of touching services[] if a fetch is currently in flight).
void busGetContextSummary(char *out, size_t outSize);
// Scans the user's saved history (not the full ~5200-stop dataset - see
// ask.cpp for why) for a stop whose resolved name appears as a case-
// insensitive substring of `text`. Returns true and fills outCode on a
// match.
bool busFindStopInHistoryByName(const char *text, char *outCode, size_t outCodeSize);
