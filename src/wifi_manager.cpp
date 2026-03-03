/**
 * @file wifi_manager.cpp
 * @brief WiFi AP + STA dual-mode management
 *
 * Always starts the AP hotspot first, then optionally connects to an
 * existing network (STA) for LAN access.  Reconnects automatically
 * with exponential backoff when the STA network drops.
 */

#include "wifi_manager.h"

extern Preferences preferences;

// Singleton for static callback routing
WiFiManager* WiFiManager::s_instance = nullptr;

// ---------------------------------------------------------------
// Construction
// ---------------------------------------------------------------

WiFiManager::WiFiManager()
    : m_apChannel(DEFAULT_AP_CHANNEL)
    , m_apHidden(false)
    , m_staEnabled(DEFAULT_STA_ENABLED)
    , m_staState(STA_DISABLED)
    , m_staLastAttemptTime(0)
    , m_staRetryInterval(STA_RETRY_INTERVAL_MS)
    , m_staRetryCount(0)
    , m_scanInProgress(false) {
    s_instance = this;
}

// ---------------------------------------------------------------
// begin()  —  called once from setup()
// ---------------------------------------------------------------

bool WiFiManager::begin() {
    DEBUG_PRINTLN("[WiFi] Initializing...");

    loadSettings();

    // Register event callback before any WiFi operations
    WiFi.onEvent(wifiEventCallback);

    // Choose mode: AP-only or AP+STA
    if (m_staEnabled && m_staSSID.length() > 0) {
        WiFi.mode(WIFI_AP_STA);
        DEBUG_PRINTLN("[WiFi] Mode: AP + STA");
    } else {
        WiFi.mode(WIFI_AP);
        DEBUG_PRINTLN("[WiFi] Mode: AP only");
    }

    // Always start the AP first
    if (!startAP()) {
        DEBUG_PRINTLN("[WiFi] ERROR: Failed to start AP!");
        return false;
    }

    // If STA is configured, attempt initial connection
    if (m_staEnabled && m_staSSID.length() > 0) {
        DEBUG_PRINTF("[WiFi] Connecting to \"%s\"...\n", m_staSSID.c_str());
        attemptSTAConnection();
    }

    return true;
}

// ---------------------------------------------------------------
// loop()  —  call from main loop() every iteration
// ---------------------------------------------------------------

void WiFiManager::loop() {
    // --- STA reconnection with exponential backoff ---
    if (m_staEnabled && m_staSSID.length() > 0 &&
        (m_staState == STA_DISCONNECTED || m_staState == STA_FAILED)) {

        unsigned long now = millis();
        if (now - m_staLastAttemptTime >= m_staRetryInterval) {
            DEBUG_PRINTF("[WiFi] STA retry #%u (interval %lu ms)\n",
                         m_staRetryCount + 1, m_staRetryInterval);

            // Ensure we're in AP+STA mode for reconnect (AP as fallback)
            if (WiFi.getMode() != WIFI_AP_STA) {
                WiFi.mode(WIFI_AP_STA);
                delay(100);
                startAP();
            }
            attemptSTAConnection();
        }
    }

    // --- Async scan completion check ---
    if (m_scanInProgress) {
        int16_t result = WiFi.scanComplete();
        if (result != WIFI_SCAN_RUNNING) {
            m_scanInProgress = false;
            // Results stay in WiFi driver until getScanResultsJSON() reads them
            if (result == WIFI_SCAN_FAILED) {
                DEBUG_PRINTLN("[WiFi] Scan failed");
            } else {
                DEBUG_PRINTF("[WiFi] Scan complete: %d networks\n", result);
            }
        }
    }
}

// ---------------------------------------------------------------
// stop()
// ---------------------------------------------------------------

void WiFiManager::stop() {
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    m_staState = STA_DISABLED;
    DEBUG_PRINTLN("[WiFi] All radios stopped");
}

// ---------------------------------------------------------------
// AP helpers
// ---------------------------------------------------------------

bool WiFiManager::startAP() {
    if (!WiFi.softAP(m_apSSID.c_str(), m_apPassword.c_str(),
                      m_apChannel, m_apHidden)) {
        return false;
    }

    if (!WiFi.softAPConfig(DEFAULT_AP_IP, DEFAULT_AP_GATEWAY, DEFAULT_AP_SUBNET)) {
        DEBUG_PRINTLN("[WiFi] ERROR: Failed to configure AP IP!");
        return false;
    }

    DEBUG_PRINTF("[WiFi] AP SSID : %s\n", m_apSSID.c_str());
    DEBUG_PRINTF("[WiFi] AP IP   : %s\n", WiFi.softAPIP().toString().c_str());
    return true;
}

uint8_t WiFiManager::getClientCount() {
    return WiFi.softAPgetStationNum();
}

