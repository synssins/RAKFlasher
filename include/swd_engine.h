/**
 * @file swd_engine.h
 * @brief SWD (Serial Wire Debug) interface for nRF52840 recovery
 *
 * Based on atc1441/ESP32_nRF52_SWD implementation.
 * Provides low-level SWD access for chip recovery, firmware backup, and restore.
 */
#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <soc/gpio_struct.h>
#include "config.h"

// ── AP register addresses (used with swdTransfer) ──────────────────────
#define AP_CSW      0x00
#define AP_TAR      0x04
#define AP_DRW      0x0C
#define AP_BD0      0x10
#define AP_BD1      0x14
#define AP_BD2      0x18
#define AP_BD3      0x1C
#define AP_IDR      0xFC

// ── DP register addresses ──────────────────────────────────────────────
#define DP_IDCODE   0x00
#define DP_ABORT    0x00
#define DP_CTRLSTAT 0x04
#define DP_SELECT   0x08
#define DP_RDBUFF   0x0C

// ── nRF52 Control-AP registers (bank 1) ────────────────────────────────
#define AP_NRF_RESET            0x00
#define AP_NRF_ERASEALL         0x04
#define AP_NRF_ERASEALLSTATUS   0x08
#define AP_NRF_APPROTECTSTATUS  0x0C

// ── CSW values ─────────────────────────────────────────────────────────
#define CSW_32BIT_AUTO_INC  0xA2000012
#define CSW_32BIT_NO_INC    0xA2000002

// ── nRF52840 memory map addresses ──────────────────────────────────────
#define NRF_FICR_BASE       0x10000000
#define NRF_UICR_BASE       0x10001000
#define NRF_UICR_SIZE       0x1000      // 4KB

#define NRF_NVMC_BASE       0x4001E000
#define NRF_NVMC_READY      (NRF_NVMC_BASE + 0x400)
#define NRF_NVMC_CONFIG     (NRF_NVMC_BASE + 0x504)
#define NRF_NVMC_ERASEPAGE  (NRF_NVMC_BASE + 0x508)
#define NRF_NVMC_ERASEALL   (NRF_NVMC_BASE + 0x50C)

// ── FICR register addresses ────────────────────────────────────────────
#define NRF_FICR_CODEPAGESIZE   0x10000010
#define NRF_FICR_CODESIZE       0x10000014
#define NRF_FICR_CONFIGID       0x1000005C
#define NRF_FICR_DEVICEID0      0x10000060
#define NRF_FICR_DEVICEID1      0x10000064
#define NRF_FICR_DEVICEADDR0    0x100000A4
#define NRF_FICR_DEVICEADDR1    0x100000A8
#define NRF_FICR_INFO_PART      0x10000100
#define NRF_FICR_INFO_VARIANT   0x10000104
#define NRF_FICR_INFO_PACKAGE   0x10000108

// ── Cortex-M4 debug registers ──────────────────────────────────────────
#define CORTEX_DHCSR           0xE000EDF0

// ── SoftDevice info ────────────────────────────────────────────────────
#define NRF_SD_INFO_AREA       0x0000300C

// ── Device info struct ─────────────────────────────────────────────────
struct DeviceInfo {
    uint32_t codepageSize;
    uint32_t codesize;
    uint32_t flashSize;
    uint32_t configId;
    uint32_t deviceId0;
    uint32_t deviceId1;
    uint32_t infoPart;
    uint32_t infoVariant;
    uint32_t infoPackage;
    uint16_t sdInfoArea;
    uint32_t uicrLock;
    bool     valid;
};

class SWDEngine {
public:
    SWDEngine();

    // ── Lifecycle ──────────────────────────────────────────────────────
    bool begin();
    bool connect();
    bool isConnected() const { return m_connected; }

    // ── Chip info ──────────────────────────────────────────────────────
    uint32_t getChipID();
    bool readDeviceInfo();
    const DeviceInfo& getDeviceInfo() const { return m_devInfo; }
    uint32_t getFlashSize() const { return m_devInfo.flashSize; }
    uint32_t getPageSize() const { return m_devInfo.codepageSize; }
    String getDeviceIDString() const;
    bool isDeviceUnlocked();

    // ── Erase operations ───────────────────────────────────────────────
    bool massErase();

