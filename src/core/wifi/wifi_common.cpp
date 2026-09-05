#include "core/wifi/wifi_common.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/powerSave.h"
#include "core/radio_mem.h"
#include "core/ram_profile.h"
#include "core/settings.h"
#include "core/utils.h"
#include "core/wifi/wifi_mac.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "modules/ble/ble_common.h"
#include <array>
#include <esp_event.h>
#include <esp_netif.h>
#include <globals.h>

static TaskHandle_t timezoneTaskHandle = NULL;
static bool wifiTransitioning = false;

static void setConfiguredWifiHostname() {
    const char *hostname = bruceConfig.wifiHostnameEnabled ? bruceConfig.wifiHostname.c_str() : "esp32 bruce";
    WiFi.setHostname(hostname);

    esp_netif_t *staNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (staNetif != nullptr) esp_netif_set_hostname(staNetif, hostname);
}

static bool wifiHostnameMatchesConfiguration() {
    if (!bruceConfig.wifiHostnameEnabled) return true;

    const char *hostname = WiFi.getHostname();
    return hostname != nullptr && bruceConfig.wifiHostname == hostname;
}

static bool reconnectWithConfiguredWifiHostname(const String &ssid, const String &pwd) {
    if (!bruceConfig.wifiHostnameEnabled || wifiHostnameMatchesConfiguration()) return true;

    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        WiFi.disconnect(true, true);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        WiFi.mode(WIFI_MODE_STA);
        setConfiguredWifiHostname();
        WiFi.begin(ssid, pwd);

        for (uint8_t wait = 0; wait < 50 && !WiFi.isConnected(); wait++) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }

        if (WiFi.isConnected() && wifiHostnameMatchesConfiguration()) return true;
    }

    return false;
}

esp_err_t wifiRawTx(wifi_interface_t ifx, const void *frame, int len, uint8_t retries) {
    esp_err_t err = esp_wifi_80211_tx(ifx, frame, len, false);
    for (uint8_t i = 0; err == ESP_ERR_NO_MEM && i < retries; i++) {
        vTaskDelay(1); // let the driver drain TX buffers and retry
        err = esp_wifi_80211_tx(ifx, frame, len, false);
    }
    return err;
}

void ensureWifiPlatform() {
    static bool netifInitialized = false;
    static bool eventLoopCreated = false;
    static portMUX_TYPE platformMux = portMUX_INITIALIZER_UNLOCKED;

    portENTER_CRITICAL(&platformMux);
    bool needNetif = !netifInitialized;
    bool needLoop = !eventLoopCreated;
    portEXIT_CRITICAL(&platformMux);

    if (needNetif) {
        ESP_ERROR_CHECK(esp_netif_init());
        portENTER_CRITICAL(&platformMux);
        netifInitialized = true;
        portEXIT_CRITICAL(&platformMux);
    }

    if (needLoop) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_ERR_INVALID_STATE) { ESP_ERROR_CHECK(err); }
        portENTER_CRITICAL(&platformMux);
        eventLoopCreated = true;
        portEXIT_CRITICAL(&platformMux);
    }
}

bool _wifiConnect(const String &ssid, int encryption, int32_t channel, const uint8_t *bssid) {
    String password = bruceConfig.getWifiPassword(ssid);
    if (password == "" && encryption > 0) { password = keyboard(password, 63, "Network Password:", true); }
    if (password == "\x1B") return false;
    bool connected = _connectToWifiNetwork(ssid, password, channel, bssid);
    bool retry = false;

    while (!connected) {
        wakeUpScreen();

        options = {
            {"Retry",  [&]() { retry = true; } },
            {"Cancel", [&]() { retry = false; }},
        };
        loopOptions(options);

        if (!retry) {
            wifiDisconnect();
            return false;
        }

        password = keyboard(password, 63, "Network Password:", true);
        if (password == "\x1B") {
            wifiDisconnect();
            return false;
        }
        connected = _connectToWifiNetwork(ssid, password, channel, bssid);
    }

    if (connected) {
        wifiConnected = true;
        wifiIP = WiFi.localIP().toString();
        bruceConfig.addWifiCredential(ssid, password);

        // Start timezone update in background if not already running
        if (timezoneTaskHandle == NULL) {
            xTaskCreate(updateTimezoneTask, "updateTimezone", 4096, NULL, 1, &timezoneTaskHandle);
        }
    }

    delay(200);
    return connected;
}

