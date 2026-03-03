/**
 * @file swd_engine.cpp
 * @brief SWD engine implementation based on atc1441/ESP32_nRF52_SWD
 *
 * Ported from the reference implementation with proper AP/DP register access,
 * turnaround tracking, auto-increment bulk transfers, and firmware dump/flash.
 */

#include "swd_engine.h"
#include "gpio_control.h"
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern GPIOControl gpioControl;

// ════════════════════════════════════════════════════════════════════════
//  Constructor / Lifecycle
// ════════════════════════════════════════════════════════════════════════

SWDEngine::SWDEngine()
    : m_connected(false)
    , m_pinCLK(DEFAULT_PIN_SWD_CLK)
    , m_pinIO(DEFAULT_PIN_SWD_IO)
    , m_clkMask(1UL << DEFAULT_PIN_SWD_CLK)
    , m_ioMask(1UL << DEFAULT_PIN_SWD_IO)
    , m_turnState(false)
    , m_devInfo{} {
}

bool SWDEngine::begin() {
    PinConfig clkPin = gpioControl.getPinConfig(PIN_SWD_CLK);
    PinConfig ioPin  = gpioControl.getPinConfig(PIN_SWD_IO);

    m_pinCLK  = clkPin.gpio;
    m_pinIO   = ioPin.gpio;
    m_clkMask = 1UL << m_pinCLK;
    m_ioMask  = 1UL << m_pinIO;

    pinMode(m_pinCLK, OUTPUT);
    pinMode(m_pinIO, INPUT_PULLUP);

    DEBUG_PRINTLN("[SWD] Interface initialized (fast GPIO)");
    return true;
}

bool SWDEngine::connect() {
    DEBUG_PRINTLN("[SWD] Attempting to connect...");

    m_connected = false;
    m_turnState = false;

    // Ensure correct initial pin states
    pinMode(m_pinCLK, OUTPUT);
    pinMode(m_pinIO, INPUT_PULLUP);

    // SWD line reset: 64 clock cycles with SWDIO high
    swdWrite(0xFFFFFFFF, 32);
    swdWrite(0xFFFFFFFF, 32);

    // JTAG-to-SWD switching sequence: 0xE79E (16 bits, LSB first)
    swdWrite(0xE79E, 16);

    // Another line reset
    swdWrite(0xFFFFFFFF, 32);
    swdWrite(0xFFFFFFFF, 32);

    // Idle cycles
    swdWrite(0, 32);
    swdWrite(0, 32);

    // Read IDCODE to verify connection
    uint32_t idcode = 0;
    dpRead(DP_IDCODE, idcode);

    if (idcode != 0 && idcode != 0xFFFFFFFF) {
        m_connected = true;
        DEBUG_PRINTF("[SWD] Connected! Chip ID: 0x%08X\n", idcode);

        // Clear sticky error flags
        abortAll();

        // Check lock state via nRF Control-AP
        if (isDeviceUnlocked()) {
            DEBUG_PRINTLN("[SWD] Device is unlocked — full access");
            haltCPU();
            readDeviceInfo();
        } else {
            DEBUG_PRINTLN("[SWD] Device is LOCKED — limited access");
        }

        return true;
    }

    m_lastError = "No response from chip";
    DEBUG_PRINTF("[SWD] Connection failed (got 0x%08X)\n", idcode);
    return false;
}

// ════════════════════════════════════════════════════════════════════════
//  Low-level SWD I/O (ported from reference swd.cpp)
// ════════════════════════════════════════════════════════════════════════

void SWDEngine::swdTurn(bool writeMode) {
    // Turnaround: clock one cycle while switching SWDIO direction
    // Uses direct GPIO enable register writes (~10ns) instead of
    // pinMode() which reconfigures the entire GPIO matrix (~100-200µs)
    ioHigh();
    ioInput();          // Was: pinMode(m_pinIO, INPUT_PULLUP)
    clkLow();
    swdDelay();
    clkHigh();
    swdDelay();
    if (writeMode) {
        ioOutput();     // Was: pinMode(m_pinIO, OUTPUT)
    }
    m_turnState = writeMode;
}

