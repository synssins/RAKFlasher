/**
 * @file backup_manager.cpp
 * @brief Implementation of backup manager using Meshtastic serial protocol
 */

#include "backup_manager.h"
#include "meshtastic_proto.h"
#include "serial_monitor.h"
#include <time.h>

extern SerialMonitor serialMonitor;

// Shared protocol instance
static MeshtasticProtocol meshProto;

BackupManager::BackupManager() {
}

bool BackupManager::begin() {
    DEBUG_PRINTLN("[Backup] Manager initialized");
    return true;
}

String BackupManager::backupSettings() {
    DEBUG_PRINTLN("[Backup] Starting full settings backup via Meshtastic serial API...");

    // Get the serial port from the serial monitor's hardware serial
    // The serial monitor uses RAK_SERIAL_PORT (Serial1)
    extern HardwareSerial RAK_SERIAL_PORT;
    meshProto.setSerial(&RAK_SERIAL_PORT);

    // Capture full config from Meshtastic device
    JsonDocument doc;
    if (!meshProto.captureFullConfig(doc, 30000)) {
        m_lastError = "Failed to retrieve configuration from Meshtastic device";
        DEBUG_PRINTLN("[Backup] ERROR: Config capture failed");
        return "";
    }

    // Generate filename
    String filename = generateFilename("settings");
    String filepath = String(PATH_BACKUPS) + "/settings/" + filename;

    // Ensure directory exists
    LittleFS.mkdir(String(PATH_BACKUPS) + "/settings");

    // Save to file
    File file = LittleFS.open(filepath, "w");
    if (!file) {
        m_lastError = "Failed to create backup file";
        DEBUG_PRINTLN("[Backup] ERROR: Failed to create file");
        return "";
    }

    serializeJson(doc, file);
    file.close();

    size_t fileSize = 0;
    File check = LittleFS.open(filepath, "r");
    if (check) {
        fileSize = check.size();
        check.close();
    }

    DEBUG_PRINTF("[Backup] Settings backed up to: %s (%d bytes)\n", filename.c_str(), fileSize);
    return filename;
}

String BackupManager::backupChannels(const String& password) {
    DEBUG_PRINTLN("[Backup] Starting channel/key backup...");

    // Get the serial port
    extern HardwareSerial RAK_SERIAL_PORT;
    meshProto.setSerial(&RAK_SERIAL_PORT);

    // Capture full config first (we extract channels from it)
    JsonDocument fullDoc;
    if (!meshProto.captureFullConfig(fullDoc, 30000)) {
        m_lastError = "Failed to retrieve configuration from Meshtastic device";
        DEBUG_PRINTLN("[Backup] ERROR: Config capture failed");
        return "";
    }

    // Extract just the channel data and security config
    JsonDocument channelDoc;
    channelDoc["version"] = 1;
    channelDoc["timestamp"] = millis();
    channelDoc["my_node_num"] = meshProto.getMyNodeNum();

    // Copy channels
    if (fullDoc["channels"].is<JsonArray>()) {
        channelDoc["channels"] = fullDoc["channels"];
    }

    // Find and copy security config from the config array
    if (fullDoc["config"].is<JsonArray>()) {
        JsonArray configs = fullDoc["config"].as<JsonArray>();
        for (JsonObject cfg : configs) {
            if (cfg["type"] == "security") {
                channelDoc["security"] = cfg;
                break;
            }
        }
    }

    String data;
    serializeJson(channelDoc, data);

    // Encrypt if password provided
    if (password.length() > 0) {
        data = encryptData(data, password);
    }

    // Generate filename
    String filename = generateFilename("channels");
    String filepath = String(PATH_BACKUPS) + "/channels/" + filename;

    // Ensure directory exists
    LittleFS.mkdir(String(PATH_BACKUPS) + "/channels");

    // Save to file
    File file = LittleFS.open(filepath, "w");
    if (!file) {
        m_lastError = "Failed to create backup file";
        return "";
    }

    file.print(data);
    file.close();

    DEBUG_PRINTF("[Backup] Channels backed up to: %s\n", filename.c_str());
    return filename;
}

bool BackupManager::restoreSettings(const String& filename) {
    String filepath = String(PATH_BACKUPS) + "/settings/" + filename;

    if (!LittleFS.exists(filepath)) {
        m_lastError = "Backup file not found";
        return false;
    }

    File file = LittleFS.open(filepath, "r");
    if (!file) {
        m_lastError = "Failed to open backup file";
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        m_lastError = "Failed to parse backup file";
        return false;
    }

    // Send configuration to Meshtastic
    if (!sendMeshtasticConfig(doc)) {
        m_lastError = "Failed to apply configuration";
        return false;
    }

    DEBUG_PRINTLN("[Backup] Settings restored successfully");
    return true;
}

