/**
 * @file web_server.cpp
 * @brief Implementation of async web server with REST API
 */

#include "web_server.h"
#include "wifi_manager.h"
#include "gpio_control.h"
#include "serial_monitor.h"
#include "serial_bridge.h"
#include "sleep_manager.h"
#include "serial_dfu.h"
#include "swd_engine.h"
#include "backup_manager.h"
#include "meshtastic_proto.h"
#include <AsyncJson.h>
#include <Update.h>
#include <Preferences.h>

// External global instances
extern WiFiManager wifiManager;
extern GPIOControl gpioControl;
extern SerialMonitor serialMonitor;
extern SerialBridge serialBridge;
extern SleepManager sleepManager;
extern Preferences preferences;
extern SystemState currentState;
extern void setSystemState(SystemState state);
extern void updateActivity();

// --- Async serial test state (FreeRTOS task) ---
static volatile bool s_serialTestRunning = false;
static volatile bool s_serialTestComplete = false;
static String s_serialTestResultJSON;

// Global instance for callbacks
WebServerManager* g_webServer = nullptr;

// Static instances for operations
static SerialDFU serialDFU;
static SWDEngine swdEngine;
static BackupManager backupManager;

// Async backup operation state
static volatile bool backupRunning = false;
static volatile bool backupComplete = false;
static String backupResultFilename = "";
static String backupResultError = "";
static String backupType = "";

// --- Async Meshtastic command state (FreeRTOS task) ---
static volatile bool s_meshCmdRunning = false;
static volatile bool s_meshCmdComplete = false;
static String s_meshCmdResultJSON;
static String s_meshCmdType;
static int s_meshCmdParam = 0;

// --- Async SWD backup/flash state (FreeRTOS task) ---
static volatile bool s_swdTaskRunning = false;
static volatile bool s_swdTaskComplete = false;
static volatile int s_swdProgress = 0;
static String s_swdProgressMsg;
static String s_swdResultJSON;
static String s_swdBackupFilename;
static String s_swdUploadedFile;

// FreeRTOS task: backup settings in background
void backupSettingsTask(void* param) {
    DEBUG_PRINTLN("[Backup] Task started: settings backup");
    serialMonitor.suspend();
    delay(200);  // Let serial monitor exit its read loop (m_suspended checked in while condition)

    String filename = backupManager.backupSettings();
    if (filename.length() > 0) {
        backupResultFilename = filename;
        backupResultError = "";
    } else {
        backupResultFilename = "";
        backupResultError = backupManager.getLastError();
    }

    serialMonitor.resume();
    backupComplete = true;
    backupRunning = false;
    setSystemState(STATE_IDLE);
    DEBUG_PRINTF("[Backup] Task complete: %s\n",
                 backupResultFilename.length() > 0 ? "success" : "failed");
    vTaskDelete(NULL);
}

// FreeRTOS task: backup channels in background
void backupChannelsTask(void* param) {
    DEBUG_PRINTLN("[Backup] Task started: channels backup");
    serialMonitor.suspend();
    delay(200);

    String filename = backupManager.backupChannels();
    if (filename.length() > 0) {
        backupResultFilename = filename;
        backupResultError = "";
    } else {
        backupResultFilename = "";
        backupResultError = backupManager.getLastError();
    }

    serialMonitor.resume();
    backupComplete = true;
    backupRunning = false;
    setSystemState(STATE_IDLE);
    vTaskDelete(NULL);
}

// FreeRTOS task: execute Meshtastic command in background
static void meshCmdTaskFunc(void* param) {
    extern HardwareSerial RAK_SERIAL_PORT;

    static MeshtasticProtocol meshProto;
    meshProto.setSerial(&RAK_SERIAL_PORT);

    serialMonitor.suspend();
    vTaskDelay(pdMS_TO_TICKS(200));

    // Send 32-byte wake burst (required for Meshtastic PROTO mode)
    uint8_t wakeBurst[32];
    memset(wakeBurst, 0xC3, sizeof(wakeBurst));
    RAK_SERIAL_PORT.write(wakeBurst, sizeof(wakeBurst));
    RAK_SERIAL_PORT.flush();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Drain pending serial data
    while (RAK_SERIAL_PORT.available()) RAK_SERIAL_PORT.read();

    // Auto-discover myNodeNum if not cached
    uint32_t myNode = meshProto.getMyNodeNum();
    if (myNode == 0) {
        DEBUG_PRINTLN("[MeshCmd] Discovering myNodeNum via want_config...");
        meshProto.requestConfigDump(69420);
        uint8_t frameBuf[MT_MAX_PAYLOAD];
        size_t frameLen;
        uint32_t discoveryStart = millis();
        while (millis() - discoveryStart < 8000) {
            if (meshProto.readFrame(frameBuf, &frameLen, 2000)) {
                uint8_t st;
                FromRadioType ft = meshProto.identifyFromRadio(frameBuf, frameLen, &st);
                if (ft == FR_MY_INFO) {
                    // Extract myNodeNum from MyNodeInfo
                    size_t pos = 0;
                    size_t vlen;
                    if (pos < frameLen && frameBuf[pos] == TAG_FROMRADIO_ID) {
                        pos++;
                        MeshtasticProtocol::decodeVarint(frameBuf + pos, &vlen);
                        pos += vlen;
                    }
                    if (pos < frameLen && frameBuf[pos] == TAG_FROMRADIO_MY_INFO) {
                        pos++;
                        uint32_t innerLen = MeshtasticProtocol::decodeVarint(frameBuf + pos, &vlen);
                        pos += vlen;
                        myNode = meshProto.extractMyNodeNum(frameBuf + pos, innerLen);
                        if (myNode != 0) {
                            meshProto.setMyNodeNum(myNode);
                            DEBUG_PRINTF("[MeshCmd] Discovered myNodeNum=0x%08X\n", myNode);
                        }
                    }
                    break;
                }
                if (ft == FR_CONFIG_COMPLETE) break;
            }
        }
        // Drain remaining config dump frames
        vTaskDelay(pdMS_TO_TICKS(200));
        while (RAK_SERIAL_PORT.available()) RAK_SERIAL_PORT.read();
    }

    JsonDocument doc;
    bool sent = false;
    bool skipRead = false;

    if (myNode == 0) {
        doc["error"] = "Could not discover myNodeNum. Is Meshtastic running in PROTO mode?";
        doc["success"] = false;
    } else {
        doc["myNodeNum"] = myNode;
        doc["myNodeNumHex"] = String("0x") + String(myNode, HEX);

        // Drain before sending command
        while (RAK_SERIAL_PORT.available()) RAK_SERIAL_PORT.read();

        // Dispatch based on command type
        if (s_meshCmdType == "get_config") {
            sent = meshProto.sendGetConfig(myNode, s_meshCmdParam);
        } else if (s_meshCmdType == "get_module") {
            sent = meshProto.sendGetModuleConfig(myNode, s_meshCmdParam);
        } else if (s_meshCmdType == "get_channel") {
            sent = meshProto.sendGetChannel(myNode, s_meshCmdParam);
        } else if (s_meshCmdType == "get_owner") {
            sent = meshProto.sendGetOwner(myNode);
        } else if (s_meshCmdType == "get_metadata") {
            sent = meshProto.sendGetMetadata(myNode);
        } else if (s_meshCmdType == "get_all") {
            // Use existing captureFullConfig
            JsonDocument configDoc;
            bool ok = meshProto.captureFullConfig(configDoc, 15000);
            doc["command"] = "get_all";
            doc["success"] = ok;
            doc["config"] = configDoc;
            skipRead = true;
        } else if (s_meshCmdType == "reboot") {
            sent = meshProto.sendReboot(myNode, s_meshCmdParam > 0 ? s_meshCmdParam : 5);
        } else if (s_meshCmdType == "factory_reset") {
            sent = meshProto.sendFactoryReset(myNode);
        } else if (s_meshCmdType == "nodedb_reset") {
            sent = meshProto.sendNodedbReset(myNode);
        } else {
            doc["error"] = "Unknown command: " + s_meshCmdType;
            doc["success"] = false;
            skipRead = true;
        }

        if (!skipRead) {
            doc["command"] = s_meshCmdType;
            doc["param"] = s_meshCmdParam;
            doc["sent"] = sent;

            if (sent) {
                int frameCount = meshProto.readResponseFrames(doc, 5000, 10);
                doc["success"] = (frameCount > 0);
            } else {
                doc["success"] = false;
                doc["error"] = "Failed to send frame";
            }
        }
    }

    serialMonitor.resume();

    // Add status field for JS polling loop
    doc["status"] = "complete";

    s_meshCmdResultJSON = "";
    serializeJson(doc, s_meshCmdResultJSON);
    s_meshCmdComplete = true;
    s_meshCmdRunning = false;
    DEBUG_PRINTF("[MeshCmd] Task complete, result=%d bytes\n", (int)s_meshCmdResultJSON.length());
    vTaskDelete(NULL);
}

// Progress callback for SWD operations — updates shared volatile state
static void swdProgressCallback(int percent, const String& message) {
    s_swdProgress = percent;
    s_swdProgressMsg = message;
}

