#pragma once

// Settings web page served by the board itself on port 80 (address shown
// under the Settings tab icons): bus stop, refresh intervals, API keys and
// calendar URL, plus a /diag diagnostics page. Protected by HTTP basic auth
// (user "admin", password WEB_CONFIG_PASSWORD in web_config.cpp).
// Three pages with a bottom tab bar: Settings (/), Diagnostics (/diag),
// Actions (/actions: provision Wi-Fi, OTA check, sleep, restart).
//
// Handlers run inside webConfigTick() on the UI loop, so they can touch
// LVGL state directly. The server is stopped while Wi-Fi provisioning runs
// (the provisioning SoftAP uses port 80 itself).
void webConfigTick(); // call every loop()
