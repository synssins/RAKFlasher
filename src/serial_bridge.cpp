/**
 * @file serial_bridge.cpp
 * @brief Serial bridge for Meshtastic client passthrough
 *
 * TCP bridge:  transparent byte-level forwarding on port 4403.
 * HTTP bridge: frame-level Meshtastic REST API (/api/v1/toradio, /api/v1/fromradio).
 *
 * Both modes suspend the serial monitor while active.
 */

#include "serial_bridge.h"
#include "serial_monitor.h"

// External serial monitor for suspend/resume
extern SerialMonitor serialMonitor;

// ---------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------

SerialBridge::SerialBridge()
    : m_serial(nullptr)
    , m_tcpServer(BRIDGE_TCP_PORT)
    , m_mode(BRIDGE_IDLE)
    , m_baudRate(RAK_SERIAL_BAUD)
    , m_activeStartTime(0)
    , m_tcpLastActivity(0)
    , m_httpLastActivity(0)
    , m_queueHead(0)
    , m_queueTail(0)
    , m_parseState(PS_SYNC1)
    , m_parseExpectedLen(0)
    , m_parseBufPos(0)
    , m_txBytes(0)
    , m_rxBytes(0) {
}

SerialBridge::~SerialBridge() {
    stop();
}

// ---------------------------------------------------------------
// begin / stop
// ---------------------------------------------------------------

bool SerialBridge::begin(HardwareSerial& serial) {
    m_serial = &serial;

    m_tcpServer.begin();
    m_tcpServer.setNoDelay(true);

    DEBUG_PRINTF("[Bridge] TCP server listening on port %d\n", BRIDGE_TCP_PORT);
    return true;
}

void SerialBridge::stop() {
    if (m_mode != BRIDGE_IDLE) {
        deactivateBridge();
    }
    m_tcpServer.end();
}

// ---------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------

void SerialBridge::loop() {
    // --- Check for new TCP connections ---
    if (m_tcpServer.hasClient()) {
        WiFiClient incoming = m_tcpServer.accept();

        if (m_mode == BRIDGE_TCP && m_tcpClient && m_tcpClient.connected()) {
            // Already have a TCP client — reject
            DEBUG_PRINTF("[Bridge] Rejecting TCP client %s (slot in use)\n",
                         incoming.remoteIP().toString().c_str());
            incoming.stop();
        } else if (m_mode == BRIDGE_HTTP) {
            // HTTP bridge is active — reject TCP
            DEBUG_PRINTLN("[Bridge] Rejecting TCP client (HTTP bridge active)");
            incoming.stop();
        } else {
            // Accept the connection
            m_tcpClient = incoming;
            activateBridge(BRIDGE_TCP);
            DEBUG_PRINTF("[Bridge] TCP client connected: %s\n",
                         m_tcpClient.remoteIP().toString().c_str());
        }
    }

    // --- Handle active bridge mode ---
    switch (m_mode) {
        case BRIDGE_TCP:
            handleTCPBridge();
            break;
        case BRIDGE_HTTP:
            handleHTTPBridge();
            break;
        case BRIDGE_IDLE:
        default:
            break;
    }
}

// ---------------------------------------------------------------
// TCP bridge — transparent byte forwarding
// ---------------------------------------------------------------

void SerialBridge::handleTCPBridge() {
    if (!m_tcpClient || !m_tcpClient.connected()) {
        DEBUG_PRINTLN("[Bridge] TCP client disconnected");
        deactivateBridge();
        return;
    }

    bool activity = false;

    // TCP → UART  (client sending to RAK)
    int tcpAvail = m_tcpClient.available();
    if (tcpAvail > 0) {
        uint8_t buf[256];
        int toRead = min(tcpAvail, (int)sizeof(buf));
        int bytesRead = m_tcpClient.read(buf, toRead);
        if (bytesRead > 0) {
            m_serial->write(buf, bytesRead);
            m_txBytes += bytesRead;
            activity = true;
        }
    }

    // UART → TCP  (RAK responding to client)
    int uartAvail = m_serial->available();
    if (uartAvail > 0) {
        uint8_t buf[256];
        int toRead = min(uartAvail, (int)sizeof(buf));
        for (int i = 0; i < toRead; i++) {
            buf[i] = m_serial->read();
        }
        m_tcpClient.write(buf, toRead);
        // setNoDelay(true) disables Nagle — data sends immediately after write()
        m_rxBytes += toRead;
        activity = true;
    }

    if (activity) {
        m_tcpLastActivity = millis();
    }

    // Check TCP inactivity timeout
    if (millis() - m_tcpLastActivity > TCP_TIMEOUT_MS) {
        DEBUG_PRINTLN("[Bridge] TCP inactivity timeout — disconnecting client");
        deactivateBridge();
    }
}

