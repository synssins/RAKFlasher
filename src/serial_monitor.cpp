/**
 * @file serial_monitor.cpp
 * @brief Serial monitor with protocol autodetection and hex dump support
 *
 * On startup, the first DETECT_SAMPLE_SIZE bytes are collected into a
 * detection buffer and analyzed to identify the serial protocol.  The
 * display mode (hex/text) is set automatically based on the result.
 * Users can override the mode manually at any time.
 *
 * Detected protocols:
 *   - Meshtastic Protobuf: 0x94 0xC3 frame header → HEX mode
 *   - Nordic SLIP/DFU:     0xC0 SLIP framing      → HEX mode
 *   - NMEA GPS:            $GP/$GN sentences       → TEXT mode
 *   - AT Commands:         AT/OK/ERROR             → TEXT mode
 *   - Generic Text:        >75% printable ASCII    → TEXT mode
 *   - Generic Binary:      otherwise               → HEX mode
 */

#include "serial_monitor.h"

static const char HEX_CHARS[] = "0123456789ABCDEF";

// ---------------------------------------------------------------
// Protocol name table
// ---------------------------------------------------------------

const char* SerialMonitor::protocolName(SerialProtocol proto) {
    switch (proto) {
        case PROTO_MESHTASTIC:  return "Meshtastic Protobuf";
        case PROTO_SLIP_DFU:    return "Nordic SLIP/DFU";
        case PROTO_NMEA:        return "NMEA GPS";
        case PROTO_AT_COMMAND:  return "AT Command";
        case PROTO_TEXT:        return "Text";
        case PROTO_BINARY:      return "Binary";
        default:                return "Unknown";
    }
}

// ---------------------------------------------------------------
// Construction
// ---------------------------------------------------------------

SerialMonitor::SerialMonitor()
    : m_serial(nullptr)
    , m_bufferIndex(0)
    , m_passthrough(false)
    , m_suspended(false)
    , m_hexMode(true)
    , m_hexLineOffset(0)
    , m_dataCallback(nullptr)
    , m_detectedProtocol(PROTO_UNKNOWN)
    , m_autoDetectDone(false)
    , m_detectBufLen(0) {
    memset(m_hexLineBuf, 0, sizeof(m_hexLineBuf));
    memset(m_detectBuf, 0, sizeof(m_detectBuf));
}

// ---------------------------------------------------------------
// begin / loop
// ---------------------------------------------------------------

bool SerialMonitor::begin(HardwareSerial& serial, uint32_t baud) {
    m_serial = &serial;
    m_serial->begin(baud, SERIAL_8N1, DEFAULT_PIN_UART_RX, DEFAULT_PIN_UART_TX);

    DEBUG_PRINTF("[Serial] Monitor started at %d baud\n", baud);
    DEBUG_PRINTF("[Serial] RX: GPIO%d, TX: GPIO%d\n",
                 DEFAULT_PIN_UART_RX, DEFAULT_PIN_UART_TX);
    DEBUG_PRINTLN("[Serial] Protocol autodetect active — collecting sample...");

    return true;
}

void SerialMonitor::loop() {
    if (!m_serial || m_suspended) return;

    while (m_serial->available() && !m_suspended) {
        uint8_t b = m_serial->read();

        // Echo to console in passthrough mode
        if (m_passthrough) {
            Serial.write(b);
        }

        // --- AUTODETECT PHASE: collect first N bytes ---
        if (!m_autoDetectDone) {
            if (m_detectBufLen < sizeof(m_detectBuf)) {
                m_detectBuf[m_detectBufLen++] = b;
            }

            if (m_detectBufLen >= DETECT_SAMPLE_SIZE) {
                analyzeProtocol();
                flushDetectBuffer();
            }
            continue;  // don't process bytes through display until detected
        }

        // --- DISPLAY PHASE: hex or text mode ---
        if (m_hexMode) {
            m_hexLineBuf[m_hexLineOffset++] = b;
            if (m_hexLineOffset >= 16) {
                addLine(formatHexLine(m_hexLineBuf, m_hexLineOffset));
                m_hexLineOffset = 0;
            }
        } else {
            char c = (char)b;
            if (c == '\n') {
                if (m_currentLine.length() > 0) {
                    addLine(m_currentLine);
                    m_currentLine = "";
                }
            } else if (c != '\r') {
                if (b < 0x20 || b > 0x7E) {
                    m_currentLine += '.';
                } else {
                    m_currentLine += c;
                }
                if (m_currentLine.length() >= SERIAL_LINE_MAX) {
                    addLine(m_currentLine);
                    m_currentLine = "";
                }
            }
        }
    }

    // Flush partial hex line when no more data is available
    if (m_autoDetectDone && m_hexMode && m_hexLineOffset > 0) {
        addLine(formatHexLine(m_hexLineBuf, m_hexLineOffset));
        m_hexLineOffset = 0;
    }

    // Timeout: if we have some detect bytes but haven't reached the
    // sample size within a reasonable time, analyze what we have
    if (!m_autoDetectDone && m_detectBufLen > 0) {
        // This path is hit when available() returns 0 — burst is over
        // If we have at least 4 bytes, try to detect
        if (m_detectBufLen >= 4) {
            analyzeProtocol();
            flushDetectBuffer();
        }
    }
}