// FreeRTOS task: SWD firmware backup
static void swdBackupTaskFunc(void* param) {
    DEBUG_PRINTLN("[SWD] Backup task started");
    setSystemState(STATE_SWD_RECOVERY);

    s_swdProgress = 0;
    s_swdProgressMsg = "Connecting...";

    swdEngine.begin();

    if (!swdEngine.isConnected()) {
        if (!swdEngine.connect()) {
            JsonDocument doc;
            doc["status"] = "error";
            doc["error"] = swdEngine.getLastError();
            s_swdResultJSON = "";
            serializeJson(doc, s_swdResultJSON);
            s_swdTaskComplete = true;
            s_swdTaskRunning = false;
            setSystemState(STATE_IDLE);
            vTaskDelete(NULL);
            return;
        }
    }

    // Generate filename with device ID
    String deviceId = swdEngine.getDeviceIDString();
    uint32_t flashSize = swdEngine.getFlashSize();

    // Create backups directory
    if (!LittleFS.exists("/backups/firmware")) {
        LittleFS.mkdir("/backups");
        LittleFS.mkdir("/backups/firmware");
    }

    // Build filename: /backups/firmware/{DEVICEID}.bin
    String filename = "/backups/firmware/" + deviceId + ".bin";
    s_swdBackupFilename = filename;

    s_swdProgressMsg = "Dumping flash...";
    bool ok = swdEngine.dumpFlash(filename, swdProgressCallback);

    if (ok) {
        // Also dump UICR
        String uicrFile = "/backups/firmware/" + deviceId + "_uicr.bin";
        swdEngine.dumpUICR(uicrFile);
    }

    JsonDocument doc;
    if (ok) {
        doc["status"] = "complete";
        doc["filename"] = filename;
        doc["flashSize"] = flashSize;
        doc["deviceId"] = deviceId;

        // Get actual file size
        File f = LittleFS.open(filename, "r");
        if (f) {
            doc["fileSize"] = (unsigned long)f.size();
            f.close();
        }
    } else {
        doc["status"] = "error";
        doc["error"] = swdEngine.getLastError();
    }

    s_swdResultJSON = "";
    serializeJson(doc, s_swdResultJSON);
    s_swdTaskComplete = true;
    s_swdTaskRunning = false;
    setSystemState(STATE_IDLE);
    DEBUG_PRINTF("[SWD] Backup task complete: %s\n", ok ? "success" : "failed");
    vTaskDelete(NULL);
}

// FreeRTOS task: SWD firmware flash from uploaded file
static void swdFlashTaskFunc(void* param) {
    DEBUG_PRINTLN("[SWD] Flash task started");
    setSystemState(STATE_SWD_RECOVERY);

    s_swdProgress = 0;
    s_swdProgressMsg = "Connecting...";

    swdEngine.begin();

    if (!swdEngine.isConnected()) {
        if (!swdEngine.connect()) {
            JsonDocument doc;
            doc["status"] = "error";
            doc["error"] = swdEngine.getLastError();
            s_swdResultJSON = "";
            serializeJson(doc, s_swdResultJSON);
            s_swdTaskComplete = true;
            s_swdTaskRunning = false;
            setSystemState(STATE_IDLE);
            vTaskDelete(NULL);
            return;
        }
    }

    // Erase chip first
    s_swdProgressMsg = "Erasing flash...";
    s_swdProgress = 5;
    if (!swdEngine.massErase()) {
        JsonDocument doc;
        doc["status"] = "error";
        doc["error"] = "Mass erase failed: " + swdEngine.getLastError();
        s_swdResultJSON = "";
        serializeJson(doc, s_swdResultJSON);
        s_swdTaskComplete = true;
        s_swdTaskRunning = false;
        setSystemState(STATE_IDLE);
        vTaskDelete(NULL);
        return;
    }

    s_swdProgressMsg = "Flashing firmware...";
    s_swdProgress = 10;
    bool ok = swdEngine.flashFromFile(s_swdUploadedFile, 0, swdProgressCallback);

    JsonDocument doc;
    if (ok) {
        doc["status"] = "complete";
        doc["message"] = "Firmware flashed successfully";
    } else {
        doc["status"] = "error";
        doc["error"] = swdEngine.getLastError();
    }

    s_swdResultJSON = "";
    serializeJson(doc, s_swdResultJSON);
    s_swdTaskComplete = true;
    s_swdTaskRunning = false;
    setSystemState(STATE_IDLE);
    DEBUG_PRINTF("[SWD] Flash task complete: %s\n", ok ? "success" : "failed");
    vTaskDelete(NULL);
}

WebServerManager::WebServerManager()
    : m_server(nullptr)
    , m_ws(nullptr) {
    g_webServer = this;
}

WebServerManager::~WebServerManager() {
    if (m_server) delete m_server;
    if (m_ws) delete m_ws;
}

bool WebServerManager::begin() {
    DEBUG_PRINTLN("[Web] Initializing web server...");

    m_server = new AsyncWebServer(WEB_SERVER_PORT);
    m_ws = new AsyncWebSocket(WEBSOCKET_PATH);

    // Add CORS headers to all responses (needed for Meshtastic web client)
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, PUT, POST, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Accept");

    // Setup WebSocket
    setupWebSocket();

    // Setup API routes
    setupRoutes();

    // Serve web UI from embedded PROGMEM (no separate LittleFS upload needed)
    m_server->on("/*", HTTP_GET, [](AsyncWebServerRequest* request) {
        String uri = request->url();

        // Default document
        if (uri == "/") uri = "/index.html";

        const EmbeddedFile* file = findEmbeddedFile(uri.c_str());
        if (file) {
            AsyncWebServerResponse* response = request->beginResponse(
                200, file->mimeType, file->data, file->length);
            response->addHeader("Content-Encoding", "gzip");
            response->addHeader("Cache-Control", "max-age=600");
            request->send(response);
        } else {
            request->send(404, "text/plain", "Not Found");
        }
    });

    // Start server
    m_server->begin();

    DEBUG_PRINTLN("[Web] Server started successfully");
    return true;
}

void WebServerManager::loop() {
    if (m_ws) {
        m_ws->cleanupClients();
    }
}

void WebServerManager::broadcastWebSocket(const String& message) {
    if (m_ws) {
        m_ws->textAll(message);
    }
}

void WebServerManager::broadcastJSON(const String& type, const String& data) {
    JsonDocument doc;
    doc["type"] = type;
    doc["data"] = data;
    doc["timestamp"] = millis();

    String json;
    serializeJson(doc, json);
    broadcastWebSocket(json);
}

void WebServerManager::sendProgress(const String& operation, int progress, const String& message) {
    JsonDocument doc;
    doc["type"] = "progress";
    doc["operation"] = operation;
    doc["progress"] = progress;
    doc["message"] = message;
    doc["timestamp"] = millis();

    String json;
    serializeJson(doc, json);
    broadcastWebSocket(json);
}

void WebServerManager::setupWebSocket() {
    m_ws->onEvent(onWebSocketEvent);
    m_server->addHandler(m_ws);
    DEBUG_PRINTLN("[Web] WebSocket configured");
}

void WebServerManager::onWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                                         AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        DEBUG_PRINTF("[WS] Client #%u connected from %s\n", client->id(),
                     client->remoteIP().toString().c_str());
        updateActivity();
    } else if (type == WS_EVT_DISCONNECT) {
        DEBUG_PRINTF("[WS] Client #%u disconnected\n", client->id());
    } else if (type == WS_EVT_DATA) {
        // Handle incoming WebSocket data
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len) {
            if (info->opcode == WS_TEXT) {
                data[len] = 0;  // Null terminate
                DEBUG_PRINTF("[WS] Received: %s\n", (char*)data);
            }
        }
    }
}