// ---------------------------------------------------------------
// HTTP bridge — frame-level Meshtastic API
// ---------------------------------------------------------------

void SerialBridge::handleHTTPBridge() {
    // Read available UART bytes and parse into frames
    while (m_serial->available()) {
        uint8_t b = m_serial->read();
        m_rxBytes++;
        parseIncomingByte(b);
    }

    // Check HTTP inactivity timeout
    if (millis() - m_httpLastActivity > HTTP_TIMEOUT_MS) {
        DEBUG_PRINTLN("[Bridge] HTTP timeout — deactivating");
        deactivateBridge();
    }
}

bool SerialBridge::httpSendToRadio(const uint8_t* protobuf, size_t len) {
    if (!m_serial || len == 0 || len > MT_MAX_PAYLOAD) return false;

    // Activate HTTP bridge if idle
    if (m_mode == BRIDGE_IDLE) {
        activateBridge(BRIDGE_HTTP);
    } else if (m_mode == BRIDGE_TCP) {
        return false;  // TCP bridge has exclusive access
    }

    m_httpLastActivity = millis();

    // Wrap in Meshtastic serial framing: 0x94 0xC3 [MSB len] [LSB len] [payload]
    uint8_t header[4] = {
        MT_START1,
        MT_START2,
        (uint8_t)(len >> 8),
        (uint8_t)(len & 0xFF)
    };
    m_serial->write(header, 4);
    m_serial->write(protobuf, len);
    m_serial->flush();

    m_txBytes += 4 + len;

    DEBUG_PRINTF("[Bridge] HTTP toradio: %d bytes (framed: %d)\n",
                 (int)len, (int)(len + 4));
    return true;
}

size_t SerialBridge::httpReadFromRadio(uint8_t* buf, size_t maxLen) {
    // Activate HTTP bridge if idle
    if (m_mode == BRIDGE_IDLE) {
        activateBridge(BRIDGE_HTTP);
    } else if (m_mode == BRIDGE_TCP) {
        return 0;
    }

    m_httpLastActivity = millis();

    // Check if queue is empty
    if (m_queueHead == m_queueTail) {
        return 0;
    }

    // Dequeue one frame
    const QueuedFrame& frame = m_frameQueue[m_queueTail];
    size_t copyLen = min((size_t)frame.len, maxLen);
    memcpy(buf, frame.data, copyLen);

    // Advance tail (atomic for SPSC)
    m_queueTail = (m_queueTail + 1) % FRAME_QUEUE_SIZE;

    DEBUG_PRINTF("[Bridge] HTTP fromradio: %d bytes\n", (int)copyLen);
    return copyLen;
}

void SerialBridge::httpKeepAlive() {
    if (m_mode == BRIDGE_HTTP) {
        m_httpLastActivity = millis();
    }
}

// ---------------------------------------------------------------
// Frame parser (for HTTP bridge mode)
// ---------------------------------------------------------------

