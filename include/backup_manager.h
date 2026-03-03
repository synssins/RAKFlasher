/**
 * @file backup_manager.h
 * @brief Meshtastic configuration backup and restore
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"

class BackupManager {
public:
    BackupManager();

    /**
     * @brief Initialize backup manager
     * @return true if successful
     */
    bool begin();

    /**
     * @brief Backup all settings via Meshtastic serial API
     * @return Backup filename or empty string on error
     */
    String backupSettings();

    /**
     * @brief Backup channels and encryption keys
     * @param password Optional encryption password
     * @return Backup filename or empty string on error
     */
    String backupChannels(const String& password = "");

    /**
     * @brief Restore settings from backup file
     * @param filename Backup file to restore
     * @return true if successful
     */
    bool restoreSettings(const String& filename);

    /**
     * @brief Restore channels from backup file
     * @param filename Backup file
     * @param password Decryption password if encrypted
     * @return true if successful
     */
    bool restoreChannels(const String& filename, const String& password = "");

    /**
     * @brief List available backups
     * @return JSON array of backup files
     */
    String listBackups();

    /**
     * @brief Delete a backup file
     * @param filename Backup file to delete
     * @return true if successful
     */
    bool deleteBackup(const String& filename);

    /**
     * @brief Get last error message
     */
    String getLastError() const { return m_lastError; }

private:
    String m_lastError;

    /**
     * @brief Generate backup filename with timestamp
     * @param prefix Filename prefix
     * @return Generated filename
     */
    String generateFilename(const String& prefix);

    /**
     * @brief Request Meshtastic configuration via serial
     * @return JSON document with configuration
     */
    bool requestMeshtasticConfig(JsonDocument& doc);

    /**
     * @brief Send Meshtastic configuration via serial
     * @param doc JSON document with configuration
     * @return true if successful
     */
    bool sendMeshtasticConfig(const JsonDocument& doc);

    /**
     * @brief Encrypt data with password
     * @param data Data to encrypt
     * @param password Encryption password
     * @return Encrypted data
     */
    String encryptData(const String& data, const String& password);

    /**
     * @brief Decrypt data with password
     * @param data Encrypted data
     * @param password Decryption password
     * @return Decrypted data
     */
    String decryptData(const String& data, const String& password);
};
