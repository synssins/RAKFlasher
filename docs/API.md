# API Documentation

Complete REST API and WebSocket reference for ESP32-RAKFlasher.

## Base URL

```
http://192.168.4.1
```

## Authentication

Currently no authentication required. All endpoints are accessible within the local WiFi AP.

---

## System Endpoints

### Get System Status

Get current system status and statistics.

**Endpoint:** `GET /api/status`

**Response:**
```json
{
  "connected": true,
  "state": 0,
  "uptime": 12345,
  "clients": 2,
  "freeHeap": 245760,
  "version": "1.0.0"
}
```

**Fields:**
- `connected` (boolean): RAK connection status
- `state` (integer): System state (0=idle, 1=flashing, etc.)
- `uptime` (integer): Seconds since boot
- `clients` (integer): Connected WiFi clients
- `freeHeap` (integer): Free heap memory in bytes
- `version` (string): Firmware version

---

### Get Device Information

Get detailed hardware and firmware information.

**Endpoint:** `GET /api/info`

**Response:**
```json
{
  "firmware": "1.0.0",
  "chipModel": "ESP32-S3",
  "chipCores": 2,
  "cpuFreq": 240,
  "flashSize": 8388608,
  "freeHeap": 245760,
  "uptime": 12345
}
```

---

### Reboot ESP32

Restart the ESP32 microcontroller.

**Endpoint:** `POST /api/reboot`

**Response:**
```json
{
  "message": "Rebooting...",
  "success": true
}
```

---

### Enter Deep Sleep

Put ESP32 into deep sleep mode.

**Endpoint:** `POST /api/sleep`

**Response:**
```json
{
  "message": "Entering deep sleep...",
  "success": true
}
```

---

## Firmware Endpoints

### Upload Firmware

Upload a Nordic DFU package (.zip file).

**Endpoint:** `POST /api/firmware/upload`

**Content-Type:** `multipart/form-data`

**Body:**
- `firmware` (file): DFU package (.zip)

**Response:**
```json
{
  "success": true
}
```

---

### Get Firmware Info

Get information about uploaded firmware.

**Endpoint:** `GET /api/firmware/info`

**Response:**
```json
{
  "filename": "firmware.zip",
  "size": 524288,
  "uploaded": true
}
```

---

### Flash Firmware

Start firmware flashing process via serial DFU.

**Endpoint:** `POST /api/firmware/flash`

**Response:**
```json
{
  "message": "Flashing started",
  "success": true
}
```

**Note:** Progress updates sent via WebSocket.

---

### Delete Uploaded Firmware

Remove uploaded firmware file.

**Endpoint:** `DELETE /api/firmware/uploaded`

**Response:**
```json
{
  "message": "Firmware deleted",
  "success": true
}
```

---

## SWD Recovery Endpoints

### Test SWD Connection

Verify SWD connection to RAK module.

**Endpoint:** `GET /api/swd/connect`

**Response:**
```json
{
  "connected": true,
  "chipId": "0x52840"
}
```

---

### Mass Erase

Perform mass erase of chip flash.

**Endpoint:** `POST /api/swd/erase`

**Response:**
```json
{
  "message": "Mass erase complete",
  "success": true
}
```

---

### Flash via SWD

Flash bootloader or firmware via SWD.

**Endpoint:** `POST /api/swd/flash`

**Content-Type:** `multipart/form-data`

**Body:**
- `bootloader` (file): Bootloader binary (.hex or .bin)

**Response:**
```json
{
  "message": "Flashed successfully",
  "success": true
}
```

---

## Backup Endpoints

### Create Settings Backup

Backup all Meshtastic configuration.

**Endpoint:** `POST /api/backup/settings`

**Response:**
```json
{
  "filename": "settings_1709251234.json",
  "success": true
}
```

---

### Create Channels Backup

Backup channels and encryption keys.

**Endpoint:** `POST /api/backup/channels`

**Content-Type:** `application/json`