void SWDEngine::swdWrite(uint32_t data, uint8_t bits) {
    if (!m_turnState) {
        swdTurn(true);  // Switch to write/output mode
    }
    while (bits--) {
        ioSet(data & 1);
        clkLow();
        swdDelay();
        data >>= 1;
        clkHigh();
        swdDelay();
    }
}

uint32_t SWDEngine::swdRead(uint8_t bits) {
    uint32_t outData = 0;
    uint32_t inputBit = 1;
    if (m_turnState) {
        swdTurn(false);  // Switch to read/input mode
    }
    while (bits--) {
        if (ioRead()) {
            outData |= inputBit;
        }
        clkLow();
        swdDelay();
        inputBit <<= 1;
        clkHigh();
        swdDelay();
    }
    return outData;
}

// ════════════════════════════════════════════════════════════════════════
//  SWD Protocol: Transfer function (ported from reference)
// ════════════════════════════════════════════════════════════════════════

bool SWDEngine::calcParity(uint32_t value) {
    value = (value & 0xFFFF) ^ (value >> 16);
    value = (value & 0xFF)   ^ (value >> 8);
    value = (value & 0xF)    ^ (value >> 4);
    value = (value & 0x3)    ^ (value >> 2);
    value = (value & 0x1)    ^ (value >> 1);
    return value & 1;
}

bool SWDEngine::swdTransfer(uint8_t portAddr, bool apNotDp, bool readNotWrite, uint32_t& data) {
    // Construct 8-bit request:
    // Bit 0: Start (always 1)
    // Bit 1: APnDP
    // Bit 2: RnW
    // Bit 3-4: ADDR[2:3]
    // Bit 5: Parity
    // Bit 6: Stop (always 0)
    // Bit 7: Park (always 1)
    bool parity = apNotDp ^ readNotWrite ^ ((portAddr >> 2) & 1) ^ ((portAddr >> 3) & 1);
    uint8_t request = (1 << 0)
                    | ((uint8_t)apNotDp << 1)
                    | ((uint8_t)readNotWrite << 2)
                    | ((portAddr & 0x0C) << 1)
                    | ((uint8_t)parity << 5)
                    | (1 << 7);

    swdWrite(request, 8);

    // Read 3-bit ACK
    if (swdRead(3) == 1) {
        if (readNotWrite) {
            // Read 32-bit data from target
            data = swdRead(32);
            if (swdRead(1) == calcParity(data)) {
                swdWrite(0, 1);  // Idle cycle
                return true;
            }
        } else {
            // Write 32-bit data to target
            swdWrite(data, 32);
            swdWrite(calcParity(data), 1);
            swdWrite(0, 1);  // Idle cycle
            return true;
        }
    }

    // On failure, send idle clocks to recover
    swdWrite(0, 32);
    return false;
}

// ════════════════════════════════════════════════════════════════════════
//  AP/DP Register Access (15-retry wrappers)
// ════════════════════════════════════════════════════════════════════════

bool SWDEngine::apWrite(uint8_t addr, uint32_t data) {
    for (int retry = 0; retry < 15; retry++) {
        if (swdTransfer(addr, true, false, data)) return true;
    }
    return false;
}

bool SWDEngine::apRead(uint8_t addr, uint32_t& data) {
    for (int retry = 0; retry < 15; retry++) {
        if (swdTransfer(addr, true, true, data)) return true;
    }
    return false;
}

bool SWDEngine::dpWrite(uint8_t addr, uint32_t data) {
    for (int retry = 0; retry < 15; retry++) {
        if (swdTransfer(addr, false, false, data)) return true;
    }
    return false;
}

bool SWDEngine::dpRead(uint8_t addr, uint32_t& data) {
    for (int retry = 0; retry < 15; retry++) {
        if (swdTransfer(addr, false, true, data)) return true;
    }
    return false;
}

// ════════════════════════════════════════════════════════════════════════
//  CPU/Port Control
// ════════════════════════════════════════════════════════════════════════