bool _connectToWifiNetwork(const String &ssid, const String &pwd, int32_t channel, const uint8_t *bssid) {
    if (FORCE_RADIO_TEARDOWN_ON_SWITCH) {
        if (BLEConnected) {
            displayWarning("Board with no PSRAM, closing BLE Stack");
            vTaskDelay(700 / portTICK_PERIOD_MS);
        }
        stopBLEStack();
        vTaskDelay(300 / portTICK_PERIOD_MS);
    }

    RAM_LOG("wifi pre-mode"); // Wi-Fi is already up from the menu scan by this point
    drawMainBorderWithTitle("WiFi Connect");
    padprintln("");
    padprint("Connecting to: " + ssid + ".");
    WiFi.mode(WIFI_MODE_STA);
    setConfiguredWifiHostname();
    RAM_LOG("wifi post-mode");
    vTaskDelay(10 / portTICK_PERIOD_MS);
    WiFi.begin(ssid.c_str(), pwd.length() > 0 ? pwd.c_str() : NULL, channel, bssid);

    int i = 1;
    while (!WiFi.isConnected()) {
        if (tft.getCursorX() >= tftWidth - 12) {
            padprintln("");
            padprint("");
        }
#ifdef HAS_SCREEN
        tft.print(".");
#else
        Serial.print(".");
#endif

        if (i > 20) {
            displayError("Wifi Offline");
            vTaskDelay(500 / portTICK_RATE_MS);
            break;
        }

        vTaskDelay(500 / portTICK_RATE_MS);
        i++;
    }

    if (WiFi.isConnected() && !reconnectWithConfiguredWifiHostname(ssid, pwd)) {
        displayError("Hostname not applied");
        return false;
    }

    return WiFi.isConnected();
}

bool _setupAP(bool internetSharing) {
    IPAddress AP_GATEWAY(172, 0, 0, 1);
    if (internetSharing) WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(AP_GATEWAY, AP_GATEWAY, IPAddress(255, 255, 255, 0));
    setConfiguredWifiHostname();
    uint8_t apChannel = internetSharing && WiFi.channel() != 0 ? WiFi.channel() : 6;
    if (!WiFi.softAP(bruceConfig.wifiAp.ssid, bruceConfig.wifiAp.pwd, apChannel, 0, 4, false)) {
        displayError("WiFi AP start failed");
        return false;
    }

    if (internetSharing) {
        esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_err_t naptError = apNetif == nullptr ? ESP_ERR_NOT_FOUND : esp_netif_napt_enable(apNetif);
        if (naptError != ESP_OK) {
            Serial.printf(
                "[WiFi] NAT enable failed: %s (0x%lx), AP netif=%p\n",
                esp_err_to_name(naptError),
                static_cast<unsigned long>(naptError),
                apNetif
            );
            WiFi.softAPdisconnect();
            displayError("Internet sharing unavailable");
            return false;
        }
    }

    wifiIP = WiFi.softAPIP().toString(); // update global var
    Serial.println("IP: " + wifiIP);
    wifiConnected = true;
    return true;
}

