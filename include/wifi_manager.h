/**
 * @file wifi_manager.h
 * @brief WiFi Access Point + Station (client) management
 *
 * Supports dual AP+STA mode: the device always runs its own hotspot
 * and can optionally join an existing WiFi network for LAN access.
 * Falls back to AP-only when the STA network is unavailable.
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"

// STA connection state machine
enum STAState {
    STA_DISABLED = 0,   // No STA network configured / cleared
    STA_CONNECTING,     // Connection attempt in progress
    STA_CONNECTED,      // Associated and got IP
    STA_DISCONNECTED,   // Was connected, lost connection (will retry)
    STA_FAILED          // Connection attempt failed (will retry with backoff)
};

class WiFiManager {
public:
    WiFiManager();

    /**
     * @brief Initialize WiFi: start AP, optionally connect STA
     * @return true if AP started successfully
     */
    bool begin();

    /**
     * @brief Periodic housekeeping — call from main loop()
     *
     * Handles STA reconnection with exponential backoff and
     * async WiFi scan completion.
     */
    void loop();

    /**
     * @brief Stop all WiFi (AP + STA)
     */
    void stop();

    // === AP Accessors ===

    String getAPSSID() const { return m_apSSID; }
    String getAPPassword() const { return m_apPassword; }
    uint8_t getAPChannel() const { return m_apChannel; }
    bool    getAPHidden() const { return m_apHidden; }
    uint8_t getClientCount();

    // Backward-compat aliases
    String getSSID() const { return getAPSSID(); }
    String getPassword() const { return getAPPassword(); }

    /**
     * @brief Update AP settings and restart AP
     */
    bool updateAPSettings(const String& ssid, const String& password,
                          uint8_t channel = DEFAULT_AP_CHANNEL, bool hidden = false);

    // === STA Accessors ===

    STAState getSTAState() const { return m_staState; }
    bool     isSTAEnabled() const { return m_staEnabled; }
    bool     isSTAConnected() const { return m_staState == STA_CONNECTED; }
    String   getSTASSID() const { return m_staSSID; }
    String   getSTAIP() const;
    int32_t  getSTARSSI() const;

    /**
     * @brief Configure STA credentials and attempt connection
     * @param ssid Network SSID
     * @param password Network password
     */
    void configureSTASettings(const String& ssid, const String& password);

    /**
     * @brief Clear STA credentials and revert to AP-only mode
     */
    void clearSTASettings();

    // === WiFi Scanning ===

    /**
     * @brief Start an async WiFi network scan
     * @return true if scan started (false if already in progress)
     */
    bool startWiFiScan();

    /**
     * @brief Get scan results as JSON
     * @param doc Output JSON document with "networks" array
     * @return true if results are ready, false if still scanning
     */
    bool getScanResultsJSON(JsonDocument& doc);

    // === Persistence ===

    void loadSettings();
    void saveAPSettings();
    void saveSTASettings();

private:
    // --- AP state ---
    String  m_apSSID;
    String  m_apPassword;
    uint8_t m_apChannel;
    bool    m_apHidden;

    // --- STA state ---
    String   m_staSSID;
    String   m_staPassword;
    bool     m_staEnabled;
    STAState m_staState;
    unsigned long m_staLastAttemptTime;
    unsigned long m_staRetryInterval;
    uint8_t  m_staRetryCount;

    // --- Scan state ---
    bool m_scanInProgress;

    // --- Internal helpers ---

    /**
     * @brief Start/restart the soft-AP with current settings
     */
    bool startAP();

    /**
     * @brief Attempt STA connection (blocking up to STA_CONNECT_TIMEOUT_MS)
     */
    void attemptSTAConnection();

    /**
     * @brief Generate default SSID with MAC suffix
     */
    String generateSSID();

    /**
     * @brief WiFi event handler (static → routes to instance)
     */
    static void wifiEventCallback(WiFiEvent_t event, WiFiEventInfo_t info);

    /**
     * @brief Instance-level event handling
     */
    void handleSTAEvent(WiFiEvent_t event, WiFiEventInfo_t info);

    /// Singleton pointer for static callback routing
    static WiFiManager* s_instance;
};