void SWDEngine::abortAll() {
    uint32_t temp = 0;
    // Clear all sticky error flags
    dpWrite(DP_ABORT, 0x1E);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);
    // Power up debug + system
    dpWrite(DP_CTRLSTAT, 0x50000000);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);
}

void SWDEngine::haltCPU() {
    // Write to DHCSR to halt the Cortex-M4 core
    apWrite(AP_CSW, CSW_32BIT_NO_INC);
    apWrite(AP_TAR, CORTEX_DHCSR);
    // Send halt command repeatedly to ensure it takes effect
    for (int i = 0; i < 500; i++) {
        apWrite(AP_DRW, 0xA05F0003);
    }
    DEBUG_PRINTLN("[SWD] CPU halted");
}

void SWDEngine::portSelect(bool nrfPort) {
    // Select AP: 0 = AHB-AP (memory access), 1 = nRF Control-AP
    dpWrite(DP_SELECT, nrfPort ? 0x01000000 : 0x00);
}

void SWDEngine::softReset() {
    portSelect(true);  // nRF Control-AP
    uint32_t temp = 0;
    apWrite(AP_NRF_RESET, 1);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);
    delay(100);
    apWrite(AP_NRF_RESET, 0);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);
    portSelect(false);  // Back to AHB-AP
}

bool SWDEngine::isDeviceUnlocked() {
    uint32_t temp = 0;
    portSelect(true);  // nRF Control-AP
    apRead(AP_NRF_APPROTECTSTATUS, temp);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);
    DEBUG_PRINTF("[SWD] APPROTECTSTATUS: 0x%08X\n", temp);
    portSelect(false);  // Back to AHB-AP
    return (temp & 1);  // Bit 0 = 1 means unlocked
}

// ════════════════════════════════════════════════════════════════════════
//  nRF Register Read/Write (single 32-bit via TAR+DRW)
// ════════════════════════════════════════════════════════════════════════

uint32_t SWDEngine::readReg(uint32_t address) {
    uint32_t temp = 0;
    apWrite(AP_TAR, address);
    apRead(AP_DRW, temp);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);
    return temp;
}

void SWDEngine::writeReg(uint32_t address, uint32_t value) {
    uint32_t temp = 0;
    apWrite(AP_TAR, address);
    apWrite(AP_DRW, value);
    dpRead(DP_RDBUFF, temp);
}

// ════════════════════════════════════════════════════════════════════════
//  Bank Read/Write (auto-increment bulk transfer)
// ════════════════════════════════════════════════════════════════════════

bool SWDEngine::readBank(uint32_t address, uint32_t* buffer, size_t len) {
    if (!m_connected) return false;

    uint32_t temp;

    // Set CSW for 32-bit, auto-increment
    apWrite(AP_CSW, CSW_32BIT_AUTO_INC);
    // Set starting address
    apWrite(AP_TAR, address);
    // Dummy read to prime the pipeline
    apRead(AP_DRW, temp);

    // Read data words
    for (size_t i = 0; i < len; i += 4) {
        uint32_t word = 0;
        apRead(AP_DRW, word);
        buffer[i / 4] = word;
    }

    // Reset CSW and flush
    apWrite(AP_CSW, CSW_32BIT_NO_INC);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);

    return true;
}

bool SWDEngine::writeBank(uint32_t address, const uint32_t* buffer, size_t len) {
    if (!m_connected) return false;

    uint32_t temp;

    // Enable NVMC write mode
    writeReg(NRF_NVMC_CONFIG, 1);
    // Wait for NVMC ready
    unsigned long timeout = millis();
    while (readReg(NRF_NVMC_READY) != 1) {
        if (millis() - timeout > 100) {
            m_lastError = "NVMC not ready for write";
            return false;
        }
    }

    // Set CSW for 32-bit, auto-increment
    apWrite(AP_CSW, CSW_32BIT_AUTO_INC);
    // Set starting address
    apWrite(AP_TAR, address);

    // Write data words with 400µs inter-word delay for flash programming
    for (size_t i = 0; i < len; i += 4) {
        unsigned long endMicros = micros() + 400;
        apWrite(AP_DRW, buffer[i / 4]);
        while (micros() < endMicros) { /* wait */ }
    }

    // Reset CSW and flush
    apWrite(AP_CSW, CSW_32BIT_NO_INC);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);

    // Disable NVMC write mode
    writeReg(NRF_NVMC_CONFIG, 0);
    timeout = millis();
    while (readReg(NRF_NVMC_READY) != 1) {
        if (millis() - timeout > 100) {
            m_lastError = "NVMC not ready after write";
            return false;
        }
    }

    return true;
}