void WebServerManager::setupRoutes() {
    // System endpoints
    m_server->on("/api/status", HTTP_GET, handleSystemStatus);
    m_server->on("/api/info", HTTP_GET, handleSystemInfo);
    m_server->on("/api/operations/active", HTTP_GET, handleOperationsActive);
    m_server->on("/api/reboot", HTTP_POST, handleReboot);
    m_server->on("/api/sleep", HTTP_POST, handleSleep);

    // Firmware endpoints
    m_server->on("/api/firmware/upload", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            request->send(200, "application/json", "{\"success\":true}");
        },
        handleFirmwareUpload
    );
    m_server->on("/api/firmware/info", HTTP_GET, handleFirmwareInfo);
    m_server->on("/api/firmware/flash", HTTP_POST, handleFirmwareFlash);
    m_server->on("/api/firmware/uploaded", HTTP_DELETE, handleFirmwareDelete);

    // OTA Update endpoints — firmware and filesystem (LittleFS web UI)
    m_server->on("/api/ota/update", HTTP_POST, handleOTAUpdate, handleOTAUpload);
    // Filesystem OTA removed — web UI is embedded in firmware via PROGMEM

    // SWD Recovery endpoints
    m_server->on("/api/swd/connect", HTTP_GET, handleSWDConnect);
    m_server->on("/api/swd/info", HTTP_GET, handleSWDInfo);
    m_server->on("/api/swd/erase", HTTP_POST, handleSWDErase);
    m_server->on("/api/swd/flash", HTTP_POST, handleSWDFlash);
    m_server->on("/api/swd/backup", HTTP_POST, handleSWDBackup);
    m_server->on("/api/swd/backup/progress", HTTP_GET, handleSWDBackupProgress);
    m_server->on("/api/swd/backup/download", HTTP_GET, handleSWDBackupDownload);
    m_server->on("/api/swd/flash/upload", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            request->send(200, "application/json", "{\"success\":true}");
        },
        handleSWDFlashUpload
    );
    m_server->on("/api/swd/flash/start", HTTP_POST, handleSWDFlashStart);
    m_server->on("/api/swd/flash/progress", HTTP_GET, handleSWDFlashProgress);

    // Flash from existing backup file on LittleFS (no upload needed)
    auto* flashFromBackupHandler = new AsyncCallbackJsonWebHandler("/api/swd/flash/from-backup",
        [](AsyncWebServerRequest* request, JsonVariant& json) {
            JsonObject obj = json.as<JsonObject>();
            String filename = obj["filename"].as<String>();

            if (filename.length() == 0) {
                sendError(request, "Missing 'filename' parameter");
                return;
            }

            if (!LittleFS.exists(filename)) {
                sendError(request, "File not found: " + filename, 404);
                return;
            }

            if (s_swdTaskRunning) {
                sendError(request, "SWD operation already in progress", 409);
                return;
            }

            // Point flash task at the backup file (no upload needed)
            s_swdUploadedFile = filename;
            s_swdTaskRunning = true;
            s_swdTaskComplete = false;
            s_swdProgress = 0;
            s_swdProgressMsg = "Starting flash from backup...";
            s_swdResultJSON = "";

            xTaskCreatePinnedToCore(swdFlashTaskFunc, "swd_flash", 16384, NULL, 5, NULL, 1);
            sendSuccess(request, "Flash from backup started");
            updateActivity();
        }
    );
    flashFromBackupHandler->setMethod(HTTP_POST);
    m_server->addHandler(flashFromBackupHandler);

    // Backup endpoints
    m_server->on("/api/backup/settings", HTTP_POST, handleBackupSettings);
    m_server->on("/api/backup/channels", HTTP_POST, handleBackupChannels);
    m_server->on("/api/backup/list", HTTP_GET, handleBackupList);
    m_server->on("/api/backup/delete", HTTP_DELETE, handleBackupDelete);
    m_server->on("/api/backup/settings/download", HTTP_GET, handleBackupDownload);
    m_server->on("/api/backup/channels/download", HTTP_GET, handleBackupDownload);
    m_server->on("/api/backup/settings/restore", HTTP_POST, handleRestoreSettings);
    m_server->on("/api/backup/channels/restore", HTTP_POST, handleRestoreChannels);

    // Backup status polling endpoint
    m_server->on("/api/backup/status", HTTP_GET, handleBackupStatus);

    // Serial endpoints
    m_server->on("/api/serial/test", HTTP_POST, handleSerialTest);
    m_server->on("/api/serial/test", HTTP_GET, handleSerialTestResult);
    m_server->on("/api/serial/buffer", HTTP_GET, handleSerialBuffer);

    // Serial send - supports text commands or raw hex bytes
    auto* serialSendHandler = new AsyncCallbackJsonWebHandler("/api/serial/send",
        [](AsyncWebServerRequest* request, JsonVariant& json) {
            extern HardwareSerial RAK_SERIAL_PORT;
            JsonObject obj = json.as<JsonObject>();

            if (obj["hex"].is<const char*>() || obj["hex"].is<String>()) {
                // Send raw hex bytes: "94 C3 00 02 18 01"
                String hexStr = obj["hex"].as<String>();
                hexStr.trim();
                int bytesSent = 0;

                for (unsigned int i = 0; i < hexStr.length(); i++) {
                    char c = hexStr[i];
                    if (c == ' ' || c == ':' || c == '-') continue;

                    // Parse two hex characters
                    if (i + 1 < hexStr.length()) {
                        char hi = hexStr[i];
                        char lo = hexStr[i + 1];
                        uint8_t b = 0;

                        if (hi >= '0' && hi <= '9') b = (hi - '0') << 4;
                        else if (hi >= 'A' && hi <= 'F') b = (hi - 'A' + 10) << 4;
                        else if (hi >= 'a' && hi <= 'f') b = (hi - 'a' + 10) << 4;
                        else continue;

                        if (lo >= '0' && lo <= '9') b |= (lo - '0');
                        else if (lo >= 'A' && lo <= 'F') b |= (lo - 'A' + 10);
                        else if (lo >= 'a' && lo <= 'f') b |= (lo - 'a' + 10);
                        else continue;

                        RAK_SERIAL_PORT.write(b);
                        bytesSent++;
                        i++;  // skip second hex char
                    }
                }

                RAK_SERIAL_PORT.flush();
                String msg = "Sent " + String(bytesSent) + " bytes";
                sendSuccess(request, msg);
            } else if (obj["command"].is<const char*>() || obj["command"].is<String>()) {
                // Send text command
                String command = obj["command"].as<String>();
                serialMonitor.sendCommand(command);
                sendSuccess(request, "Command sent");
            } else {
                sendError(request, "Missing command or hex parameter");
            }
            updateActivity();
        });
    m_server->addHandler(serialSendHandler);

    // Serial passthrough - uses AsyncCallbackJsonWebHandler for JSON body
    auto* serialPassthroughHandler = new AsyncCallbackJsonWebHandler("/api/serial/passthrough",
        [](AsyncWebServerRequest* request, JsonVariant& json) {
            JsonObject obj = json.as<JsonObject>();
            if (obj["enable"].is<bool>() || obj["enable"].is<int>()) {
                bool enable = obj["enable"].as<bool>();
                serialMonitor.setPassthrough(enable);
                sendSuccess(request, enable ? "Passthrough enabled" : "Passthrough disabled");
            } else {
                sendError(request, "Missing enable parameter");
            }
            updateActivity();
        });
    m_server->addHandler(serialPassthroughHandler);

    // Serial hex mode toggle
    auto* serialHexHandler = new AsyncCallbackJsonWebHandler("/api/serial/hexmode",
        [](AsyncWebServerRequest* request, JsonVariant& json) {
            JsonObject obj = json.as<JsonObject>();
            if (obj["enable"].is<bool>() || obj["enable"].is<int>()) {
                bool enable = obj["enable"].as<bool>();
                serialMonitor.setHexMode(enable);
                sendSuccess(request, enable ? "Hex mode enabled" : "Text mode enabled");
            } else {
                sendError(request, "Missing enable parameter");
            }
            updateActivity();
        });
    m_server->addHandler(serialHexHandler);

    // Serial protocol override (manual select or reset to auto)
    auto* serialProtoHandler = new AsyncCallbackJsonWebHandler("/api/serial/protocol",
        [](AsyncWebServerRequest* request, JsonVariant& json) {
            JsonObject obj = json.as<JsonObject>();
            if (obj["protocol"].is<int>()) {
                int proto = obj["protocol"].as<int>();
                if (proto < 0 || proto > 6) {
                    sendError(request, "Invalid protocol ID (0-6)");
                    return;
                }
                if (proto == 0) {
                    // "Auto" — reset detection so next bytes are analyzed
                    serialMonitor.resetDetection();
                    sendSuccess(request, "Protocol detection reset to Auto");
                } else {
                    serialMonitor.setProtocol(static_cast<SerialProtocol>(proto));
                    String msg = String("Protocol forced: ") +
                                 SerialMonitor::protocolName(static_cast<SerialProtocol>(proto));
                    sendSuccess(request, msg.c_str());
                }
            } else {
                sendError(request, "Missing protocol parameter (int 0-6)");
            }
            updateActivity();
        });
    m_server->addHandler(serialProtoHandler);

    // Meshtastic command console endpoints
    auto* meshCmdHandler = new AsyncCallbackJsonWebHandler("/api/meshtastic/command",
        [](AsyncWebServerRequest* request, JsonVariant& json) {
            if (s_meshCmdRunning) {
                sendError(request, "Meshtastic command already running", 409);
                return;
            }

            if (serialBridge.isActive()) {
                sendError(request, "Serial bridge is active - disconnect bridge client first", 409);
                return;
            }

            JsonObject obj = json.as<JsonObject>();
            if (!obj["command"].is<const char*>() && !obj["command"].is<String>()) {
                sendError(request, "Missing 'command' field");
                return;
            }

            s_meshCmdType = obj["command"].as<String>();
            s_meshCmdParam = obj["type"].is<int>() ? obj["type"].as<int>() : 0;
            if (obj["index"].is<int>()) s_meshCmdParam = obj["index"].as<int>();
            if (obj["seconds"].is<int>()) s_meshCmdParam = obj["seconds"].as<int>();

            s_meshCmdRunning = true;
            s_meshCmdComplete = false;
            s_meshCmdResultJSON = "";

            xTaskCreatePinnedToCore(meshCmdTaskFunc, "mesh_cmd", 8192, NULL, 1, NULL, 1);

            JsonDocument doc;
            doc["status"] = "running";
            doc["command"] = s_meshCmdType;
            sendJSON(request, doc);
            updateActivity();
        });
    m_server->addHandler(meshCmdHandler);

    m_server->on("/api/meshtastic/command", HTTP_GET, handleMeshtasticCommandResult);

    // GPIO endpoints
    m_server->on("/api/gpio/config", HTTP_GET, handleGPIOConfig);
    m_server->on("/api/gpio/config", HTTP_POST, handleGPIOConfigPost);
    m_server->on("/api/gpio/test", HTTP_POST, handleGPIOTest);
    m_server->on("/api/gpio/reset", HTTP_POST, handleGPIOReset);
    m_server->on("/api/gpio/dfu", HTTP_POST, handleGPIODFU);

    // Settings endpoints
    m_server->on("/api/settings", HTTP_GET, handleSettings);
    m_server->on("/api/settings/factory", HTTP_POST, handleFactoryReset);

    // Settings POST — uses AsyncCallbackJsonWebHandler for JSON body
    auto* settingsPostHandler = new AsyncCallbackJsonWebHandler("/api/settings",
        [](AsyncWebServerRequest* request, JsonVariant& json) {
            JsonObject obj = json.as<JsonObject>();

            // --- AP WiFi settings ---
            if (obj["wifi"].is<JsonObject>()) {
                JsonObject wifi = obj["wifi"].as<JsonObject>();
                String ssid     = wifi["ssid"] | wifiManager.getAPSSID();
                String password = wifi["password"] | wifiManager.getAPPassword();
                uint8_t channel = wifi["channel"] | wifiManager.getAPChannel();
                bool hidden     = wifi["hidden"] | wifiManager.getAPHidden();
                wifiManager.updateAPSettings(ssid, password, channel, hidden);
            }

            // --- STA WiFi settings ---
            if (obj["sta"].is<JsonObject>()) {
                JsonObject sta = obj["sta"].as<JsonObject>();

                if (sta["clear"].is<bool>() && sta["clear"].as<bool>()) {
                    // "Disconnect and forget"
                    wifiManager.clearSTASettings();
                } else if (sta["ssid"].is<const char*>()) {
                    String ssid = sta["ssid"].as<String>();
                    String pass = sta["password"] | "";
                    wifiManager.configureSTASettings(ssid, pass);
                }
            }

            sendSuccess(request, "Settings updated");
            updateActivity();
        });
    m_server->addHandler(settingsPostHandler);

    // WiFi scan endpoints
    m_server->on("/api/wifi/scan", HTTP_POST, handleWiFiScan);
    m_server->on("/api/wifi/scan", HTTP_GET, handleWiFiScanResults);

    // ---------------------------------------------------------------
    // Meshtastic-compatible REST API (serial bridge)
    // ---------------------------------------------------------------

    // CORS preflight for /api/v1/ endpoints
    m_server->on("/api/v1/fromradio", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
        request->send(200);
    });
    m_server->on("/api/v1/toradio", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
        request->send(200);
    });

    // GET /api/v1/fromradio — return one raw protobuf FromRadio message
    m_server->on("/api/v1/fromradio", HTTP_GET, [](AsyncWebServerRequest* request) {
        if (serialBridge.getMode() == BRIDGE_TCP) {
            sendError(request, "TCP bridge active — HTTP API unavailable", 409);
            return;
        }

        static uint8_t fromBuf[MT_MAX_PAYLOAD];
        size_t len = serialBridge.httpReadFromRadio(fromBuf, sizeof(fromBuf));

        if (len > 0) {
            request->send(200, "application/x-protobuf", fromBuf, len);
        } else {
            request->send(204);  // No content available
        }
        updateActivity();
    });

    // PUT /api/v1/toradio — receive raw protobuf ToRadio message, frame and send to UART
    static uint8_t toRadioBuf[MT_MAX_PAYLOAD];
    static size_t toRadioLen = 0;

    m_server->on("/api/v1/toradio", HTTP_PUT,
        // Request handler (called after body is fully received)
        [](AsyncWebServerRequest* request) {
            if (serialBridge.getMode() == BRIDGE_TCP) {
                sendError(request, "TCP bridge active", 409);
                toRadioLen = 0;
                return;
            }

            if (toRadioLen > 0 && serialBridge.httpSendToRadio(toRadioBuf, toRadioLen)) {
                request->send(200);
            } else {
                request->send(400, "text/plain", "Empty or oversized payload");
            }
            toRadioLen = 0;
            updateActivity();
        },
        nullptr,  // no upload handler
        // Body handler (receives raw binary body in chunks)
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len,
           size_t index, size_t total) {
            if (index + len <= MT_MAX_PAYLOAD) {
                memcpy(toRadioBuf + index, data, len);
                if (index + len > toRadioLen) toRadioLen = index + len;
            }
        }
    );

    // Bridge status / control
    m_server->on("/api/bridge/status", HTTP_GET, [](AsyncWebServerRequest* request) {
        JsonDocument doc;
        doc["mode"] = (int)serialBridge.getMode();
        doc["modeName"] = serialBridge.getMode() == BRIDGE_TCP ? "tcp" :
                          serialBridge.getMode() == BRIDGE_HTTP ? "http" : "idle";
        doc["active"] = serialBridge.isActive();
        doc["tcpPort"] = BRIDGE_TCP_PORT;
        doc["tcpClient"] = serialBridge.getTCPClientIPStr();
        doc["activeSeconds"] = serialBridge.getActiveSeconds();
        doc["txBytes"] = serialBridge.getTxBytes();
        doc["rxBytes"] = serialBridge.getRxBytes();
        doc["baudRate"] = serialBridge.getBaudRate();
        sendJSON(request, doc);
    });

    // Disconnect bridge client
    m_server->on("/api/bridge/disconnect", HTTP_POST, [](AsyncWebServerRequest* request) {
        if (serialBridge.isActive()) {
            serialBridge.disconnectTCPClient();
            // For HTTP mode, just let the timeout handle it
            sendSuccess(request, "Bridge disconnected");
        } else {
            sendSuccess(request, "No active bridge");
        }
        updateActivity();
    });

    // Baud rate change
    auto* baudHandler = new AsyncCallbackJsonWebHandler("/api/serial/baud",
        [](AsyncWebServerRequest* request, JsonVariant& json) {
            JsonObject obj = json.as<JsonObject>();
            if (!obj["baud"].is<uint32_t>() && !obj["baud"].is<int>()) {
                sendError(request, "Missing baud parameter");
                return;
            }

            uint32_t baud = obj["baud"].as<uint32_t>();

            // Validate baud rate
            const uint32_t validBauds[] = {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
            bool valid = false;
            for (auto b : validBauds) {
                if (baud == b) { valid = true; break; }
            }
            if (!valid) {
                sendError(request, "Invalid baud rate");
                return;
            }

            if (serialBridge.isActive()) {
                sendError(request, "Cannot change baud while bridge is active", 409);
                return;
            }

            // Save to NVS
            preferences.putUInt(NVS_KEY_BAUD_RATE, baud);

            // Reinitialize serial port
            extern HardwareSerial RAK_SERIAL_PORT;
            RAK_SERIAL_PORT.updateBaudRate(baud);
            serialBridge.setBaudRate(baud);

            // Reset protocol detection
            serialMonitor.resetDetection();
            serialMonitor.clearBuffer();

            DEBUG_PRINTF("[Serial] Baud rate changed to %u\n", baud);
            sendSuccess(request, "Baud rate changed to " + String(baud));
            updateActivity();
        });
    m_server->addHandler(baudHandler);

    DEBUG_PRINTLN("[Web] API routes configured (incl. Meshtastic bridge)");
}

