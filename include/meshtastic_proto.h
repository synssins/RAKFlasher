/**
 * @file meshtastic_proto.h
 * @brief Meshtastic protobuf serial protocol handler
 *
 * Implements the Meshtastic serial API framing and minimal protobuf
 * encoding/decoding for configuration backup and restore.
 *
 * Protocol: 0x94 0xC3 [MSB len] [LSB len] [protobuf payload]
 */
#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include "config.h"

// Meshtastic serial framing constants
#define MT_START1           0x94
#define MT_START2           0xC3
#define MT_MAX_PAYLOAD      512
#define MT_FRAME_HEADER     4

// Protobuf wire types
#define PB_WIRE_VARINT      0
#define PB_WIRE_I64         1
#define PB_WIRE_LEN         2
#define PB_WIRE_I32         5

// ToRadio field tags
#define TAG_TORADIO_PACKET          0x0A  // field 1, LEN (MeshPacket)
#define TAG_TORADIO_WANT_CONFIG     0x18  // field 3, VARINT

// FromRadio field tags (for identifying response type)
#define TAG_FROMRADIO_ID            0x08  // field 1, VARINT
#define TAG_FROMRADIO_PACKET        0x12  // field 2, LEN
#define TAG_FROMRADIO_MY_INFO       0x1A  // field 3, LEN
#define TAG_FROMRADIO_NODE_INFO     0x22  // field 4, LEN
#define TAG_FROMRADIO_CONFIG        0x2A  // field 5, LEN
#define TAG_FROMRADIO_LOG           0x32  // field 6, LEN
#define TAG_FROMRADIO_CONFIG_DONE   0x38  // field 7, VARINT
#define TAG_FROMRADIO_REBOOTED      0x40  // field 8, VARINT
#define TAG_FROMRADIO_MODULE_CFG    0x4A  // field 9, LEN
#define TAG_FROMRADIO_CHANNEL       0x52  // field 10, LEN
#define TAG_FROMRADIO_QUEUE_STATUS  0x5A  // field 11, LEN
#define TAG_FROMRADIO_METADATA      0x6A  // field 13, LEN

// AdminMessage GET request field tags (verified against meshtastic/admin.proto)
// These are varint-typed fields whose VALUE is the sub-type enum to request
#define TAG_ADMIN_GET_CHANNEL_REQ   0x08  // field 1, VARINT (channel index 0-7)
#define TAG_ADMIN_GET_OWNER_REQ     0x18  // field 3, VARINT (bool true)
#define TAG_ADMIN_GET_CONFIG_REQ    0x28  // field 5, VARINT (ConfigType enum 0-9)
#define TAG_ADMIN_GET_MODULE_REQ    0x38  // field 7, VARINT (ModuleConfigType enum 0-15)
#define TAG_ADMIN_GET_METADATA_REQ  0x60  // field 12, VARINT (bool true)

// AdminMessage action field tags (2-byte varint tags for high field numbers)
#define TAG_ADMIN_FACTORY_RESET_H   0xF0  // field 94, VARINT (int32)
#define TAG_ADMIN_FACTORY_RESET_L   0x05
#define TAG_ADMIN_REBOOT_OTA_H      0xF8  // field 95, VARINT (int32, deprecated)
#define TAG_ADMIN_REBOOT_OTA_L      0x05
#define TAG_ADMIN_REBOOT_H          0x88  // field 97, VARINT (int32 seconds)
#define TAG_ADMIN_REBOOT_L          0x06
#define TAG_ADMIN_NODEDB_RESET_H    0xA0  // field 100, VARINT (bool)
#define TAG_ADMIN_NODEDB_RESET_L    0x06

// AdminMessage SET field tags (2-byte varints for field numbers 32-35)
#define TAG_ADMIN_SET_OWNER_H       0x82  // field 32, LEN (first byte)
#define TAG_ADMIN_SET_OWNER_L       0x02
#define TAG_ADMIN_SET_CHANNEL_H     0x8A  // field 33, LEN
#define TAG_ADMIN_SET_CHANNEL_L     0x02
#define TAG_ADMIN_SET_CONFIG_H      0x92  // field 34, LEN
#define TAG_ADMIN_SET_CONFIG_L      0x02
#define TAG_ADMIN_SET_MODULE_H      0x9A  // field 35, LEN
#define TAG_ADMIN_SET_MODULE_L      0x02

