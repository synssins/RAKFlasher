/**
 * @file serial_monitor.h
 * @brief Serial monitor with protocol autodetection and hex dump support
 *
 * Supports automatic detection of serial protocols:
 *   - Meshtastic Protobuf (PROTO mode)
 *   - Nordic SLIP / DFU framing
 *   - NMEA GPS sentences
 *   - AT command interface
 *   - Generic text / binary
 */
#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include "config.h"

// Detected serial protocol
enum SerialProtocol {
    PROTO_UNKNOWN = 0,      // Not yet detected
    PROTO_MESHTASTIC,       // Meshtastic protobuf (0x94 0xC3 framing)
    PROTO_SLIP_DFU,         // Nordic SLIP / DFU (0xC0 framing)
    PROTO_NMEA,             // NMEA GPS ($GP/$GN sentences)
    PROTO_AT_COMMAND,       // AT command interface
    PROTO_TEXT,             // Generic text (printable ASCII)
    PROTO_BINARY            // Generic binary data
};

class SerialMonitor {
public:
    SerialMonitor();

    /**
     * @brief Initialize serial monitor
     * @param serial Hardware serial port
     * @param baud Baud rate
     * @return true if successful
     */
    bool begin(HardwareSerial& serial, uint32_t baud);

    /**
     * @brief Process serial data (call in loop)
     */
    void loop();

    /**
     * @brief Get buffered output
     * @return String containing buffered lines
     */
    String getBuffer();

    /**
     * @brief Clear buffer and reset detection
     */
    void clearBuffer();

    /**
     * @brief Send command to RAK
     * @param command Command string
     */
    void sendCommand(const String& command);

    /**
     * @brief Enable/disable passthrough mode
     */
    void setPassthrough(bool enable);
    bool isPassthrough() const { return m_passthrough; }

    /**
     * @brief Enable/disable hex display mode
     * @param enable true for hex dump, false for text
     */
    void setHexMode(bool enable);
    bool isHexMode() const { return m_hexMode; }

    /**
     * @brief Get the detected serial protocol
     */
    SerialProtocol getDetectedProtocol() const { return m_detectedProtocol; }

    /**
     * @brief Get protocol name as string
     */
    static const char* protocolName(SerialProtocol proto);

    /**
     * @brief Force a specific protocol (disables autodetect)
     */
    void setProtocol(SerialProtocol proto);

    /**
     * @brief Reset autodetection (re-analyze next bytes)
     */
    void resetDetection();

    /**
     * @brief Suspend serial reading (for protobuf operations)
     */
    void suspend();

    /**
     * @brief Resume serial reading after protobuf operations
     */
    void resume();

    bool isSuspended() const { return m_suspended; }

    /**
     * @brief Get the underlying HardwareSerial port
     */
    HardwareSerial* getSerial() { return m_serial; }

    /**
     * @brief Get last N lines from buffer
     */
    String getLastLines(size_t count);

    /**
     * @brief Set callback for new data
     */
    void setDataCallback(void (*callback)(const String&));

private:
    HardwareSerial* m_serial;
    String m_buffer[SERIAL_BUFFER_SIZE];
    size_t m_bufferIndex;
    String m_currentLine;          // text mode line accumulator
    bool m_passthrough;
    volatile bool m_suspended;
    bool m_hexMode;                // true = hex dump display
    uint8_t m_hexLineBuf[16];      // hex mode: 16-byte line accumulator
    size_t m_hexLineOffset;        // bytes accumulated in current hex line
    void (*m_dataCallback)(const String&);

    // --- Protocol autodetection ---
    SerialProtocol m_detectedProtocol;
    bool m_autoDetectDone;         // true once protocol is determined
    uint8_t m_detectBuf[128];      // first bytes for analysis
    size_t m_detectBufLen;
    static const size_t DETECT_SAMPLE_SIZE = 64;  // bytes needed to decide

    /**
     * @brief Analyze detection buffer and identify protocol
     */
    void analyzeProtocol();

    /**
     * @brief Process detection buffer through the appropriate display mode
     */
    void flushDetectBuffer();

    /**
     * @brief Add line to circular buffer
     */
    void addLine(const String& line);

    /**
     * @brief Format a hex dump line (up to 16 bytes)
     */
    static String formatHexLine(const uint8_t* data, size_t len);
};
