#include "diag.h"
#include <Arduino.h>
#include <string.h>

struct DiagEntry
{
    bool reported;
    bool ok;
    unsigned long atMs;
    char detail[64];
};

static DiagEntry entries[DIAG_COUNT];
static const char *names[DIAG_COUNT] = {"LTA", "Calendar", "Weather", "OpenAI", "Audio", "OTA"};

// Reported from the network worker (core 0) and read by the web server on
// core 1 - a torn read only garbles one status line for one page load.
void diagReport(DiagService service, bool ok, const char *detail)
{
    DiagEntry &e = entries[service];
    strlcpy(e.detail, detail ? detail : "", sizeof(e.detail));
    e.ok = ok;
    e.atMs = millis();
    e.reported = true;
}

const char *diagServiceName(DiagService service)
{
    return names[service];
}

bool diagGet(DiagService service, bool *ok, const char **detail, long *ageSec)
{
    const DiagEntry &e = entries[service];
    *ok = e.ok;
    *detail = e.detail;
    *ageSec = e.reported ? (long)((millis() - e.atMs) / 1000UL) : -1;
    return e.reported;
}
