/**
 * @file config.h
 * @brief Global configuration and pin definitions for ESP32-RAKFlasher
 */
#pragma once

#include <Arduino.h>

// Firmware version — FIRMWARE_VERSION is the semantic version (manually bumped),
// BUILD_NUMBER auto-increments with every build via tools/version_increment.py,
// BUILD_DATE and BUILD_TIME are set automatically by the compiler.
#define FIRMWARE_VERSION "1.2.5"
#define PROJECT_NAME "ESP32-RAKFlasher"
#define BUILD_DATE __DATE__   // e.g. "Mar  3 2026"
#define BUILD_TIME __TIME__   // e.g. "14:30:05"
#include "build_info.h"       // AUTO-GENERATED: provides BUILD_NUMBER

// Default GPIO Pin Configuration (user-configurable via WebUI)
// Pin assignments verified against Seeed XIAO ESP32-S3 and RAK19001 datasheets
#define DEFAULT_PIN_UART_TX   43  // GPIO43/D6 - Serial TX (ESP32 TX → RAK RX at Pin 34/RXD1)
#define DEFAULT_PIN_UART_RX   44  // GPIO44/D7 - Serial RX (ESP32 RX ← RAK TX at Pin 33/TXD1)
#define DEFAULT_PIN_RESET     3   // GPIO3/D2 - RAK reset control (output, active LOW)
#define DEFAULT_PIN_WAKE      4   // GPIO4/D3 - Wake signal from RAK (input)
#define DEFAULT_PIN_SWD_CLK   7   // GPIO7/D8 - SWD Clock
#define DEFAULT_PIN_SWD_IO    8   // GPIO8/D9 - SWD Data

// UART Configuration
#define RAK_SERIAL_BAUD       115200
#define RAK_SERIAL_PORT       Serial1

// WiFi AP Configuration (defaults, user-configurable)
#define DEFAULT_AP_SSID_PREFIX "RAKFlasher"
#define DEFAULT_AP_PASSWORD    "flasher123"
#define DEFAULT_AP_CHANNEL     6
#define DEFAULT_AP_IP          IPAddress(192, 168, 4, 1)
#define DEFAULT_AP_GATEWAY     IPAddress(192, 168, 4, 1)
#define DEFAULT_AP_SUBNET      IPAddress(255, 255, 255, 0)

// WiFi STA (Client) Configuration (build-time defaults, overridden by NVS)
// To pre-configure STA at build time, set your credentials here or via
// platformio.ini build_flags: -DDEFAULT_STA_SSID=\"YourSSID\"
#ifndef DEFAULT_STA_SSID
#define DEFAULT_STA_SSID        ""                    // Empty = STA disabled at build
#endif
#ifndef DEFAULT_STA_PASSWORD
#define DEFAULT_STA_PASSWORD    ""
#endif
#ifndef DEFAULT_STA_ENABLED
#define DEFAULT_STA_ENABLED     false      // Set true if STA credentials configured
#endif
#define STA_CONNECT_TIMEOUT_MS  12000       // 12s boot connect timeout
#define STA_RETRY_INTERVAL_MS   300000      // 5min reconnect retry
#define STA_MAX_RETRY_BACKOFF   600000      // 10min max backoff

// Web Server Configuration
#define WEB_SERVER_PORT        80
#define WEBSOCKET_PATH         "/ws"

// Serial Bridge Configuration
#define BRIDGE_TCP_PORT        4403    // Meshtastic-compatible TCP bridge port
#define BRIDGE_HTTP_TIMEOUT_MS 30000   // HTTP API inactivity timeout (30s)

// Deep Sleep Configuration
#define SLEEP_DELAY_MS         300000  // 5 minutes idle before sleep
#define WAKE_GPIO_LEVEL        HIGH    // Wake when GPIO goes HIGH

// Serial Buffer Configuration
#define SERIAL_BUFFER_SIZE     500     // Lines to buffer
#define SERIAL_LINE_MAX        256     // Max chars per line

// File Upload Limits
#define MAX_FIRMWARE_SIZE      (1024 * 1024)  // 1MB max firmware
#define MAX_BACKUP_SIZE        (128 * 1024)   // 128KB max backup

// Timeout Values (milliseconds)
#define DFU_TIMEOUT            60000   // 1 minute for DFU operation
#define SWD_TIMEOUT            30000   // 30 seconds for SWD operation
#define SERIAL_CMD_TIMEOUT     5000    // 5 seconds for serial command
#define WEB_REQUEST_TIMEOUT    10000   // 10 seconds for web requests

// Retry Limits
#define DFU_MAX_RETRIES        3
#define SWD_MAX_RETRIES        5

// NVS Keys for persistent storage
#define NVS_NAMESPACE          "rakflasher"
#define NVS_KEY_WIFI_SSID      "wifi_ssid"
#define NVS_KEY_WIFI_PASS      "wifi_pass"
#define NVS_KEY_PINS           "pin_config"
#define NVS_KEY_SETTINGS       "settings"
#define NVS_KEY_STA_SSID       "sta_ssid"
#define NVS_KEY_STA_PASS       "sta_pass"
#define NVS_KEY_STA_ENABLED    "sta_enabled"
#define NVS_KEY_BAUD_RATE      "baud_rate"

// LittleFS Paths
#define PATH_CONFIG            "/config.json"
#define PATH_PINS              "/pins.json"
#define PATH_WIFI              "/wifi.json"
#define PATH_UPLOADED          "/uploaded"
#define PATH_BACKUPS           "/backups"
#define PATH_LOGS              "/logs"
#define PATH_WWW               "/www"

// DFU Protocol Constants
#define DFU_MTU                512
#define SLIP_END               0xC0
#define SLIP_ESC               0xDB
#define SLIP_ESC_END           0xDC
#define SLIP_ESC_ESC           0xDD

// Meshtastic Protocol
#define MESHTASTIC_PORTNUM_ADMIN  6

// Debug Macros
#define DEBUG_PRINT(x)         Serial.print(x)
#define DEBUG_PRINTLN(x)       Serial.println(x)
#define DEBUG_PRINTF(...)      Serial.printf(__VA_ARGS__)

// Error Codes
enum ErrorCode {
    ERR_NONE = 0,
    ERR_FILE_NOT_FOUND,
    ERR_FILE_TOO_LARGE,
    ERR_UPLOAD_FAILED,
    ERR_DFU_TIMEOUT,
    ERR_DFU_FAILED,
    ERR_SWD_NO_CONNECTION,
    ERR_SWD_FAILED,
    ERR_BACKUP_FAILED,
    ERR_RESTORE_FAILED,
    ERR_INVALID_JSON,
    ERR_STORAGE_FULL,
    ERR_SERIAL_TIMEOUT,
    ERR_UNKNOWN
};

// Pin Functions
enum PinFunction {
    PIN_WAKE = 0,
    PIN_RESET,
    PIN_UART_TX,
    PIN_UART_RX,
    PIN_SWD_CLK,
    PIN_SWD_IO,
    PIN_FUNCTION_COUNT
};

// System States
enum SystemState {
    STATE_IDLE = 0,
    STATE_FLASHING,
    STATE_SWD_RECOVERY,
    STATE_BACKUP,
    STATE_RESTORE,
    STATE_MONITORING,
    STATE_ERROR
};