// ════════════════════════════════════════════════════════════════════════
//  Public Memory Access
// ════════════════════════════════════════════════════════════════════════

bool SWDEngine::readMemory(uint32_t address, uint8_t* buffer, size_t len) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }

    // Align to 4-byte boundary
    uint32_t alignedAddr = address & ~3UL;
    size_t offset = address - alignedAddr;
    size_t alignedLen = ((offset + len + 3) / 4) * 4;

    // Allocate word-aligned buffer
    uint32_t* wordBuf = (uint32_t*)malloc(alignedLen);
    if (!wordBuf) {
        m_lastError = "Out of memory";
        return false;
    }

    bool ok = readBank(alignedAddr, wordBuf, alignedLen);
    if (ok) {
        memcpy(buffer, ((uint8_t*)wordBuf) + offset, len);
    }

    free(wordBuf);
    return ok;
}

bool SWDEngine::writeMemory(uint32_t address, const uint8_t* data, size_t len) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }

    // Align to 4-byte boundary
    uint32_t alignedAddr = address & ~3UL;
    size_t offset = address - alignedAddr;
    size_t alignedLen = ((offset + len + 3) / 4) * 4;

    // For simple aligned writes, cast directly
    if (offset == 0 && (len % 4 == 0)) {
        return writeBank(address, (const uint32_t*)data, len);
    }

    // Handle unaligned: read-modify-write
    uint32_t* wordBuf = (uint32_t*)malloc(alignedLen);
    if (!wordBuf) {
        m_lastError = "Out of memory";
        return false;
    }

    // Read existing data for partial words at boundaries
    readBank(alignedAddr, wordBuf, alignedLen);
    // Overlay new data
    memcpy(((uint8_t*)wordBuf) + offset, data, len);

    bool ok = writeBank(alignedAddr, wordBuf, alignedLen);
    free(wordBuf);
    return ok;
}

// ════════════════════════════════════════════════════════════════════════
//  NVMC Wait
// ════════════════════════════════════════════════════════════════════════

bool SWDEngine::waitNVMCReady(uint32_t timeout) {
    unsigned long startTime = millis();
    while (millis() - startTime < timeout) {
        if (readReg(NRF_NVMC_READY) & 0x01) {
            return true;
        }
        delay(10);
    }
    return false;
}

// ════════════════════════════════════════════════════════════════════════
//  Chip Info
// ════════════════════════════════════════════════════════════════════════

uint32_t SWDEngine::getChipID() {
    uint32_t idcode = 0;
    dpRead(DP_IDCODE, idcode);
    return idcode;
}

bool SWDEngine::readDeviceInfo() {
    m_devInfo.codepageSize = readReg(NRF_FICR_CODEPAGESIZE);
    m_devInfo.codesize     = readReg(NRF_FICR_CODESIZE);
    m_devInfo.flashSize    = m_devInfo.codepageSize * m_devInfo.codesize;
    m_devInfo.configId     = readReg(NRF_FICR_CONFIGID);
    m_devInfo.deviceId0    = readReg(NRF_FICR_DEVICEID0);
    m_devInfo.deviceId1    = readReg(NRF_FICR_DEVICEID1);
    m_devInfo.infoPart     = readReg(NRF_FICR_INFO_PART);
    m_devInfo.infoVariant  = readReg(NRF_FICR_INFO_VARIANT);
    m_devInfo.infoPackage  = readReg(NRF_FICR_INFO_PACKAGE);
    m_devInfo.sdInfoArea   = readReg(NRF_SD_INFO_AREA) & 0xFFFF;
    m_devInfo.uicrLock     = readReg(0x10001208);
    m_devInfo.valid        = true;

    DEBUG_PRINTF("[SWD] Flash: %u bytes (%u pages x %u bytes)\n",
                 m_devInfo.flashSize, m_devInfo.codesize, m_devInfo.codepageSize);
    DEBUG_PRINTF("[SWD] Device ID: %08X-%08X\n", m_devInfo.deviceId0, m_devInfo.deviceId1);
    DEBUG_PRINTF("[SWD] Part: 0x%08X  Variant: 0x%08X\n", m_devInfo.infoPart, m_devInfo.infoVariant);

    return m_devInfo.flashSize > 0 && m_devInfo.codepageSize > 0;
}

