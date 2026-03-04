# ESP32-RAKFlasher

**Remote firmware management for RAK4631 Meshtastic nodes via ESP32-S3**

Node out of reach? Firmware out of date? Just lazy? Manage your RAK4631 remotely -- config backup/restore, firmware flashing, SWD recovery, and more -- from the comfort of your couch, car, or bicycle.

Built for the **RAK4630/4631** (nRF52840) module on a **RAK19001** baseboard, this ESP32-S3 firmware connects via UART and SWD to provide a full-featured web-based management interface over WiFi.

> **Status:** Active development. Core features are functional and tested. See [Feature Status](#feature-status) below.

---

## Key Features

- **SWD Flash Backup & Restore** -- Full 1MB flash dump and restore via bitbang SWD. Back up your entire nRF52 firmware and restore it later, or flash a completely new image. No bootloader needed.
- **Meshtastic Config Backup & Restore** -- Full settings, channels, and encryption keys backed up via the native Meshtastic protobuf serial API. Restore to the same node or transfer to another.
- **Meshtastic Command Console** -- Query and change Meshtastic settings from your browser. GET any config section, view channels, check device metadata, reboot, factory reset -- all through a web UI.
- **TCP Bridge (Port 4403)** -- Transparent serial bridge compatible with the Meshtastic Python CLI. Use `meshtastic --host <ip>` as if directly connected via USB.
- **HTTP Bridge API** -- Meshtastic-compatible REST endpoints (`/api/v1/fromradio`, `/api/v1/toradio`) for integration with Meshtastic web clients and tools.
- **Real-Time Serial Monitor** -- Protocol auto-detection (Meshtastic protobuf, SLIP/DFU, NMEA, AT commands, plain text), hex and text display modes, bidirectional passthrough, WebSocket streaming.
- **SWD Recovery** -- Mass erase, reflash bootloader and SoftDevice when your node is bricked. Direct memory read/write for debugging.
- **Web UI** -- 7-page responsive dark-themed interface accessible from any device on WiFi. Embedded in firmware -- no separate filesystem upload needed.
- **WiFi AP + STA** -- Creates its own hotspot, or joins your existing WiFi network for LAN access. Automatic fallback to AP if STA connection drops.
- **GPIO Control** -- All pin assignments configurable via WebUI. Test individual pins, trigger reset, enter DFU mode.
- **Deep Sleep** -- Auto-sleep after idle timeout with GPIO wake-on-signal for low-power remote deployments.
- **OTA Updates** -- Update the ESP32-RAKFlasher firmware itself over WiFi.

---

## Feature Status

### Functional (Tested & Working)

| Feature | Details |
|---------|---------|
| SWD Connect & Info | JTAG-to-SWD init, IDCODE read, FICR device info, lock detection |
| SWD Mass Erase | Via nRF Control-AP with completion polling |
| SWD Flash Backup | Full 1MB flash + 4KB UICR dump to file with real-time KB/s progress |
| SWD Flash Restore | From uploaded file or from stored backup on device |
| SWD Memory Read/Write | Bulk read with auto-increment, NVMC-aware writes |
| Meshtastic Config Backup | Full protobuf config dump (10 config types, 16 module types, 8 channels) |
| Meshtastic Config Restore | Admin messages for all config/module/channel sections |
| Meshtastic Channel Backup | Channel names, PSK, roles, uplink/downlink settings |
| Meshtastic Channel Restore | Per-channel admin SET messages |
| Meshtastic Command Console | GET config/module/channel/owner/metadata + reboot/factory reset/nodedb reset |
| TCP Serial Bridge | Port 4403, transparent byte forwarding, Meshtastic CLI compatible |
| HTTP Bridge API | `/api/v1/fromradio` + `/api/v1/toradio` with frame queuing |
| Serial Monitor | Protocol autodetect, hex/text modes, passthrough, WebSocket streaming |
| WiFi AP Mode | Configurable SSID/password/channel, hidden SSID option |
| WiFi STA (Client) Mode | Join existing networks, async scanning, exponential backoff reconnect |
| WiFi AP+STA Dual Mode | Auto-disable AP when STA connects, auto-fallback when STA drops |
| Web UI (7 pages) | Dashboard, Firmware, Recovery, Backup, Serial, Meshtastic, Settings |
| GPIO Control | Pin config, reset, DFU mode entry, pin testing |
| Deep Sleep | GPIO + timer wake, auto-sleep on idle |
| OTA Firmware Update | Upload `.bin` via Settings page, auto-reboot |
| mDNS | `rakflasher-xxxx.local` with service discovery |

### Work In Progress

| Feature | Status | Notes |
|---------|--------|-------|
| Serial DFU Flashing | SLIP framing implemented | Data transfer pipeline not yet complete |
| Channel Backup Encryption | XOR obfuscation only | Needs AES-256 implementation |

### Planned / TODO

| Feature | Description |
|---------|-------------|
| AES-256 encryption | Secure channel/key backup encryption |
| Serial DFU completion | Full Nordic DFU protocol data transfer |
| SET commands in console | Modify individual config fields from the web UI |
| Selective restore | Restore only specific settings (channels only, LoRa only, etc.) |
| Batch operations | Flash/configure multiple nodes |
| Additional platform support | Other ESP32 variants, other nRF52-based modules |

---

## Hardware Requirements

### Components

| Component | Specification | Notes |
|-----------|--------------|-------|
| ESP32-S3 | Seeed XIAO ESP32-S3 (or equivalent) | 8MB flash minimum |
| RAK4631 | nRF52840-based WisBlock Core | With Adafruit nRF52 bootloader |
| RAK19001 | WisBlock Base Board | Carrier board for RAK4631 |
| Jumper wires | Dupont-style | 6-8 wires for connections |
| USB-C cable | Data + Power | For ESP32 programming |

### Wiring

Connect the ESP32-S3 to the RAK19001 baseboard:

```
ESP32-S3 (XIAO)                RAK19001 Baseboard
┌──────────────┐                ┌───────────────────┐
│              │                │    RAK4631 (nRF52) │
│  D6 (GPIO43) ├────── TX ─────┤ TXD1 (→ nRF P0.16)│
│  D7 (GPIO44) ├────── RX ─────┤ RXD1 (→ nRF P0.15)│
│  D2 (GPIO3)  ├──── RESET ────┤ RESET              │
│  D3 (GPIO4)  ├──── WAKE ─────┤ IO8                │
│  D8 (GPIO7)  ├──── SWCLK ────┤ SWCLK              │
│  D9 (GPIO8)  ├──── SWDIO ────┤ SWDIO              │
│  GND         ├──── GND ──────┤ GND                │
│              │                │                    │
└──────────────┘                └───────────────────┘
```

| ESP32-S3 GPIO | XIAO Silk | RAK19001 Header | Function | Direction |
|---------------|-----------|-----------------|----------|-----------|
| GPIO43 | D6 | TXD1 | Serial TX | ESP32 TX --> RAK RX |
| GPIO44 | D7 | RXD1 | Serial RX | ESP32 RX <-- RAK TX |
| GPIO3 | D2 | RESET | Reset control | ESP32 --> RAK (active LOW) |
| GPIO4 | D3 | IO8 | Wake signal | RAK --> ESP32 (input) |
| GPIO7 | D8 | SWCLK | SWD Clock | Bidirectional |
| GPIO8 | D9 | SWDIO | SWD Data | Bidirectional |
| GND | GND | GND | Ground | Common |

> **Important:** TXD1/RXD1 labels on the RAK19001 are from the **baseboard's perspective**. ESP32 TX goes to TXD1 (which routes to the RAK module's RX pin P0.15). All pin assignments are configurable via the web UI Settings page.