// System API Handlers
void WebServerManager::handleSystemStatus(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["connected"] = true;  // RAK connection would be detected
    doc["state"] = currentState;
    doc["uptime"] = millis() / 1000;
    doc["clients"] = WiFi.softAPgetStationNum();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["version"] = FIRMWARE_VERSION;
    doc["buildNumber"] = BUILD_NUMBER;

    // AP info
    doc["ap"]["ssid"] = wifiManager.getAPSSID();
    doc["ap"]["ip"]   = WiFi.softAPIP().toString();

    // STA info
    doc["sta"]["connected"] = wifiManager.isSTAConnected();
    doc["sta"]["ssid"]      = wifiManager.getSTASSID();
    doc["sta"]["ip"]        = wifiManager.getSTAIP();
    doc["sta"]["rssi"]      = wifiManager.getSTARSSI();
    doc["sta"]["state"]     = (int)wifiManager.getSTAState();

    // Bridge info
    BridgeMode bMode = serialBridge.getMode();
    doc["bridge"]["mode"]    = bMode == BRIDGE_TCP ? "TCP" : (bMode == BRIDGE_HTTP ? "HTTP" : "idle");
    doc["bridge"]["active"]  = serialBridge.isActive();
    doc["bridge"]["baud"]    = serialBridge.getBaudRate();

    sendJSON(request, doc);
    updateActivity();
}

