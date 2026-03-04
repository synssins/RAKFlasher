/**
 * @file gpio_control.h
 * @brief GPIO pin configuration and control
 */
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"

struct PinConfig {
    uint8_t gpio;
    uint8_t mode;        // INPUT, OUTPUT, INPUT_PULLUP, INPUT_PULLDOWN
    uint8_t activeLevel; // HIGH or LOW
    String function;     // Description of function
};

class GPIOControl {
public:
    GPIOControl();

    /**
     * @brief Initialize GPIO pins
     * @return true if successful
     */
    bool begin();

    /**
     * @brief Get pin configuration for a function
     * @param func Pin function enum
     * @return Pin configuration
     */
    PinConfig getPinConfig(PinFunction func);

    /**
     * @brief Set pin configuration
     * @param func Pin function enum
     * @param config New configuration
     */
    void setPinConfig(PinFunction func, const PinConfig& config);

    /**
     * @brief Perform RAK reset (toggle reset pin)
     */
    void resetRAK();

    /**
     * @brief Hard reset — hold RESET LOW for extended period
     * Forces all peripherals (W5500, etc.) to fully power down
     * @param holdMs Duration to hold RESET LOW (default 2000ms)
     */
    void hardResetRAK(uint32_t holdMs = 2000);

    /**
     * @brief Enter DFU mode (double-tap reset)
     * Nordic bootloader enters DFU on double reset within 500ms
     */
    void enterDFUMode();

    /**
     * @brief Read wake pin state
     * @return true if asserted
     */
    bool readWakePin();

    /**
     * @brief Test a pin (toggle if output, read if input)
     * @param gpio GPIO number
     * @return Pin state or result
     */
    int testPin(uint8_t gpio);

    /**
     * @brief Load configuration from NVS/LittleFS
     */
    void loadConfig();

    /**
     * @brief Save configuration to NVS/LittleFS
     */
    void saveConfig();

    /**
     * @brief Get all pin configurations as JSON
     */
    String getConfigJSON();

    /**
     * @brief Set pin configurations from JSON
     * @param json JSON string with pin configurations
     * @return true if successful
     */
    bool setConfigJSON(const String& json);

private:
    PinConfig m_pins[PIN_FUNCTION_COUNT];

    /**
     * @brief Apply pin configuration to hardware
     */
    void applyPinMode(const PinConfig& config);

    /**
     * @brief Initialize with default configuration
     */
    void setDefaults();
};