// MeshPacket field tags
#define TAG_MESHPACKET_TO           0x15  // field 2, fixed32
#define TAG_MESHPACKET_DECODED      0x22  // field 4, LEN
#define TAG_MESHPACKET_ID           0x35  // field 6, fixed32
#define TAG_MESHPACKET_WANT_ACK    0x50  // field 10, VARINT

// Data field tags
#define TAG_DATA_PORTNUM            0x08  // field 1, VARINT
#define TAG_DATA_PAYLOAD            0x12  // field 2, LEN
#define TAG_DATA_WANT_RESPONSE      0x18  // field 3, VARINT

// Meshtastic port numbers
#define PORTNUM_ADMIN               6

// Special nonce for config-only dump (skips node info)
#define MT_NONCE_CONFIG_ONLY        69420

// Config sub-type names (for JSON output)
static const char* CONFIG_TYPE_NAMES[] = {
    "device", "position", "power", "network", "display",
    "lora", "bluetooth", "security", "sessionkey", "deviceui"
};

static const char* MODULE_TYPE_NAMES[] = {
    "mqtt", "serial", "ext_notif", "store_forward", "range_test",
    "telemetry", "canned_msg", "audio", "remote_hw", "neighbor_info",
    "ambient_light", "detection", "paxcounter", "statusmessage",
    "traffic_mgmt", "tak"
};

// Types for categorizing FromRadio messages
enum FromRadioType {
    FR_UNKNOWN = 0,
    FR_MY_INFO,
    FR_NODE_INFO,
    FR_CONFIG,
    FR_MODULE_CONFIG,
    FR_CHANNEL,
    FR_CONFIG_COMPLETE,
    FR_METADATA,
    FR_PACKET,
    FR_LOG,
    FR_REBOOTED
};

// Stored protobuf frame for backup
struct ProtoFrame {
    FromRadioType type;
    uint8_t subType;        // Config/ModuleConfig sub-type index
    uint16_t dataLen;
    uint8_t* data;          // Raw protobuf of the inner message (Config/Channel/etc.)
};

class MeshtasticProtocol {
public:
    MeshtasticProtocol();
    ~MeshtasticProtocol();

    /**
     * @brief Set the serial port to use
     */
    void setSerial(HardwareSerial* serial) { m_serial = serial; }

    // === Protobuf Encoding Helpers ===
    static size_t encodeVarint(uint8_t* buf, uint32_t value);
    static uint32_t decodeVarint(const uint8_t* buf, size_t* bytesRead);
    static size_t encodeTag(uint8_t* buf, uint32_t fieldNum, uint8_t wireType);
    static size_t encodeFixed32(uint8_t* buf, uint32_t fieldNum, uint32_t value);
    static size_t encodeLenDelimited(uint8_t* buf, uint32_t fieldNum,
                                      const uint8_t* data, size_t len);

    // === Serial Framing ===

    /**
     * @brief Send a framed protobuf message to Meshtastic
     */
    bool sendFrame(const uint8_t* payload, size_t len);

    /**
     * @brief Read one framed protobuf message from Meshtastic
     * @param buf Buffer to store payload (must be MT_MAX_PAYLOAD bytes)
     * @param len Output: payload length
     * @param timeoutMs Timeout in milliseconds
     * @return true if a valid frame was received
     */
    bool readFrame(uint8_t* buf, size_t* len, uint32_t timeoutMs = 5000);

    // === Config Dump ===

    /**
     * @brief Send want_config_id request to trigger full config dump
     * @param nonce Non-zero nonce value (returned in config_complete_id)
     */
    bool requestConfigDump(uint32_t nonce = MT_NONCE_CONFIG_ONLY);

    /**
     * @brief Identify the type of a FromRadio message
     * @param buf Protobuf payload
     * @param len Payload length
     * @param subType Output: sub-type index for Config/ModuleConfig
     * @return FromRadioType
     */
    FromRadioType identifyFromRadio(const uint8_t* buf, size_t len, uint8_t* subType = nullptr);

    /**
     * @brief Extract my_node_num from a MyNodeInfo message
     * @param buf Raw MyNodeInfo protobuf (inner data after tag+length)
     * @param len Length
     * @return my_node_num or 0 on failure
     */
    uint32_t extractMyNodeNum(const uint8_t* buf, size_t len);