void WebServerManager::handleSystemInfo(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["buildNumber"] = BUILD_NUMBER;
    doc["buildDate"] = String(BUILD_DATE) + " " + String(BUILD_TIME);
    doc["chipModel"] = ESP.getChipModel();
    doc["chipCores"] = ESP.getChipCores();
    doc["cpuFreq"] = ESP.getCpuFreqMHz();
    doc["flashSize"] = ESP.getFlashChipSize();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["freeStorage"] = LittleFS.totalBytes() - LittleFS.usedBytes();
    doc["totalStorage"] = LittleFS.totalBytes();
    doc["uptime"] = millis() / 1000;

    sendJSON(request, doc);
    updateActivity();
}

// Unified active operations endpoint — reports all running/completed async tasks
void WebServerManager::handleOperationsActive(AsyncWebServerRequest* request) {
    JsonDocument doc;
    JsonArray ops = doc["operations"].to<JsonArray>();

    // SWD backup/flash task
    if (s_swdTaskRunning || s_swdTaskComplete) {
        JsonObject op = ops.add<JsonObject>();
        op["type"] = "swd";
        op["running"] = (bool)s_swdTaskRunning;
        op["complete"] = (bool)s_swdTaskComplete;
        op["progress"] = (int)s_swdProgress;
        op["message"] = s_swdProgressMsg;
        op["page"] = "recovery.html";
        if (s_swdTaskComplete && s_swdResultJSON.length() > 0) {
            // Parse and embed result
            JsonDocument resultDoc;
            deserializeJson(resultDoc, s_swdResultJSON);
            op["result"] = resultDoc.as<JsonVariant>();
        }
    }

    // Meshtastic settings/channels backup task
    if (backupRunning || backupComplete) {
        JsonObject op = ops.add<JsonObject>();
        op["type"] = "backup";
        op["running"] = (bool)backupRunning;
        op["complete"] = (bool)backupComplete;
        op["page"] = "backup.html";
        if (backupComplete) {
            op["success"] = backupResultFilename.length() > 0;
            op["filename"] = backupResultFilename;
            op["error"] = backupResultError;
        }
    }

    // Meshtastic command task
    if (s_meshCmdRunning || s_meshCmdComplete) {
        JsonObject op = ops.add<JsonObject>();
        op["type"] = "meshtastic";
        op["running"] = (bool)s_meshCmdRunning;
        op["complete"] = (bool)s_meshCmdComplete;
        op["page"] = "meshtastic.html";
    }

    // Serial test task
    if (s_serialTestRunning || s_serialTestComplete) {
        JsonObject op = ops.add<JsonObject>();
        op["type"] = "serial_test";
        op["running"] = (bool)s_serialTestRunning;
        op["complete"] = (bool)s_serialTestComplete;
        op["page"] = "backup.html";
    }

    sendJSON(request, doc);
}

void WebServerManager::handleReboot(AsyncWebServerRequest* request) {
    sendSuccess(request, "Rebooting...");
    delay(100);
    ESP.restart();
}

void WebServerManager::handleSleep(AsyncWebServerRequest* request) {
    sendSuccess(request, "Entering deep sleep...");
    delay(100);
    sleepManager.enterDeepSleep();
}

// Firmware API Handlers
void WebServerManager::handleFirmwareUpload(AsyncWebServerRequest* request,
                                            String filename, size_t index,
                                            uint8_t* data, size_t len, bool final) {
    static File uploadFile;

    if (index == 0) {
        // Start upload
        DEBUG_PRINTF("[Upload] Starting: %s\n", filename.c_str());
        String path = String(PATH_UPLOADED) + "/firmware.zip";
        uploadFile = LittleFS.open(path, "w");
        if (!uploadFile) {
            DEBUG_PRINTLN("[Upload] ERROR: Failed to open file!");
        }
    }

    if (uploadFile && len) {
        uploadFile.write(data, len);
    }

    if (final) {
        if (uploadFile) {
            uploadFile.close();
            DEBUG_PRINTF("[Upload] Complete: %d bytes\n", index + len);
        }
    }

    updateActivity();
}

void WebServerManager::handleFirmwareInfo(AsyncWebServerRequest* request) {
    String path = String(PATH_UPLOADED) + "/firmware.zip";

    if (!LittleFS.exists(path)) {
        sendError(request, "No firmware uploaded", 404);
        return;
    }

    File file = LittleFS.open(path, "r");
    if (file) {
        JsonDocument doc;
        doc["filename"] = "firmware.zip";
        doc["size"] = file.size();
        doc["uploaded"] = true;
        file.close();

        sendJSON(request, doc);
    } else {
        sendError(request, "Failed to read file info");
    }
    updateActivity();
}

void WebServerManager::handleFirmwareFlash(AsyncWebServerRequest* request) {
    String path = String(PATH_UPLOADED) + "/firmware.zip";

    if (!LittleFS.exists(path)) {
        sendError(request, "No firmware uploaded", 404);
        return;
    }

    // Start flashing in background
    setSystemState(STATE_FLASHING);
    sendSuccess(request, "Flashing started");

    // TODO: Implement actual DFU flashing
    // For now, placeholder
    updateActivity();
}

void WebServerManager::handleFirmwareDelete(AsyncWebServerRequest* request) {
    String path = String(PATH_UPLOADED) + "/firmware.zip";

    if (LittleFS.remove(path)) {
        sendSuccess(request, "Firmware deleted");
    } else {
        sendError(request, "Failed to delete firmware");
    }
    updateActivity();
}

// SWD API Handlers

void WebServerManager::handleSWDConnect(AsyncWebServerRequest* request) {
    swdEngine.begin();

    if (swdEngine.connect()) {
        JsonDocument doc;
        doc["connected"] = true;
        doc["chipId"] = swdEngine.getChipID();

        // Include device info if available
        const DeviceInfo& info = swdEngine.getDeviceInfo();
        if (info.valid) {
            doc["flashSize"] = info.flashSize;
            doc["pageSize"] = info.codepageSize;
            doc["deviceId"] = swdEngine.getDeviceIDString();
            char partStr[12];
            snprintf(partStr, sizeof(partStr), "0x%08X", info.infoPart);
            doc["partNumber"] = partStr;
            doc["unlocked"] = true;
        }

        sendJSON(request, doc);
    } else {
        sendError(request, "SWD connection failed: " + swdEngine.getLastError());
    }
    updateActivity();
}

void WebServerManager::handleSWDInfo(AsyncWebServerRequest* request) {
    if (!swdEngine.isConnected()) {
        sendError(request, "Not connected — use Test Connection first");
        return;
    }

    JsonDocument doc;
    doc["chipId"] = swdEngine.getChipID();

    const DeviceInfo& info = swdEngine.getDeviceInfo();
    doc["flashSize"] = info.flashSize;
    doc["pageSize"] = info.codepageSize;
    doc["pages"] = info.codesize;
    doc["deviceId"] = swdEngine.getDeviceIDString();
    doc["deviceId0"] = info.deviceId0;
    doc["deviceId1"] = info.deviceId1;

    char partStr[12];
    snprintf(partStr, sizeof(partStr), "0x%08X", info.infoPart);
    doc["partNumber"] = partStr;

    char varStr[12];
    snprintf(varStr, sizeof(varStr), "0x%08X", info.infoVariant);
    doc["variant"] = varStr;

    doc["sdInfoArea"] = info.sdInfoArea;
    doc["unlocked"] = swdEngine.isDeviceUnlocked();

    sendJSON(request, doc);
    updateActivity();
}

void WebServerManager::handleSWDErase(AsyncWebServerRequest* request) {
    swdEngine.begin();
    setSystemState(STATE_SWD_RECOVERY);

    if (!swdEngine.isConnected()) {
        swdEngine.connect();
    }

    if (swdEngine.massErase()) {
        sendSuccess(request, "Mass erase complete");
    } else {
        sendError(request, "Mass erase failed: " + swdEngine.getLastError());
    }

    setSystemState(STATE_IDLE);
    updateActivity();
}

void WebServerManager::handleSWDFlash(AsyncWebServerRequest* request) {
    sendError(request, "Use /api/swd/flash/upload + /api/swd/flash/start instead");
    updateActivity();
}

// --- SWD Firmware Backup ---

void WebServerManager::handleSWDBackup(AsyncWebServerRequest* request) {
    if (s_swdTaskRunning) {
        sendError(request, "SWD operation already in progress");
        return;
    }

    s_swdTaskRunning = true;
    s_swdTaskComplete = false;
    s_swdProgress = 0;
    s_swdProgressMsg = "Starting...";
    s_swdResultJSON = "";

    xTaskCreatePinnedToCore(swdBackupTaskFunc, "swd_backup", 16384, NULL, 5, NULL, 1);
    sendSuccess(request, "Backup started");
    updateActivity();
}

void WebServerManager::handleSWDBackupProgress(AsyncWebServerRequest* request) {
    JsonDocument doc;

    if (s_swdTaskComplete) {
        // Return final result
        JsonDocument result;
        deserializeJson(result, s_swdResultJSON);
        // Merge progress fields
        doc["status"] = result["status"];
        if (result["error"].is<const char*>()) doc["error"] = result["error"];
        if (result["filename"].is<const char*>()) doc["filename"] = result["filename"];
        if (result["flashSize"].is<unsigned long>()) doc["flashSize"] = result["flashSize"];
        if (result["fileSize"].is<unsigned long>()) doc["fileSize"] = result["fileSize"];
        if (result["deviceId"].is<const char*>()) doc["deviceId"] = result["deviceId"];
        doc["progress"] = 100;
    } else if (s_swdTaskRunning) {
        doc["status"] = "running";
        doc["progress"] = s_swdProgress;
        doc["message"] = s_swdProgressMsg;
    } else {
        doc["status"] = "idle";
    }

    sendJSON(request, doc);
}

