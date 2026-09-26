#pragma once
#include <stdint.h>

// Small spinner in the top status strip (next to the Wi-Fi icon), shown
// while any tab is doing a slow background fetch - added because the
// Today tab's calendar fetch used to block the whole UI with zero visual
// indication that anything was happening at all.
void setLoadingVisible(bool visible);

// Recolors the spinner's arc to match whichever tab is currently active
// (Today=purple, Bus=green, Ask=its own tint) - called from main.cpp's
// tab-switch handler, not tied to which specific tab's fetch triggered
// setLoadingVisible(true), so the spinner always reads as "this tab's own
// color" regardless of which background task actually started it.
void setLoadingSpinnerColor(uint32_t hexColor);

// Every fetch (bus.cpp, bus_stops.cpp, today.cpp) reports how many bytes
// it actually pulled down here, so the top strip can show a real KB/s
// figure instead of nothing - there's no single built-in "total Wi-Fi
// bytes" counter in the Arduino API, so this is tracked by hand at the
// one place all of them funnel through (an HTTP response body/stream).
void addNetworkBytes(uint32_t bytes);