String SWDEngine::getDeviceIDString() const {
    char buf[20];
    snprintf(buf, sizeof(buf), "%08X%08X", m_devInfo.deviceId0, m_devInfo.deviceId1);
    return String(buf);
}

// ════════════════════════════════════════════════════════════════════════
//  Mass Erase (via nRF Control-AP — more reliable than NVMC)
// ════════════════════════════════════════════════════════════════════════

bool SWDEngine::massErase() {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }

    DEBUG_PRINTLN("[SWD] Performing mass erase via Control-AP...");

    uint32_t temp = 0;

    // Switch to nRF Control-AP
    portSelect(true);

    // Trigger erase all
    apWrite(AP_NRF_ERASEALL, 1);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);

    delayMicroseconds(100);

    // Wait for erase to complete (poll ERASEALLSTATUS)
    unsigned long startTime = millis();
    while (true) {
        temp = 0;
        apRead(AP_NRF_ERASEALLSTATUS, temp);
        dpRead(DP_RDBUFF, temp);
        dpRead(DP_RDBUFF, temp);
        if (temp == 0) break;  // Erase complete

        if (millis() - startTime > 10000) {
            m_lastError = "Erase timeout";
            portSelect(false);
            return false;
        }
        delay(10);
    }

    // Clear erase command
    apWrite(AP_NRF_ERASEALL, 0);
    dpRead(DP_RDBUFF, temp);
    dpRead(DP_RDBUFF, temp);

    // Back to AHB-AP
    portSelect(false);

    // Soft reset and reconnect
    softReset();

    // Re-init connection after erase
    delay(100);
    connect();

    DEBUG_PRINTLN("[SWD] Mass erase complete");
    return true;
}

// ════════════════════════════════════════════════════════════════════════
//  Flash Data to Address
// ════════════════════════════════════════════════════════════════════════

bool SWDEngine::flashData(uint32_t address, const uint8_t* data, size_t len,
                          void (*progressCallback)(int, const String&)) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }

    DEBUG_PRINTF("[SWD] Flashing %d bytes to 0x%08X...\n", len, address);

    size_t written = 0;
    while (written < len) {
        size_t chunkSize = min((size_t)4096, len - written);

        if (!writeBank(address + written, (const uint32_t*)(data + written), chunkSize)) {
            m_lastError = "Write failed at offset " + String(written);
            return false;
        }

        written += chunkSize;

        if (progressCallback) {
            int progress = (written * 100) / len;
            progressCallback(progress, "Writing...");
        }
    }

    DEBUG_PRINTLN("[SWD] Flash complete");
    return true;
}

// ════════════════════════════════════════════════════════════════════════
//  File-based Operations
// ════════════════════════════════════════════════════════════════════════