### RAK4631 Configuration

For serial communication (backup/restore, command console), the RAK4631 must be configured:

```bash
meshtastic --set serial.enabled true
meshtastic --set serial.mode PROTO
meshtastic --set serial.rxd 15
meshtastic --set serial.txd 16
meshtastic --set serial.baud BAUD_38400
meshtastic --set position.gps_mode NOT_PRESENT
```

> **Note:** GPS mode must be set to `NOT_PRESENT` because P0.15/P0.16 are shared with the GPS UART on RAK4631. SWD operations (backup, erase, flash) do NOT require any serial configuration -- they work regardless of firmware state.

---

## Getting Started

### Build & Flash

```bash
# Clone the repository
git clone https://github.com/synssins/RAKFlasher.git
cd RAKFlasher

# Build firmware (auto-increments build number, embeds web UI)
pio run

# Flash via USB
pio run -t upload

# Or flash via OTA (if already running RAKFlasher)
curl -F "firmware=@.pio/build/seeed_xiao_esp32s3/firmware.bin" http://192.168.4.1/api/ota/update
```

### First Connection

1. Power on the ESP32-S3
2. Connect to WiFi: **RAKFlasher-XXXX** (password: `flasher123`)
3. Open **http://192.168.4.1** in your browser
4. (Optional) Go to Settings to join your home WiFi for LAN access

