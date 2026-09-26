#pragma once

// Owns Wi-Fi connection/provisioning for the whole app - replaces the old
// WiFiMulti multi-network setup. Only ONE saved network at a time:
// - Until the device has been provisioned once, connects with secrets.h's
//   WIFI_DEFAULT_SSID/PASSWORD.
// - wifiProvisioningStart() (Settings tab's Wi-Fi button) starts Espressif's
//   SoftAP provisioning - the protocol the "ESP SoftAP Provisioning" phone
//   app speaks. The board brings up its own open AP named
//   wifiProvisioningServiceName(); the app joins it (via the on-screen QR
//   code) and sends the new network's credentials, which esp-idf's Wi-Fi
//   driver persists in its own NVS config. Deliberately SoftAP, not BLE:
//   no Bluetooth stack is linked into the firmware at all.
void wifiManagerInit(); // call once from setup(), starts the connection task

// Starts SoftAP provisioning. No-op if already in progress.
void wifiProvisioningStart();

// True once a provisioning session has succeeded and the device should
// restart onto the new network - settings.cpp's settingsTick() acts on it
// (flushing its own pending NVS writes first, same as the reboot button).
bool wifiRestartRequested();

// Stops an in-progress provisioning session without changing the saved
// network (e.g. the user tapped the overlay to give up).
void wifiProvisioningCancel();

// True from wifiProvisioningStart() until the session has fully ended and
// the provisioning manager has been torn down.
bool wifiProvisioningInProgress();

// JSON payload Espressif's provisioning apps expect in the QR code:
// {"ver":"v1","name":"<ap name>","pop":"<pin>","transport":"softap"}
const char *wifiProvisioningQrPayload();

// The provisioning AP's SSID (PROV_xxxxxx, from the MAC) - shown on screen
// too, since iOS can't auto-join a network from the app and the user has
// to pick it in Settings by hand.
const char *wifiProvisioningServiceName();