void wifiDisconnect() {
    wifiTransitioning = true;

    wifi_mode_t mode = WiFi.getMode();
    if (mode & WIFI_MODE_AP) {
        esp_netif_t *apNetif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (apNetif != nullptr) esp_netif_napt_disable(apNetif);
        WiFi.softAPdisconnect();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    if (mode & WIFI_MODE_STA) {
        WiFi.disconnect(true, true);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
#ifndef CONFIG_IDF_TARGET_ESP32P4
    if (mode != WIFI_MODE_NULL) {
        WiFi.mode(WIFI_OFF);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
#endif

    wifiConnected = false;
    wifiTransitioning = false;
}

bool wifiConnectMenu(wifi_mode_t mode) {
    if (WiFi.isConnected() && mode != WIFI_AP_STA) return false; // safeguard

    if (FORCE_RADIO_TEARDOWN_ON_SWITCH) {
        stopBLEStack();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // Check if WiFi is in transition
    if (wifiTransitioning) {
        displayTextLine("WiFi busy, please wait...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return false;
    }

    switch (mode) {
        case WIFI_AP: // access point
            WiFi.mode(WIFI_AP);
            setConfiguredWifiHostname();
            return _setupAP();
            break;

        case WIFI_STA: { // station mode
            int nets;
            if (!radioHasMemForWifi()) {
                displayError("Low RAM: free BLE/SD first", true);
                return false;
            }
            WiFi.mode(WIFI_MODE_STA);
            setConfiguredWifiHostname();

            // wifiMACMenu();
            applyConfiguredMAC();

            bool refresh_scan = false;
            do {
                displayTextLine("Scanning..");
                nets = WiFi.scanNetworks();

                String selSsid = "";
                int selEnc = 0;
                int32_t selCh = 0;
                uint8_t selBssid[6] = {0};
                bool selHidden = false;

                options = {};
                for (int i = 0; i < nets; i++) {
                    if (options.size() < 250) {
                        String ssid = WiFi.SSID(i);
                        int encryptionType = WiFi.encryptionType(i);
                        int32_t rssi = WiFi.RSSI(i);
                        int32_t ch = WiFi.channel(i);
                        uint8_t *bssidPtr = WiFi.BSSID(i);
                        std::array<uint8_t, 6> bssidArr;
                        if (bssidPtr) memcpy(bssidArr.data(), bssidPtr, 6);

                        // Check if the network is secured
                        String encryptionPrefix = (encryptionType == WIFI_AUTH_OPEN) ? "" : "#";
                        String encryptionTypeStr;
                        switch (encryptionType) {
                            case WIFI_AUTH_OPEN: encryptionTypeStr = "Open"; break;
                            case WIFI_AUTH_WEP: encryptionTypeStr = "WEP"; break;
                            case WIFI_AUTH_WPA_PSK: encryptionTypeStr = "WPA/PSK"; break;
                            case WIFI_AUTH_WPA2_PSK: encryptionTypeStr = "WPA2/PSK"; break;
                            case WIFI_AUTH_WPA_WPA2_PSK: encryptionTypeStr = "WPA/WPA2/PSK"; break;
                            case WIFI_AUTH_WPA2_ENTERPRISE: encryptionTypeStr = "WPA2/Enterprise"; break;
                            case WIFI_AUTH_WPA3_PSK: encryptionTypeStr = "WPA3/PSK"; break;
                            case WIFI_AUTH_WPA2_WPA3_PSK: encryptionTypeStr = "WPA2/WPA3/PSK"; break;
                            default: encryptionTypeStr = "Unknown"; break;
                        }

                        String optionText = encryptionPrefix + ssid + "(" + String(rssi) + "|" +
                                            encryptionTypeStr + "|ch." + String(ch) + ")";

                        options.push_back(
                            {optionText.c_str(),
                             [&selSsid, &selEnc, &selCh, &selBssid, ssid, encryptionType, ch, bssidArr]() {
                                 selSsid = ssid;
                                 selEnc = encryptionType;
                                 selCh = ch;
                                 memcpy(selBssid, bssidArr.data(), 6);
                             }}
                        );
                    }
                }
                WiFi.scanDelete();
                options.push_back({"Hidden SSID", [&selHidden]() { selHidden = true; }});
                addOptionToMainMenu();

                loopOptions(options);
                options.clear();

                if (returnToMenu) {
                    refresh_scan = false;
                } else if (selHidden) {
                    String __ssid = keyboard("", 32, "Your SSID");
                    if (__ssid != "\x1B") _wifiConnect(__ssid.c_str(), 8);
                    refresh_scan = false;
                } else if (selSsid != "") {
                    _wifiConnect(selSsid, selEnc, selCh, selBssid);
                    refresh_scan = false;
                } else if (check(EscPress)) {
                    refresh_scan = true;
                } else {
                    refresh_scan = false;
                }
            } while (refresh_scan);
        } break;

        case WIFI_AP_STA: // repeater mode
            return wifiStartInternetAP();

        default: // error handling
            Serial.println("Unknown wifi mode: " + String(mode));
            break;
    }

    if (returnToMenu) {
        wifiDisconnect(); // Forced turning off the wifi module if exiting back to the menu
        return false;
    }
    return wifiConnected;
}

bool wifiStartInternetAP() {
    if (WiFi.getMode() & WIFI_MODE_AP) return true;

    if (!radioHasMemForWifi()) {
        displayError("Low RAM: free BLE/SD first", true);
        return false;
    }

    WiFi.mode(WIFI_MODE_STA);
    setConfiguredWifiHostname();
    displayTextLine("Select Internet WiFi");
    int networks = WiFi.scanNetworks();
    String selectedSsid;
    int selectedEncryption = WIFI_AUTH_OPEN;
    int32_t selectedChannel = 0;
    uint8_t selectedBssid[6] = {0};
    bool selectedHidden = false;

    options = {};
    for (int i = 0; i < networks && options.size() < 250; i++) {
        String ssid = WiFi.SSID(i);
        int encryption = WiFi.encryptionType(i);
        int32_t channel = WiFi.channel(i);
        uint8_t *bssid = WiFi.BSSID(i);
        std::array<uint8_t, 6> bssidArray = {};
        if (bssid) memcpy(bssidArray.data(), bssid, 6);

        String prefix = encryption == WIFI_AUTH_OPEN ? "" : "#";
        String label = prefix + ssid + " (" + String(WiFi.RSSI(i)) + "|ch." + String(channel) + ")";
        options.push_back(
            {label.c_str(),
             [&selectedSsid,
              &selectedEncryption,
              &selectedChannel,
              &selectedBssid,
              ssid,
              encryption,
              channel,
              bssidArray]() {
                 selectedSsid = ssid;
                 selectedEncryption = encryption;
                 selectedChannel = channel;
                 memcpy(selectedBssid, bssidArray.data(), 6);
             }}
        );
    }
    WiFi.scanDelete();
    options.push_back({"Hidden SSID", [&selectedHidden]() { selectedHidden = true; }});
    addOptionToMainMenu();
    loopOptions(options);
    options.clear();

    if (selectedHidden) {
        selectedSsid = keyboard("", 32, "Your SSID");
        if (selectedSsid != "\x1B") _wifiConnect(selectedSsid, WIFI_AUTH_WPA_PSK);
    } else if (selectedSsid != "") {
        _wifiConnect(selectedSsid, selectedEncryption, selectedChannel, selectedBssid);
    }

    if (!WiFi.isConnected()) {
        displayError("Internet WiFi unavailable", true);
        return false;
    }

    return _setupAP(true);
}

void wifiConnectTask(void *pvParameters) {
    if (WiFi.isConnected()) return;

    if (FORCE_RADIO_TEARDOWN_ON_SWITCH) {
        stopBLEStack();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // Check if WiFi is in transition
    if (wifiTransitioning) {
        vTaskDelete(NULL);
        return;
    }

    // No-PSRAM guard: don't bring Wi-Fi up if the contiguous DMA block is too
    // small (e.g. BLE already active) — the scan would half-init the driver and
    // crash on teardown. Silent bail: this is a background auto-connect task.
    if (!radioHasMemForWifi()) {
        vTaskDelete(NULL);
        return;
    }

    WiFi.mode(WIFI_MODE_STA);
    setConfiguredWifiHostname();
    int nets = WiFi.scanNetworks();
    String ssid;
    String pwd;

    for (int i = 0; i < nets; i++) {
        ssid = WiFi.SSID(i);
        pwd = bruceConfig.getWifiPassword(ssid);
        if (pwd == "") continue;

        int32_t ch = WiFi.channel(i);
        uint8_t *bssid = WiFi.BSSID(i);
        WiFi.begin(ssid.c_str(), pwd.length() > 0 ? pwd.c_str() : NULL, ch, bssid);
        for (int i = 0; i < 50; i++) {
            if (WiFi.isConnected()) {
                if (!reconnectWithConfiguredWifiHostname(ssid, pwd)) continue;
                wifiConnected = true;
                wifiIP = WiFi.localIP().toString();

                // Start timezone update in background if not already running
                if (timezoneTaskHandle == NULL) {
                    xTaskCreate(updateTimezoneTask, "updateTimezone", 4096, NULL, 1, &timezoneTaskHandle);
                }
                drawStatusBar();
                break;
            }
            vTaskDelay(100 / portTICK_RATE_MS);
        }
    }
    WiFi.scanDelete();

    vTaskDelete(NULL);
    return;
}

String checkMAC() { return String(WiFi.macAddress()); }

bool wifiConnecttoKnownNet(void) {
    if (WiFi.isConnected()) return true; // safeguard

    if (FORCE_RADIO_TEARDOWN_ON_SWITCH) {
        stopBLEStack();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // Check if WiFi is in transition
    if (wifiTransitioning) {
        displayTextLine("WiFi busy, please wait...");
        vTaskDelay(500 / portTICK_PERIOD_MS);
        return false;
    }

    // No-PSRAM guard: refuse before the scan brings Wi-Fi up in low memory.
    if (!radioHasMemForWifi()) {
        displayError("Low RAM: free BLE/SD first", true);
        return false;
    }

    bool result = false;
    int nets;
    WiFi.mode(WIFI_MODE_STA);
    displayTextLine("Scanning Networks..");
    WiFi.disconnect(true, true);
    vTaskDelay(10 / portTICK_PERIOD_MS);
    WiFi.mode(WIFI_MODE_STA);
    setConfiguredWifiHostname();
    nets = WiFi.scanNetworks();
    for (int i = 0; i < nets; i++) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
        String ssid = WiFi.SSID(i);
        String password = bruceConfig.getWifiPassword(ssid);
        if (password != "") {
            Serial.println("Connecting to: " + ssid);
            result = _connectToWifiNetwork(ssid, password);
        }
        // Maybe it finds a known network and can't connect, then try the next
        // until it gets connected (or not)
        if (result) {
            Serial.println("Connected to: " + ssid);
            break;
        }
    }
    if (WiFi.isConnected()) {
        wifiConnected = true;
        wifiIP = WiFi.localIP().toString();

        // Start timezone update in background if not already running
        if (timezoneTaskHandle == NULL) {
            xTaskCreate(updateTimezoneTask, "updateTimezone", 4096, NULL, 1, &timezoneTaskHandle);
        }
    }
    return result;
}

void updateTimezoneTask(void *pvParameters) {
    // Wait a bit for connection to stabilize before updating timezone
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    // Only update timezone if WiFi is still connected
    if (WiFi.isConnected() && wifiConnected) { updateClockTimezone(); }

    // Clear the task handle before deleting
    timezoneTaskHandle = NULL;
    vTaskDelete(NULL);
}
