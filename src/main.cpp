/**
 * @file main.cpp
 * @brief Main entry point for ESP32-RAKFlasher
 *
 * Remote firmware management system for RAK4631 (nRF52840) modules.
 * Provides WiFi AP with WebUI for firmware flashing, SWD recovery,
 * configuration backup/restore, and serial monitoring.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include "config.h"
#include "sleep_manager.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "gpio_control.h"
#include "serial_monitor.h"
#include "serial_bridge.h"

// Global instances
Preferences preferences;
SleepManager sleepManager;
WiFiManager wifiManager;
WebServerManager webServer;
GPIOControl gpioControl;
SerialMonitor serialMonitor;
SerialBridge serialBridge;

// System state
SystemState currentState = STATE_IDLE;
unsigned long lastActivityTime = 0;

/**
 * @brief Forward serial data to WebSocket clients in real-time
 */
void onSerialData(const String& line) {
    if (g_webServer) {
        g_webServer->broadcastJSON("serial", line);
    }
}

/**
 * @brief Initialize LittleFS filesystem
 */
bool initFilesystem() {
    DEBUG_PRINTLN("[FS] Initializing LittleFS...");

    if (!LittleFS.begin(true)) {  // true = format on fail
        DEBUG_PRINTLN("[FS] ERROR: Failed to mount LittleFS!");
        return false;
    }

    // Create required directories
    const char* dirs[] = {PATH_UPLOADED, PATH_BACKUPS, PATH_LOGS, PATH_WWW};
    for (const char* dir : dirs) {
        if (!LittleFS.exists(dir)) {
            LittleFS.mkdir(dir);
            DEBUG_PRINTF("[FS] Created directory: %s\n", dir);
        }
    }

    // Display filesystem info
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    DEBUG_PRINTF("[FS] Total: %d KB, Used: %d KB, Free: %d KB\n",
                 total / 1024, used / 1024, (total - used) / 1024);

    return true;
}

/**
 * @brief Initialize all subsystems
 */
