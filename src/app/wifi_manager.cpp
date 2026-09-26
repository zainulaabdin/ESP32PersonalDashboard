#include "wifi_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>
#include <esp_wifi.h>
#include <stdio.h>
#include "secrets_select.h"

// Talks to esp-idf's provisioning manager directly rather than through
// Arduino's WiFiProv library: WiFiProv references the BLE scheme
// unconditionally when Bluedroid is enabled in the SDK, which links the
// whole Bluetooth stack in (~500KB flash and ~21KB of permanently reserved
// internal DRAM - enough to starve mbedTLS, which needs two ~16KB
// contiguous internal-RAM buffers per handshake and can't use PSRAM in this
// SDK). Only the SoftAP scheme is referenced here, so none of that links.

// Proof-of-possession PIN - encoded in the QR code, so the user never types
// it. Fixed rather than random: there's nowhere to show a random one before
// the QR itself exists.
#define PROV_POP "esp32bus"


static char serviceName[16] = "PROV_ESP32";
static char qrPayload[128] = "";
static volatile bool provisioningActive = false;
// Set from the Wi-Fi event callback / UI thread, acted on in
// wifiConnectTask() - wifi_prov_mgr_deinit() waits on the manager's own
// cleanup, so it isn't called from inside an event callback.
static volatile bool provisioningEndRequested = false;
static volatile bool provisioningFailed = false;
static volatile bool provisioningSucceeded = false;
static volatile bool restartRequested = false;
// STA config from before the session. Starting provisioning wipes esp-idf's
// saved STA config, so a cancelled session would otherwise leave nothing to
// reconnect to (real log: WiFi.begin() -> "connect failed! 0x300a",
// ESP_ERR_WIFI_SSID).
static wifi_config_t savedStaConfig;
static bool haveSavedStaConfig = false;

// Whether esp-idf's own persisted STA config holds a network - the single
// source of truth for "is there a saved network". (A separate NVS flag used
// to be kept for this, but it could say "provisioned" while the real config
// had been wiped by a cancelled session, leaving the board stuck retrying
// an empty SSID - real log: "connect failed! 0x300a".)
static bool haveSavedNetwork()
{
    wifi_config_t cfg;
    return esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0';
}

static void onWifiEvent(arduino_event_t *event)
{
    switch (event->event_id)
    {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.print("wifi: got IP ");
        Serial.print(IPAddress(event->event_info.got_ip.ip_info.ip.addr));
        Serial.print(", DNS ");
        Serial.println(WiFi.dnsIP());
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        Serial.println("wifi: disconnected, will retry");
        break;
    case ARDUINO_EVENT_PROV_START:
        Serial.printf("wifi: SoftAP provisioning started - AP \"%s\", scan the QR with the ESP SoftAP Provisioning app\n", serviceName);
        break;
    case ARDUINO_EVENT_PROV_CRED_RECV:
        Serial.printf("wifi: provisioning received credentials for SSID: %s\n",
                      (const char *)event->event_info.prov_cred_recv.ssid);
        break;
    case ARDUINO_EVENT_PROV_CRED_FAIL:
        Serial.println("wifi: provisioned credentials failed (wrong password or AP not found) - app can retry");
        provisioningFailed = true;
        break;
    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
        Serial.println("wifi: provisioning succeeded, credentials saved");
        provisioningSucceeded = true;
        break;
    case ARDUINO_EVENT_PROV_END:
        Serial.println("wifi: provisioning ended");
        provisioningEndRequested = true;
        break;
    default:
        break;
    }
}

static void finishProvisioning()
{
    bool succeeded = provisioningSucceeded;
    wifi_prov_mgr_deinit(); // also stops the AP/HTTP server if still running (cancel path)
    WiFi.mode(WIFI_STA);    // the SoftAP scheme switched the radio to AP+STA
    provisioningEndRequested = false;
    provisioningSucceeded = false;
    provisioningFailed = false;

    if (succeeded)
    {
        // A successful session leaves internal RAM behind that a cancelled
        // one doesn't (real log: every TLS connect then failed with "SSL -
        // Memory allocation failed" until a reboot, and mbedTLS can't use
        // PSRAM here), plus the AP+STA-mode DHCP lease that clobbered DNS.
        // The new network is already saved by esp-idf, so restart and come
        // up clean on it. settings.cpp performs the restart (flushes its own
        // pending NVS writes first); provisioningActive stays true until
        // then so the overlay remains up.
        Serial.println("wifi: provisioning succeeded - restarting to come up clean on the new network");
        restartRequested = true;
        return;
    }

    if (haveSavedStaConfig)
    {
        esp_wifi_set_config(WIFI_IF_STA, &savedStaConfig);
        Serial.printf("wifi: provisioning cancelled, restored previous network (%s)\n", (const char *)savedStaConfig.sta.ssid);
    }
    // Fresh STA-only lease - the SoftAP's DHCP server replaced lwIP's DNS
    // server while it was up.
    WiFi.disconnect();
    WiFi.begin();
    provisioningActive = false;
    Serial.println("wifi: provisioning manager released, back to STA mode");
}

