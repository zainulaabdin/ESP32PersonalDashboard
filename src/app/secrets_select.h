#pragma once

// Release builds (pio run -e release) and checkouts without a secrets.h use
// the empty template, so a published firmware.bin never contains keys. The
// board then uses the values saved in its flash (web settings page, or
// seeded from an earlier local build - see app_config.cpp).
#if defined(RELEASE_BUILD) || !__has_include("secrets.h")
#include "secrets.h.example"
#else
#include "secrets.h"
#endif