### WiFi STA (Client) Mode

To pre-configure WiFi STA credentials at build time without editing source:

```ini
# In platformio.ini, add under build_flags:
build_flags =
    -DDEFAULT_STA_SSID=\"YourNetworkName\"
    -DDEFAULT_STA_PASSWORD=\"YourPassword\"
```

Or configure via the Settings page in the web UI after first boot.

---

## Web UI Pages

| Page | Purpose |
|------|---------|
| **Dashboard** | System status, uptime, memory, WiFi clients, RAK connection status, quick actions |
| **Firmware** | Upload DFU packages for serial firmware flashing (WIP) |
| **Recovery** | SWD operations: connect, info, mass erase, flash backup, flash restore, flash from file |
| **Backup** | Meshtastic config backup/restore, channel backup/restore, backup history |
| **Serial** | Real-time serial monitor, hex/text display, protocol detection, command input |
| **Meshtastic** | Command console: GET configs, channels, metadata; reboot, factory reset |
| **Settings** | WiFi AP/STA config, GPIO pins, serial baud rate, OTA update, factory reset, about |

---

## API Reference

### System
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/status` | System status, uptime, memory |
| GET | `/api/info` | Firmware version, build info |
| POST | `/api/reboot` | Reboot ESP32 |
| POST | `/api/sleep` | Enter deep sleep |

### SWD Recovery
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/swd/connect` | Test SWD connection, return chip info |
| GET | `/api/swd/info` | Device info (FICR, flash size, device ID) |
| POST | `/api/swd/erase` | Mass erase nRF52 flash |
| POST | `/api/swd/backup` | Start async flash dump (1MB + UICR) |
| GET | `/api/swd/backup/progress` | Poll backup progress (%, KB/s) |
| GET | `/api/swd/backup/download` | Download backup `.bin` file |
| POST | `/api/swd/flash/upload` | Upload firmware file for flashing |
| POST | `/api/swd/flash/start` | Start async flash (erase + write) |
| POST | `/api/swd/flash/from-backup` | Flash from stored backup on device |
| GET | `/api/swd/flash/progress` | Poll flash progress |

### Meshtastic Backup
| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/backup/settings` | Create full settings backup (async) |
| POST | `/api/backup/channels` | Create channel/key backup (async) |
| GET | `/api/backup/status` | Poll backup completion |
| GET | `/api/backup/list` | List stored backups |
| DELETE | `/api/backup/{id}` | Delete a backup |
| POST | `/api/backup/settings/restore` | Restore settings from file |
| POST | `/api/backup/channels/restore` | Restore channels from file |

### Meshtastic Command Console
| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/api/meshtastic/command` | Send command (get_config, reboot, etc.) |
| GET | `/api/meshtastic/command` | Poll command result |