void WebServerManager::handleSWDBackupDownload(AsyncWebServerRequest* request) {
    String filename = s_swdBackupFilename;

    // Allow override via query parameter
    if (request->hasParam("file")) {
        filename = request->getParam("file")->value();
    }

    if (filename.length() == 0 || !LittleFS.exists(filename)) {
        sendError(request, "No backup file available", 404);
        return;
    }

    // Extract basename for Content-Disposition header
    String basename = filename;
    int lastSlash = filename.lastIndexOf('/');
    if (lastSlash >= 0) {
        basename = filename.substring(lastSlash + 1);
    }

    // Serve the binary file with proper filename
    AsyncWebServerResponse* response = request->beginResponse(
        LittleFS, filename, "application/octet-stream");
    response->addHeader("Content-Disposition",
        "attachment; filename=\"" + basename + "\"");
    request->send(response);
    updateActivity();
}

// --- SWD Firmware Flash (upload + start) ---

void WebServerManager::handleSWDFlashUpload(AsyncWebServerRequest* request,
                                             String filename, size_t index,
                                             uint8_t* data, size_t len, bool final) {
    static File uploadFile;
    String path = "/uploaded/swd_firmware.bin";

    if (index == 0) {
        DEBUG_PRINTF("[SWD] Upload start: %s\n", filename.c_str());
        if (!LittleFS.exists("/uploaded")) {
            LittleFS.mkdir("/uploaded");
        }
        uploadFile = LittleFS.open(path, "w");
    }

    if (uploadFile) {
        uploadFile.write(data, len);
    }

    if (final) {
        if (uploadFile) {
            uploadFile.close();
            DEBUG_PRINTF("[SWD] Upload complete: %u bytes\n", index + len);
            s_swdUploadedFile = path;
        }
    }
}

void WebServerManager::handleSWDFlashStart(AsyncWebServerRequest* request) {
    if (s_swdTaskRunning) {
        sendError(request, "SWD operation already in progress");
        return;
    }

    if (s_swdUploadedFile.length() == 0 || !LittleFS.exists(s_swdUploadedFile)) {
        sendError(request, "No firmware file uploaded — upload first via /api/swd/flash/upload");
        return;
    }

    s_swdTaskRunning = true;
    s_swdTaskComplete = false;
    s_swdProgress = 0;
    s_swdProgressMsg = "Starting...";
    s_swdResultJSON = "";

    xTaskCreatePinnedToCore(swdFlashTaskFunc, "swd_flash", 16384, NULL, 5, NULL, 1);
    sendSuccess(request, "Flash started");
    updateActivity();
}

void WebServerManager::handleSWDFlashProgress(AsyncWebServerRequest* request) {
    JsonDocument doc;

    if (s_swdTaskComplete) {
        JsonDocument result;
        deserializeJson(result, s_swdResultJSON);
        doc["status"] = result["status"];
        if (result["error"].is<const char*>()) doc["error"] = result["error"];
        if (result["message"].is<const char*>()) doc["message"] = result["message"];
        doc["progress"] = 100;
    } else if (s_swdTaskRunning) {
        doc["status"] = "running";
        doc["progress"] = s_swdProgress;
        doc["message"] = s_swdProgressMsg;
    } else {
        doc["status"] = "idle";
    }

    sendJSON(request, doc);
}

// Backup API Handlers — async via FreeRTOS tasks
void WebServerManager::handleBackupSettings(AsyncWebServerRequest* request) {
    if (serialBridge.isActive()) {
        sendError(request, "Serial bridge is active — disconnect bridge client first", 409);
        return;
    }
    if (backupRunning) {
        sendError(request, "Backup already in progress");
        return;
    }

    backupRunning = true;
    backupComplete = false;
    backupResultFilename = "";
    backupResultError = "";
    backupType = "settings";
    setSystemState(STATE_BACKUP);

    // Run backup in a separate FreeRTOS task to avoid blocking the async web server
    xTaskCreatePinnedToCore(backupSettingsTask, "backup_set", 8192, NULL, 1, NULL, 1);
    sendSuccess(request, "Backup started");
    updateActivity();
}

void WebServerManager::handleBackupChannels(AsyncWebServerRequest* request) {
    if (serialBridge.isActive()) {
        sendError(request, "Serial bridge is active — disconnect bridge client first", 409);
        return;
    }
    if (backupRunning) {
        sendError(request, "Backup already in progress");
        return;
    }

    backupRunning = true;
    backupComplete = false;
    backupResultFilename = "";
    backupResultError = "";
    backupType = "channels";
    setSystemState(STATE_BACKUP);

    xTaskCreatePinnedToCore(backupChannelsTask, "backup_ch", 8192, NULL, 1, NULL, 1);
    sendSuccess(request, "Channel backup started");
    updateActivity();
}

void WebServerManager::handleBackupStatus(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["running"] = (bool)backupRunning;
    doc["complete"] = (bool)backupComplete;
    doc["type"] = backupType;

    if (backupComplete) {
        if (backupResultFilename.length() > 0) {
            doc["success"] = true;
            doc["filename"] = backupResultFilename;
        } else {
            doc["success"] = false;
            doc["error"] = backupResultError.length() > 0 ? backupResultError : String("Backup failed");
        }
    }

    sendJSON(request, doc);
    updateActivity();
}

void WebServerManager::handleBackupList(AsyncWebServerRequest* request) {
    // Use BackupManager which properly traverses settings/ and channels/ subdirectories
    String json = backupManager.listBackups();

    // Wrap the array in an object with "backups" key to match UI expectations
    JsonDocument doc;
    JsonDocument backupsDoc;
    deserializeJson(backupsDoc, json);
    doc["backups"] = backupsDoc.as<JsonArray>();

    sendJSON(request, doc);
    updateActivity();
}

void WebServerManager::handleBackupDownload(AsyncWebServerRequest* request) {
    String uri = request->url();

    // Determine backup type from URL
    String type;
    String subdir;
    if (uri.indexOf("/settings/") >= 0) {
        type = "settings";
        subdir = "/settings";
    } else if (uri.indexOf("/channels/") >= 0) {
        type = "channels";
        subdir = "/channels";
    } else {
        sendError(request, "Invalid backup type", 400);
        return;
    }

    // Find the most recent backup in the subdirectory
    String dirPath = String(PATH_BACKUPS) + subdir;
    File dir = LittleFS.open(dirPath);
    if (!dir) {
        sendError(request, "No backups found", 404);
        return;
    }

    String latestFile;
    File file = dir.openNextFile();
    while (file) {
        // Take the last file (most recently created)
        latestFile = String(file.name());
        file = dir.openNextFile();
    }

    if (latestFile.length() == 0) {
        sendError(request, "No backups found", 404);
        return;
    }

    String filepath = dirPath + "/" + latestFile;
    if (!LittleFS.exists(filepath)) {
        sendError(request, "Backup file not found", 404);
        return;
    }

    // Send file as download
    String contentType = type == "settings" ? "application/json" : "application/octet-stream";
    request->send(LittleFS, filepath, contentType, true);
    updateActivity();
}

void WebServerManager::handleBackupDelete(AsyncWebServerRequest* request) {
    if (request->hasParam("filename")) {
        String filename = request->getParam("filename")->value();
        String path = String(PATH_BACKUPS) + "/" + filename;

        if (LittleFS.remove(path)) {
            sendSuccess(request, "Backup deleted");
        } else {
            sendError(request, "Failed to delete backup");
        }
    } else {
        sendError(request, "Missing filename parameter");
    }
    updateActivity();
}

void WebServerManager::handleRestoreSettings(AsyncWebServerRequest* request) {
    // Placeholder
    sendSuccess(request, "Restore not yet implemented");
    updateActivity();
}

void WebServerManager::handleRestoreChannels(AsyncWebServerRequest* request) {
    // Placeholder
    sendSuccess(request, "Channel restore not yet implemented");
    updateActivity();
}

// Serial API Handlers
void WebServerManager::handleSerialBuffer(AsyncWebServerRequest* request) {
    String buffer = serialMonitor.getBuffer();

    // In text mode, sanitize control characters that break JSON parsing
    if (!serialMonitor.isHexMode()) {
        for (unsigned int i = 0; i < buffer.length(); i++) {
            char c = buffer[i];
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
                buffer[i] = '.';
            }
        }
    }

    JsonDocument doc;
    doc["buffer"] = buffer;
    doc["hexMode"] = serialMonitor.isHexMode();
    doc["protocol"] = SerialMonitor::protocolName(serialMonitor.getDetectedProtocol());
    doc["protocolId"] = (int)serialMonitor.getDetectedProtocol();
    sendJSON(request, doc);
    updateActivity();
}