**Body:**
```json
{
  "password": "optional_encryption_password"
}
```

**Response:**
```json
{
  "filename": "channels_1709251234.enc",
  "success": true
}
```

---

### List Backups

Get list of all stored backups.

**Endpoint:** `GET /api/backup/list`

**Response:**
```json
{
  "backups": [
    {
      "type": "settings",
      "filename": "settings_1709251234.json",
      "size": 4096
    },
    {
      "type": "channels",
      "filename": "channels_1709251234.enc",
      "size": 1024
    }
  ]
}
```

---

### Delete Backup

Delete a specific backup file.

**Endpoint:** `DELETE /api/backup/delete?filename=backup.json`

**Query Parameters:**
- `filename` (string): Name of backup file to delete

**Response:**
```json
{
  "message": "Backup deleted",
  "success": true
}
```

---

### Restore Settings

Restore configuration from backup.

**Endpoint:** `POST /api/backup/settings/restore`

**Content-Type:** `multipart/form-data`

**Body:**
- `backup` (file): Settings backup file (.json)

**Response:**
```json
{
  "message": "Settings restored",
  "success": true
}
```

---

### Restore Channels

Restore channels from encrypted backup.

**Endpoint:** `POST /api/backup/channels/restore`

**Content-Type:** `multipart/form-data`

**Body:**
- `backup` (file): Channels backup file
- `password` (string): Decryption password (if encrypted)

**Response:**
```json
{
  "message": "Channels restored",
  "success": true
}
```

---

## Serial Endpoints

### Get Serial Buffer

Retrieve buffered serial output.

**Endpoint:** `GET /api/serial/buffer`

**Response:**
```json
{
  "buffer": "[12:34:56] Device started\n[12:34:57] Meshtastic ready\n"
}
```

---

### Send Serial Command

Send command to RAK module via serial.

**Endpoint:** `POST /api/serial/send`

**Content-Type:** `application/json`

**Body:**
```json
{
  "command": "info"
}
```

**Response:**
```json
{
  "message": "Command sent",
  "success": true
}
```

---

### Toggle Passthrough Mode

Enable or disable serial passthrough mode.

**Endpoint:** `POST /api/serial/passthrough`

**Content-Type:** `application/json`

**Body:**
```json
{
  "enable": true
}
```

**Response:**
```json
{
  "message": "Passthrough enabled",
  "success": true
}
```

---

## GPIO Endpoints

### Get GPIO Configuration

Retrieve current pin configuration.

**Endpoint:** `GET /api/gpio/config`

**Response:**
```json
{
  "0": {
    "gpio": 1,
    "mode": "INPUT",
    "activeLevel": "HIGH",
    "function": "Wake Signal"
  },
  "1": {
    "gpio": 2,
    "mode": "OUTPUT",
    "activeLevel": "LOW",
    "function": "RAK Reset"
  }
}
```

---

### Save GPIO Configuration

Update pin configuration.

**Endpoint:** `POST /api/gpio/config`

**Content-Type:** `application/json`

**Body:**
```json
{
  "0": {
    "gpio": 1,
    "mode": "INPUT",
    "activeLevel": "HIGH",
    "function": "Wake Signal"
  }
}
```

**Response:**
```json
{
  "message": "GPIO config updated",
  "success": true
}
```

---

### Test GPIO Pin

Test a specific GPIO pin.

**Endpoint:** `POST /api/gpio/test?gpio=5`

**Query Parameters:**
- `gpio` (integer): GPIO number to test

**Response:**
```json
{
  "gpio": 5,
  "state": 1
}
```

---

### Reset RAK Module

Perform hardware reset of RAK module.

**Endpoint:** `POST /api/gpio/reset`

**Response:**
```json
{
  "message": "RAK reset",
  "success": true
}
```

---

### Enter DFU Mode

Put RAK into DFU bootloader mode.

**Endpoint:** `POST /api/gpio/dfu`