### Serial
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/serial/buffer` | Get buffered serial output |
| POST | `/api/serial/send` | Send command or raw hex bytes |
| POST | `/api/serial/test` | Test serial connection (async) |
| GET | `/api/serial/test` | Poll test result |

### Bridge
| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/v1/fromradio` | Read one protobuf frame (HTTP bridge) |
| PUT | `/api/v1/toradio` | Send protobuf frame (HTTP bridge) |
| GET | `/api/bridge/status` | Bridge mode and activity |
| -- | `tcp://ip:4403` | Transparent TCP serial bridge |

### WebSocket
| Path | Description |
|------|-------------|
| `ws://ip/ws` | Real-time events: serial output, progress updates, status changes |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                       ESP32-S3 Firmware                         │
├─────────────────────────────────────────────────────────────────┤
│  ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌────────────────┐ │
│  │ WiFi Mgr  │ │ Web Server│ │ WebSocket │ │   OTA Update   │ │
│  │ (AP+STA)  │ │ (Async)   │ │ Streaming │ │                │ │
│  └─────┬─────┘ └─────┬─────┘ └─────┬─────┘ └───────┬────────┘ │
│        └──────────────┼─────────────┘               │          │
│                       │                              │          │
│  ┌────────────────────┴──────────────────────────────┘────────┐ │
│  │                    Core Controller                         │ │
│  └──┬──────────┬──────────┬──────────┬──────────┬────────────┘ │
│     │          │          │          │          │               │
│  ┌──┴───┐ ┌───┴───┐ ┌───┴───┐ ┌───┴───┐ ┌───┴───┐           │
│  │Serial│ │Serial │ │  SWD  │ │Config │ │ GPIO  │           │
│  │Monit.│ │Bridge │ │Engine │ │Backup │ │Control│           │
│  │      │ │TCP/HTTP│ │      │ │Restore│ │      │           │
│  └──┬───┘ └───┬───┘ └───┬───┘ └───┬───┘ └───┬───┘           │
│     └─────┬───┘         │         │          │               │
│           │  UART       │  SWD    │  Proto   │  GPIO         │
└───────────┼─────────────┼─────────┼──────────┼───────────────┘
            │             │         │          │
            ▼             ▼         ▼          ▼
