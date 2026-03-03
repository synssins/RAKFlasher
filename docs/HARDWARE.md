# Hardware Setup Guide

Detailed hardware setup and wiring instructions for ESP32-RAKFlasher.

## Bill of Materials (BOM)

### Required Components

| Component | Specification | Quantity | Notes |
|-----------|--------------|----------|-------|
| ESP32-S3 | Seeed XIAO ESP32-S3 or equivalent | 1 | 8MB flash minimum |
| RAK4631 | nRF52840-based Meshtastic module | 1 | With Adafruit bootloader |
| RAK19001 | WisBlock Base Board | 1 | Carrier for RAK4631 |
| USB-C Cable | Data + Power | 1 | For ESP32 programming |
| Jumper Wires | Dupont style | 8 | For connections |

### Optional Components

| Component | Purpose | Notes |
|-----------|---------|-------|
| USB Power Bank | Portable power | For field use |
| Enclosure | Protection | 3D printable STL available |
| Header Pins | Permanent connections | Instead of jumper wires |

## Wiring Connections

### Pin Mapping Table

**Pin assignments verified against Seeed XIAO ESP32-S3 and RAK19001 datasheets**

| ESP32-S3 GPIO | XIAO Pin | RAK19001 Pin | Signal Name | Type | Description |
|---------------|----------|--------------|-------------|------|-------------|
| GPIO43 | D6 | TXD1 | UART_TX | Output | Serial TX: ESP32 TX → RAK RX (nRF52 P0.15) |
| GPIO44 | D7 | RXD1 | UART_RX | Input | Serial RX: ESP32 RX ← RAK TX (nRF52 P0.16) |
| GPIO3 | D2 | RESET | RESET | Output | RAK reset control (active LOW) |
| GPIO4 | D3 | IO8 | WAKE | Input | Wake signal from RAK |
| GPIO7 | D8 | SWCLK | SWD_CLK | Bidirectional | SWD clock signal |
| GPIO8 | D9 | SWDIO | SWD_IO | Bidirectional | SWD data signal |
| GND | GND | GND | GND | Ground | Common ground |
| 3V3 | 3V3 | VDD | POWER | Power | Optional power supply |

**Important Notes:**
- TXD1/RXD1 labels on the RAK19001 are from the **baseboard's perspective** -- ESP32 TX goes to TXD1 (which routes to nRF52 P0.15/RX)
- RAK19001 UART1 (TXD1/RXD1) connects to nRF52840 P0.16/P0.15
- Meshtastic serial module must be configured: `serial.rxd=15 serial.txd=16 serial.mode=PROTO`
- GPS mode must be set to `NOT_PRESENT` to free P0.15/P0.16 for serial use
- GPIO43/44 are the dedicated UART pins (D6/D7) on the XIAO ESP32-S3
- All pin assignments are configurable via the WebUI Settings page

### Detailed Wiring Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                  ESP32-S3 (Seeed XIAO)                          │
│                                                                 │
│  USB-C                                                          │
│   ││                                                            │
│   ││  ┌────────┐                                                │
│   ││  │  CPU   │                                                │
│   ││  └────────┘                                                │
│   ││                                                            │
│   ││  Pin Layout (Top View):                                   │
│   ││                                                            │
│        5V  ●────────────────────────────●  GND                 │
│       GND  ●                            ●  3V3                 │
│      3V3   ●                            ●  GPIO0              │
│    GPIO1   ●───────RX───────────────────────────────┐         │
│    GPIO2   ●───────TX───────────────────────────────┼─┐       │
│    GPIO3   ●───────RESET────────────────────────────┼─┼─┐     │
│    GPIO4   ●───────WAKE─────────────────────────────┼─┼─┼─┐   │
│    GPIO5   ●                            ●  GPIO8─────┼─┼─┼─┼───SWD_IO
│    GPIO6   ●                            ●  GPIO7─────┼─┼─┼─┼───SWD_CLK
│   GPIO43   ●  (USB UART - Not Used)    ●  GPIO10    │ │ │ │   │
│   GPIO44   ●  (USB UART - Not Used)    ●  GPIO9     │ │ │ │   │
│            └──────────────────────────────────────────┼─┼─┼─┼─┐ │
└───────────────────────────────────────────────────────┼─┼─┼─┼─┼─┘
                                                        │ │ │ │ │
                                                        │ │ │ │ │