bool WiFiManager::updateAPSettings(const String& ssid, const String& password,
                                    uint8_t channel, bool hidden) {
    m_apSSID     = ssid;
    m_apPassword = password;
    m_apChannel  = channel;
    m_apHidden   = hidden;

    saveAPSettings();

    // Restart AP with new settings
    WiFi.softAPdisconnect(true);
    delay(100);
    return startAP();
}

// ---------------------------------------------------------------
// STA connection
// ---------------------------------------------------------------

void WiFiManager::attemptSTAConnection() {
    m_staState = STA_CONNECTING;
    m_staLastAttemptTime = millis();

    WiFi.begin(m_staSSID.c_str(), m_staPassword.c_str());

    // Block up to STA_CONNECT_TIMEOUT_MS — AP is already running so
    // clients can still reach 192.168.4.1 while we wait.
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - start < STA_CONNECT_TIMEOUT_MS) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        m_staState = STA_CONNECTED;
        m_staRetryCount = 0;
        m_staRetryInterval = STA_RETRY_INTERVAL_MS;  // reset backoff
        DEBUG_PRINTF("[WiFi] STA connected — IP: %s  RSSI: %d dBm\n",
                     WiFi.localIP().toString().c_str(), WiFi.RSSI());
        DEBUG_PRINTF("[WiFi] Operating channel: %d\n", WiFi.channel());

        // Disable AP — device is now reachable on the LAN
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        DEBUG_PRINTLN("[WiFi] AP disabled (STA connected)");
    } else {
        m_staState = STA_FAILED;
        m_staRetryCount++;
        // Exponential backoff: double interval, cap at max
        m_staRetryInterval = min((unsigned long)STA_MAX_RETRY_BACKOFF,
                                  m_staRetryInterval * 2);
        WiFi.disconnect(false);  // stop trying, keep STA mode active
        DEBUG_PRINTF("[WiFi] STA connection failed (status %d)\n", WiFi.status());
    }
}

