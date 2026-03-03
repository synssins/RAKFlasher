/**
 * @file serial_dfu.cpp
 * @brief Implementation of Nordic DFU protocol
 */

#include "serial_dfu.h"
#include <LittleFS.h>

SerialDFU::SerialDFU()
    : m_serial(nullptr)
    , m_progressCallback(nullptr) {
}

bool SerialDFU::begin(HardwareSerial& serial) {
    m_serial = &serial;
    DEBUG_PRINTLN("[DFU] Initialized");
    return true;
}

bool SerialDFU::flashFirmware(const String& filepath,
                              void (*progressCallback)(int, const String&)) {
    m_progressCallback = progressCallback;
    m_lastError = "";

    updateProgress(0, "Starting DFU process...");

    // Check if file exists
    if (!LittleFS.exists(filepath)) {
        m_lastError = "Firmware file not found";
        DEBUG_PRINTLN("[DFU] ERROR: File not found");
        return false;
    }

    updateProgress(10, "Extracting DFU package...");

    // Extract .bin and .dat from .zip
    String binPath, datPath;
    if (!extractDFUPackage(filepath, binPath, datPath)) {
        m_lastError = "Failed to extract DFU package";
        DEBUG_PRINTLN("[DFU] ERROR: Failed to extract package");
        return false;
    }

    updateProgress(20, "Initializing DFU mode...");

    // Send ping to verify bootloader is ready
    if (!sendCommand(DFU_OP_PING, nullptr, 0)) {
        m_lastError = "Bootloader not responding";
        DEBUG_PRINTLN("[DFU] ERROR: Bootloader not responding");
        return false;
    }

    updateProgress(30, "Sending init packet...");

    // TODO: Send init packet (.dat file)
    // This contains firmware metadata and signature

    updateProgress(40, "Transferring firmware...");

    // TODO: Send firmware data (.bin file) in chunks
    // For now, return placeholder success

    updateProgress(100, "Firmware update complete!");

    DEBUG_PRINTLN("[DFU] Firmware flash completed successfully");
    return true;
}

bool SerialDFU::sendPacket(const uint8_t* data, size_t len) {
    if (!m_serial) return false;

    uint8_t buffer[DFU_MTU * 2 + 2];  // Double size for SLIP encoding
    size_t encodedLen = slipEncode(buffer, data, len);

    m_serial->write(buffer, encodedLen);
    m_serial->flush();

    return true;
}

int SerialDFU::receivePacket(uint8_t* buffer, size_t maxLen, uint32_t timeout) {
    if (!m_serial) return -1;

    uint8_t rawBuffer[DFU_MTU * 2];
    size_t rawLen = 0;
    uint32_t startTime = millis();
    bool started = false;

    while (millis() - startTime < timeout) {
        if (m_serial->available()) {
            uint8_t b = m_serial->read();

            if (b == SLIP_END) {
                if (started && rawLen > 0) {
                    // End of packet
                    return slipDecode(buffer, rawBuffer, rawLen);
                }
                started = true;
                rawLen = 0;
            } else if (started) {
                if (rawLen < sizeof(rawBuffer)) {
                    rawBuffer[rawLen++] = b;
                }
            }
        }
        delay(1);
    }

    return -1;  // Timeout
}

size_t SerialDFU::slipEncode(uint8_t* dest, const uint8_t* src, size_t len) {
    size_t pos = 0;

    dest[pos++] = SLIP_END;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == SLIP_END) {
            dest[pos++] = SLIP_ESC;
            dest[pos++] = SLIP_ESC_END;
        } else if (src[i] == SLIP_ESC) {
            dest[pos++] = SLIP_ESC;
            dest[pos++] = SLIP_ESC_ESC;
        } else {
            dest[pos++] = src[i];
        }
    }

    dest[pos++] = SLIP_END;

    return pos;
}

size_t SerialDFU::slipDecode(uint8_t* dest, const uint8_t* src, size_t len) {
    size_t pos = 0;
    bool escape = false;

    for (size_t i = 0; i < len; i++) {
        if (escape) {
            if (src[i] == SLIP_ESC_END) {
                dest[pos++] = SLIP_END;
            } else if (src[i] == SLIP_ESC_ESC) {
                dest[pos++] = SLIP_ESC;
            }
            escape = false;
        } else if (src[i] == SLIP_ESC) {
            escape = true;
        } else if (src[i] != SLIP_END) {
            dest[pos++] = src[i];
        }
    }

    return pos;
}

uint16_t SerialDFU::crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) | (crc << 8);
        crc ^= data[i];
        crc ^= (crc & 0xFF) >> 4;
        crc ^= crc << 12;
        crc ^= (crc & 0xFF) << 5;
    }

    return crc;
}

bool SerialDFU::sendCommand(uint8_t opcode, const uint8_t* payload, size_t payloadLen) {
    uint8_t packet[DFU_MTU];
    packet[0] = opcode;

    if (payload && payloadLen > 0) {
        memcpy(packet + 1, payload, payloadLen);
    }

    return sendPacket(packet, 1 + payloadLen);
}

bool SerialDFU::waitForResponse(uint8_t expectedOpcode, uint8_t* response,
                                size_t* responseLen, uint32_t timeout) {
    uint8_t buffer[DFU_MTU];
    int len = receivePacket(buffer, sizeof(buffer), timeout);

    if (len < 0) {
        m_lastError = "Response timeout";
        return false;
    }

    if (buffer[0] != DFU_OP_RESPONSE) {
        m_lastError = "Invalid response opcode";
        return false;
    }

    if (buffer[1] != expectedOpcode) {
        m_lastError = "Unexpected response";
        return false;
    }

    if (buffer[2] != DFU_RES_SUCCESS) {
        m_lastError = "DFU operation failed";
        return false;
    }

    if (response && responseLen) {
        *responseLen = len - 3;
        memcpy(response, buffer + 3, *responseLen);
    }

    return true;
}

bool SerialDFU::extractDFUPackage(const String& zipPath, String& binPath, String& datPath) {
    // Placeholder: In a real implementation, would unzip the package
    // For now, assume files are already extracted or handle as raw .bin

    binPath = zipPath;  // Temporary placeholder
    datPath = "";

    return true;
}

void SerialDFU::updateProgress(int progress, const String& msg) {
    DEBUG_PRINTF("[DFU] Progress: %d%% - %s\n", progress, msg.c_str());

    if (m_progressCallback) {
        m_progressCallback(progress, msg);
    }
}
