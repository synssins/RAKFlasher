/**
 * @file serial_dfu.h
 * @brief Nordic DFU protocol over serial (SLIP-encoded)
 *
 * Implements the Nordic Device Firmware Update protocol for nRF52840.
 * Uses SLIP encoding for packet framing over UART.
 */
#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include "config.h"

// DFU Operation Codes
// Based on Nordic Semiconductor pc-nrfutil specification
enum DFUOpCode {
    DFU_OP_OBJECT_CREATE = 0x01,      // CreateObject
    DFU_OP_RECEIPT_NOTIF_SET = 0x02,  // SetPRN
    DFU_OP_CRC_GET = 0x03,            // CalcChecSum
    DFU_OP_OBJECT_EXECUTE = 0x04,     // Execute
    DFU_OP_READ_ERROR = 0x05,         // ReadError
    DFU_OP_READ_OBJECT = 0x06,        // ReadObject (was OBJECT_SELECT)
    DFU_OP_MTU_GET = 0x07,            // GetSerialMTU
    DFU_OP_OBJECT_WRITE = 0x08,       // WriteObject
    DFU_OP_PING = 0x09,               // Ping
    DFU_OP_RESPONSE = 0x60            // Response
};

// DFU Result Codes
enum DFUResult {
    DFU_RES_SUCCESS = 0x01,
    DFU_RES_INVALID_OBJECT = 0x02,
    DFU_RES_UNSUPPORTED = 0x03,
    DFU_RES_DATA_SIZE = 0x04,
    DFU_RES_CRC_ERROR = 0x05,
    DFU_RES_OPERATION_FAILED = 0x0A
};

class SerialDFU {
public:
    SerialDFU();

    /**
     * @brief Initialize DFU engine
     * @param serial Hardware serial for communication
     * @return true if successful
     */
    bool begin(HardwareSerial& serial);

    /**
     * @brief Flash firmware from file
     * @param filepath Path to .zip DFU package
     * @param progressCallback Callback for progress updates
     * @return true if successful
     */
    bool flashFirmware(const String& filepath,
                       void (*progressCallback)(int progress, const String& msg) = nullptr);

    /**
     * @brief Get last error message
     */
    String getLastError() const { return m_lastError; }

private:
    HardwareSerial* m_serial;
    String m_lastError;
    void (*m_progressCallback)(int, const String&);

    /**
     * @brief Send SLIP-encoded packet
     * @param data Data to send
     * @param len Length of data
     * @return true if sent successfully
     */
    bool sendPacket(const uint8_t* data, size_t len);

    /**
     * @brief Receive SLIP-encoded packet
     * @param buffer Buffer to store received data
     * @param maxLen Maximum buffer length
     * @param timeout Timeout in milliseconds
     * @return Number of bytes received, -1 on error
     */
    int receivePacket(uint8_t* buffer, size_t maxLen, uint32_t timeout);

    /**
     * @brief SLIP encode data
     * @param dest Destination buffer
     * @param src Source data
     * @param len Source length
     * @return Encoded length
     */
    size_t slipEncode(uint8_t* dest, const uint8_t* src, size_t len);

    /**
     * @brief SLIP decode data
     * @param dest Destination buffer
     * @param src Source data
     * @param len Source length
     * @return Decoded length
     */
    size_t slipDecode(uint8_t* dest, const uint8_t* src, size_t len);

    /**
     * @brief Calculate CRC-16 CCITT
     */
    uint16_t crc16(const uint8_t* data, size_t len);

    /**
     * @brief Send DFU command and wait for response
     */
    bool sendCommand(uint8_t opcode, const uint8_t* payload, size_t payloadLen);

    /**
     * @brief Wait for DFU response
     */
    bool waitForResponse(uint8_t expectedOpcode, uint8_t* response, size_t* responseLen,
                         uint32_t timeout = DFU_TIMEOUT);

    /**
     * @brief Extract .bin and .dat from .zip file
     */
    bool extractDFUPackage(const String& zipPath, String& binPath, String& datPath);

    /**
     * @brief Update progress
     */
    void updateProgress(int progress, const String& msg);
};