void SerialBridge::parseIncomingByte(uint8_t b) {
    switch (m_parseState) {
        case PS_SYNC1:
            if (b == MT_START1) {
                m_parseState = PS_SYNC2;
            }
            break;

        case PS_SYNC2:
            if (b == MT_START2) {
                m_parseState = PS_LEN_MSB;
            } else {
                m_parseState = PS_SYNC1;
            }
            break;

        case PS_LEN_MSB:
            m_parseExpectedLen = (uint16_t)b << 8;
            m_parseState = PS_LEN_LSB;
            break;

        case PS_LEN_LSB:
            m_parseExpectedLen |= b;
            if (m_parseExpectedLen == 0 || m_parseExpectedLen > MT_MAX_PAYLOAD) {
                // Invalid length — resync
                DEBUG_PRINTF("[Bridge] Invalid frame len: %d — resyncing\n", m_parseExpectedLen);
                m_parseState = PS_SYNC1;
            } else {
                m_parseBufPos = 0;
                m_parseState = PS_PAYLOAD;
            }
            break;

        case PS_PAYLOAD:
            m_parseBuf[m_parseBufPos++] = b;
            if (m_parseBufPos >= m_parseExpectedLen) {
                // Complete frame — enqueue the payload
                enqueueFrame(m_parseBuf, m_parseExpectedLen);
                m_parseState = PS_SYNC1;
            }
            break;
    }
}

void SerialBridge::enqueueFrame(const uint8_t* data, uint16_t len) {
    int nextHead = (m_queueHead + 1) % FRAME_QUEUE_SIZE;

    if (nextHead == m_queueTail) {
        // Queue full — drop oldest frame
        DEBUG_PRINTLN("[Bridge] Frame queue full — dropping oldest");
        m_queueTail = (m_queueTail + 1) % FRAME_QUEUE_SIZE;
    }

    QueuedFrame& slot = m_frameQueue[m_queueHead];
    memcpy(slot.data, data, len);
    slot.len = len;

    // Advance head (atomic for SPSC)
    m_queueHead = nextHead;
}

// ---------------------------------------------------------------
// Bridge activation / deactivation
// ---------------------------------------------------------------

void SerialBridge::activateBridge(BridgeMode mode) {
    if (m_mode != BRIDGE_IDLE) return;

    m_mode = mode;
    m_activeStartTime = millis();

    // Suspend serial monitor — it checks m_suspended on every byte read
    serialMonitor.suspend();

    if (mode == BRIDGE_TCP) {
        m_tcpLastActivity = millis();
    } else if (mode == BRIDGE_HTTP) {
        m_httpLastActivity = millis();
        // Reset frame parser and queue
        m_parseState = PS_SYNC1;
        m_parseExpectedLen = 0;
        m_parseBufPos = 0;
        m_queueHead = 0;
        m_queueTail = 0;
    }

    resetStats();
    DEBUG_PRINTF("[Bridge] Activated: %s\n",
                 mode == BRIDGE_TCP ? "TCP" : "HTTP");
}

void SerialBridge::deactivateBridge() {
    BridgeMode prev = m_mode;
    m_mode = BRIDGE_IDLE;

    if (m_tcpClient && m_tcpClient.connected()) {
        m_tcpClient.stop();
    }

    // Resume serial monitor and re-detect protocol
    serialMonitor.resume();
    serialMonitor.resetDetection();

    unsigned long elapsed = (millis() - m_activeStartTime) / 1000;
    DEBUG_PRINTF("[Bridge] Deactivated %s after %lus (TX: %u, RX: %u bytes)\n",
                 prev == BRIDGE_TCP ? "TCP" : "HTTP",
                 elapsed, m_txBytes, m_rxBytes);
}

// ---------------------------------------------------------------
// State accessors
// ---------------------------------------------------------------

bool SerialBridge::hasTCPClient() {
    return m_mode == BRIDGE_TCP && m_tcpClient && m_tcpClient.connected();
}

void SerialBridge::disconnectTCPClient() {
    if (m_mode == BRIDGE_TCP) {
        deactivateBridge();
    }
}

String SerialBridge::getTCPClientIPStr() {
    if (hasTCPClient()) {
        return m_tcpClient.remoteIP().toString();
    }
    return "";
}

unsigned long SerialBridge::getActiveSeconds() const {
    if (m_mode == BRIDGE_IDLE) return 0;
    return (millis() - m_activeStartTime) / 1000;
}

void SerialBridge::resetStats() {
    m_txBytes = 0;
    m_rxBytes = 0;
}