bool BackupManager::restoreChannels(const String& filename, const String& password) {
    String filepath = String(PATH_BACKUPS) + "/channels/" + filename;

    if (!LittleFS.exists(filepath)) {
        m_lastError = "Backup file not found";
        return false;
    }

    File file = LittleFS.open(filepath, "r");
    if (!file) {
        m_lastError = "Failed to open backup file";
        return false;
    }

    String data = file.readString();
    file.close();

    // Decrypt if password provided
    if (password.length() > 0) {
        data = decryptData(data, password);
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, data);
    if (error) {
        m_lastError = "Failed to parse backup (wrong password?)";
        return false;
    }

    // Restore channels using admin messages
    uint32_t myNodeNum = doc["my_node_num"] | meshProto.getMyNodeNum();
    if (myNodeNum == 0) {
        m_lastError = "Unknown node number - cannot send admin packets";
        return false;
    }

    extern HardwareSerial RAK_SERIAL_PORT;
    meshProto.setSerial(&RAK_SERIAL_PORT);

    JsonArray channels = doc["channels"].as<JsonArray>();
    int restored = 0;
    for (JsonObject ch : channels) {
        String rawHex = ch["raw"].as<String>();
        if (rawHex.length() == 0) continue;

        // The raw field contains the full FromRadio frame — we need to extract
        // just the Channel protobuf inner data for the set_channel admin message.
        // For now, we need to parse the FromRadio to get the Channel data.
        // This is stored in the raw hex of the complete FromRadio frame.

        // Convert hex back to bytes
        size_t byteLen = rawHex.length() / 2;
        uint8_t* bytes = (uint8_t*)malloc(byteLen);
        if (!bytes) continue;

        for (size_t i = 0; i < byteLen; i++) {
            String hexByte = rawHex.substring(i * 2, i * 2 + 2);
            bytes[i] = (uint8_t)strtol(hexByte.c_str(), NULL, 16);
        }

        // Parse FromRadio to find Channel inner data
        size_t pos = 0;
        size_t vlen;
        // Skip id field
        if (bytes[pos] == 0x08) {
            pos++;
            MeshtasticProtocol::decodeVarint(bytes + pos, &vlen);
            pos += vlen;
        }
        // Should be at Channel tag (0x52)
        if (pos < byteLen && bytes[pos] == TAG_FROMRADIO_CHANNEL) {
            pos++;
            uint32_t channelLen = MeshtasticProtocol::decodeVarint(bytes + pos, &vlen);
            pos += vlen;
            // bytes[pos..pos+channelLen] is the raw Channel protobuf
            if (pos + channelLen <= byteLen) {
                meshProto.sendSetChannel(myNodeNum, bytes + pos, channelLen);
                restored++;
                delay(500);  // Allow device to process
            }
        }

        free(bytes);
    }

    DEBUG_PRINTF("[Backup] Restored %d channels\n", restored);
    return restored > 0;
}

String BackupManager::listBackups() {
    JsonDocument doc;
    JsonArray backups = doc.to<JsonArray>();

    // List settings backups
    File dir = LittleFS.open(String(PATH_BACKUPS) + "/settings");
    if (dir && dir.isDirectory()) {
        File file = dir.openNextFile();
        while (file) {
            if (!file.isDirectory()) {
                JsonObject backup = backups.add<JsonObject>();
                backup["type"] = "settings";
                backup["filename"] = String(file.name());
                backup["size"] = file.size();
            }
            file = dir.openNextFile();
        }
    }

    // List channel backups
    dir = LittleFS.open(String(PATH_BACKUPS) + "/channels");
    if (dir && dir.isDirectory()) {
        File file = dir.openNextFile();
        while (file) {
            if (!file.isDirectory()) {
                JsonObject backup = backups.add<JsonObject>();
                backup["type"] = "channels";
                backup["filename"] = String(file.name());
                backup["size"] = file.size();
            }
            file = dir.openNextFile();
        }
    }

    String json;
    serializeJson(doc, json);
    return json;
}

bool BackupManager::deleteBackup(const String& filename) {
    String paths[] = {
        String(PATH_BACKUPS) + "/settings/" + filename,
        String(PATH_BACKUPS) + "/channels/" + filename
    };

    for (const String& path : paths) {
        if (LittleFS.exists(path)) {
            return LittleFS.remove(path);
        }
    }

    m_lastError = "Backup file not found";
    return false;
}

String BackupManager::generateFilename(const String& prefix) {
    char filename[64];
    snprintf(filename, sizeof(filename), "%s_%lu.json", prefix.c_str(), millis());
    return String(filename);
}

bool BackupManager::requestMeshtasticConfig(JsonDocument& doc) {
    extern HardwareSerial RAK_SERIAL_PORT;
    meshProto.setSerial(&RAK_SERIAL_PORT);
    return meshProto.captureFullConfig(doc, 30000);
}