void setup() {
    // Initialize serial console
    Serial.begin(115200);
    delay(500);  // Allow serial to stabilize

    DEBUG_PRINTLN("\n\n");
    DEBUG_PRINTLN("========================================");
    DEBUG_PRINTLN("  ESP32-RAKFlasher v" FIRMWARE_VERSION);
    DEBUG_PRINTLN("  RAK4631 Remote Firmware Manager");
    DEBUG_PRINTLN("========================================");

    // Initialize NVS for persistent storage
    preferences.begin(NVS_NAMESPACE, false);

    // Initialize filesystem
    if (!initFilesystem()) {
        DEBUG_PRINTLN("[FATAL] Filesystem initialization failed!");
        while(1) delay(1000);  // Halt on filesystem failure
    }

    // Initialize GPIO control
    if (!gpioControl.begin()) {
        DEBUG_PRINTLN("[ERROR] GPIO initialization failed!");
    }

    // Check wake reason
    esp_sleep_wakeup_cause_t wakeup_reason = sleepManager.getWakeupReason();
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
        DEBUG_PRINTLN("[WAKE] Woken by external GPIO signal");
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        DEBUG_PRINTLN("[WAKE] Woken by timer");
    } else {
        DEBUG_PRINTLN("[WAKE] Power on or hard reset");
    }

    // Initialize WiFi AP
    if (!wifiManager.begin()) {
        DEBUG_PRINTLN("[ERROR] WiFi initialization failed!");
    }

    // Load configured baud rate from NVS (defaults to build-time constant)
    uint32_t baudRate = preferences.getUInt(NVS_KEY_BAUD_RATE, RAK_SERIAL_BAUD);

    // Initialize serial monitor for RAK communication
    if (!serialMonitor.begin(RAK_SERIAL_PORT, baudRate)) {
        DEBUG_PRINTLN("[ERROR] Serial monitor initialization failed!");
    }

    // Initialize web server
    if (!webServer.begin()) {
        DEBUG_PRINTLN("[ERROR] Web server initialization failed!");
    }

    // Initialize serial bridge (TCP server on port 4403 + HTTP bridge)
    if (!serialBridge.begin(RAK_SERIAL_PORT)) {
        DEBUG_PRINTLN("[ERROR] Serial bridge initialization failed!");
    }
    serialBridge.setBaudRate(baudRate);

    // Start mDNS for network discovery
    String hostname = wifiManager.getAPSSID();
    hostname.toLowerCase();
    hostname.replace(" ", "-");
    if (MDNS.begin(hostname.c_str())) {
        MDNS.addService("http", "tcp", WEB_SERVER_PORT);
        MDNS.addService("meshtastic", "tcp", BRIDGE_TCP_PORT);
        DEBUG_PRINTF("[mDNS] Registered: %s.local\n", hostname.c_str());
    } else {
        DEBUG_PRINTLN("[mDNS] Failed to start mDNS");
    }

    // Connect serial monitor to WebSocket for real-time browser updates
    serialMonitor.setDataCallback(onSerialData);

    // Print connection info
    DEBUG_PRINTLN("\n[READY] System initialized successfully!");
    DEBUG_PRINTF("[WiFi] AP SSID: %s\n", wifiManager.getAPSSID().c_str());
    DEBUG_PRINTF("[WiFi] AP Pass: %s\n", wifiManager.getAPPassword().c_str());
    DEBUG_PRINTF("[WiFi] AP IP  : %s\n", WiFi.softAPIP().toString().c_str());
    if (wifiManager.isSTAConnected()) {
        DEBUG_PRINTF("[WiFi] STA Net: %s\n", wifiManager.getSTASSID().c_str());
        DEBUG_PRINTF("[WiFi] STA IP : %s\n", wifiManager.getSTAIP().c_str());
    } else if (wifiManager.isSTAEnabled()) {
        DEBUG_PRINTLN("[WiFi] STA: configured but not connected");
    }
    DEBUG_PRINTF("[Serial] Baud rate: %u\n", baudRate);
    DEBUG_PRINTF("[Bridge] TCP port %d | HTTP: /api/v1/toradio, /api/v1/fromradio\n",
                 BRIDGE_TCP_PORT);
    DEBUG_PRINTF("[mDNS] http://%s.local\n", hostname.c_str());
    DEBUG_PRINTF("[Web] Open http://%s in your browser\n", WiFi.softAPIP().toString().c_str());
    DEBUG_PRINTLN("========================================\n");

    lastActivityTime = millis();
}

/**
 * @brief Main loop - non-blocking processing
 */
void loop() {
    // Update activity timestamp on any client connection, STA link, or active bridge
    if (WiFi.softAPgetStationNum() > 0 || wifiManager.isSTAConnected() ||
        serialBridge.isActive()) {
        lastActivityTime = millis();
    }

    // Process serial bridge (TCP connections, HTTP frame parsing)
    serialBridge.loop();

    // Process serial monitor only when bridge is idle
    // (bridge suspends monitor, but skip the call entirely for efficiency)
    if (!serialBridge.isActive()) {
        serialMonitor.loop();
    }

    // WiFi housekeeping (STA reconnect, scan completion)
    wifiManager.loop();

    // Process WebSocket messages
    webServer.loop();

    // Check for auto-sleep timeout — don't sleep if bridge is active
    unsigned long idleTime = millis() - lastActivityTime;
    if (idleTime > SLEEP_DELAY_MS && currentState == STATE_IDLE) {
        if (WiFi.softAPgetStationNum() == 0 && !wifiManager.isSTAConnected() &&
            !serialBridge.isActive()) {
            DEBUG_PRINTLN("[SLEEP] Idle timeout - entering deep sleep");
            sleepManager.enterDeepSleep();
        }
    }

    // Small delay to prevent watchdog triggers
    delay(10);
}

/**
 * @brief Update system activity timestamp
 * Call this whenever user interaction occurs
 */
void updateActivity() {
    lastActivityTime = millis();
}

/**
 * @brief Get current system state
 */
SystemState getSystemState() {
    return currentState;
}

/**
 * @brief Set current system state
 */
void setSystemState(SystemState state) {
    currentState = state;
    updateActivity();
}