    /**
     * @brief Run full config dump and store results as JSON
     * @param doc Output JSON document
     * @param timeoutMs Total timeout for dump
     * @return true if config dump completed successfully
     */
    bool captureFullConfig(JsonDocument& doc, uint32_t timeoutMs = 30000);

    /**
     * @brief Extract channel data from captured config into JSON
     * @param channelBuf Raw channel protobuf
     * @param len Length
     * @param channelObj Output JSON object
     */
    void decodeChannelToJSON(const uint8_t* buf, size_t len, JsonObject& obj);

    // === Admin Message Construction (for restore) ===

    /**
     * @brief Build and send an admin message to set a config
     * @param myNodeNum Local node number (destination)
     * @param configData Raw Config protobuf bytes
     * @param configLen Length
     * @return true if sent successfully
     */
    bool sendSetConfig(uint32_t myNodeNum, const uint8_t* configData, size_t configLen);

    /**
     * @brief Build and send an admin message to set a channel
     */
    bool sendSetChannel(uint32_t myNodeNum, const uint8_t* channelData, size_t channelLen);

    /**
     * @brief Build and send an admin message to set a module config
     */
    bool sendSetModuleConfig(uint32_t myNodeNum, const uint8_t* moduleData, size_t moduleLen);

    uint32_t getMyNodeNum() const { return m_myNodeNum; }
    void setMyNodeNum(uint32_t num) { m_myNodeNum = num; }

    // === Admin GET Requests (for command console) ===

    /**
     * @brief Build a raw admin packet with arbitrary admin body bytes.
     * Unlike buildAdminPacket() which uses 2-byte tag + len-delimited,
     * this takes pre-formed admin body bytes directly (for GET requests
     * which use 1-byte tag + varint).
     */
    size_t buildAdminRawPacket(uint32_t myNodeNum,
                                const uint8_t* adminBody, size_t adminBodyLen,
                                uint8_t* outBuf);

    bool sendGetConfig(uint32_t myNodeNum, uint8_t configType);
    bool sendGetModuleConfig(uint32_t myNodeNum, uint8_t moduleType);
    bool sendGetChannel(uint32_t myNodeNum, uint8_t channelIndex);
    bool sendGetOwner(uint32_t myNodeNum);
    bool sendGetMetadata(uint32_t myNodeNum);

    // === Admin Actions ===

    bool sendReboot(uint32_t myNodeNum, int32_t seconds = 5);
    bool sendFactoryReset(uint32_t myNodeNum, int32_t resetVal = 1);
    bool sendNodedbReset(uint32_t myNodeNum);

    // === Multi-frame Response Reader ===

    /**
     * @brief Read multiple response frames and store as hex strings in JSON
     * @param doc Output JSON document with "frames" array
     * @param timeoutMs Total time to wait for frames
     * @param maxFrames Maximum number of frames to collect
     * @return Number of frames read
     */
    int readResponseFrames(JsonDocument& doc, uint32_t timeoutMs = 5000,
                           int maxFrames = 20);

private:
    HardwareSerial* m_serial;
    uint32_t m_myNodeNum;

    /**
     * @brief Build a complete admin packet (ToRadio > MeshPacket > Data > AdminMessage)
     * @param myNodeNum Destination node
     * @param adminTagH High byte of admin field tag
     * @param adminTagL Low byte of admin field tag
     * @param innerData Raw protobuf of the inner message
     * @param innerLen Length
     * @param outBuf Output buffer
     * @return Total encoded length
     */
    size_t buildAdminPacket(uint32_t myNodeNum,
                            uint8_t adminTagH, uint8_t adminTagL,
                            const uint8_t* innerData, size_t innerLen,
                            uint8_t* outBuf);

    /**
     * @brief Skip a protobuf field (advance position past its value)
     */
    static size_t skipField(const uint8_t* buf, size_t len, uint8_t wireType);

    /**
     * @brief Get the inner payload position after tag+length for a LEN-delimited field
     */
    static bool getLenDelimitedInner(const uint8_t* buf, size_t len,
                                      size_t* innerStart, size_t* innerLen);

    /**
     * @brief Identify config sub-type from inner Config protobuf
     */
    static uint8_t identifyConfigSubType(const uint8_t* buf, size_t len);

    /**
     * @brief Identify module config sub-type from inner ModuleConfig protobuf
     */
    static uint8_t identifyModuleSubType(const uint8_t* buf, size_t len);
};