┌───────────────────────────────────────────────────────────────┐
│                     RAK4631 (nRF52840)                        │
│  ┌──────────────┐  ┌───────────────┐  ┌────────────────────┐ │
│  │  Adafruit    │  │ Meshtastic FW │  │ SWD Debug Port     │ │
│  │  Bootloader  │  │ (Serial API)  │  │ (SWCLK + SWDIO)    │ │
│  └──────────────┘  └───────────────┘  └────────────────────┘ │
└───────────────────────────────────────────────────────────────┘
```

---

## Project Structure

```
RAKFlasher/
├── platformio.ini              # PlatformIO build configuration
├── partitions_8mb.csv          # Custom 8MB flash partition table
├── include/
│   ├── config.h                # Pin mappings, constants, defaults
│   ├── meshtastic_proto.h      # Protobuf protocol definitions
│   ├── swd_engine.h            # SWD engine interface
│   ├── serial_monitor.h        # Serial monitor interface
│   ├── serial_bridge.h         # TCP/HTTP bridge interface
│   ├── serial_dfu.h            # Nordic DFU protocol (WIP)
│   ├── wifi_manager.h          # WiFi AP+STA manager
│   ├── web_server.h            # Web server + API handlers
│   ├── backup_manager.h        # Backup/restore manager
│   ├── gpio_control.h          # GPIO control interface
│   └── sleep_manager.h         # Deep sleep manager
├── src/
│   ├── main.cpp                # Boot sequence, main loop
│   ├── web_server.cpp          # REST API + WebSocket (40+ endpoints)
│   ├── meshtastic_proto.cpp    # Protobuf encode/decode/framing
│   ├── swd_engine.cpp          # Bitbang SWD implementation
│   ├── serial_monitor.cpp      # Protocol autodetection, display
│   ├── serial_bridge.cpp       # TCP + HTTP serial bridges
│   ├── serial_dfu.cpp          # Nordic DFU (SLIP framing only)
│   ├── wifi_manager.cpp        # Dual-mode WiFi management
│   ├── backup_manager.cpp      # Config backup/restore via protobuf
│   ├── gpio_control.cpp        # Pin configuration and control
│   └── sleep_manager.cpp       # Deep sleep with wake sources
├── data/www/                   # Web UI source files
│   ├── index.html              # Dashboard
│   ├── firmware.html           # Firmware upload/flash
│   ├── recovery.html           # SWD recovery operations
│   ├── backup.html             # Backup/restore interface
│   ├── serial.html             # Serial monitor
│   ├── meshtastic.html         # Meshtastic command console
│   ├── settings.html           # System settings
│   ├── css/style.css           # Dark theme styles
│   └── js/
│       ├── app.js              # Core application logic
│       └── meshtastic-decoder.js  # Client-side protobuf decoder
├── tools/
│   ├── embed_webui.py          # Compress + embed web UI into firmware
│   └── version_increment.py    # Auto-increment build number
├── docs/
│   ├── HARDWARE.md             # Detailed wiring guide
│   └── API.md                  # API documentation
└── .github/                    # CI workflow, issue/PR templates
```

---

## Development

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- Python 3.x (for build scripts)

### Build Commands

```bash
pio run                    # Build firmware (auto-embeds web UI)
pio run -t upload          # Flash via USB
pio run -t clean           # Clean build artifacts
pio device monitor         # Serial console monitor
```

### Web UI Development

The web UI is embedded directly into the firmware binary via `tools/embed_webui.py`. This script runs automatically during the build and gzip-compresses all HTML/CSS/JS files into a C header file (`include/web_content.h`). No separate `uploadfs` step is needed.

To modify the UI:
1. Edit files in `data/www/`
2. Run `pio run` -- the embed script regenerates the header automatically
3. Flash the new firmware

### OTA Update

Once RAKFlasher is running, firmware updates can be pushed over WiFi:

```bash
# From the web UI: Settings page > OTA Update > Upload .bin file

# Or via API:
curl -F "firmware=@.pio/build/seeed_xiao_esp32s3/firmware.bin" http://<ip>/api/ota/update
```

---

## Security Notes

1. **Change the default WiFi password** (`flasher123`) on first use
2. **Channel backups** currently use XOR obfuscation -- not cryptographically secure. Do not rely on the password feature for sensitive key material until AES-256 is implemented.
3. **No internet access** -- the ESP32 does not make any external network calls
4. **Local-only** -- all communication is on the local WiFi network

---

## Credits & References

- [atc1441/ESP32_nRF52_SWD](https://github.com/atc1441/ESP32_nRF52_SWD) -- SWD bitbang implementation reference
- [Meshtastic Protobufs](https://github.com/meshtastic/protobufs) -- Protocol buffer definitions
- [Adafruit nRF52 Bootloader](https://github.com/adafruit/Adafruit_nRF52_Bootloader) -- Target bootloader
- [ESPAsyncWebServer](https://github.com/mathieucarbou/ESPAsyncWebServer) -- Async web server library
- [Nordic DFU Protocol](https://docs.nordicsemi.com/bundle/sdk_nrf5_v17.1.0/page/lib_bootloader_dfu.html) -- DFU specification

## License

MIT License -- See [LICENSE](LICENSE) for details.

---

**Built for the Meshtastic community**