String WiFiManager::getSTAIP() const {
    if (m_staState == STA_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "";
}

int32_t WiFiManager::getSTARSSI() const {
    if (m_staState == STA_CONNECTED) {
        return WiFi.RSSI();
    }
    return 0;
}

// ---------------------------------------------------------------
// STA configuration
// ---------------------------------------------------------------

void WiFiManager::configureSTASettings(const String& ssid, const String& password) {
    m_staSSID     = ssid;
    m_staPassword = password;
    m_staEnabled  = true;
    m_staRetryCount = 0;
    m_staRetryInterval = STA_RETRY_INTERVAL_MS;

    saveSTASettings();

    // We need AP+STA mode to attempt connection while keeping AP as fallback
    wifi_mode_t currentMode = WiFi.getMode();
    if (currentMode != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
        delay(100);
        startAP();  // Re-start AP (mode change can reset it)
    }

    DEBUG_PRINTF("[WiFi] Connecting to \"%s\"...\n", m_staSSID.c_str());
    attemptSTAConnection();
    // If connection succeeds, attemptSTAConnection() will disable AP
}

void WiFiManager::clearSTASettings() {
    DEBUG_PRINTLN("[WiFi] Clearing STA settings");

    m_staSSID     = "";
    m_staPassword = "";
    m_staEnabled  = false;
    m_staState    = STA_DISABLED;
    m_staRetryCount = 0;
    m_staRetryInterval = STA_RETRY_INTERVAL_MS;

    // Remove STA keys from NVS
    preferences.remove(NVS_KEY_STA_SSID);
    preferences.remove(NVS_KEY_STA_PASS);
    preferences.remove(NVS_KEY_STA_ENABLED);

    // Disconnect STA and switch to AP-only
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    delay(100);
    startAP();  // Restore AP (mode change resets it)

    DEBUG_PRINTLN("[WiFi] Reverted to AP-only mode");
}

// ---------------------------------------------------------------
// WiFi scanning
// ---------------------------------------------------------------

bool WiFiManager::startWiFiScan() {
    if (m_scanInProgress) {
        DEBUG_PRINTLN("[WiFi] Scan already in progress");
        return false;
    }

    int16_t result = WiFi.scanNetworks(true, false);  // async=true, showHidden=false
    if (result == WIFI_SCAN_RUNNING) {
        m_scanInProgress = true;
        DEBUG_PRINTLN("[WiFi] Async scan started");
        return true;
    }

    // If scanNetworks returned immediately (e.g. cached results)
    if (result >= 0) {
        m_scanInProgress = false;
        DEBUG_PRINTF("[WiFi] Scan returned %d results immediately\n", result);
        return true;
    }

    DEBUG_PRINTLN("[WiFi] Failed to start scan");
    return false;
}

bool WiFiManager::getScanResultsJSON(JsonDocument& doc) {
    int16_t n = WiFi.scanComplete();

    if (n == WIFI_SCAN_RUNNING) {
        doc["scanning"] = true;
        return false;
    }

    doc["scanning"] = false;
    JsonArray networks = doc["networks"].to<JsonArray>();

    if (n > 0) {
        for (int i = 0; i < n; i++) {
            JsonObject net = networks.add<JsonObject>();
            net["ssid"]       = WiFi.SSID(i);
            net["rssi"]       = WiFi.RSSI(i);
            net["channel"]    = WiFi.channel(i);
            net["encryption"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);

            switch (WiFi.encryptionType(i)) {
                case WIFI_AUTH_OPEN:            net["auth"] = "Open"; break;
                case WIFI_AUTH_WEP:             net["auth"] = "WEP"; break;
                case WIFI_AUTH_WPA_PSK:         net["auth"] = "WPA"; break;
                case WIFI_AUTH_WPA2_PSK:        net["auth"] = "WPA2"; break;
                case WIFI_AUTH_WPA_WPA2_PSK:    net["auth"] = "WPA/WPA2"; break;
                case WIFI_AUTH_WPA3_PSK:        net["auth"] = "WPA3"; break;
                case WIFI_AUTH_WPA2_WPA3_PSK:   net["auth"] = "WPA2/WPA3"; break;
                default:                        net["auth"] = "Unknown"; break;
            }
        }
    }

    WiFi.scanDelete();  // Free memory used by scan results
    return true;
}

// ---------------------------------------------------------------
// WiFi event callback
// ---------------------------------------------------------------

void WiFiManager::wifiEventCallback(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (s_instance) {
        s_instance->handleSTAEvent(event, info);
    }
}

void WiFiManager::handleSTAEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            if (m_staState != STA_CONNECTED) {
                m_staState = STA_CONNECTED;
                m_staRetryCount = 0;
                m_staRetryInterval = STA_RETRY_INTERVAL_MS;
                DEBUG_PRINTF("[WiFi] STA got IP: %s\n",
                             WiFi.localIP().toString().c_str());

                // Disable AP now that STA is connected — device is
                // reachable on the LAN, no need for the hotspot.
                WiFi.softAPdisconnect(true);
                WiFi.mode(WIFI_STA);
                DEBUG_PRINTLN("[WiFi] AP disabled (STA connected)");
            }
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (m_staEnabled && m_staState == STA_CONNECTED) {
                m_staState = STA_DISCONNECTED;
                m_staLastAttemptTime = millis();  // start retry timer
                DEBUG_PRINTLN("[WiFi] STA disconnected — re-enabling AP fallback");

                // Re-enable AP as fallback so the device is still reachable
                WiFi.mode(WIFI_AP_STA);
                delay(100);
                startAP();
                DEBUG_PRINTLN("[WiFi] AP re-enabled for fallback access");
            }
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------

void WiFiManager::loadSettings() {
    // --- AP settings (backward-compatible with existing NVS keys) ---
    String savedSSID = preferences.getString(NVS_KEY_WIFI_SSID, "");
    String savedPass = preferences.getString(NVS_KEY_WIFI_PASS, "");

    if (savedSSID.length() > 0) {
        m_apSSID     = savedSSID;
        m_apPassword = savedPass;
        DEBUG_PRINTLN("[WiFi] AP settings loaded from NVS");
    } else {
        m_apSSID     = generateSSID();
        m_apPassword = DEFAULT_AP_PASSWORD;
        saveAPSettings();
        DEBUG_PRINTLN("[WiFi] AP using defaults");
    }

    // --- STA settings ---
    m_staEnabled  = preferences.getBool(NVS_KEY_STA_ENABLED, DEFAULT_STA_ENABLED);
    m_staSSID     = preferences.getString(NVS_KEY_STA_SSID, DEFAULT_STA_SSID);
    m_staPassword = preferences.getString(NVS_KEY_STA_PASS, DEFAULT_STA_PASSWORD);

    if (m_staEnabled && m_staSSID.length() > 0) {
        m_staState = STA_DISCONNECTED;  // will trigger connection in begin()
        DEBUG_PRINTF("[WiFi] STA configured for \"%s\"\n", m_staSSID.c_str());
    } else {
        m_staState   = STA_DISABLED;
        m_staEnabled = false;
    }
}

void WiFiManager::saveAPSettings() {
    preferences.putString(NVS_KEY_WIFI_SSID, m_apSSID);
    preferences.putString(NVS_KEY_WIFI_PASS, m_apPassword);
    DEBUG_PRINTLN("[WiFi] AP settings saved");
}

void WiFiManager::saveSTASettings() {
    preferences.putBool(NVS_KEY_STA_ENABLED, m_staEnabled);
    preferences.putString(NVS_KEY_STA_SSID, m_staSSID);
    preferences.putString(NVS_KEY_STA_PASS, m_staPassword);
    DEBUG_PRINTLN("[WiFi] STA settings saved");
}

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------

String WiFiManager::generateSSID() {
    // Use eFuse MAC — always available, even before WiFi.mode() is set
    uint64_t efuseMac = ESP.getEfuseMac();
    uint8_t* mac = reinterpret_cast<uint8_t*>(&efuseMac);

    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X",
             DEFAULT_AP_SSID_PREFIX, mac[4], mac[5]);

    return String(ssid);
}