// Own task on core 0 so reconnects never block loop() (LVGL + touch).
static void wifiConnectTask(void *)
{
    uint32_t ticks = 0;
    for (;;)
    {
        if (provisioningActive)
        {
            if (provisioningFailed)
            {
                // Lets the app retry within the same session instead of
                // the manager refusing further credentials.
                provisioningFailed = false;
                wifi_prov_mgr_reset_sm_state_on_failure();
            }
            if (provisioningEndRequested && !restartRequested)
                finishProvisioning();
        }
        else if (++ticks % 10 == 0 && WiFi.status() != WL_CONNECTED)
        {
            WiFi.reconnect();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void wifiManagerInit()
{
    WiFi.onEvent(onWifiEvent);
    WiFi.mode(WIFI_STA);

    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    snprintf(serviceName, sizeof(serviceName), "PROV_%02X%02X%02X", mac[3], mac[4], mac[5]);

    bool haveDefault = WIFI_DEFAULT_SSID != nullptr && WIFI_DEFAULT_SSID[0] != '\0';
    bool needsProvisioning = false;
    if (haveSavedNetwork())
    {
        // No arguments: reconnect with the STA config esp-idf persisted
        // (from provisioning, or an earlier secrets.h default connection).
        Serial.println("wifi: reconnecting with saved credentials");
        WiFi.begin();
    }
    else if (haveDefault)
    {
        Serial.printf("wifi: not yet provisioned, connecting with secrets.h default (%s)\n", WIFI_DEFAULT_SSID);
        WiFi.begin(WIFI_DEFAULT_SSID, WIFI_DEFAULT_PASSWORD);
    }
    else
    {
        needsProvisioning = true;
    }

    xTaskCreatePinnedToCore(wifiConnectTask, "wifiConnect", 4096, NULL, 1, NULL, 0);

    // No credentials anywhere - go straight to provisioning so the QR shows
    // at boot (settingsTick() raises the overlay) instead of sitting offline.
    if (needsProvisioning)
    {
        Serial.println("wifi: no saved or default credentials, starting provisioning");
        wifiProvisioningStart();
    }
}

void wifiProvisioningStart()
{
    if (provisioningActive)
        return;

    snprintf(qrPayload, sizeof(qrPayload),
             "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}",
             serviceName, PROV_POP);

    haveSavedStaConfig = esp_wifi_get_config(WIFI_IF_STA, &savedStaConfig) == ESP_OK && savedStaConfig.sta.ssid[0] != '\0';

    wifi_prov_mgr_config_t config = {};
    config.scheme = wifi_prov_scheme_softap;
    config.scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
    config.app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;

    esp_err_t err = wifi_prov_mgr_init(config);
    if (err != ESP_OK)
    {
        Serial.printf("wifi: wifi_prov_mgr_init failed (%d)\n", err);
        return;
    }
    // Open AP (no service_key) - the POP still protects the session itself
    // (Security1: X25519 handshake + AES-CTR), and it's only up while the
    // user has deliberately opened this screen.
    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, PROV_POP, serviceName, NULL);
    if (err != ESP_OK)
    {
        Serial.printf("wifi: wifi_prov_mgr_start_provisioning failed (%d)\n", err);
        wifi_prov_mgr_deinit();
        WiFi.mode(WIFI_STA);
        return;
    }
    provisioningEndRequested = false;
    provisioningFailed = false;
    provisioningSucceeded = false;
    provisioningActive = true;
}

void wifiProvisioningCancel()
{
    if (provisioningActive)
        provisioningEndRequested = true;
}

bool wifiProvisioningInProgress()
{
    return provisioningActive;
}

const char *wifiProvisioningQrPayload()
{
    return qrPayload;
}

const char *wifiProvisioningServiceName()
{
    return serviceName;
}

bool wifiRestartRequested()
{
    return restartRequested;
}