// ---------------------------------------------------------------
// Protocol detection
// ---------------------------------------------------------------

void SerialMonitor::analyzeProtocol() {
    if (m_autoDetectDone) return;

    size_t len = m_detectBufLen;
    const uint8_t* buf = m_detectBuf;

    // --- Check for Meshtastic protobuf framing (0x94 0xC3) ---
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == 0x94 && buf[i + 1] == 0xC3) {
            m_detectedProtocol = PROTO_MESHTASTIC;
            m_hexMode = true;
            m_autoDetectDone = true;
            DEBUG_PRINTLN("[Serial] Detected: Meshtastic Protobuf (0x94 0xC3 header)");
            return;
        }
    }

    // --- Check for SLIP framing (0xC0) ---
    if (buf[0] == 0xC0) {
        // SLIP frames start and end with 0xC0
        for (size_t i = 1; i < len; i++) {
            if (buf[i] == 0xC0) {
                m_detectedProtocol = PROTO_SLIP_DFU;
                m_hexMode = true;
                m_autoDetectDone = true;
                DEBUG_PRINTLN("[Serial] Detected: Nordic SLIP/DFU (0xC0 framing)");
                return;
            }
        }
    }

    // --- Check for NMEA GPS sentences ($GP, $GN, $GL, $GA, $BD) ---
    for (size_t i = 0; i + 2 < len; i++) {
        if (buf[i] == '$' && (
            (buf[i + 1] == 'G' && (buf[i + 2] == 'P' || buf[i + 2] == 'N' ||
                                    buf[i + 2] == 'L' || buf[i + 2] == 'A')) ||
            (buf[i + 1] == 'B' && buf[i + 2] == 'D'))) {
            m_detectedProtocol = PROTO_NMEA;
            m_hexMode = false;
            m_autoDetectDone = true;
            DEBUG_PRINTLN("[Serial] Detected: NMEA GPS sentences");
            return;
        }
    }

    // --- Check for AT commands (case-insensitive) ---
    // Look for "AT", "OK\r\n", "+CME", "ERROR"
    for (size_t i = 0; i + 1 < len; i++) {
        // "AT" at start or after \n
        if ((i == 0 || buf[i - 1] == '\n') &&
            (buf[i] == 'A' || buf[i] == 'a') &&
            (buf[i + 1] == 'T' || buf[i + 1] == 't')) {
            m_detectedProtocol = PROTO_AT_COMMAND;
            m_hexMode = false;
            m_autoDetectDone = true;
            DEBUG_PRINTLN("[Serial] Detected: AT Command interface");
            return;
        }

        // "OK\r" response
        if ((i == 0 || buf[i - 1] == '\n') &&
            buf[i] == 'O' && i + 1 < len && buf[i + 1] == 'K') {
            m_detectedProtocol = PROTO_AT_COMMAND;
            m_hexMode = false;
            m_autoDetectDone = true;
            DEBUG_PRINTLN("[Serial] Detected: AT Command interface (OK response)");
            return;
        }
    }

    // --- Heuristic: count printable vs non-printable bytes ---
    size_t printable = 0;
    for (size_t i = 0; i < len; i++) {
        if ((buf[i] >= 0x20 && buf[i] <= 0x7E) ||
            buf[i] == '\n' || buf[i] == '\r' || buf[i] == '\t') {
            printable++;
        }
    }

    float printableRatio = (float)printable / (float)len;

    if (printableRatio > 0.75f) {
        m_detectedProtocol = PROTO_TEXT;
        m_hexMode = false;
        m_autoDetectDone = true;
        DEBUG_PRINTF("[Serial] Detected: Generic Text (%.0f%% printable)\n",
                     printableRatio * 100.0f);
    } else {
        m_detectedProtocol = PROTO_BINARY;
        m_hexMode = true;
        m_autoDetectDone = true;
        DEBUG_PRINTF("[Serial] Detected: Generic Binary (%.0f%% printable)\n",
                     printableRatio * 100.0f);
    }
}

void SerialMonitor::flushDetectBuffer() {
    // Process all buffered detect bytes through the now-active display mode
    addLine(String("--- Protocol detected: ") + protocolName(m_detectedProtocol) + " ---");

    for (size_t i = 0; i < m_detectBufLen; i++) {
        uint8_t b = m_detectBuf[i];

        if (m_hexMode) {
            m_hexLineBuf[m_hexLineOffset++] = b;
            if (m_hexLineOffset >= 16) {
                addLine(formatHexLine(m_hexLineBuf, m_hexLineOffset));
                m_hexLineOffset = 0;
            }
        } else {
            char c = (char)b;
            if (c == '\n') {
                if (m_currentLine.length() > 0) {
                    addLine(m_currentLine);
                    m_currentLine = "";
                }
            } else if (c != '\r') {
                if (b < 0x20 || b > 0x7E) {
                    m_currentLine += '.';
                } else {
                    m_currentLine += c;
                }
                if (m_currentLine.length() >= SERIAL_LINE_MAX) {
                    addLine(m_currentLine);
                    m_currentLine = "";
                }
            }
        }
    }

    // Flush any remaining hex data
    if (m_hexMode && m_hexLineOffset > 0) {
        addLine(formatHexLine(m_hexLineBuf, m_hexLineOffset));
        m_hexLineOffset = 0;
    }
}

