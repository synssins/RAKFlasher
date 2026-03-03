/**
 * Meshtastic Protobuf Decoder for RAKFlasher Command Console
 * Decodes raw FromRadio hex frames into human-readable output.
 * Field definitions verified against meshtastic/protobufs (2026-03).
 */

const MeshDecoder = {

    // ========== Low-level protobuf helpers ==========

    hexToBytes(hex) {
        const bytes = new Uint8Array(hex.length / 2);
        for (let i = 0; i < hex.length; i += 2) {
            bytes[i / 2] = parseInt(hex.substr(i, 2), 16);
        }
        return bytes;
    },

    bytesToHex(bytes, start, len) {
        let hex = '';
        for (let i = start; i < start + len && i < bytes.length; i++) {
            hex += bytes[i].toString(16).padStart(2, '0').toUpperCase();
        }
        return hex;
    },

    readVarint(bytes, offset) {
        let value = 0;
        let shift = 0;
        let bytesRead = 0;
        while (offset + bytesRead < bytes.length) {
            const b = bytes[offset + bytesRead];
            value |= (b & 0x7F) << shift;
            bytesRead++;
            if ((b & 0x80) === 0) break;
            shift += 7;
            if (shift > 35) break; // overflow protection
        }
        return { value: value >>> 0, bytesRead };
    },

    readTag(bytes, offset) {
        const { value, bytesRead } = this.readVarint(bytes, offset);
        return {
            fieldNum: value >>> 3,
            wireType: value & 0x07,
            bytesRead
        };
    },

    readField(bytes, offset, wireType) {
        switch (wireType) {
            case 0: { // VARINT
                return this.readVarint(bytes, offset);
            }
            case 1: { // I64 (fixed 64-bit)
                return { value: bytes.slice(offset, offset + 8), bytesRead: 8 };
            }
            case 2: { // LEN (length-delimited)
                const { value: len, bytesRead: lenBytes } = this.readVarint(bytes, offset);
                return {
                    value: bytes.slice(offset + lenBytes, offset + lenBytes + len),
                    bytesRead: lenBytes + len
                };
            }
            case 5: { // I32 (fixed 32-bit)
                const val = bytes[offset] | (bytes[offset+1] << 8) |
                           (bytes[offset+2] << 16) | (bytes[offset+3] << 24);
                return { value: val >>> 0, bytesRead: 4 };
            }
            default:
                return { value: null, bytesRead: 0 };
        }
    },

    /** Decode a protobuf message into an array of {fieldNum, wireType, value} */
    decodeFields(bytes, offset, length) {
        const fields = [];
        const end = offset + length;
        let pos = offset;
        while (pos < end) {
            const tag = this.readTag(bytes, pos);
            pos += tag.bytesRead;
            if (pos >= end) break;
            const field = this.readField(bytes, pos, tag.wireType);
            pos += field.bytesRead;
            fields.push({
                fieldNum: tag.fieldNum,
                wireType: tag.wireType,
                value: field.value
            });
        }
        return fields;
    },

    /** Decode bytes to UTF-8 string */
    bytesToString(bytes) {
        if (bytes instanceof Uint8Array) {
            return new TextDecoder().decode(bytes);
        }
        return String(bytes);
    },

    /** Decode fixed32 to float (IEEE 754) */
    fixed32ToFloat(bytes, offset) {
        const buf = new ArrayBuffer(4);
        const view = new DataView(buf);
        for (let i = 0; i < 4; i++) view.setUint8(i, bytes[offset + i]);
        return view.getFloat32(0, true); // little-endian
    },

    // ========== FromRadio Frame Tags ==========

    FROMRADIO_TAGS: {
        0x08: 'id',          // field 1, VARINT
        0x12: 'packet',      // field 2, LEN
        0x1A: 'my_info',     // field 3, LEN
        0x22: 'node_info',   // field 4, LEN
        0x2A: 'config',      // field 5, LEN
        0x32: 'log',         // field 6, LEN
        0x38: 'config_done', // field 7, VARINT
        0x40: 'rebooted',    // field 8, VARINT
        0x4A: 'module_config', // field 9, LEN
        0x52: 'channel',     // field 10, LEN
        0x5A: 'queue_status', // field 11, LEN
        0x6A: 'metadata',    // field 13, LEN
    },

    // ========== Config oneof field mapping ==========
    // Config message: oneof payload_variant { device=1, position=2, power=3, ... }
    CONFIG_ONEOF: {
        1: 'device', 2: 'position', 3: 'power', 4: 'network', 5: 'display',
        6: 'lora', 7: 'bluetooth', 8: 'security', 9: 'sessionkey', 10: 'device_ui'
    },

    // ModuleConfig message: oneof payload_variant
    MODULE_ONEOF: {
        1: 'mqtt', 2: 'serial', 3: 'external_notification', 4: 'store_forward',
        5: 'range_test', 6: 'telemetry', 7: 'canned_message', 8: 'audio',
        9: 'remote_hardware', 10: 'neighbor_info', 11: 'ambient_lighting',
        12: 'detection_sensor', 13: 'paxcounter', 14: 'statusmessage',
        15: 'traffic_management', 16: 'tak'
    },

    // ========== Enum Definitions ==========

    ENUMS: {
        'DeviceRole': {
            0:'CLIENT', 1:'CLIENT_MUTE', 2:'ROUTER', 3:'ROUTER_CLIENT',
            4:'REPEATER', 5:'TRACKER', 6:'SENSOR', 7:'TAK',
            8:'CLIENT_HIDDEN', 9:'LOST_AND_FOUND', 10:'TAK_TRACKER',
            11:'ROUTER_LATE', 12:'CLIENT_BASE'
        },
        'RebroadcastMode': {
            0:'ALL', 1:'ALL_SKIP_DECODING', 2:'LOCAL_ONLY', 3:'KNOWN_ONLY', 4:'NONE'
        },
        'RegionCode': {
            0:'UNSET', 1:'US', 2:'EU_433', 3:'EU_868', 4:'CN', 5:'JP',
            6:'ANZ', 7:'KR', 8:'TW', 9:'RU', 10:'IN', 11:'NZ_865',
            12:'TH', 13:'LORA_24', 14:'UA_433', 15:'UA_868', 16:'MY_433',
            17:'MY_919', 18:'SG_923', 19:'PH_433', 20:'PH_868', 21:'PH_915',
            22:'ANZ_433', 23:'KZ_433', 24:'KZ_863', 25:'NP_865', 26:'BR_902'
        },
        'ModemPreset': {
            0:'LONG_FAST', 1:'LONG_SLOW', 2:'VERY_LONG_SLOW', 3:'MEDIUM_SLOW',
            4:'MEDIUM_FAST', 5:'SHORT_SLOW', 6:'SHORT_FAST', 7:'LONG_MODERATE',
            8:'SHORT_TURBO', 9:'LONG_TURBO'
        },
        'HardwareModel': {
            0:'UNSET', 1:'TLORA_V2', 2:'TLORA_V1', 4:'TBEAM', 5:'HELTEC_V2_0',
            7:'T_ECHO', 9:'RAK4631', 10:'HELTEC_V2_1', 11:'HELTEC_V1',
            12:'LILYGO_TBEAM_S3_CORE', 13:'RAK11200', 14:'NANO_G1',
            15:'TLORA_V2_1_1P8', 16:'TLORA_T3_S3', 17:'NANO_G1_EXPLORER',
            18:'NANO_G2_ULTRA', 21:'WIO_WM1110', 22:'RAK2560',
            25:'STATION_G1', 26:'RAK11310', 29:'CANARYONE', 30:'RP2040_LORA',
            39:'HELTEC_V3', 43:'HELTEC_WSL_V3', 47:'BETAFPV_2400_TX',
            255:'PRIVATE_HW'
        },
        'ChannelRole': { 0:'DISABLED', 1:'PRIMARY', 2:'SECONDARY' },
        'GpsMode': { 0:'DISABLED', 1:'ENABLED', 2:'NOT_PRESENT' },
        'DisplayUnits': { 0:'METRIC', 1:'IMPERIAL' },
        'OledType': { 0:'OLED_AUTO', 1:'OLED_SSD1306', 2:'OLED_SH1106', 3:'OLED_SH1107' },
        'DisplayMode': { 0:'DEFAULT', 1:'TWOCOLOR', 2:'INVERTED', 3:'COLOR' },
        'PairingMode': { 0:'RANDOM_PIN', 1:'FIXED_PIN', 2:'NO_PIN' },
        'SerialBaud': {
            0:'BAUD_DEFAULT', 1:'BAUD_110', 2:'BAUD_300', 3:'BAUD_600',
            4:'BAUD_1200', 5:'BAUD_2400', 6:'BAUD_4800', 7:'BAUD_9600',
            8:'BAUD_19200', 9:'BAUD_38400', 10:'BAUD_57600', 11:'BAUD_115200',
            12:'BAUD_230400', 13:'BAUD_460800', 14:'BAUD_576000', 15:'BAUD_921600'
        },
        'SerialMode': {
            0:'DEFAULT', 1:'SIMPLE', 2:'PROTO', 3:'TEXTMSG', 4:'NMEA',
            5:'CALTOPO', 6:'GARMIN'
        },
        'AddressMode': { 0:'DHCP', 1:'STATIC' },
    },

    enumName(enumType, value) {
        const map = this.ENUMS[enumType];
        if (map && map[value] !== undefined) return map[value] + ' (' + value + ')';
        return String(value);
    },

    // ========== Field Definitions ==========
    // Verified against meshtastic/protobufs master (2026-03)

    CONFIG_FIELDS: {
        device: {
            1: { name: 'role', type: 'enum', enumType: 'DeviceRole' },
            2: { name: 'serial_enabled', type: 'bool', deprecated: true },
            4: { name: 'button_gpio', type: 'uint32' },
            5: { name: 'buzzer_gpio', type: 'uint32' },
            6: { name: 'rebroadcast_mode', type: 'enum', enumType: 'RebroadcastMode' },
            7: { name: 'node_info_broadcast_secs', type: 'uint32' },
            8: { name: 'double_tap_as_button_press', type: 'bool' },
            9: { name: 'is_managed', type: 'bool', deprecated: true },
            10: { name: 'disable_triple_click', type: 'bool' },
            11: { name: 'tzdef', type: 'string' },
            12: { name: 'led_heartbeat_disabled', type: 'bool' },
        },
        position: {
            1: { name: 'position_broadcast_secs', type: 'uint32' },
            2: { name: 'position_broadcast_smart_enabled', type: 'bool' },
            3: { name: 'fixed_position', type: 'bool' },
            5: { name: 'gps_update_interval', type: 'uint32' },
            7: { name: 'position_flags', type: 'uint32' },
            8: { name: 'rx_gpio', type: 'uint32' },
            9: { name: 'tx_gpio', type: 'uint32' },
            10: { name: 'broadcast_smart_minimum_distance', type: 'uint32' },
            11: { name: 'broadcast_smart_minimum_interval_secs', type: 'uint32' },
            12: { name: 'gps_en_gpio', type: 'uint32' },
            13: { name: 'gps_mode', type: 'enum', enumType: 'GpsMode' },
        },
        power: {
            1: { name: 'is_power_saving', type: 'bool' },
            2: { name: 'on_battery_shutdown_after_secs', type: 'uint32' },
            3: { name: 'adc_multiplier_override', type: 'float' },
            4: { name: 'wait_bluetooth_secs', type: 'uint32' },
            6: { name: 'sds_secs', type: 'uint32' },
            7: { name: 'ls_secs', type: 'uint32' },
            8: { name: 'min_wake_secs', type: 'uint32' },
            9: { name: 'device_battery_ina_address', type: 'uint32' },
        },
        network: {
            1: { name: 'wifi_enabled', type: 'bool' },
            3: { name: 'wifi_ssid', type: 'string' },
            4: { name: 'wifi_psk', type: 'string' },
            5: { name: 'ntp_server', type: 'string' },
            6: { name: 'eth_enabled', type: 'bool' },
            7: { name: 'address_mode', type: 'enum', enumType: 'AddressMode' },
            9: { name: 'rsyslog_server', type: 'string' },
        },
        display: {
            1: { name: 'screen_on_secs', type: 'uint32' },
            3: { name: 'auto_screen_carousel_secs', type: 'uint32' },
            5: { name: 'flip_screen', type: 'bool' },
            6: { name: 'units', type: 'enum', enumType: 'DisplayUnits' },
            7: { name: 'oled', type: 'enum', enumType: 'OledType' },
            8: { name: 'displaymode', type: 'enum', enumType: 'DisplayMode' },
            9: { name: 'heading_bold', type: 'bool' },
            10: { name: 'wake_on_tap_or_motion', type: 'bool' },
            12: { name: 'use_12h_clock', type: 'bool' },
        },
        lora: {
            1: { name: 'use_preset', type: 'bool' },
            2: { name: 'modem_preset', type: 'enum', enumType: 'ModemPreset' },
            3: { name: 'bandwidth', type: 'uint32' },
            4: { name: 'spread_factor', type: 'uint32' },
            5: { name: 'coding_rate', type: 'uint32' },
            6: { name: 'frequency_offset', type: 'float' },
            7: { name: 'region', type: 'enum', enumType: 'RegionCode' },
            8: { name: 'hop_limit', type: 'uint32' },
            9: { name: 'tx_enabled', type: 'bool' },
            10: { name: 'tx_power', type: 'int32' },
            11: { name: 'channel_num', type: 'uint32' },
            12: { name: 'override_duty_cycle', type: 'bool' },
            13: { name: 'sx126x_rx_boosted_gain', type: 'bool' },
            14: { name: 'override_frequency', type: 'float' },
            15: { name: 'pa_fan_disabled', type: 'bool' },
            104: { name: 'ignore_mqtt', type: 'bool' },
            105: { name: 'config_ok_to_mqtt', type: 'bool' },
        },
        bluetooth: {
            1: { name: 'enabled', type: 'bool' },
            2: { name: 'mode', type: 'enum', enumType: 'PairingMode' },
            3: { name: 'fixed_pin', type: 'uint32' },
        },
        security: {
            1: { name: 'public_key', type: 'bytes' },
            2: { name: 'private_key', type: 'bytes' },
            3: { name: 'admin_key', type: 'bytes' },
            4: { name: 'is_managed', type: 'bool' },
            5: { name: 'serial_enabled', type: 'bool' },
            6: { name: 'debug_log_api_enabled', type: 'bool' },
            8: { name: 'admin_channel_enabled', type: 'bool' },
        },
    },

    MODULE_FIELDS: {
        mqtt: {
            1: { name: 'enabled', type: 'bool' },
            2: { name: 'address', type: 'string' },
            3: { name: 'username', type: 'string' },
            4: { name: 'password', type: 'string' },
            5: { name: 'encryption_enabled', type: 'bool' },
            6: { name: 'json_enabled', type: 'bool' },
            7: { name: 'tls_enabled', type: 'bool' },
            8: { name: 'root', type: 'string' },
            9: { name: 'proxy_to_client_enabled', type: 'bool' },
            10: { name: 'map_reporting_enabled', type: 'bool' },
        },
        serial: {
            1: { name: 'enabled', type: 'bool' },
            2: { name: 'echo', type: 'bool' },
            3: { name: 'rxd', type: 'uint32' },
            4: { name: 'txd', type: 'uint32' },
            5: { name: 'baud', type: 'enum', enumType: 'SerialBaud' },
            6: { name: 'timeout', type: 'uint32' },
            7: { name: 'mode', type: 'enum', enumType: 'SerialMode' },
            8: { name: 'override_console_serial_port', type: 'bool' },
        },
        external_notification: {
            1: { name: 'enabled', type: 'bool' },
            2: { name: 'output_ms', type: 'uint32' },
            3: { name: 'output', type: 'uint32' },
            7: { name: 'active', type: 'bool' },
            8: { name: 'alert_message', type: 'bool' },
            9: { name: 'alert_message_buzzer', type: 'bool' },
            10: { name: 'alert_message_vibra', type: 'bool' },
            11: { name: 'alert_bell', type: 'bool' },
            12: { name: 'alert_bell_buzzer', type: 'bool' },
            13: { name: 'alert_bell_vibra', type: 'bool' },
            14: { name: 'use_pwm', type: 'bool' },
            15: { name: 'output_vibra', type: 'uint32' },
            16: { name: 'output_buzzer', type: 'uint32' },
            17: { name: 'nag_timeout', type: 'uint32' },
        },
        store_forward: {
            1: { name: 'enabled', type: 'bool' },
            2: { name: 'heartbeat', type: 'bool' },
            3: { name: 'records', type: 'uint32' },
            4: { name: 'history_return_max', type: 'uint32' },
            5: { name: 'history_return_window', type: 'uint32' },
        },
        range_test: {
            1: { name: 'enabled', type: 'bool' },
            2: { name: 'sender', type: 'uint32' },
            3: { name: 'save', type: 'bool' },
        },
        telemetry: {
            1: { name: 'device_update_interval', type: 'uint32' },
            2: { name: 'environment_update_interval', type: 'uint32' },
            3: { name: 'environment_measurement_enabled', type: 'bool' },
            4: { name: 'environment_screen_enabled', type: 'bool' },
            5: { name: 'environment_display_fahrenheit', type: 'bool' },
            6: { name: 'air_quality_enabled', type: 'bool' },
            7: { name: 'air_quality_interval', type: 'uint32' },
            8: { name: 'power_measurement_enabled', type: 'bool' },
            9: { name: 'power_update_interval', type: 'uint32' },
            10: { name: 'power_screen_enabled', type: 'bool' },
        },
        canned_message: {
            1: { name: 'rotary1_enabled', type: 'bool' },
            2: { name: 'inputbroker_pin_a', type: 'uint32' },
            3: { name: 'inputbroker_pin_b', type: 'uint32' },
            4: { name: 'inputbroker_pin_press', type: 'uint32' },
            5: { name: 'inputbroker_event_cw', type: 'uint32' },
            6: { name: 'inputbroker_event_ccw', type: 'uint32' },
            7: { name: 'inputbroker_event_press', type: 'uint32' },
            8: { name: 'updown1_enabled', type: 'bool' },
            9: { name: 'enabled', type: 'bool' },
            10: { name: 'allow_input_source', type: 'string' },
            11: { name: 'send_bell', type: 'bool' },
        },
        audio: { 1: { name: 'codec2_enabled', type: 'bool' } },
        remote_hardware: { 1: { name: 'enabled', type: 'bool' } },
        neighbor_info: {
            1: { name: 'enabled', type: 'bool' },
            2: { name: 'update_interval', type: 'uint32' },
        },
        ambient_lighting: {
            1: { name: 'led_state', type: 'bool' },
            2: { name: 'current', type: 'uint32' },
            3: { name: 'red', type: 'uint32' },
            4: { name: 'green', type: 'uint32' },
            5: { name: 'blue', type: 'uint32' },
        },
        detection_sensor: {
            1: { name: 'enabled', type: 'bool' },
            2: { name: 'minimum_broadcast_secs', type: 'uint32' },
            3: { name: 'state_broadcast_secs', type: 'uint32' },
            4: { name: 'send_bell', type: 'bool' },
            5: { name: 'name', type: 'string' },
            6: { name: 'monitor_pin', type: 'uint32' },
            7: { name: 'detection_trigger_type', type: 'uint32' },
            8: { name: 'use_pullup', type: 'bool' },
        },
        paxcounter: {
            1: { name: 'enabled', type: 'bool' },
            2: { name: 'paxcounter_update_interval', type: 'uint32' },
        },
    },

    CHANNEL_SETTINGS_FIELDS: {
        1: { name: 'channel_num', type: 'uint32', deprecated: true },
        2: { name: 'psk', type: 'bytes' },
        3: { name: 'name', type: 'string' },
        4: { name: 'id', type: 'fixed32' },
        5: { name: 'uplink_enabled', type: 'bool' },
        6: { name: 'downlink_enabled', type: 'bool' },
    },

    CHANNEL_FIELDS: {
        1: { name: 'index', type: 'int32' },
        2: { name: 'settings', type: 'message', fieldDefs: 'CHANNEL_SETTINGS_FIELDS' },
        3: { name: 'role', type: 'enum', enumType: 'ChannelRole' },
    },

    USER_FIELDS: {
        1: { name: 'id', type: 'string' },
        2: { name: 'long_name', type: 'string' },
        3: { name: 'short_name', type: 'string' },
        5: { name: 'hw_model', type: 'enum', enumType: 'HardwareModel' },
        6: { name: 'is_licensed', type: 'bool' },
        7: { name: 'role', type: 'enum', enumType: 'DeviceRole' },
        8: { name: 'public_key', type: 'bytes' },
    },

    METADATA_FIELDS: {
        1: { name: 'firmware_version', type: 'string' },
        2: { name: 'device_state_version', type: 'uint32' },
        3: { name: 'canShutdown', type: 'bool' },
        4: { name: 'hasWifi', type: 'bool' },
        5: { name: 'hasBluetooth', type: 'bool' },
        6: { name: 'hasEthernet', type: 'bool' },
        7: { name: 'role', type: 'enum', enumType: 'DeviceRole' },
        8: { name: 'position_flags', type: 'uint32' },
        9: { name: 'hw_model', type: 'enum', enumType: 'HardwareModel' },
        10: { name: 'hasRemoteHardware', type: 'bool' },
        11: { name: 'hasPKC', type: 'bool' },
    },

    MY_INFO_FIELDS: {
        1: { name: 'my_node_num', type: 'uint32' },
        8: { name: 'reboot_count', type: 'uint32' },
        11: { name: 'min_app_version', type: 'uint32' },
    },

    // ========== Decoding ==========

    /** Decode a protobuf message using field definitions, return object */
    decodeWithDefs(bytes, offset, length, fieldDefs) {
        const result = {};
        const fields = this.decodeFields(bytes, offset, length);
        for (const f of fields) {
            const def = fieldDefs[f.fieldNum];
            if (!def) continue;

            let value;
            switch (def.type) {
                case 'bool':
                    value = !!f.value;
                    break;
                case 'enum':
                    value = this.enumName(def.enumType, f.value);
                    break;
                case 'string':
                    value = (f.value instanceof Uint8Array) ? this.bytesToString(f.value) : String(f.value);
                    break;
                case 'bytes':
                    if (f.value instanceof Uint8Array) {
                        value = this.bytesToHex(f.value, 0, f.value.length);
                        if (f.value.length <= 1) value += ' (' + f.value.length + ' byte)';
                        else value += ' (' + f.value.length + ' bytes)';
                    } else {
                        value = String(f.value);
                    }
                    break;
                case 'float':
                    if (f.wireType === 5 && typeof f.value === 'number') {
                        // fixed32 → float
                        const buf = new ArrayBuffer(4);
                        new DataView(buf).setUint32(0, f.value, true);
                        value = new DataView(buf).getFloat32(0, true);
                        value = Math.round(value * 1000) / 1000;
                    } else {
                        value = f.value;
                    }
                    break;
                case 'int32':
                    // Interpret as signed 32-bit
                    value = f.value | 0;
                    break;
                case 'fixed32':
                    value = '0x' + (f.value >>> 0).toString(16).padStart(8, '0');
                    break;
                case 'message':
                    if (f.value instanceof Uint8Array && def.fieldDefs) {
                        const subDefs = this[def.fieldDefs] || {};
                        value = this.decodeWithDefs(f.value, 0, f.value.length, subDefs);
                    } else {
                        value = '[message]';
                    }
                    break;
                default:
                    value = f.value;
            }
            result[def.name] = value;
        }
        return result;
    },

    /** Extract inner payload from a FromRadio frame for a specific type */
    extractFromRadioInner(bytes) {
        const fields = this.decodeFields(bytes, 0, bytes.length);
        let id = null;
        let innerType = null;
        let innerBytes = null;

        for (const f of fields) {
            if (f.fieldNum === 1 && f.wireType === 0) {
                id = f.value;
            } else if (f.wireType === 2 && f.value instanceof Uint8Array) {
                // LEN-delimited field — this is the oneof content
                innerType = f.fieldNum;
                innerBytes = f.value;
            } else if (f.wireType === 0 && f.fieldNum >= 7) {
                // VARINT fields like config_done (7), rebooted (8)
                innerType = f.fieldNum;
                innerBytes = f.value;
            }
        }
        return { id, innerType, innerBytes };
    },

    // ========== Frame Formatting ==========

    /** Format a frame from the ESP32 API response into readable text */
    formatFrame(frame) {
        if (!frame || !frame.hex) return '[empty frame]';

        const bytes = this.hexToBytes(frame.hex);
        const { id, innerType, innerBytes } = this.extractFromRadioInner(bytes);

        const type = frame.type || 'unknown';

        switch (type) {
            case 'config':
                return this.formatConfig(innerBytes, frame.subType);
            case 'module_config':
                return this.formatModuleConfig(innerBytes, frame.subType);
            case 'channel':
                return this.formatChannel(innerBytes);
            case 'metadata':
                return this.formatMetadata(innerBytes);
            case 'my_info':
                return this.formatMyInfo(innerBytes);
            case 'node_info':
                return this.formatNodeInfo(innerBytes);
            case 'packet':
                return this.formatPacket(innerBytes);
            case 'config_complete':
                return '=== Config Complete ===';
            case 'rebooted':
                return '=== Device Rebooted ===';
            case 'log':
                return this.formatLog(innerBytes);
            default:
                return '=== ' + type + ' === (' + frame.len + ' bytes)\n' +
                       '  [raw] ' + frame.hex.substring(0, 80) +
                       (frame.hex.length > 80 ? '...' : '');
        }
    },

    formatConfig(innerBytes, subTypeName) {
        if (!(innerBytes instanceof Uint8Array)) return '[no config data]';

        // innerBytes is the Config message. First field identifies the variant.
        const configFields = this.decodeFields(innerBytes, 0, innerBytes.length);
        if (configFields.length === 0) return '[empty config]';

        // The first (and usually only) field is the config variant
        const variant = configFields[0];
        const variantName = this.CONFIG_ONEOF[variant.fieldNum] || subTypeName || ('field_' + variant.fieldNum);
        const titleName = variantName.charAt(0).toUpperCase() + variantName.slice(1);

        let output = '=== ' + titleName + ' Config ===\n';

        if (variant.value instanceof Uint8Array) {
            const fieldDefs = this.CONFIG_FIELDS[variantName];
            if (fieldDefs) {
                const decoded = this.decodeWithDefs(variant.value, 0, variant.value.length, fieldDefs);
                output += this.formatObject(decoded);
            } else {
                output += '  [raw] ' + this.bytesToHex(variant.value, 0, variant.value.length) + '\n';
            }
        }
        return output;
    },

    formatModuleConfig(innerBytes, subTypeName) {
        if (!(innerBytes instanceof Uint8Array)) return '[no module data]';

        const fields = this.decodeFields(innerBytes, 0, innerBytes.length);
        if (fields.length === 0) return '[empty module config]';

        const variant = fields[0];
        const variantName = this.MODULE_ONEOF[variant.fieldNum] || subTypeName || ('field_' + variant.fieldNum);
        const titleName = variantName.charAt(0).toUpperCase() + variantName.slice(1);

        let output = '=== ' + titleName + ' Module Config ===\n';

        if (variant.value instanceof Uint8Array) {
            const fieldDefs = this.MODULE_FIELDS[variantName];
            if (fieldDefs) {
                const decoded = this.decodeWithDefs(variant.value, 0, variant.value.length, fieldDefs);
                output += this.formatObject(decoded);
            } else {
                output += '  [raw] ' + this.bytesToHex(variant.value, 0, variant.value.length) + '\n';
            }
        }
        return output;
    },

    formatChannel(innerBytes) {
        if (!(innerBytes instanceof Uint8Array)) return '[no channel data]';

        const decoded = this.decodeWithDefs(innerBytes, 0, innerBytes.length, this.CHANNEL_FIELDS);
        const idx = decoded.index !== undefined ? decoded.index : '?';
        const role = decoded.role || 'DISABLED';
        let output = '=== Channel ' + idx + ' (' + role + ') ===\n';

        if (decoded.settings && typeof decoded.settings === 'object') {
            const s = decoded.settings;
            if (s.name) output += '  name: ' + s.name + '\n';
            else output += '  name: (default)\n';
            if (s.psk) output += '  psk: ' + s.psk + '\n';
            if (s.id) output += '  id: ' + s.id + '\n';
            output += '  uplink_enabled: ' + (s.uplink_enabled || false) + '\n';
            output += '  downlink_enabled: ' + (s.downlink_enabled || false) + '\n';
        }
        return output;
    },

    formatMetadata(innerBytes) {
        if (!(innerBytes instanceof Uint8Array)) return '[no metadata]';

        const decoded = this.decodeWithDefs(innerBytes, 0, innerBytes.length, this.METADATA_FIELDS);
        let output = '=== Device Metadata ===\n';
        output += this.formatObject(decoded);
        return output;
    },

    formatMyInfo(innerBytes) {
        if (!(innerBytes instanceof Uint8Array)) return '[no my_info data]';

        const decoded = this.decodeWithDefs(innerBytes, 0, innerBytes.length, this.MY_INFO_FIELDS);
        let output = '=== My Node Info ===\n';
        if (decoded.my_node_num) {
            const num = decoded.my_node_num;
            output += '  my_node_num: 0x' + num.toString(16).toUpperCase().padStart(8, '0') +
                      ' (' + num + ')\n';
        }
        if (decoded.reboot_count) output += '  reboot_count: ' + decoded.reboot_count + '\n';
        if (decoded.min_app_version) output += '  min_app_version: ' + decoded.min_app_version + '\n';
        return output;
    },

    formatNodeInfo(innerBytes) {
        if (!(innerBytes instanceof Uint8Array)) return '[no node_info data]';
        // NodeInfo contains nested User and Position — decode top-level fields
        const fields = this.decodeFields(innerBytes, 0, innerBytes.length);
        let output = '=== Node Info ===\n';
        for (const f of fields) {
            if (f.fieldNum === 1 && f.wireType === 0) {
                output += '  num: 0x' + f.value.toString(16).toUpperCase().padStart(8, '0') + '\n';
            } else if (f.fieldNum === 2 && f.value instanceof Uint8Array) {
                const user = this.decodeWithDefs(f.value, 0, f.value.length, this.USER_FIELDS);
                output += '  User:\n';
                for (const [k, v] of Object.entries(user)) {
                    output += '    ' + k + ': ' + v + '\n';
                }
            }
        }
        return output;
    },

    formatPacket(innerBytes) {
        if (!(innerBytes instanceof Uint8Array)) return '[packet]';
        // Admin GET responses come wrapped in MeshPacket > Data > AdminMessage
        // Try to extract admin response
        const fields = this.decodeFields(innerBytes, 0, innerBytes.length);
        let output = '=== Packet ===\n';

        for (const f of fields) {
            if (f.fieldNum === 4 && f.value instanceof Uint8Array) {
                // decoded (Data message)
                const dataFields = this.decodeFields(f.value, 0, f.value.length);
                for (const df of dataFields) {
                    if (df.fieldNum === 1 && df.wireType === 0) {
                        output += '  portnum: ' + df.value + '\n';
                    }
                    if (df.fieldNum === 2 && df.value instanceof Uint8Array) {
                        // payload — could be admin response
                        output += '  payload: ' + this.bytesToHex(df.value, 0, Math.min(df.value.length, 32));
                        if (df.value.length > 32) output += '...';
                        output += ' (' + df.value.length + ' bytes)\n';

                        // Try to decode as admin response containing config/module/channel/user/metadata
                        const adminDecoded = this.tryDecodeAdminResponse(df.value);
                        if (adminDecoded) output += adminDecoded;
                    }
                }
            } else if (f.fieldNum === 2 && f.wireType === 5) {
                output += '  from: 0x' + (f.value >>> 0).toString(16).padStart(8, '0') + '\n';
            }
        }
        return output;
    },

    tryDecodeAdminResponse(adminBytes) {
        // AdminMessage response fields:
        // get_config_response (field 6) = Config
        // get_channel_response (field 8) = Channel
        // get_owner_response (field 10) = User
        // get_module_config_response (field 12) = ModuleConfig
        // get_device_metadata_response (field 14) = DeviceMetadata
        const fields = this.decodeFields(adminBytes, 0, adminBytes.length);
        let output = '';

        for (const f of fields) {
            if (!(f.value instanceof Uint8Array)) continue;

            if (f.fieldNum === 6) {
                // Config response
                output += this.formatConfig(f.value, null);
            } else if (f.fieldNum === 8) {
                // Channel response
                output += this.formatChannel(f.value);
            } else if (f.fieldNum === 10) {
                // Owner/User response
                const user = this.decodeWithDefs(f.value, 0, f.value.length, this.USER_FIELDS);
                output += '=== Owner ===\n' + this.formatObject(user);
            } else if (f.fieldNum === 12) {
                // ModuleConfig response
                output += this.formatModuleConfig(f.value, null);
            } else if (f.fieldNum === 14) {
                // DeviceMetadata response
                const meta = this.decodeWithDefs(f.value, 0, f.value.length, this.METADATA_FIELDS);
                output += '=== Device Metadata ===\n' + this.formatObject(meta);
            }
        }
        return output || null;
    },

    formatLog(innerBytes) {
        if (!(innerBytes instanceof Uint8Array)) return '[log]';
        // LogRecord: message (1, string), time (2, fixed32), source (3, string), level (4, enum)
        const fields = this.decodeFields(innerBytes, 0, innerBytes.length);
        let msg = '';
        for (const f of fields) {
            if (f.fieldNum === 1 && f.value instanceof Uint8Array) {
                msg = this.bytesToString(f.value);
            }
        }
        return '[LOG] ' + msg;
    },

    /** Format a decoded object as indented key: value lines */
    formatObject(obj, indent) {
        indent = indent || '  ';
        let output = '';
        for (const [key, val] of Object.entries(obj)) {
            if (typeof val === 'object' && val !== null && !(val instanceof Uint8Array)) {
                output += indent + key + ':\n';
                output += this.formatObject(val, indent + '  ');
            } else {
                output += indent + key + ': ' + val + '\n';
            }
        }
        return output;
    },

    // ========== Command Parser ==========

    COMMAND_MAP: {
        // Config GET commands
        'get device':     { command: 'get_config', type: 0 },
        'get position':   { command: 'get_config', type: 1 },
        'get power':      { command: 'get_config', type: 2 },
        'get network':    { command: 'get_config', type: 3 },
        'get display':    { command: 'get_config', type: 4 },
        'get lora':       { command: 'get_config', type: 5 },
        'get bluetooth':  { command: 'get_config', type: 6 },
        'get security':   { command: 'get_config', type: 7 },
        // Module GET commands
        'get mqtt':           { command: 'get_module', type: 0 },
        'get serial':         { command: 'get_module', type: 1 },
        'get ext_notif':      { command: 'get_module', type: 2 },
        'get store_forward':  { command: 'get_module', type: 3 },
        'get range_test':     { command: 'get_module', type: 4 },
        'get telemetry':      { command: 'get_module', type: 5 },
        'get canned_msg':     { command: 'get_module', type: 6 },
        'get audio':          { command: 'get_module', type: 7 },
        'get remote_hw':      { command: 'get_module', type: 8 },
        'get neighbor_info':  { command: 'get_module', type: 9 },
        'get ambient_light':  { command: 'get_module', type: 10 },
        'get detection':      { command: 'get_module', type: 11 },
        'get paxcounter':     { command: 'get_module', type: 12 },
        // Other GET commands
        'get owner':      { command: 'get_owner' },
        'get metadata':   { command: 'get_metadata' },
        'info':           { command: 'get_metadata' },
        'get all':        { command: 'get_all' },
        // Actions
        'reboot':         { command: 'reboot', type: 5 },
        'factory_reset':  { command: 'factory_reset' },
        'nodedb_reset':   { command: 'nodedb_reset' },
    },

    parseCommand(text) {
        if (!text) return null;
        const input = text.trim().toLowerCase();

        // Direct command match
        if (this.COMMAND_MAP[input]) {
            return Object.assign({}, this.COMMAND_MAP[input]);
        }

        // Handle "get channel N"
        const channelMatch = input.match(/^get\s+channel\s+(\d+)$/);
        if (channelMatch) {
            const idx = parseInt(channelMatch[1]);
            if (idx >= 0 && idx <= 7) return { command: 'get_channel', index: idx };
            return null;
        }

        // "get channels" → special multi-request
        if (input === 'get channels') return 'GET_ALL_CHANNELS';

        // "reboot N" → reboot with custom delay
        const rebootMatch = input.match(/^reboot\s+(\d+)$/);
        if (rebootMatch) {
            return { command: 'reboot', seconds: parseInt(rebootMatch[1]) };
        }

        // "help"
        if (input === 'help' || input === '?') return 'HELP';

        return null;
    },

    getHelpText() {
        return [
            '=== Available Commands ===',
            '',
            'Config:',
            '  get lora         LoRa radio settings (region, preset, power)',
            '  get device       Device role, serial, debug settings',
            '  get position     Position broadcast settings, GPS mode',
            '  get power        Power saving, sleep timers',
            '  get network      WiFi, Ethernet, NTP settings',
            '  get display      Screen, OLED, display mode',
            '  get bluetooth    Bluetooth mode, PIN',
            '  get security     Keys, serial, debug API settings',
            '',
            'Modules:',
            '  get mqtt         MQTT configuration',
            '  get serial       Serial module config (baud, mode)',
            '  get telemetry    Telemetry intervals and settings',
            '  get store_forward Store & Forward config',
            '  get ext_notif    External notification (buzzer, LED)',
            '  get range_test   Range test settings',
            '',
            'Channels:',
            '  get channels     All 8 channels',
            '  get channel N    Single channel (0-7)',
            '',
            'Device:',
            '  info             Device metadata (firmware, hw model)',
            '  get metadata     Same as info',
            '  get owner        Device owner name',
            '  get all          Full config dump',
            '',
            'Actions:',
            '  reboot           Reboot device (5 sec delay)',
            '  reboot N         Reboot with N second delay',
            '  factory_reset    Factory reset (WARNING: erases all!)',
            '  nodedb_reset     Reset node database',
        ].join('\n');
    }
};