    // ── Flash data to address ──────────────────────────────────────────
    bool flashData(uint32_t address, const uint8_t* data, size_t len,
                   void (*progressCallback)(int, const String&) = nullptr);

    // ── File-based operations ──────────────────────────────────────────
    bool flashBootloader(const String& filepath);
    bool flashFromFile(const String& filepath, uint32_t targetAddr = 0,
                       void (*progressCb)(int, const String&) = nullptr);
    bool dumpFlash(const String& filepath,
                   void (*progressCb)(int, const String&) = nullptr);
    bool dumpUICR(const String& filepath);

    // ── Raw memory access ──────────────────────────────────────────────
    bool readMemory(uint32_t address, uint8_t* buffer, size_t len);
    bool writeMemory(uint32_t address, const uint8_t* data, size_t len);

    // ── Error reporting ────────────────────────────────────────────────
    String getLastError() const { return m_lastError; }

private:
    bool m_connected;
    uint8_t m_pinCLK;
    uint8_t m_pinIO;
    uint32_t m_clkMask;     // Precomputed (1 << m_pinCLK)
    uint32_t m_ioMask;      // Precomputed (1 << m_pinIO)
    String m_lastError;
    bool m_turnState;       // 1 = write/output, 0 = read/input
    DeviceInfo m_devInfo;

    // ── Fast GPIO helpers (direct register access) ────────────────────
    inline void clkLow()  { GPIO.out_w1tc = m_clkMask; }
    inline void clkHigh() { GPIO.out_w1ts = m_clkMask; }
    inline void ioLow()   { GPIO.out_w1tc = m_ioMask; }
    inline void ioHigh()  { GPIO.out_w1ts = m_ioMask; }
    inline void ioSet(bool val) { if (val) GPIO.out_w1ts = m_ioMask; else GPIO.out_w1tc = m_ioMask; }
    inline bool ioRead()  { return (GPIO.in >> m_pinIO) & 1; }

    // ── Fast direction switching (direct register, ~10ns vs ~100-200µs for pinMode) ─
    inline void ioOutput() { GPIO.enable_w1ts = m_ioMask; }
    inline void ioInput()  { GPIO.enable_w1tc = m_ioMask; }

    // ── Minimal SWD clock delay (~100ns at 240MHz) ──────────────────
    // 24 NOPs ≈ 100ns — enough for SWD signal settling without the
    // massive overhead of delayMicroseconds() which adds 3-5µs per call
    static inline void __attribute__((always_inline)) swdDelay() {
        __asm__ __volatile__(
            "nop; nop; nop; nop; nop; nop; nop; nop;"
            "nop; nop; nop; nop; nop; nop; nop; nop;"
            "nop; nop; nop; nop; nop; nop; nop; nop;"
        );
    }

    // ── Low-level SWD I/O ──────────────────────────────────────────────
    void swdWrite(uint32_t data, uint8_t bits);
    uint32_t swdRead(uint8_t bits);
    void swdTurn(bool writeMode);

    // ── SWD protocol ───────────────────────────────────────────────────
    bool swdTransfer(uint8_t portAddr, bool apNotDp, bool readNotWrite, uint32_t& data);
    static bool calcParity(uint32_t value);

    // ── AP/DP register access (15-retry wrappers) ──────────────────────
    bool apWrite(uint8_t addr, uint32_t data);
    bool apRead(uint8_t addr, uint32_t& data);
    bool dpWrite(uint8_t addr, uint32_t data);
    bool dpRead(uint8_t addr, uint32_t& data);

    // ── nRF register read/write (via TAR+DRW) ─────────────────────────
    uint32_t readReg(uint32_t address);
    void writeReg(uint32_t address, uint32_t value);

    // ── Bank read/write (auto-increment bulk transfer) ─────────────────
    bool readBank(uint32_t address, uint32_t* buffer, size_t len);
    bool writeBank(uint32_t address, const uint32_t* buffer, size_t len);

    // ── NVMC helpers ───────────────────────────────────────────────────
    bool waitNVMCReady(uint32_t timeout = 1000);

    // ── CPU/port control ───────────────────────────────────────────────
    void haltCPU();
    void abortAll();
    void portSelect(bool nrfPort);
    void softReset();
};
