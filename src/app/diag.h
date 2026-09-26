#pragma once

// Last known health of each external service, shown on the web
// diagnostics page (web_config.cpp). Each module reports the outcome of
// its own fetch/operation; nothing here does any network I/O itself.
enum DiagService
{
    DIAG_LTA,
    DIAG_CALENDAR,
    DIAG_WEATHER,
    DIAG_OPENAI,
    DIAG_AUDIO,
    DIAG_OTA,
    DIAG_COUNT,
};

void diagReport(DiagService service, bool ok, const char *detail = "");

const char *diagServiceName(DiagService service);
// Last result for one service; returns false (ageSec = -1) if never reported.
bool diagGet(DiagService service, bool *ok, const char **detail, long *ageSec);