// --- Serial Test FreeRTOS task ---
// Runs comprehensive serial diagnostic in a separate task so it doesn't
// block the async TCP task (which would kill the HTTP connection).
//
// Steps:
//   0. GPIO-level check on RX pin (is the line idle-HIGH or floating/disconnected?)
//   1. Listen at current baud rate for incoming data
//   2. If nothing heard, try alternative baud rates (multi-baud scan)
//   3. Send Meshtastic want_config probe and listen for response
static void serialTestTaskFunc(void* param) {
    extern HardwareSerial RAK_SERIAL_PORT;

    int preAvailable = RAK_SERIAL_PORT.available();
    SerialProtocol detectedProto = serialMonitor.getDetectedProtocol();
    uint32_t originalBaud = serialBridge.getBaudRate();

    serialMonitor.suspend();
    vTaskDelay(pdMS_TO_TICKS(200));

    // --- STEP 0: Check RX pin state WITHOUT tearing down Serial1 ---
    // Reading the pin while UART is attached still works for basic level check.
    // We avoid calling Serial1.end() because re-begin() on ESP32-S3 can fail
    // to properly reattach GPIO43/44.
    int rxPinState = digitalRead(DEFAULT_PIN_UART_RX);
    int rxToggleCount = 0;  // Can't reliably sample while UART owns the pin

    // --- STEP 1: Drain + passive listen at current baud ---
    int drained = 0;
    uint8_t drainBuf[64];
    int drainSaved = 0;
    while (RAK_SERIAL_PORT.available()) {
        uint8_t b = RAK_SERIAL_PORT.read();
        if (drainSaved < 64) drainBuf[drainSaved++] = b;
        drained++;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    int passiveReceived = 0;
    uint8_t passiveBuf[64];
    while (RAK_SERIAL_PORT.available() && passiveReceived < 64) {
        passiveBuf[passiveReceived++] = RAK_SERIAL_PORT.read();
    }
    int passiveExtra = 0;
    while (RAK_SERIAL_PORT.available()) { RAK_SERIAL_PORT.read(); passiveExtra++; }

    // --- STEP 2: Baud scan skipped — no longer tears down Serial1 ---
    int baudScanHits = 0;
    uint32_t bestBaud = originalBaud;
    String baudScanResults = "skipped (preserving UART)";

    // --- STEP 3: Active test — send Meshtastic want_config probe ---
    // Meshtastic protocol requires a wake-up burst before the first command.
    // The Python CLI sends 32 bytes of 0xC3 (START2) then waits 100ms.
    while (RAK_SERIAL_PORT.available()) RAK_SERIAL_PORT.read();

    uint8_t wakeBurst[32];
    memset(wakeBurst, 0xC3, sizeof(wakeBurst));
    RAK_SERIAL_PORT.write(wakeBurst, sizeof(wakeBurst));
    RAK_SERIAL_PORT.flush();
    vTaskDelay(pdMS_TO_TICKS(100));
    DEBUG_PRINTLN("[Serial Test] Sent 32-byte wake-up burst (0xC3)");

    // Drain any data that arrived during wake-up
    while (RAK_SERIAL_PORT.available()) RAK_SERIAL_PORT.read();

    uint32_t nonce = (millis() & 0x7FFFFFFF) | 1;
    if (nonce == 69420) nonce++;
    uint8_t frame[10];
    frame[0] = 0x94;
    frame[1] = 0xC3;
    uint8_t varintBuf[5];
    int varintLen = 0;
    uint32_t v = nonce;
    do {
        varintBuf[varintLen] = v & 0x7F;
        v >>= 7;
        if (v) varintBuf[varintLen] |= 0x80;
        varintLen++;
    } while (v);
    uint16_t payloadLen = 1 + varintLen;
    frame[2] = (payloadLen >> 8) & 0xFF;
    frame[3] = payloadLen & 0xFF;
    frame[4] = 0x18;
    memcpy(&frame[5], varintBuf, varintLen);
    int frameLen = 5 + varintLen;

    int received = 0;
    bool gotFrameHeader = false;
    uint8_t respBuf[128];
    int respSaved = 0;
    size_t totalSent = 0;
    int attempt = 0;

    for (attempt = 0; attempt < 2 && received == 0; attempt++) {
        if (attempt > 0) {
            RAK_SERIAL_PORT.write(wakeBurst, sizeof(wakeBurst));
            RAK_SERIAL_PORT.flush();
            vTaskDelay(pdMS_TO_TICKS(100));
            while (RAK_SERIAL_PORT.available()) RAK_SERIAL_PORT.read();
        }

        size_t frameSent = RAK_SERIAL_PORT.write(frame, frameLen);
        RAK_SERIAL_PORT.flush();
        totalSent += frameSent;

        DEBUG_PRINTF("[Serial Test] Attempt %d @ %d baud: sent %d bytes (want_config nonce=%u)\n",
                     attempt + 1, bestBaud, frameSent, nonce);

        uint32_t startTime = millis();
        while (!RAK_SERIAL_PORT.available() && (millis() - startTime < 3000)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (RAK_SERIAL_PORT.available()) vTaskDelay(pdMS_TO_TICKS(200));

        while (RAK_SERIAL_PORT.available() && received < 512) {
            uint8_t b = RAK_SERIAL_PORT.read();
            if (received == 0 && b == 0x94) gotFrameHeader = true;
            if (respSaved < 128) respBuf[respSaved++] = b;
            received++;
        }
    }

    serialMonitor.resume();

    // --- Build JSON result ---
    JsonDocument doc;
    doc["tx_pin"] = DEFAULT_PIN_UART_TX;
    doc["rx_pin"] = DEFAULT_PIN_UART_RX;
    doc["baud"] = bestBaud;
    doc["original_baud"] = originalBaud;
    doc["detected_protocol"] = SerialMonitor::protocolName(detectedProto);

    // GPIO diagnostics
    doc["rx_pin_idle"] = rxPinState == HIGH ? "HIGH (normal)" : "LOW (abnormal — check wiring)";
    doc["rx_pin_toggles"] = rxToggleCount;

    // Passive receive
    doc["pre_available"] = preAvailable;
    doc["drained"] = drained;
    doc["passive_received"] = passiveReceived + passiveExtra;

    // Baud scan results
    if (baudScanResults.length() > 0) {
        doc["baud_scan"] = baudScanResults;
        if (baudScanHits > 0) {
            doc["baud_scan_best"] = String(bestBaud) + " (" + String(baudScanHits) + " bytes)";
        }
    }

    // Active test
    doc["sent_bytes"] = (int)totalSent;
    doc["attempts"] = attempt;
    doc["received_bytes"] = received;
    doc["got_frame_header"] = gotFrameHeader;

    // Hex samples
    auto fmtHex = [](const uint8_t* buf, int len, int mx) -> String {
        String h;
        for (int i = 0; i < min(len, mx); i++) {
            char c[4]; snprintf(c, sizeof(c), "%02X ", buf[i]); h += c;
        }
        if (len > mx) h += "...";
        return h;
    };
    if (drainSaved > 0) doc["drain_sample"] = fmtHex(drainBuf, drainSaved, 32);
    if (passiveReceived > 0) doc["passive_sample"] = fmtHex(passiveBuf, passiveReceived, 32);
    if (respSaved > 0) doc["response_hex"] = fmtHex(respBuf, respSaved, 48);

    // --- Status determination ---
    bool rxWorks = (drained > 0 || passiveReceived > 0 || preAvailable > 0 ||
                    detectedProto != PROTO_UNKNOWN || baudScanHits > 0);

    if (received > 0 && gotFrameHeader) {
        doc["status"] = "SUCCESS: Meshtastic protobuf response received";
    } else if (received > 0) {
        doc["status"] = "PARTIAL: Device responded but not with Meshtastic protobuf";
    } else if (rxWorks) {
        doc["status"] = "RX_ONLY: Receiving data but no response to Meshtastic probe";
    } else if (rxPinState == LOW) {
        doc["status"] = "FAIL: RX pin reads LOW — line is disconnected or pulled down. "
                        "Check wiring: ESP32 GPIO" + String(DEFAULT_PIN_UART_RX) +
                        " must connect to RAK TX";
    } else if (rxToggleCount > 0) {
        doc["status"] = "FAIL: RX pin shows activity but UART received nothing. "
                        "Likely baud rate mismatch. Check RAK serial module config.";
    } else {
        doc["status"] = "FAIL: RX pin is HIGH (idle) but no data received. "
                        "RAK may not be sending — check that serial module is enabled "
                        "with mode=PROTO, and that RAK is powered on.";
    }

    DEBUG_PRINTF("[Serial Test] rxPin=%s toggles=%d drain=%d passive=%d sent=%d recv=%d baud=%d\n",
                 rxPinState == HIGH ? "HIGH" : "LOW", rxToggleCount,
                 drained, passiveReceived + passiveExtra,
                 (int)totalSent, received, bestBaud);

    serializeJson(doc, s_serialTestResultJSON);
    s_serialTestComplete = true;
    s_serialTestRunning = false;
    vTaskDelete(NULL);
}

// POST /api/serial/test — kick off the test
void WebServerManager::handleSerialTest(AsyncWebServerRequest* request) {
    if (s_serialTestRunning) {
        sendError(request, "Serial test already running", 409);
        return;
    }

    if (serialBridge.isActive()) {
        JsonDocument doc;
        doc["status"] = "BLOCKED";
        doc["message"] = "Serial bridge is active. Disconnect bridge client first.";
        doc["bridge_mode"] = serialBridge.getMode() == BRIDGE_TCP ? "TCP" : "HTTP";
        sendJSON(request, doc);
        return;
    }

    s_serialTestRunning = true;
    s_serialTestComplete = false;
    s_serialTestResultJSON = "";

    xTaskCreatePinnedToCore(serialTestTaskFunc, "serial_test", 8192, NULL, 1, NULL, 1);

    JsonDocument doc;
    doc["status"] = "testing";
    doc["message"] = "Serial test started";
    sendJSON(request, doc);
    updateActivity();
}

// GET /api/serial/test — poll for results
void WebServerManager::handleSerialTestResult(AsyncWebServerRequest* request) {
    if (s_serialTestRunning) {
        request->send(200, "application/json", "{\"status\":\"testing\"}");
    } else if (s_serialTestComplete) {
        request->send(200, "application/json", s_serialTestResultJSON);
    } else {
        request->send(200, "application/json", "{\"status\":\"idle\"}");
    }
    updateActivity();
}

// handleSerialSend and handleSerialPassthrough are now inline lambdas in setupRoutes()
// using AsyncCallbackJsonWebHandler for proper JSON body parsing

// GET /api/meshtastic/command — poll for command results
void WebServerManager::handleMeshtasticCommandResult(AsyncWebServerRequest* request) {
    if (s_meshCmdRunning) {
        request->send(200, "application/json", "{\"status\":\"running\"}");
    } else if (s_meshCmdComplete) {
        request->send(200, "application/json", s_meshCmdResultJSON);
        // Clear stale state so next command starts fresh
        s_meshCmdComplete = false;
        s_meshCmdResultJSON = "";
    } else {
        request->send(200, "application/json", "{\"status\":\"idle\"}");
    }
    updateActivity();
}

// GPIO API Handlers
void WebServerManager::handleGPIOConfig(AsyncWebServerRequest* request) {
    String config = gpioControl.getConfigJSON();
    request->send(200, "application/json", config);
    updateActivity();
}

void WebServerManager::handleGPIOConfigPost(AsyncWebServerRequest* request) {
    // Would handle POST body with new config
    sendSuccess(request, "GPIO config updated");
    updateActivity();
}

void WebServerManager::handleGPIOTest(AsyncWebServerRequest* request) {
    if (request->hasParam("gpio")) {
        uint8_t gpio = request->getParam("gpio")->value().toInt();
        int result = gpioControl.testPin(gpio);

        JsonDocument doc;
        doc["gpio"] = gpio;
        doc["state"] = result;
        sendJSON(request, doc);
    } else {
        sendError(request, "Missing gpio parameter");
    }
    updateActivity();
}

void WebServerManager::handleGPIOReset(AsyncWebServerRequest* request) {
    gpioControl.resetRAK();
    sendSuccess(request, "RAK reset");
    updateActivity();
}

void WebServerManager::handleGPIODFU(AsyncWebServerRequest* request) {
    gpioControl.enterDFUMode();
    sendSuccess(request, "DFU mode activated");
    updateActivity();
}

// Settings API Handlers
void WebServerManager::handleSettings(AsyncWebServerRequest* request) {
    JsonDocument doc;

    // AP settings
    doc["wifi"]["ssid"]     = wifiManager.getAPSSID();
    doc["wifi"]["password"] = wifiManager.getAPPassword();
    doc["wifi"]["channel"]  = wifiManager.getAPChannel();
    doc["wifi"]["hidden"]   = wifiManager.getAPHidden();

    // STA settings (never return password)
    doc["sta"]["enabled"]   = wifiManager.isSTAEnabled();
    doc["sta"]["ssid"]      = wifiManager.getSTASSID();
    doc["sta"]["connected"] = wifiManager.isSTAConnected();
    doc["sta"]["ip"]        = wifiManager.getSTAIP();
    doc["sta"]["rssi"]      = wifiManager.getSTARSSI();
    doc["sta"]["state"]     = (int)wifiManager.getSTAState();

    // System settings
    doc["system"]["sleepTimeout"]  = SLEEP_DELAY_MS;
    doc["system"]["serialBuffer"]  = SERIAL_BUFFER_SIZE;

    // Serial / bridge settings
    doc["baud"] = serialBridge.getBaudRate();

    sendJSON(request, doc);
    updateActivity();
}

void WebServerManager::handleFactoryReset(AsyncWebServerRequest* request) {
    sendSuccess(request, "Factory reset initiated");

    // Clear all NVS settings (including STA keys)
    extern Preferences preferences;
    preferences.clear();

    // Format filesystem
    LittleFS.format();
    delay(100);
    ESP.restart();
}

// WiFi Scan Handlers
void WebServerManager::handleWiFiScan(AsyncWebServerRequest* request) {
    if (wifiManager.startWiFiScan()) {
        sendSuccess(request, "Scan started");
    } else {
        sendError(request, "Scan already in progress");
    }
    updateActivity();
}

void WebServerManager::handleWiFiScanResults(AsyncWebServerRequest* request) {
    JsonDocument doc;
    bool ready = wifiManager.getScanResultsJSON(doc);

    if (!ready) {
        // Still scanning
        sendJSON(request, doc);  // { "scanning": true }
    } else {
        sendJSON(request, doc);  // { "scanning": false, "networks": [...] }
    }
    updateActivity();
}

// OTA Update Handlers
void WebServerManager::handleOTAUpdate(AsyncWebServerRequest* request) {
    // This is called after the upload is complete
    bool success = !Update.hasError() && Update.isFinished();

    JsonDocument doc;
    doc["success"] = success;
    if (success) {
        doc["message"] = "Update successful! Rebooting in 3 seconds...";
    } else {
        String errStr = Update.errorString();
        if (errStr.length() == 0 || !Update.isRunning()) {
            doc["error"] = "File rejected. Use the firmware.bin file for OTA updates.";
        } else {
            doc["error"] = "Update failed: " + errStr;
        }
    }

    String json;
    serializeJson(doc, json);
    // Send response before reboot
    AsyncWebServerResponse* response = request->beginResponse(
        success ? 200 : 500, "application/json", json);
    response->addHeader("Connection", "close");
    request->send(response);

    if (success) {
        delay(3000);
        ESP.restart();
    }
}

void WebServerManager::handleOTAUpload(AsyncWebServerRequest* request,
                                        String filename, size_t index,
                                        uint8_t* data, size_t len, bool final) {
    static bool otaAborted = false;

    if (index == 0) {
        otaAborted = false;
        DEBUG_PRINTF("[OTA] Starting firmware update: %s\n", filename.c_str());

        // Web UI is embedded in firmware — only firmware OTA needed
        size_t maxFirmwareSize = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;

        // Reject oversized files (combined images, etc.)
        size_t contentLength = request->contentLength();
        if (contentLength > 0 && contentLength > maxFirmwareSize + 4096) {
            DEBUG_PRINTF("[OTA] File too large: %u bytes (max ~%u)\n",
                         contentLength, maxFirmwareSize);
            otaAborted = true;
            return;
        }

        if (!Update.begin(maxFirmwareSize, U_FLASH)) {
            DEBUG_PRINTF("[OTA] Update.begin failed: %s\n", Update.errorString());
            otaAborted = true;
        }
    }

    if (otaAborted) return;

    if (len) {
        if (Update.write(data, len) != len) {
            DEBUG_PRINTF("[OTA] Update.write failed: %s\n", Update.errorString());
        }

        // Send progress via WebSocket
        if (g_webServer && Update.size() > 0) {
            int progress = (int)((Update.progress() * 100) / Update.size());
            static int lastProgress = -1;
            if (progress != lastProgress && progress % 5 == 0) {
                lastProgress = progress;
                g_webServer->sendProgress("ota", progress, "Uploading firmware...");
            }
        }
    }

    if (final) {
        if (otaAborted) {
            DEBUG_PRINTLN("[OTA] Upload aborted — file rejected");
            return;
        }
        if (Update.end(true)) {
            DEBUG_PRINTF("[OTA] Update complete: %d bytes\n", index + len);
        } else {
            DEBUG_PRINTF("[OTA] Update.end failed: %s\n", Update.errorString());
        }
    }

    updateActivity();
}

// OTA filesystem handlers removed — web UI is embedded in firmware via PROGMEM

// Helper Functions
void WebServerManager::sendJSON(AsyncWebServerRequest* request, const JsonDocument& doc) {
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

void WebServerManager::sendError(AsyncWebServerRequest* request, const String& error, int code) {
    JsonDocument doc;
    doc["error"] = error;
    doc["success"] = false;

    String json;
    serializeJson(doc, json);
    request->send(code, "application/json", json);
}

void WebServerManager::sendSuccess(AsyncWebServerRequest* request, const String& message) {
    JsonDocument doc;
    doc["message"] = message;
    doc["success"] = true;

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}
