/**
 * @file web_server.h
 * @brief Async web server with REST API and WebSocket support
 */
#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"
#include "web_content.h"

class WebServerManager {
public:
    WebServerManager();
    ~WebServerManager();

    /**
     * @brief Initialize and start web server
     * @return true if successful
     */
    bool begin();

    /**
     * @brief Process WebSocket events (call in loop)
     */
    void loop();

    /**
     * @brief Send message to all WebSocket clients
     * @param message Message to send
     */
    void broadcastWebSocket(const String& message);

    /**
     * @brief Send JSON message to all WebSocket clients
     * @param type Message type
     * @param data JSON data
     */
    void broadcastJSON(const String& type, const String& data);

    /**
     * @brief Update progress for operation
     * @param operation Operation name
     * @param progress Progress percentage (0-100)
     * @param message Status message
     */
    void sendProgress(const String& operation, int progress, const String& message);

private:
    AsyncWebServer* m_server;
    AsyncWebSocket* m_ws;

    /**
     * @brief Setup all API routes
     */
    void setupRoutes();

    /**
     * @brief Setup WebSocket handlers
     */
    void setupWebSocket();

    /**
     * @brief Handle WebSocket events
     */
    static void onWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                                 AwsEventType type, void* arg, uint8_t* data, size_t len);

    // API Route Handlers
    static void handleSystemStatus(AsyncWebServerRequest* request);
    static void handleSystemInfo(AsyncWebServerRequest* request);
    static void handleReboot(AsyncWebServerRequest* request);
    static void handleSleep(AsyncWebServerRequest* request);

    static void handleFirmwareUpload(AsyncWebServerRequest* request,
                                     String filename, size_t index,
                                     uint8_t* data, size_t len, bool final);
    static void handleFirmwareInfo(AsyncWebServerRequest* request);
    static void handleFirmwareFlash(AsyncWebServerRequest* request);
    static void handleFirmwareDelete(AsyncWebServerRequest* request);

    static void handleSWDConnect(AsyncWebServerRequest* request);
    static void handleSWDInfo(AsyncWebServerRequest* request);
    static void handleSWDErase(AsyncWebServerRequest* request);
    static void handleSWDFlash(AsyncWebServerRequest* request);

    // SWD firmware backup
    static void handleSWDBackup(AsyncWebServerRequest* request);
    static void handleSWDBackupProgress(AsyncWebServerRequest* request);
    static void handleSWDBackupDownload(AsyncWebServerRequest* request);

    // SWD firmware flash (upload + start)
    static void handleSWDFlashUpload(AsyncWebServerRequest* request,
                                     String filename, size_t index,
                                     uint8_t* data, size_t len, bool final);
    static void handleSWDFlashStart(AsyncWebServerRequest* request);
    static void handleSWDFlashProgress(AsyncWebServerRequest* request);

    static void handleBackupSettings(AsyncWebServerRequest* request);
    static void handleBackupChannels(AsyncWebServerRequest* request);
    static void handleBackupList(AsyncWebServerRequest* request);
    static void handleBackupDownload(AsyncWebServerRequest* request);
    static void handleBackupDelete(AsyncWebServerRequest* request);
    static void handleRestoreSettings(AsyncWebServerRequest* request);
    static void handleRestoreChannels(AsyncWebServerRequest* request);

    static void handleSerialBuffer(AsyncWebServerRequest* request);
    // handleSerialSend and handleSerialPassthrough use AsyncCallbackJsonWebHandler lambdas

    static void handleGPIOConfig(AsyncWebServerRequest* request);
    static void handleGPIOConfigPost(AsyncWebServerRequest* request);
    static void handleGPIOTest(AsyncWebServerRequest* request);
    static void handleGPIOReset(AsyncWebServerRequest* request);
    static void handleGPIODFU(AsyncWebServerRequest* request);

    static void handleSettings(AsyncWebServerRequest* request);
    // handleSettingsPost uses AsyncCallbackJsonWebHandler lambda in setupRoutes()
    static void handleFactoryReset(AsyncWebServerRequest* request);

    static void handleWiFiScan(AsyncWebServerRequest* request);
    static void handleWiFiScanResults(AsyncWebServerRequest* request);

    static void handleBackupStatus(AsyncWebServerRequest* request);
    static void handleSerialTest(AsyncWebServerRequest* request);
    static void handleSerialTestResult(AsyncWebServerRequest* request);

    // Meshtastic command console
    static void handleMeshtasticCommandResult(AsyncWebServerRequest* request);

    // Unified active operations endpoint
    static void handleOperationsActive(AsyncWebServerRequest* request);

    // OTA Update (firmware only — web UI is embedded via PROGMEM)
    static void handleOTAUpdate(AsyncWebServerRequest* request);
    static void handleOTAUpload(AsyncWebServerRequest* request,
                                String filename, size_t index,
                                uint8_t* data, size_t len, bool final);

    // Helper functions
    static void sendJSON(AsyncWebServerRequest* request, const JsonDocument& doc);
    static void sendError(AsyncWebServerRequest* request, const String& error, int code = 400);
    static void sendSuccess(AsyncWebServerRequest* request, const String& message);
};

// Global instance for callback access
extern WebServerManager* g_webServer;
