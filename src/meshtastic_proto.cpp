/**
 * @file meshtastic_proto.cpp
 * @brief Meshtastic protobuf serial protocol implementation
 *
 * Handles the Meshtastic serial framing (0x94 0xC3) and provides
 * minimal protobuf encoding/decoding for config backup and restore.
 */

#include "meshtastic_proto.h"

MeshtasticProtocol::MeshtasticProtocol()
    : m_serial(nullptr)
    , m_myNodeNum(0) {
}

MeshtasticProtocol::~MeshtasticProtocol() {
}

// === Protobuf Encoding Helpers ===

size_t MeshtasticProtocol::encodeVarint(uint8_t* buf, uint32_t value) {
    size_t i = 0;
    while (value > 0x7F) {
        buf[i++] = (value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[i++] = value & 0x7F;
    return i;
}

uint32_t MeshtasticProtocol::decodeVarint(const uint8_t* buf, size_t* bytesRead) {
    uint32_t result = 0;
    size_t shift = 0;
    size_t i = 0;
    do {
        result |= (uint32_t)(buf[i] & 0x7F) << shift;
        shift += 7;
        if (shift > 35) { // Overflow protection
            *bytesRead = i + 1;
            return result;
        }
    } while (buf[i++] & 0x80);
    *bytesRead = i;
    return result;
}

size_t MeshtasticProtocol::encodeTag(uint8_t* buf, uint32_t fieldNum, uint8_t wireType) {
    return encodeVarint(buf, (fieldNum << 3) | wireType);
}

size_t MeshtasticProtocol::encodeFixed32(uint8_t* buf, uint32_t fieldNum, uint32_t value) {
    size_t pos = 0;
    // Tag for fixed32: (fieldNum << 3) | 5
    pos += encodeVarint(buf + pos, (fieldNum << 3) | PB_WIRE_I32);
    buf[pos++] = value & 0xFF;
    buf[pos++] = (value >> 8) & 0xFF;
    buf[pos++] = (value >> 16) & 0xFF;
    buf[pos++] = (value >> 24) & 0xFF;
    return pos;
}

size_t MeshtasticProtocol::encodeLenDelimited(uint8_t* buf, uint32_t fieldNum,
                                                const uint8_t* data, size_t len) {
    size_t pos = 0;
    pos += encodeTag(buf + pos, fieldNum, PB_WIRE_LEN);
    pos += encodeVarint(buf + pos, len);
    memcpy(buf + pos, data, len);
    pos += len;
    return pos;
}

// === Serial Framing ===

bool MeshtasticProtocol::sendFrame(const uint8_t* payload, size_t len) {
    if (!m_serial || len > MT_MAX_PAYLOAD) return false;

    uint8_t header[MT_FRAME_HEADER];
    header[0] = MT_START1;
    header[1] = MT_START2;
    header[2] = (len >> 8) & 0xFF;  // MSB
    header[3] = len & 0xFF;          // LSB

    m_serial->write(header, MT_FRAME_HEADER);
    m_serial->write(payload, len);
    m_serial->flush();

    DEBUG_PRINTF("[MT] Sent frame: %d bytes [", len);
    for (size_t i = 0; i < min(len, (size_t)16); i++) {
        DEBUG_PRINTF("%02X ", payload[i]);
    }
    DEBUG_PRINTLN("]");
    return true;
}

bool MeshtasticProtocol::readFrame(uint8_t* buf, size_t* len, uint32_t timeoutMs) {
    if (!m_serial) {
        DEBUG_PRINTLN("[MT] readFrame: no serial port!");
        return false;
    }

    DEBUG_PRINTF("[MT] readFrame: waiting up to %dms, avail=%d\n",
                 timeoutMs, m_serial->available());

    uint32_t startTime = millis();
    int state = 0;    // 0=looking for START1, 1=looking for START2, 2=MSB, 3=LSB, 4=payload
    uint16_t payloadLen = 0;
    size_t received = 0;
    int totalBytesRead = 0;

    while (millis() - startTime < timeoutMs) {
        if (!m_serial->available()) {
            delay(1);
            continue;
        }

        uint8_t b = m_serial->read();
        totalBytesRead++;

        switch (state) {
            case 0:  // Looking for START1
                if (b == MT_START1) {
                    state = 1;
                }
                // Non-frame bytes are debug text - ignore during protobuf read
                break;

            case 1:  // Looking for START2
                if (b == MT_START2) {
                    state = 2;
                } else {
                    state = 0;  // Reset - false start
                }
                break;

            case 2:  // MSB of length
                payloadLen = (uint16_t)b << 8;
                state = 3;
                break;

            case 3:  // LSB of length
                payloadLen |= b;
                if (payloadLen > MT_MAX_PAYLOAD || payloadLen == 0) {
                    DEBUG_PRINTF("[MT] Invalid frame length: %d\n", payloadLen);
                    state = 0;  // Reset
                } else {
                    received = 0;
                    state = 4;
                }
                break;

            case 4:  // Receiving payload
                buf[received++] = b;
                if (received >= payloadLen) {
                    *len = payloadLen;
                    return true;  // Complete frame received
                }
                break;
        }
    }

    DEBUG_PRINTF("[MT] Frame read timeout (read %d bytes total, state=%d)\n", totalBytesRead, state);
    return false;
}

// === Config Dump ===

bool MeshtasticProtocol::requestConfigDump(uint32_t nonce) {
    // Build ToRadio { want_config_id = nonce }
    // Field 3, wire type 0 (VARINT): tag = 0x18
    uint8_t payload[8];
    size_t pos = 0;

    payload[pos++] = TAG_TORADIO_WANT_CONFIG;  // 0x18
    pos += encodeVarint(payload + pos, nonce);

    DEBUG_PRINTF("[MT] Requesting config dump (nonce=%u)\n", nonce);
    return sendFrame(payload, pos);
}

FromRadioType MeshtasticProtocol::identifyFromRadio(const uint8_t* buf, size_t len,
                                                      uint8_t* subType) {
    if (len < 2) return FR_UNKNOWN;

    size_t pos = 0;
    size_t vlen;

    // Skip optional 'id' field (tag 0x08)
    if (buf[pos] == TAG_FROMRADIO_ID) {
        pos++;
        decodeVarint(buf + pos, &vlen);
        pos += vlen;
    }

    if (pos >= len) return FR_UNKNOWN;

    // The next tag identifies the payload type
    uint8_t tag = buf[pos];

    switch (tag) {
        case TAG_FROMRADIO_MY_INFO:     return FR_MY_INFO;
        case TAG_FROMRADIO_NODE_INFO:   return FR_NODE_INFO;
        case TAG_FROMRADIO_CONFIG: {
            // Identify config sub-type
            if (subType) {
                pos++;
                uint32_t innerLen = decodeVarint(buf + pos, &vlen);
                pos += vlen;
                if (pos + innerLen <= len) {
                    *subType = identifyConfigSubType(buf + pos, innerLen);
                }
            }
            return FR_CONFIG;
        }
        case TAG_FROMRADIO_CONFIG_DONE: return FR_CONFIG_COMPLETE;
        case TAG_FROMRADIO_MODULE_CFG: {
            if (subType) {
                pos++;
                uint32_t innerLen = decodeVarint(buf + pos, &vlen);
                pos += vlen;
                if (pos + innerLen <= len) {
                    *subType = identifyModuleSubType(buf + pos, innerLen);
                }
            }
            return FR_MODULE_CONFIG;
        }
        case TAG_FROMRADIO_CHANNEL:     return FR_CHANNEL;
        case TAG_FROMRADIO_METADATA:    return FR_METADATA;
        case TAG_FROMRADIO_PACKET:      return FR_PACKET;
        case TAG_FROMRADIO_LOG:         return FR_LOG;
        case TAG_FROMRADIO_REBOOTED:    return FR_REBOOTED;
        default:                         return FR_UNKNOWN;
    }
}

uint32_t MeshtasticProtocol::extractMyNodeNum(const uint8_t* buf, size_t len) {
    // MyNodeInfo: field 1 (my_node_num) is a uint32 varint with tag 0x08
    size_t pos = 0;
    while (pos < len) {
        size_t vlen;
        uint32_t tag = decodeVarint(buf + pos, &vlen);
        pos += vlen;

        uint8_t fieldNum = tag >> 3;
        uint8_t wireType = tag & 0x07;

        if (fieldNum == 1 && wireType == PB_WIRE_VARINT) {
            return decodeVarint(buf + pos, &vlen);
        }

        // Skip this field
        pos += skipField(buf + pos, len - pos, wireType);
    }
    return 0;
}

bool MeshtasticProtocol::captureFullConfig(JsonDocument& doc, uint32_t timeoutMs) {
    if (!m_serial) return false;

    uint32_t nonce = (uint32_t)(millis() & 0xFFFF) | 0x10000;  // Random-ish non-zero nonce

    // Drain any pending serial data
    int drained = 0;
    while (m_serial->available()) { m_serial->read(); drained++; }
    DEBUG_PRINTF("[MT] Drained %d pending bytes from serial\n", drained);

    // Request config dump
    if (!requestConfigDump(nonce)) {
        DEBUG_PRINTLN("[MT] Failed to send config request");
        return false;
    }

    doc["version"] = 1;
    doc["timestamp"] = millis();
    doc["nonce"] = nonce;
    JsonArray configs = doc["config"].to<JsonArray>();
    JsonArray modules = doc["moduleConfig"].to<JsonArray>();
    JsonArray channels = doc["channels"].to<JsonArray>();

    uint8_t frameBuf[MT_MAX_PAYLOAD];
    size_t frameLen;
    uint32_t startTime = millis();
    bool complete = false;
    int frameCount = 0;

    while (!complete && (millis() - startTime < timeoutMs)) {
        if (!readFrame(frameBuf, &frameLen, 3000)) {
            // Timeout reading frame - could be end of dump or slow response
            if (frameCount > 0) {
                DEBUG_PRINTLN("[MT] No more frames received");
                break;
            }
            continue;
        }

        frameCount++;
        uint8_t subType = 0;
        FromRadioType type = identifyFromRadio(frameBuf, frameLen, &subType);

        switch (type) {
            case FR_MY_INFO: {
                DEBUG_PRINTLN("[MT] Received: MyNodeInfo");
                // Extract the inner MyNodeInfo data
                size_t pos = 0;
                size_t vlen;
                // Skip id field if present
                if (frameBuf[pos] == TAG_FROMRADIO_ID) {
                    pos++;
                    decodeVarint(frameBuf + pos, &vlen);
                    pos += vlen;
                }
                // Now at my_info tag
                if (frameBuf[pos] == TAG_FROMRADIO_MY_INFO) {
                    pos++;
                    uint32_t innerLen = decodeVarint(frameBuf + pos, &vlen);
                    pos += vlen;
                    m_myNodeNum = extractMyNodeNum(frameBuf + pos, innerLen);
                    doc["device"]["my_node_num"] = m_myNodeNum;
                    DEBUG_PRINTF("[MT] my_node_num = %u\n", m_myNodeNum);
                }
                break;
            }

            case FR_CONFIG: {
                const char* typeName = (subType < 10) ? CONFIG_TYPE_NAMES[subType] : "unknown";
                DEBUG_PRINTF("[MT] Received: Config (%s)\n", typeName);

                // Store the raw frame for this config
                JsonObject cfg = configs.add<JsonObject>();
                cfg["type"] = typeName;
                cfg["subType"] = subType;
                // Store raw protobuf as hex string for exact restore
                String hex;
                for (size_t i = 0; i < frameLen; i++) {
                    char h[3];
                    snprintf(h, sizeof(h), "%02X", frameBuf[i]);
                    hex += h;
                }
                cfg["raw"] = hex;
                break;
            }

            case FR_MODULE_CONFIG: {
                const char* typeName = (subType < 16) ? MODULE_TYPE_NAMES[subType] : "unknown";
                DEBUG_PRINTF("[MT] Received: ModuleConfig (%s)\n", typeName);

                JsonObject mod = modules.add<JsonObject>();
                mod["type"] = typeName;
                mod["subType"] = subType;
                String hex;
                for (size_t i = 0; i < frameLen; i++) {
                    char h[3];
                    snprintf(h, sizeof(h), "%02X", frameBuf[i]);
                    hex += h;
                }
                mod["raw"] = hex;
                break;
            }

            case FR_CHANNEL: {
                DEBUG_PRINTLN("[MT] Received: Channel");

                JsonObject ch = channels.add<JsonObject>();
                // Decode channel details into JSON
                size_t pos = 0;
                size_t vlen;
                // Skip id field
                if (frameBuf[pos] == TAG_FROMRADIO_ID) {
                    pos++;
                    decodeVarint(frameBuf + pos, &vlen);
                    pos += vlen;
                }
                // Get channel inner data
                if (pos < frameLen && frameBuf[pos] == TAG_FROMRADIO_CHANNEL) {
                    pos++;
                    uint32_t innerLen = decodeVarint(frameBuf + pos, &vlen);
                    pos += vlen;
                    decodeChannelToJSON(frameBuf + pos, innerLen, ch);
                }
                // Also store raw for restore
                String hex;
                for (size_t i = 0; i < frameLen; i++) {
                    char h[3];
                    snprintf(h, sizeof(h), "%02X", frameBuf[i]);
                    hex += h;
                }
                ch["raw"] = hex;
                break;
            }

            case FR_METADATA: {
                DEBUG_PRINTLN("[MT] Received: Metadata");
                // Could parse firmware version etc. - skip for now
                break;
            }

            case FR_CONFIG_COMPLETE: {
                // Extract the nonce to verify
                size_t pos = 0;
                size_t vlen;
                if (frameBuf[pos] == TAG_FROMRADIO_ID) {
                    pos++;
                    decodeVarint(frameBuf + pos, &vlen);
                    pos += vlen;
                }
                if (pos < frameLen && frameBuf[pos] == TAG_FROMRADIO_CONFIG_DONE) {
                    pos++;
                    uint32_t doneNonce = decodeVarint(frameBuf + pos, &vlen);
                    DEBUG_PRINTF("[MT] Config complete (nonce=%u, expected=%u)\n", doneNonce, nonce);
                    if (doneNonce == nonce) {
                        complete = true;
                    }
                }
                break;
            }

            case FR_NODE_INFO:
                DEBUG_PRINTLN("[MT] Received: NodeInfo (skipping)");
                break;

            case FR_LOG:
            case FR_PACKET:
            case FR_REBOOTED:
            case FR_UNKNOWN:
            default:
                break;
        }
    }

    DEBUG_PRINTF("[MT] Config dump: %d frames, complete=%s\n",
                 frameCount, complete ? "yes" : "no");

    doc["complete"] = complete;
    doc["frameCount"] = frameCount;

    return frameCount > 0;  // Return true even if not "complete" but we got data
}

void MeshtasticProtocol::decodeChannelToJSON(const uint8_t* buf, size_t len, JsonObject& obj) {
    // Channel message:
    //   index (field 1, varint) - channel index 0-7
    //   settings (field 2, LEN) - ChannelSettings
    //   role (field 3, varint) - 0=DISABLED, 1=PRIMARY, 2=SECONDARY

    size_t pos = 0;
    while (pos < len) {
        size_t vlen;
        uint32_t tag = decodeVarint(buf + pos, &vlen);
        pos += vlen;
        uint8_t fieldNum = tag >> 3;
        uint8_t wireType = tag & 0x07;

        if (fieldNum == 1 && wireType == PB_WIRE_VARINT) {
            // Channel index
            uint32_t index = decodeVarint(buf + pos, &vlen);
            pos += vlen;
            obj["index"] = index;
        } else if (fieldNum == 2 && wireType == PB_WIRE_LEN) {
            // ChannelSettings
            uint32_t settingsLen = decodeVarint(buf + pos, &vlen);
            pos += vlen;
            const uint8_t* settingsBuf = buf + pos;

            // Parse ChannelSettings fields
            size_t spos = 0;
            while (spos < settingsLen) {
                uint32_t stag = decodeVarint(settingsBuf + spos, &vlen);
                spos += vlen;
                uint8_t sfn = stag >> 3;
                uint8_t swt = stag & 0x07;

                if (sfn == 2 && swt == PB_WIRE_LEN) {
                    // psk (bytes)
                    uint32_t pskLen = decodeVarint(settingsBuf + spos, &vlen);
                    spos += vlen;
                    obj["psk_len"] = pskLen;
                    if (pskLen == 1) {
                        obj["psk_type"] = (settingsBuf[spos] == 0x01) ? "default" : "indexed";
                    } else if (pskLen == 16) {
                        obj["psk_type"] = "aes128";
                    } else if (pskLen == 32) {
                        obj["psk_type"] = "aes256";
                    } else if (pskLen == 0) {
                        obj["psk_type"] = "none";
                    }
                    // Store PSK as hex
                    String pskHex;
                    for (uint32_t i = 0; i < pskLen; i++) {
                        char h[3];
                        snprintf(h, sizeof(h), "%02X", settingsBuf[spos + i]);
                        pskHex += h;
                    }
                    obj["psk"] = pskHex;
                    spos += pskLen;
                } else if (sfn == 3 && swt == PB_WIRE_LEN) {
                    // name (string)
                    uint32_t nameLen = decodeVarint(settingsBuf + spos, &vlen);
                    spos += vlen;
                    String name;
                    for (uint32_t i = 0; i < nameLen; i++) {
                        name += (char)settingsBuf[spos + i];
                    }
                    obj["name"] = name;
                    spos += nameLen;
                } else if (sfn == 5 && swt == PB_WIRE_VARINT) {
                    // uplink_enabled
                    uint32_t val = decodeVarint(settingsBuf + spos, &vlen);
                    spos += vlen;
                    obj["uplink"] = (bool)val;
                } else if (sfn == 6 && swt == PB_WIRE_VARINT) {
                    // downlink_enabled
                    uint32_t val = decodeVarint(settingsBuf + spos, &vlen);
                    spos += vlen;
                    obj["downlink"] = (bool)val;
                } else {
                    spos += skipField(settingsBuf + spos, settingsLen - spos, swt);
                }
            }
            pos += settingsLen;
        } else if (fieldNum == 3 && wireType == PB_WIRE_VARINT) {
            // Role
            uint32_t role = decodeVarint(buf + pos, &vlen);
            pos += vlen;
            const char* roleNames[] = {"DISABLED", "PRIMARY", "SECONDARY"};
            obj["role"] = (role < 3) ? roleNames[role] : "UNKNOWN";
        } else {
            pos += skipField(buf + pos, len - pos, wireType);
        }
    }
}

// === Admin Message Construction ===

size_t MeshtasticProtocol::buildAdminPacket(uint32_t myNodeNum,
                                             uint8_t adminTagH, uint8_t adminTagL,
                                             const uint8_t* innerData, size_t innerLen,
                                             uint8_t* outBuf) {
    // Build from inside out:
    // AdminMessage { set_xxx = innerData }
    uint8_t adminBuf[MT_MAX_PAYLOAD];
    size_t adminPos = 0;
    adminBuf[adminPos++] = adminTagH;
    adminBuf[adminPos++] = adminTagL;
    adminPos += encodeVarint(adminBuf + adminPos, innerLen);
    memcpy(adminBuf + adminPos, innerData, innerLen);
    adminPos += innerLen;

    // Data { portnum=6, payload=adminBuf, want_response=true }
    uint8_t dataBuf[MT_MAX_PAYLOAD];
    size_t dataPos = 0;
    dataBuf[dataPos++] = TAG_DATA_PORTNUM;  // field 1, varint
    dataPos += encodeVarint(dataBuf + dataPos, PORTNUM_ADMIN);
    dataPos += encodeLenDelimited(dataBuf + dataPos, 2, adminBuf, adminPos);  // field 2, payload
    dataBuf[dataPos++] = TAG_DATA_WANT_RESPONSE;  // field 3
    dataBuf[dataPos++] = 0x01;  // true

    // MeshPacket { to=myNodeNum, decoded=dataBuf, id=random, want_ack=true }
    uint8_t meshBuf[MT_MAX_PAYLOAD];
    size_t meshPos = 0;
    meshPos += encodeFixed32(meshBuf + meshPos, 2, myNodeNum);     // to (field 2, fixed32)
    meshPos += encodeLenDelimited(meshBuf + meshPos, 4, dataBuf, dataPos); // decoded (field 4)
    meshPos += encodeFixed32(meshBuf + meshPos, 6, millis());      // id (field 6, fixed32)
    meshBuf[meshPos++] = TAG_MESHPACKET_WANT_ACK;  // field 10, varint
    meshBuf[meshPos++] = 0x01;  // true

    // ToRadio { packet = meshBuf }
    size_t pos = 0;
    pos += encodeLenDelimited(outBuf + pos, 1, meshBuf, meshPos);  // field 1

    return pos;
}

bool MeshtasticProtocol::sendSetConfig(uint32_t myNodeNum, const uint8_t* configData, size_t configLen) {
    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminPacket(myNodeNum,
                                   TAG_ADMIN_SET_CONFIG_H, TAG_ADMIN_SET_CONFIG_L,
                                   configData, configLen, packet);
    return sendFrame(packet, len);
}

bool MeshtasticProtocol::sendSetChannel(uint32_t myNodeNum, const uint8_t* channelData, size_t channelLen) {
    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminPacket(myNodeNum,
                                   TAG_ADMIN_SET_CHANNEL_H, TAG_ADMIN_SET_CHANNEL_L,
                                   channelData, channelLen, packet);
    return sendFrame(packet, len);
}

bool MeshtasticProtocol::sendSetModuleConfig(uint32_t myNodeNum, const uint8_t* moduleData, size_t moduleLen) {
    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminPacket(myNodeNum,
                                   TAG_ADMIN_SET_MODULE_H, TAG_ADMIN_SET_MODULE_L,
                                   moduleData, moduleLen, packet);
    return sendFrame(packet, len);
}

// === Admin GET/Action Methods (for command console) ===

size_t MeshtasticProtocol::buildAdminRawPacket(uint32_t myNodeNum,
                                                 const uint8_t* adminBody, size_t adminBodyLen,
                                                 uint8_t* outBuf) {
    // Data { portnum=6, payload=adminBody, want_response=true }
    uint8_t dataBuf[MT_MAX_PAYLOAD];
    size_t dataPos = 0;
    dataBuf[dataPos++] = TAG_DATA_PORTNUM;  // field 1, varint
    dataPos += encodeVarint(dataBuf + dataPos, PORTNUM_ADMIN);
    dataPos += encodeLenDelimited(dataBuf + dataPos, 2, adminBody, adminBodyLen);  // field 2
    dataBuf[dataPos++] = TAG_DATA_WANT_RESPONSE;  // field 3
    dataBuf[dataPos++] = 0x01;  // true

    // MeshPacket { to=myNodeNum, decoded=dataBuf, id=random, want_ack=true }
    uint8_t meshBuf[MT_MAX_PAYLOAD];
    size_t meshPos = 0;
    meshPos += encodeFixed32(meshBuf + meshPos, 2, myNodeNum);     // to (field 2, fixed32)
    meshPos += encodeLenDelimited(meshBuf + meshPos, 4, dataBuf, dataPos); // decoded (field 4)
    meshPos += encodeFixed32(meshBuf + meshPos, 6, millis());      // id (field 6, fixed32)
    meshBuf[meshPos++] = TAG_MESHPACKET_WANT_ACK;  // field 10, varint
    meshBuf[meshPos++] = 0x01;  // true

    // ToRadio { packet = meshBuf }
    size_t pos = 0;
    pos += encodeLenDelimited(outBuf + pos, 1, meshBuf, meshPos);  // field 1

    return pos;
}

bool MeshtasticProtocol::sendGetConfig(uint32_t myNodeNum, uint8_t configType) {
    uint8_t adminBody[4];
    size_t pos = 0;
    adminBody[pos++] = TAG_ADMIN_GET_CONFIG_REQ;  // 0x28 = field 5, varint
    pos += encodeVarint(adminBody + pos, configType);

    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminRawPacket(myNodeNum, adminBody, pos, packet);
    DEBUG_PRINTF("[MT] sendGetConfig type=%d (%s), packet=%d bytes\n",
                 configType, configType < 10 ? CONFIG_TYPE_NAMES[configType] : "?", (int)len);
    return sendFrame(packet, len);
}

bool MeshtasticProtocol::sendGetModuleConfig(uint32_t myNodeNum, uint8_t moduleType) {
    uint8_t adminBody[4];
    size_t pos = 0;
    adminBody[pos++] = TAG_ADMIN_GET_MODULE_REQ;  // 0x38 = field 7, varint
    pos += encodeVarint(adminBody + pos, moduleType);

    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminRawPacket(myNodeNum, adminBody, pos, packet);
    DEBUG_PRINTF("[MT] sendGetModuleConfig type=%d (%s), packet=%d bytes\n",
                 moduleType, moduleType < 16 ? MODULE_TYPE_NAMES[moduleType] : "?", (int)len);
    return sendFrame(packet, len);
}

bool MeshtasticProtocol::sendGetChannel(uint32_t myNodeNum, uint8_t channelIndex) {
    uint8_t adminBody[4];
    size_t pos = 0;
    adminBody[pos++] = TAG_ADMIN_GET_CHANNEL_REQ;  // 0x08 = field 1, varint
    pos += encodeVarint(adminBody + pos, channelIndex);

    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminRawPacket(myNodeNum, adminBody, pos, packet);
    DEBUG_PRINTF("[MT] sendGetChannel index=%d, packet=%d bytes\n", channelIndex, (int)len);
    return sendFrame(packet, len);
}

bool MeshtasticProtocol::sendGetOwner(uint32_t myNodeNum) {
    uint8_t adminBody[2] = { TAG_ADMIN_GET_OWNER_REQ, 0x01 };  // field 3, value=true

    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminRawPacket(myNodeNum, adminBody, 2, packet);
    DEBUG_PRINTF("[MT] sendGetOwner, packet=%d bytes\n", (int)len);
    return sendFrame(packet, len);
}

bool MeshtasticProtocol::sendGetMetadata(uint32_t myNodeNum) {
    uint8_t adminBody[2] = { TAG_ADMIN_GET_METADATA_REQ, 0x01 };  // field 12, value=true

    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminRawPacket(myNodeNum, adminBody, 2, packet);
    DEBUG_PRINTF("[MT] sendGetMetadata, packet=%d bytes\n", (int)len);
    return sendFrame(packet, len);
}

bool MeshtasticProtocol::sendReboot(uint32_t myNodeNum, int32_t seconds) {
    uint8_t adminBody[8];
    size_t pos = 0;
    adminBody[pos++] = TAG_ADMIN_REBOOT_H;   // 0x88 = field 97 low byte
    adminBody[pos++] = TAG_ADMIN_REBOOT_L;   // 0x06 = field 97 high byte
    pos += encodeVarint(adminBody + pos, (uint32_t)seconds);

    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminRawPacket(myNodeNum, adminBody, pos, packet);
    DEBUG_PRINTF("[MT] sendReboot seconds=%d, packet=%d bytes\n", seconds, (int)len);
    return sendFrame(packet, len);
}

bool MeshtasticProtocol::sendFactoryReset(uint32_t myNodeNum, int32_t resetVal) {
    uint8_t adminBody[8];
    size_t pos = 0;
    adminBody[pos++] = TAG_ADMIN_FACTORY_RESET_H;  // 0xF0 = field 94 low byte
    adminBody[pos++] = TAG_ADMIN_FACTORY_RESET_L;  // 0x05 = field 94 high byte
    pos += encodeVarint(adminBody + pos, (uint32_t)resetVal);

    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminRawPacket(myNodeNum, adminBody, pos, packet);
    DEBUG_PRINTF("[MT] sendFactoryReset, packet=%d bytes\n", (int)len);
    return sendFrame(packet, len);
}

bool MeshtasticProtocol::sendNodedbReset(uint32_t myNodeNum) {
    uint8_t adminBody[4];
    size_t pos = 0;
    adminBody[pos++] = TAG_ADMIN_NODEDB_RESET_H;  // 0xA0 = field 100 low byte
    adminBody[pos++] = TAG_ADMIN_NODEDB_RESET_L;  // 0x06 = field 100 high byte
    pos += encodeVarint(adminBody + pos, 1);  // true

    uint8_t packet[MT_MAX_PAYLOAD];
    size_t len = buildAdminRawPacket(myNodeNum, adminBody, pos, packet);
    DEBUG_PRINTF("[MT] sendNodedbReset, packet=%d bytes\n", (int)len);
    return sendFrame(packet, len);
}

int MeshtasticProtocol::readResponseFrames(JsonDocument& doc, uint32_t timeoutMs, int maxFrames) {
    JsonArray frames = doc["frames"].to<JsonArray>();
    uint8_t frameBuf[MT_MAX_PAYLOAD];
    size_t frameLen;
    int count = 0;
    uint32_t startTime = millis();

    while (count < maxFrames && (millis() - startTime < timeoutMs)) {
        uint32_t remaining = timeoutMs - (millis() - startTime);
        uint32_t frameTimeout = (remaining < 2000) ? remaining : 2000;

        if (!readFrame(frameBuf, &frameLen, frameTimeout)) {
            if (count > 0) break;  // Got frames already, no more coming
            continue;
        }

        uint8_t subType = 0;
        FromRadioType type = identifyFromRadio(frameBuf, frameLen, &subType);

        JsonObject f = frames.add<JsonObject>();

        // Store type name
        switch (type) {
            case FR_MY_INFO:         f["type"] = "my_info"; break;
            case FR_NODE_INFO:       f["type"] = "node_info"; break;
            case FR_CONFIG:
                f["type"] = "config";
                f["subType"] = (subType < 10) ? CONFIG_TYPE_NAMES[subType] : "unknown";
                f["subTypeId"] = subType;
                break;
            case FR_MODULE_CONFIG:
                f["type"] = "module_config";
                f["subType"] = (subType < 16) ? MODULE_TYPE_NAMES[subType] : "unknown";
                f["subTypeId"] = subType;
                break;
            case FR_CHANNEL:         f["type"] = "channel"; break;
            case FR_CONFIG_COMPLETE: f["type"] = "config_complete"; break;
            case FR_METADATA:        f["type"] = "metadata"; break;
            case FR_PACKET:          f["type"] = "packet"; break;
            case FR_LOG:             f["type"] = "log"; break;
            case FR_REBOOTED:        f["type"] = "rebooted"; break;
            default:                 f["type"] = "unknown"; break;
        }

        // Store raw hex for JS-side decoding
        String hex;
        hex.reserve(frameLen * 2);
        for (size_t i = 0; i < frameLen; i++) {
            char h[3];
            snprintf(h, sizeof(h), "%02X", frameBuf[i]);
            hex += h;
        }
        f["hex"] = hex;
        f["len"] = (int)frameLen;

        // Extract myNodeNum if this is MY_INFO
        if (type == FR_MY_INFO) {
            size_t pos = 0;
            size_t vlen;
            // Skip optional id field
            if (pos < frameLen && frameBuf[pos] == TAG_FROMRADIO_ID) {
                pos++;
                decodeVarint(frameBuf + pos, &vlen);
                pos += vlen;
            }
            // Read my_info LEN-delimited field
            if (pos < frameLen && frameBuf[pos] == TAG_FROMRADIO_MY_INFO) {
                pos++;
                uint32_t innerLen = decodeVarint(frameBuf + pos, &vlen);
                pos += vlen;
                uint32_t nodeNum = extractMyNodeNum(frameBuf + pos, innerLen);
                if (nodeNum != 0) {
                    m_myNodeNum = nodeNum;
                    doc["myNodeNum"] = m_myNodeNum;
                    DEBUG_PRINTF("[MT] readResponseFrames: discovered myNodeNum=0x%08X\n", m_myNodeNum);
                }
            }
        }

        count++;
        DEBUG_PRINTF("[MT] readResponseFrames: frame %d type=%d len=%d\n", count, (int)type, (int)frameLen);
    }

    doc["frameCount"] = count;
    return count;
}

// === Helper Functions ===

size_t MeshtasticProtocol::skipField(const uint8_t* buf, size_t len, uint8_t wireType) {
    size_t vlen;
    switch (wireType) {
        case PB_WIRE_VARINT:
            decodeVarint(buf, &vlen);
            return vlen;
        case PB_WIRE_I64:
            return 8;
        case PB_WIRE_LEN: {
            uint32_t fieldLen = decodeVarint(buf, &vlen);
            return vlen + fieldLen;
        }
        case PB_WIRE_I32:
            return 4;
        default:
            return len;  // Unknown - skip to end
    }
}

bool MeshtasticProtocol::getLenDelimitedInner(const uint8_t* buf, size_t len,
                                                size_t* innerStart, size_t* innerLen) {
    if (len < 2) return false;
    size_t vlen;
    *innerLen = decodeVarint(buf, &vlen);
    *innerStart = vlen;
    return (*innerStart + *innerLen <= len);
}

uint8_t MeshtasticProtocol::identifyConfigSubType(const uint8_t* buf, size_t len) {
    // The first tag in the Config message tells us which config variant it is
    // device=1(0x0A), position=2(0x12), power=3(0x1A), network=4(0x22),
    // display=5(0x2A), lora=6(0x32), bluetooth=7(0x3A), security=8(0x42),
    // sessionkey=9(0x4A), deviceui=10(0x52)
    if (len < 1) return 0xFF;
    uint8_t tag = buf[0];
    uint8_t fieldNum = tag >> 3;
    if (fieldNum >= 1 && fieldNum <= 10) {
        return fieldNum - 1;  // 0-indexed
    }
    return 0xFF;
}

uint8_t MeshtasticProtocol::identifyModuleSubType(const uint8_t* buf, size_t len) {
    if (len < 1) return 0xFF;

    // For field numbers 1-15, tag fits in one byte
    // For field 16 (tak), tag is 2 bytes: 0x82 0x01
    size_t vlen;
    uint32_t tag = decodeVarint(buf, &vlen);
    uint8_t fieldNum = tag >> 3;
    if (fieldNum >= 1 && fieldNum <= 16) {
        return fieldNum - 1;  // 0-indexed
    }
    return 0xFF;
}