bool BackupManager::sendMeshtasticConfig(const JsonDocument& doc) {
    uint32_t myNodeNum = doc["device"]["my_node_num"] | meshProto.getMyNodeNum();
    if (myNodeNum == 0) {
        DEBUG_PRINTLN("[Backup] ERROR: No node number for restore");
        return false;
    }

    extern HardwareSerial RAK_SERIAL_PORT;
    meshProto.setSerial(&RAK_SERIAL_PORT);

    int sent = 0;

    // Restore configs
    JsonArrayConst configs = doc["config"].as<JsonArrayConst>();
    for (JsonObjectConst cfg : configs) {
        String rawHex = cfg["raw"].as<String>();
        if (rawHex.length() == 0) continue;

        size_t byteLen = rawHex.length() / 2;
        uint8_t* bytes = (uint8_t*)malloc(byteLen);
        if (!bytes) continue;

        for (size_t i = 0; i < byteLen; i++) {
            String hexByte = rawHex.substring(i * 2, i * 2 + 2);
            bytes[i] = (uint8_t)strtol(hexByte.c_str(), NULL, 16);
        }

        // Parse FromRadio to extract Config inner data
        size_t pos = 0;
        size_t vlen;
        if (bytes[pos] == 0x08) {
            pos++;
            MeshtasticProtocol::decodeVarint(bytes + pos, &vlen);
            pos += vlen;
        }
        if (pos < byteLen && bytes[pos] == TAG_FROMRADIO_CONFIG) {
            pos++;
            uint32_t configLen = MeshtasticProtocol::decodeVarint(bytes + pos, &vlen);
            pos += vlen;
            if (pos + configLen <= byteLen) {
                meshProto.sendSetConfig(myNodeNum, bytes + pos, configLen);
                sent++;
                delay(500);
            }
        }

        free(bytes);
    }

    // Restore module configs
    JsonArrayConst modules = doc["moduleConfig"].as<JsonArrayConst>();
    for (JsonObjectConst mod : modules) {
        String rawHex = mod["raw"].as<String>();
        if (rawHex.length() == 0) continue;

        size_t byteLen = rawHex.length() / 2;
        uint8_t* bytes = (uint8_t*)malloc(byteLen);
        if (!bytes) continue;

        for (size_t i = 0; i < byteLen; i++) {
            String hexByte = rawHex.substring(i * 2, i * 2 + 2);
            bytes[i] = (uint8_t)strtol(hexByte.c_str(), NULL, 16);
        }

        size_t pos = 0;
        size_t vlen;
        if (bytes[pos] == 0x08) {
            pos++;
            MeshtasticProtocol::decodeVarint(bytes + pos, &vlen);
            pos += vlen;
        }
        if (pos < byteLen && bytes[pos] == TAG_FROMRADIO_MODULE_CFG) {
            pos++;
            uint32_t moduleLen = MeshtasticProtocol::decodeVarint(bytes + pos, &vlen);
            pos += vlen;
            if (pos + moduleLen <= byteLen) {
                meshProto.sendSetModuleConfig(myNodeNum, bytes + pos, moduleLen);
                sent++;
                delay(500);
            }
        }

        free(bytes);
    }

    // Restore channels
    JsonArrayConst channels = doc["channels"].as<JsonArrayConst>();
    for (JsonObjectConst ch : channels) {
        String rawHex = ch["raw"].as<String>();
        if (rawHex.length() == 0) continue;

        size_t byteLen = rawHex.length() / 2;
        uint8_t* bytes = (uint8_t*)malloc(byteLen);
        if (!bytes) continue;

        for (size_t i = 0; i < byteLen; i++) {
            String hexByte = rawHex.substring(i * 2, i * 2 + 2);
            bytes[i] = (uint8_t)strtol(hexByte.c_str(), NULL, 16);
        }

        size_t pos = 0;
        size_t vlen;
        if (bytes[pos] == 0x08) {
            pos++;
            MeshtasticProtocol::decodeVarint(bytes + pos, &vlen);
            pos += vlen;
        }
        if (pos < byteLen && bytes[pos] == TAG_FROMRADIO_CHANNEL) {
            pos++;
            uint32_t channelLen = MeshtasticProtocol::decodeVarint(bytes + pos, &vlen);
            pos += vlen;
            if (pos + channelLen <= byteLen) {
                meshProto.sendSetChannel(myNodeNum, bytes + pos, channelLen);
                sent++;
                delay(500);
            }
        }

        free(bytes);
    }

    DEBUG_PRINTF("[Backup] Restore: sent %d admin messages\n", sent);
    return sent > 0;
}

String BackupManager::encryptData(const String& data, const String& password) {
    // TODO: Implement AES-256 encryption
    // For now, simple XOR obfuscation (NOT cryptographically secure)
    String result = data;
    for (unsigned int i = 0; i < result.length(); i++) {
        result[i] ^= password[i % password.length()];
    }
    return result;
}

String BackupManager::decryptData(const String& data, const String& password) {
    // XOR is symmetric
    return encryptData(data, password);
}