// ---------------------------------------------------------------
// Display formatting
// ---------------------------------------------------------------

String SerialMonitor::formatHexLine(const uint8_t* data, size_t len) {
    String line;
    line.reserve(80);

    // Hex portion
    for (size_t i = 0; i < 16; i++) {
        if (i == 8) line += ' ';
        if (i < len) {
            line += HEX_CHARS[data[i] >> 4];
            line += HEX_CHARS[data[i] & 0x0F];
            line += ' ';
        } else {
            line += "   ";
        }
    }

    // ASCII portion
    line += " |";
    for (size_t i = 0; i < len; i++) {
        line += (data[i] >= 0x20 && data[i] <= 0x7E) ? (char)data[i] : '.';
    }
    line += '|';

    return line;
}

// ---------------------------------------------------------------
// Buffer management
// ---------------------------------------------------------------

String SerialMonitor::getBuffer() {
    String result;
    for (size_t i = 0; i < SERIAL_BUFFER_SIZE; i++) {
        size_t idx = (m_bufferIndex + i) % SERIAL_BUFFER_SIZE;
        if (m_buffer[idx].length() > 0) {
            result += m_buffer[idx];
            result += "\n";
        }
    }
    return result;
}

void SerialMonitor::clearBuffer() {
    for (size_t i = 0; i < SERIAL_BUFFER_SIZE; i++) {
        m_buffer[i] = "";
    }
    m_bufferIndex = 0;
    m_currentLine = "";
    m_hexLineOffset = 0;
    DEBUG_PRINTLN("[Serial] Buffer cleared");
}

void SerialMonitor::sendCommand(const String& command) {
    if (!m_serial) return;
    m_serial->println(command);
    DEBUG_PRINTF("[Serial] Sent command: %s\n", command.c_str());
}

void SerialMonitor::setPassthrough(bool enable) {
    m_passthrough = enable;
    DEBUG_PRINTF("[Serial] Passthrough mode: %s\n", enable ? "ON" : "OFF");
}

void SerialMonitor::setHexMode(bool enable) {
    if (m_hexMode == enable) return;

    // Flush pending data in current mode
    if (m_hexMode && m_hexLineOffset > 0) {
        addLine(formatHexLine(m_hexLineBuf, m_hexLineOffset));
        m_hexLineOffset = 0;
    } else if (!m_hexMode && m_currentLine.length() > 0) {
        addLine(m_currentLine);
        m_currentLine = "";
    }

    m_hexMode = enable;
    DEBUG_PRINTF("[Serial] Display mode: %s\n", enable ? "HEX" : "TEXT");
}

void SerialMonitor::setProtocol(SerialProtocol proto) {
    m_detectedProtocol = proto;
    m_autoDetectDone = true;

    // Set appropriate display mode
    switch (proto) {
        case PROTO_MESHTASTIC:
        case PROTO_SLIP_DFU:
        case PROTO_BINARY:
            setHexMode(true);
            break;
        case PROTO_NMEA:
        case PROTO_AT_COMMAND:
        case PROTO_TEXT:
            setHexMode(false);
            break;
        default:
            break;
    }

    DEBUG_PRINTF("[Serial] Protocol forced: %s\n", protocolName(proto));
}

void SerialMonitor::resetDetection() {
    m_autoDetectDone = false;
    m_detectedProtocol = PROTO_UNKNOWN;
    m_detectBufLen = 0;
    memset(m_detectBuf, 0, sizeof(m_detectBuf));
    DEBUG_PRINTLN("[Serial] Protocol detection reset — will re-analyze");
}

String SerialMonitor::getLastLines(size_t count) {
    String result;
    size_t linesToGet = min(count, static_cast<size_t>(SERIAL_BUFFER_SIZE));

    for (size_t i = SERIAL_BUFFER_SIZE - linesToGet; i < SERIAL_BUFFER_SIZE; i++) {
        size_t idx = (m_bufferIndex + i) % SERIAL_BUFFER_SIZE;
        if (m_buffer[idx].length() > 0) {
            result += m_buffer[idx];
            result += "\n";
        }
    }
    return result;
}

void SerialMonitor::suspend() {
    m_suspended = true;
    DEBUG_PRINTLN("[Serial] Monitor SUSPENDED for protobuf operation");
}

void SerialMonitor::resume() {
    m_suspended = false;
    DEBUG_PRINTLN("[Serial] Monitor RESUMED");
}

void SerialMonitor::setDataCallback(void (*callback)(const String&)) {
    m_dataCallback = callback;
}

void SerialMonitor::addLine(const String& line) {
    m_buffer[m_bufferIndex] = line;
    m_bufferIndex = (m_bufferIndex + 1) % SERIAL_BUFFER_SIZE;

    if (m_dataCallback) {
        m_dataCallback(line);
    }
}
