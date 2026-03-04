# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Planned
- Meshtastic protobuf integration for full settings management
- AES-256 encryption for channel backups
- Batch firmware flashing for multiple devices
- Advanced SWD debugging features

## [1.2.4] - 2026-03-03

### Fixed
- **TCP bridge hang**: Meshtastic CLI (`meshtastic --host <ip>`) no longer hangs after command completion — added 60s inactivity timeout and immediate TCP flush
- **Meshtastic console**: Fixed text commands (`get channels`, `get channel N`, `help`, `?`, `reboot N`) not working from the command input box
- **Meshtastic console**: Fixed stale command results being served on subsequent requests
- **WiFi STA persistence**: Build-time WiFi credentials now saved to NVS on first boot so they survive OTA updates
- **DEFAULT_STA_ENABLED**: Now overridable via `platformio_override.ini` build flags (wrapped in `#ifndef`)

## [1.2.3] - 2026-03-03

### Fixed
- **SWD backup/flash speed**: Replaced slow `pinMode()` calls (~100-200µs each) in SWD turnarounds with direct GPIO output-enable register writes (~10ns), dramatically improving SWD throughput

## [1.2.2] - 2026-03-03

### Added
- **Flash from Backup**: New "Flash Last Backup" button on Recovery page lets you restore a backup directly from the device without downloading and re-uploading
- New `/api/swd/flash/from-backup` endpoint to flash firmware from any file already on LittleFS

### Fixed
- **Stale completion alert**: Starting a new SWD operation (backup or flash) no longer shows the previous operation's "completed" alert
- **Meshtastic busy state**: Fixed duplicate polling causing "Reconnected — command still running..." messages immediately after sending a command

## [1.2.1] - 2026-03-03

### Fixed
- **SWD backup speed**: Replaced `delayMicroseconds(1)` with NOP-based ~100ns delays and raised FreeRTOS task priority from 1 to 5, dramatically improving SWD read throughput
- **Download filename**: SWD backup downloads now use proper filename (e.g. `1C77E41493C0F83C.bin`) instead of generic "download"
- **Repeated success alerts**: Fixed `activeOperations` polling creating duplicate "Operation completed!" alerts every 3 seconds after operation finished

### Changed
- SWD progress messages now include real-time speed (KB/s) during flash dump
- Reduced `yield()` frequency in flash dump from every chunk to every 8 chunks for less context-switch overhead

## [1.2.0] - 2026-03-03

### Added
- Auto-incrementing build numbers for easy version tracking
- "What's New" changelog section on Settings page
- Build number displayed on Dashboard and Settings pages
- Build number included in `/api/info` and `/api/status` API responses

### Changed
- Version display now shows build number (e.g. `1.2.0 (#47)`)

## [1.1.0] - 2026-03-02

### Added
- **SWD Firmware Backup & Restore**: Full flash dump (1MB) and UICR backup via SWD
- **Meshtastic Command Console**: AT command interface with real-time output
- **TCP Serial Bridge**: Meshtastic-compatible TCP bridge on port 4403
- **WiFi STA (Client) Mode**: Join existing WiFi networks for LAN access
- **OTA Firmware Updates**: Single .bin upload includes embedded web UI
- **Serial Baud Rate Configuration**: Configurable from Settings page

### Changed
- Complete SWD engine rewrite based on proven atc1441/ESP32_nRF52_SWD reference
- Web UI now embedded in firmware binary (no separate LittleFS upload needed)
- Recovery page enhanced with firmware backup/restore and device info display

### Fixed
- SWD `readMemory()` was stubbed (returned false), breaking all SWD operations
- SWD `writeWord()` parity calculation bug (calculated on already-shifted value)
- SWD mass erase and flash now use nRF Control-AP for reliability
- CPU halt added before memory access operations

## [1.0.0] - 2026-03-01

### Added
- Initial release of ESP32-RAKFlasher
- Nordic DFU serial protocol implementation
- SWD recovery engine for nRF52840
- WiFi Access Point with configurable settings
- Responsive WebUI with 6 pages (Dashboard, Firmware, Recovery, Backup, Serial, Settings)
- Real-time serial monitor with WebSocket streaming
- Configuration backup and restore framework
- GPIO pin configuration with test functions
- Deep sleep mode with GPIO wake
- REST API for all operations
- WebSocket support for real-time updates
- LittleFS filesystem for data storage
- Comprehensive documentation and setup guide

### Features
- **Firmware Flashing**: Upload and flash DFU packages via serial
- **SWD Recovery**: Low-level chip recovery for bricked devices
- **Configuration Backup**: Save and restore Meshtastic settings
- **Serial Monitor**: Real-time serial output viewing
- **GPIO Control**: Configurable pin assignments
- **WiFi AP**: Customizable SSID and password
- **Mobile-Responsive**: Works on all device sizes
- **Offline Operation**: No external dependencies

### Technical Details
- ESP32-S3 target (Seeed XIAO ESP32-S3)
- PlatformIO build system
- ESPAsyncWebServer for non-blocking web server
- ArduinoJson v7 for configuration management
- Nanopb for Meshtastic protobuf support
- Custom 8MB partition table

[Unreleased]: https://github.com/synssins/RAKFlasher/compare/v1.2.4...HEAD
[1.2.4]: https://github.com/synssins/RAKFlasher/compare/v1.2.3...v1.2.4
[1.2.3]: https://github.com/synssins/RAKFlasher/compare/v1.2.2...v1.2.3
[1.2.2]: https://github.com/synssins/RAKFlasher/compare/v1.2.1...v1.2.2
[1.2.1]: https://github.com/synssins/RAKFlasher/compare/v1.2.0...v1.2.1
[1.2.0]: https://github.com/synssins/RAKFlasher/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/synssins/RAKFlasher/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/synssins/RAKFlasher/releases/tag/v1.0.0
