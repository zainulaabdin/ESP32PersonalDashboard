#pragma once

// Over-the-air updates from this project's GitHub releases
// (github.com/zainulaabdin/ESP32PersonalDashboard, private - needs
// GITHUB_OTA_TOKEN in secrets.h). A release is installed when its tag is
// newer than FIRMWARE_VERSION and it has a "firmware.bin" asset; the image
// goes into the idle OTA slot (default_16MB.csv: app0/app1) and the board
// restarts into it.

// Call every loop(): checks shortly after Wi-Fi comes up, then every 6h.
void otaTick();

// Queue a check right now (serial "ota" command); installs a newer release.
void otaCheckNow();
// Queue a check that only reports (OTA_UPDATE_AVAILABLE) - web page.
void otaCheckOnly();
// Latest release version seen by the last successful check ("" if none).
const char *otaLatestVersion();

// Outcome of the most recent check, and a counter that increments each time
// a check finishes (so the Settings tab can tell its own tap's result apart
// from an older one).
enum OtaCheckState
{
    OTA_IDLE,
    OTA_CHECKING,
    OTA_UP_TO_DATE,
    OTA_UPDATE_AVAILABLE,
    OTA_CHECK_FAILED,
};
OtaCheckState otaCheckState();
unsigned otaCheckCount();

// True while a download/flash is in progress; progress 0-100.
bool otaInProgress();
int otaProgressPct();
