// LTA's static BusStops dataset (~5200 stops): the only source for a human
// -readable stop name, since v3/BusArrival never returns one (confirmed by
// inspecting its raw JSON - see CLAUDE.md). Fetched once (paginated, 500/
// page) and cached to flash, since this device stays in one physical
// location and the dataset barely changes.
#pragma once
#include <Arduino.h>

void busStopsInit();
// Call every loop() iteration - does nothing once loaded; drives the
// one-time cache-load-or-fetch state machine without blocking setup().
void busStopsTick();
bool busStopsReady();
// Returns "" (not nullptr) if code is unknown, so callers can always print
// the result directly without a null check.
const char *busStopsLookupName(const char *code);
