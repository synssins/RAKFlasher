/**
 * @file serial_bridge.h
 * @brief Serial bridge for Meshtastic client passthrough
 *
 * Provides two bridge modes so external tools (Python CLI, web client, etc.)
 * can communicate with the RAK4631 through the ESP32:
 *
 *   TCP bridge  (port 4403) — transparent byte forwarding, identical to
 *                              connecting a serial cable.  Uses the same
 *                              0x94 0xC3 framing as the physical UART.
 *
 *   HTTP bridge (/api/v1/)  — Meshtastic-compatible REST API.
 *                              PUT  /api/v1/toradio   — send raw protobuf
 *                              GET  /api/v1/fromradio — receive raw protobuf
 *                              Framing is added/stripped automatically.
 *
 * While either bridge is active the serial monitor is suspended.
 * Only one bridge mode (and one client) at a time.
 */
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <HardwareSerial.h>
#include "config.h"
#include "meshtastic_proto.h"

// Bridge operating modes
enum BridgeMode {
    BRIDGE_IDLE = 0,    // No bridge active — serial monitor runs normally
    BRIDGE_TCP,         // TCP client connected — transparent byte forwarding
    BRIDGE_HTTP         // HTTP REST API in use — frame-level bridging
};

class SerialBridge {
public:
    SerialBridge();
    ~SerialBridge();

    /**
     * @brief Start the TCP server and prepare the bridge
     * @param serial Reference to the HardwareSerial used for RAK comms
     * @return true on success
     */
    bool begin(HardwareSerial& serial);

    /**
     * @brief Main loop — call from Arduino loop()
     *
     * Checks for new TCP connections, forwards bytes when a TCP client
     * is connected, and parses incoming UART frames when the HTTP bridge
     * is active.
     */
    void loop();

    /**
     * @brief Stop the bridge and TCP server
     */
    void stop();

    // === TCP bridge ===

    bool hasTCPClient();
    void disconnectTCPClient();

    // === HTTP bridge API (called from web server handlers) ===

    /**
     * @brief Send a raw protobuf ToRadio message to the RAK via UART
     *
     * Wraps the payload in 0x94 0xC3 serial framing before sending.
     * Activates HTTP bridge mode if not already active.
     *
     * @param protobuf Raw protobuf bytes (no framing)
     * @param len      Payload length (max MT_MAX_PAYLOAD)
     * @return true if sent successfully
     */
    bool httpSendToRadio(const uint8_t* protobuf, size_t len);

    /**
     * @brief Read one queued FromRadio protobuf payload
     *
     * Returns the raw protobuf bytes (framing stripped) of the next
     * queued message.  Returns 0 if the queue is empty.
     *
     * @param buf    Output buffer
     * @param maxLen Buffer size (should be >= MT_MAX_PAYLOAD)
     * @return Number of bytes written, or 0 if nothing available
     */
    size_t httpReadFromRadio(uint8_t* buf, size_t maxLen);

    /**
     * @brief Reset the HTTP inactivity timer
     */
    void httpKeepAlive();

    // === Configuration ===

    uint32_t getBaudRate() const { return m_baudRate; }
    void setBaudRate(uint32_t baud) { m_baudRate = baud; }

    // === State ===

    BridgeMode getMode() const { return m_mode; }
    bool isActive() const { return m_mode != BRIDGE_IDLE; }
    String getTCPClientIPStr();
    unsigned long getActiveSeconds() const;

    // === Statistics ===

    uint32_t getTxBytes() const { return m_txBytes; }
    uint32_t getRxBytes() const { return m_rxBytes; }
    void resetStats();

private:
    HardwareSerial* m_serial;
    WiFiServer      m_tcpServer;
    WiFiClient      m_tcpClient;

    volatile BridgeMode m_mode;
    uint32_t            m_baudRate;
    unsigned long       m_activeStartTime;

    // --- TCP bridge state ---
    unsigned long m_tcpLastActivity;
    static const unsigned long TCP_TIMEOUT_MS = 60000;   // 60 s inactivity

    // --- HTTP bridge state ---
    unsigned long m_httpLastActivity;
    static const unsigned long HTTP_TIMEOUT_MS = 30000;  // 30 s inactivity

    // Frame queue (SPSC ring buffer: producer = loop/core 1, consumer = HTTP handler/core 0)
    static const int FRAME_QUEUE_SIZE = 16;
    struct QueuedFrame {
        uint16_t len;
        uint8_t  data[MT_MAX_PAYLOAD];
    };
    QueuedFrame   m_frameQueue[FRAME_QUEUE_SIZE];
    volatile int  m_queueHead;   // next write slot (modified by loop)
    volatile int  m_queueTail;   // next read  slot (modified by HTTP handler)

    // Meshtastic frame parser (used in HTTP mode to extract payloads)
    enum ParseState { PS_SYNC1, PS_SYNC2, PS_LEN_MSB, PS_LEN_LSB, PS_PAYLOAD };
    ParseState m_parseState;
    uint16_t   m_parseExpectedLen;
    uint16_t   m_parseBufPos;
    uint8_t    m_parseBuf[MT_MAX_PAYLOAD];

    // Statistics
    uint32_t m_txBytes;
    uint32_t m_rxBytes;

    // --- Internal helpers ---
    void handleTCPBridge();
    void handleHTTPBridge();
    void activateBridge(BridgeMode mode);
    void deactivateBridge();
    void parseIncomingByte(uint8_t b);
    void enqueueFrame(const uint8_t* data, uint16_t len);
};