**Response:**
```json
{
  "message": "DFU mode activated",
  "success": true
}
```

---

## Settings Endpoints

### Get All Settings

Retrieve all system settings.

**Endpoint:** `GET /api/settings`

**Response:**
```json
{
  "wifi": {
    "ssid": "RAKFlasher-1234",
    "password": "flasher123"
  },
  "system": {
    "sleepTimeout": 300000,
    "serialBuffer": 500
  }
}
```

---

### Update Settings

Save new settings.

**Endpoint:** `POST /api/settings`

**Content-Type:** `application/json`

**Body:**
```json
{
  "wifi": {
    "ssid": "MyRAKFlasher",
    "password": "newsecurepass",
    "channel": 6,
    "hidden": false
  },
  "system": {
    "sleepTimeout": 600000,
    "serialBuffer": 1000
  }
}
```

**Response:**
```json
{
  "message": "Settings updated",
  "success": true
}
```

---

### Factory Reset

Reset all settings to defaults.

**Endpoint:** `POST /api/settings/factory`

**Response:**
```json
{
  "message": "Factory reset initiated",
  "success": true
}
```

**Note:** Device will reboot after factory reset.

---

## WebSocket API

### Connection

**URL:** `ws://192.168.4.1/ws`

**Protocol:** Standard WebSocket

### Message Format

All WebSocket messages are JSON formatted:

```json
{
  "type": "message_type",
  "data": {},
  "timestamp": 1709251234
}
```

### Message Types

#### Progress Update

Sent during long-running operations.

```json
{
  "type": "progress",
  "operation": "firmware_flash",
  "progress": 45,
  "message": "Flashing... 45%",
  "timestamp": 1709251234
}
```

#### Serial Data

Real-time serial output from RAK module.

```json
{
  "type": "serial",
  "data": "[12:34:56] Device started\n",
  "timestamp": 1709251234
}
```

#### Status Update

System status change notification.

```json
{
  "type": "status",
  "data": {
    "connected": true,
    "state": 1
  },
  "timestamp": 1709251234
}
```

---

## Error Responses

All endpoints return error responses in this format:

```json
{
  "error": "Error message",
  "success": false
}
```

**Common HTTP Status Codes:**
- `200` - Success
- `400` - Bad Request
- `404` - Not Found
- `500` - Internal Server Error

---

## Rate Limiting

No rate limiting currently implemented. Use responsibly.

---

## Examples

### cURL Examples

**Get Status:**
```bash
curl http://192.168.4.1/api/status
```

**Upload Firmware:**
```bash
curl -X POST http://192.168.4.1/api/firmware/upload \
  -F "firmware=@firmware.zip"
```

**Send Serial Command:**
```bash
curl -X POST http://192.168.4.1/api/serial/send \
  -H "Content-Type: application/json" \
  -d '{"command": "info"}'
```

### JavaScript Examples

**Fetch Status:**
```javascript
const response = await fetch('http://192.168.4.1/api/status');
const data = await response.json();
console.log('System uptime:', data.uptime);
```

**Upload File:**
```javascript
const formData = new FormData();
formData.append('firmware', fileInput.files[0]);

const response = await fetch('http://192.168.4.1/api/firmware/upload', {
  method: 'POST',
  body: formData
});
```

**WebSocket Connection:**
```javascript
const ws = new WebSocket('ws://192.168.4.1/ws');

ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  console.log('Received:', data);
};
```

### Python Examples

**Get System Info:**
```python
import requests

response = requests.get('http://192.168.4.1/api/info')
info = response.json()
print(f"Firmware: {info['firmware']}")
```

**Upload and Flash:**
```python
import requests

# Upload
with open('firmware.zip', 'rb') as f:
    files = {'firmware': f}
    response = requests.post('http://192.168.4.1/api/firmware/upload', files=files)

# Flash
response = requests.post('http://192.168.4.1/api/firmware/flash')
print(response.json())
```