┌───────────────────────────────────────────────────────┼─┼─┼─┼─┼─┐
│                  RAK19001 WisBlock Base               │ │ │ │ │ │
│                                                       │ │ │ │ │ │
│  ┌────────────────────────────────┐                  │ │ │ │ │ │
│  │       RAK4631 Module            │                  │ │ │ │ │ │
│  │     (nRF52840 SoC)             │                  │ │ │ │ │ │
│  │  ┌──────────────────────┐      │                  │ │ │ │ │ │
│  │  │   Meshtastic FW      │      │                  │ │ │ │ │ │
│  │  │ Adafruit Bootloader  │      │                  │ │ │ │ │ │
│  │  └──────────────────────┘      │                  │ │ │ │ │ │
│  └────────────────────────────────┘                  │ │ │ │ │ │
│                                                       │ │ │ │ │ │
│  Expansion Pins:                                     │ │ │ │ │ │
│                                                       │ │ │ │ │ │
│  Pin 34 (RXD1) ●────────────────────────────────────<┘ │ │ │ │ │ (UART TX from ESP32)
│  Pin 33 (TXD1) ●────────────────────────────────────<──┘ │ │ │ │ (UART RX to ESP32)
│   RESET  ●──────────────────────────────────────────<────┘ │ │ │ (Reset control)
│   IO8    ●──────────────────────────────────────────<──────┘ │ │ (Wake signal)
│   SWCLK  ●──────────────────────────────────────────<────────┘ │ (SWD Clock)
│   SWDIO  ●──────────────────────────────────────────<──────────┘ (SWD Data)
│   GND    ●─────────────────────────────────Common Ground        │
│   VDD    ●─────────────────────────────────3.3V Power (opt)     │
│                                                                  │
│  ┌──────────────┐                                                │
│  │  USB Port    │  (For RAK programming if needed)               │
│  └──────────────┘                                                │
└──────────────────────────────────────────────────────────────────┘
```

## Step-by-Step Assembly

### 1. Prepare the RAK19001 Board
1. Insert RAK4631 module into the WisBlock slot
2. Ensure it's firmly seated and locked
3. Verify the module is recognized (check with RAK Serial Tool if unsure)

### 2. Wire the Serial Connections
1. **UART TX (ESP32 D6/GPIO43 → RAK TXD1)**
   - Connect jumper wire from ESP32 D6 (GPIO43) to RAK19001 TXD1 header pin
   - TXD1 label is from the baseboard perspective -- this routes to nRF52 P0.15 (RX)
   - This allows ESP32 to send data to RAK

2. **UART RX (ESP32 D7/GPIO44 ← RAK RXD1)**
   - Connect jumper wire from ESP32 D7 (GPIO44) to RAK19001 RXD1 header pin
   - RXD1 label is from the baseboard perspective -- this routes to nRF52 P0.16 (TX)
   - This allows ESP32 to receive data from RAK

### 3. Wire the Control Signals
1. **Reset (ESP32 D2/GPIO3 → RAK RESET)**
   - Connect jumper wire from ESP32 D2 (GPIO3) to RAK19001 RESET pin
   - This allows ESP32 to reset the RAK module

2. **Wake (ESP32 D3/GPIO4 ← RAK IO8)**
   - Connect jumper wire from ESP32 D3 (GPIO4) to RAK19001 IO8 pin
   - This allows RAK to wake ESP32 from deep sleep

### 4. Wire the SWD Interface (Recovery Only)
1. **SWD Clock (ESP32 GPIO7 ↔ RAK SWCLK)**
   - Connect jumper wire from ESP32 GPIO7 to RAK19001 SWCLK pin

2. **SWD Data (ESP32 GPIO8 ↔ RAK SWDIO)**
   - Connect jumper wire from ESP32 GPIO8 to RAK19001 SWDIO pin

### 5. Common Connections
1. **Ground (GND)**
   - Connect ESP32 GND to RAK19001 GND
   - **Critical:** This must be connected for any communication to work

2. **Power (Optional)**
   - If powering RAK from ESP32: Connect ESP32 3V3 to RAK VDD
   - Otherwise, power RAK via USB or battery

## Power Considerations

### Power Source Options

**Option 1: Independent Power (Recommended)**
- ESP32: USB-C power
- RAK: Battery or separate USB
- Advantages: No power issues, each device can be reset independently
- Disadvantages: Requires two power sources

**Option 2: ESP32 Powers RAK**
- ESP32: USB-C power (5V 1A minimum)
- RAK: Powered from ESP32 3V3 pin
- Advantages: Single power source
- Disadvantages: Limited current, may cause brownouts

**Option 3: Shared Battery**
- Both devices: Connected to common battery
- Requires voltage regulator if battery > 5V
- Good for portable operation

### Current Requirements
- **ESP32-S3**: ~80mA active, ~10µA deep sleep
- **RAK4631**: ~50mA active, ~2µA sleep
- **Total**: ~150mA when both active

## Verification Steps

### 1. Visual Inspection
- [ ] All wires are firmly connected
- [ ] No short circuits between adjacent pins
- [ ] GND is connected between both devices
- [ ] Polarity is correct for power connections

### 2. Continuity Test (Multimeter)
Test continuity between:
- [ ] ESP32 GND ↔ RAK GND
- [ ] ESP32 GPIO43 ↔ RAK IO6
- [ ] ESP32 GPIO44 ↔ RAK IO7
- [ ] ESP32 GPIO2 ↔ RAK RESET
- [ ] ESP32 GPIO1 ↔ RAK IO8
- [ ] ESP32 GPIO7 ↔ RAK SWCLK (if used)
- [ ] ESP32 GPIO8 ↔ RAK SWDIO (if used)

### 3. Power On Test
1. Power ESP32 via USB-C
2. Check for power LED on ESP32
3. Power RAK (if separate power)
4. Check for LED activity on RAK

### 4. Software Test
1. Flash ESP32-RAKFlasher firmware
2. Connect to WiFi AP
3. Open WebUI
4. Check Dashboard for RAK connection status
5. View Serial Monitor for RAK output

## Troubleshooting

### No Serial Data from RAK
- Check TX/RX not swapped
- Verify baud rate is 115200
- Ensure GND is connected
- Check RAK is powered

### Cannot Reset RAK
- Verify GPIO2 → RESET connection
- Check reset pin is not damaged
- Try manual reset button on RAK

### SWD Connection Failed
- Verify SWCLK and SWDIO connections
- Ensure RAK is powered
- Check for voltage level compatibility
- Try lower SWD clock speed

### ESP32 Doesn't Wake
- Check GPIO1 connection
- Verify wake signal polarity
- Test wake pin manually (connect to 3V3)

## Enclosure Design

A 3D-printable enclosure design is available in the `hardware/` directory.

### Features
- Cutouts for all USB ports
- Ventilation for heat dissipation
- Cable management channels
- Mounting holes for M3 screws
- Snap-fit assembly

### Printing Instructions
- Material: PLA or PETG
- Layer height: 0.2mm
- Infill: 20%
- Supports: Required for USB cutouts

## Safety Notes

⚠️ **Important Safety Information:**

1. **ESD Protection**: Handle boards with proper ESD precautions
2. **Voltage Levels**: Verify 3.3V logic levels on all pins
3. **Current Limits**: Don't exceed GPIO current ratings (40mA per pin)
4. **Short Circuits**: Double-check connections before powering on
5. **Hot Components**: Power regulators may get warm during operation

## Additional Resources

- [ESP32-S3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- [RAK4631 Documentation](https://docs.rakwireless.com/Product-Categories/WisBlock/RAK4631/Overview/)
- [RAK19001 Pinout](https://docs.rakwireless.com/Product-Categories/WisBlock/RAK19001/Datasheet/)
- [Seeed XIAO ESP32-S3](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