bool SWDEngine::dumpFlash(const String& filepath,
                          void (*progressCb)(int, const String&)) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }

    if (!m_devInfo.valid || m_devInfo.flashSize == 0) {
        m_lastError = "Device info not available — connect first";
        return false;
    }

    uint32_t flashSize = m_devInfo.flashSize;

    File file = LittleFS.open(filepath, "w");
    if (!file) {
        m_lastError = "Failed to create file: " + filepath;
        return false;
    }

    DEBUG_PRINTF("[SWD] Dumping %u bytes of flash to %s\n", flashSize, filepath.c_str());
    unsigned long startMs = millis();
    unsigned long chunkStartMs = millis();

    uint8_t buffer[4096];
    uint32_t totalChunks = (flashSize + 4095) / 4096;
    uint32_t chunkNum = 0;

    for (uint32_t offset = 0; offset < flashSize; offset += 4096) {
        uint32_t chunkLen = min((uint32_t)4096, flashSize - offset);

        if (!readBank(offset, (uint32_t*)buffer, chunkLen)) {
            m_lastError = "Read failed at offset " + String(offset);
            file.close();
            return false;
        }

        file.write(buffer, chunkLen);
        chunkNum++;

        if (progressCb) {
            int pct = ((offset + chunkLen) * 100) / flashSize;
            unsigned long now = millis();
            unsigned long elapsed = now - startMs;
            float kbps = (elapsed > 0) ? ((float)(offset + chunkLen) / elapsed) : 0;
            char msg[64];
            snprintf(msg, sizeof(msg), "Reading flash... %.1f KB/s", kbps);
            progressCb(pct, String(msg));
        }

        // Yield every 8 chunks to feed watchdog without excessive context switching
        if (chunkNum % 8 == 0) {
            vTaskDelay(1);  // Minimal yield — 1 tick
        }
    }

    file.close();

    unsigned long elapsed = millis() - startMs;
    float speed = (elapsed > 0) ? ((float)flashSize / (float)elapsed) : 0;
    DEBUG_PRINTF("[SWD] Flash dump complete: %ums (%.1f KB/s)\n", elapsed, speed);

    return true;
}

bool SWDEngine::dumpUICR(const String& filepath) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }

    File file = LittleFS.open(filepath, "w");
    if (!file) {
        m_lastError = "Failed to create file: " + filepath;
        return false;
    }

    DEBUG_PRINTF("[SWD] Dumping UICR to %s\n", filepath.c_str());

    uint8_t buffer[NRF_UICR_SIZE];
    if (!readBank(NRF_UICR_BASE, (uint32_t*)buffer, NRF_UICR_SIZE)) {
        m_lastError = "UICR read failed";
        file.close();
        return false;
    }

    file.write(buffer, NRF_UICR_SIZE);
    file.close();

    DEBUG_PRINTLN("[SWD] UICR dump complete");
    return true;
}

bool SWDEngine::flashFromFile(const String& filepath, uint32_t targetAddr,
                              void (*progressCb)(int, const String&)) {
    if (!m_connected) {
        m_lastError = "Not connected";
        return false;
    }

    File file = LittleFS.open(filepath, "r");
    if (!file) {
        m_lastError = "File not found: " + filepath;
        return false;
    }

    size_t fileSize = file.size();
    DEBUG_PRINTF("[SWD] Flashing %u bytes from %s to 0x%08X\n",
                 fileSize, filepath.c_str(), targetAddr);

    unsigned long startMs = millis();
    uint8_t buffer[4096];

    for (size_t offset = 0; offset < fileSize; offset += 4096) {
        size_t chunkLen = min((size_t)4096, fileSize - offset);
        size_t bytesRead = file.read(buffer, chunkLen);

        // Pad to 4-byte alignment if needed
        while (bytesRead % 4 != 0) {
            buffer[bytesRead++] = 0xFF;
        }

        if (!writeBank(targetAddr + offset, (const uint32_t*)buffer, bytesRead)) {
            m_lastError = "Write failed at offset " + String(offset);
            file.close();
            return false;
        }

        if (progressCb) {
            int pct = ((offset + chunkLen) * 100) / fileSize;
            progressCb(pct, "Writing flash...");
        }

        yield();  // Feed watchdog
    }

    file.close();

    unsigned long elapsed = millis() - startMs;
    DEBUG_PRINTF("[SWD] Flash from file complete: %ums\n", elapsed);

    return true;
}

bool SWDEngine::flashBootloader(const String& filepath) {
    // Bootloader is flashed starting at address 0
    return flashFromFile(filepath, 0);
}
