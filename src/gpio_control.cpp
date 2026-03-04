/**
 * @file gpio_control.cpp
 * @brief Implementation of GPIO control
 */

#include "gpio_control.h"
#include <LittleFS.h>

extern Preferences preferences;

GPIOControl::GPIOControl() {
}

bool GPIOControl::begin() {
    DEBUG_PRINTLN("[GPIO] Initializing GPIO control...");

    // Load saved configuration or use defaults
    loadConfig();

    // Apply pin configurations — skip UART pins (managed by HardwareSerial)
    for (int i = 0; i < PIN_FUNCTION_COUNT; i++) {
        if (i == PIN_UART_TX || i == PIN_UART_RX) {
            DEBUG_PRINTF("[GPIO] GPIO%d (%s) — skipped (managed by HardwareSerial)\n",
                         m_pins[i].gpio, m_pins[i].function.c_str());
            continue;
        }
        applyPinMode(m_pins[i]);
    }

    DEBUG_PRINTLN("[GPIO] GPIO initialized successfully");
    return true;
}

PinConfig GPIOControl::getPinConfig(PinFunction func) {
    if (func < PIN_FUNCTION_COUNT) {
        return m_pins[func];
    }
    return PinConfig{0, INPUT, LOW, "Invalid"};
}

void GPIOControl::setPinConfig(PinFunction func, const PinConfig& config) {
    if (func < PIN_FUNCTION_COUNT) {
        m_pins[func] = config;
        applyPinMode(config);
        saveConfig();
    }
}

void GPIOControl::resetRAK() {
    PinConfig resetPin = m_pins[PIN_RESET];

    DEBUG_PRINTLN("[GPIO] Resetting RAK module...");

    // Assert reset (active LOW by default)
    digitalWrite(resetPin.gpio, resetPin.activeLevel == LOW ? LOW : HIGH);
    delay(100);

    // Release reset
    digitalWrite(resetPin.gpio, resetPin.activeLevel == LOW ? HIGH : LOW);
    delay(500);  // Allow RAK to boot

    DEBUG_PRINTLN("[GPIO] RAK reset complete");
}

void GPIOControl::hardResetRAK(uint32_t holdMs) {
    PinConfig resetPin = m_pins[PIN_RESET];

    DEBUG_PRINTF("[GPIO] Hard reset: holding RESET LOW for %u ms...\n", holdMs);

    // Assert reset (active LOW by default)
    digitalWrite(resetPin.gpio, resetPin.activeLevel == LOW ? LOW : HIGH);
    delay(holdMs);

    // Release reset
    digitalWrite(resetPin.gpio, resetPin.activeLevel == LOW ? HIGH : LOW);
    delay(1000);  // Allow RAK to fully boot

    DEBUG_PRINTLN("[GPIO] Hard reset complete");
}

void GPIOControl::enterDFUMode() {
    DEBUG_PRINTLN("[GPIO] Entering DFU mode (double reset)...");

    // First reset
    resetRAK();
    delay(100);

    // Second reset within 500ms window
    resetRAK();

    delay(1000);  // Allow bootloader to start

    DEBUG_PRINTLN("[GPIO] DFU mode activated (if bootloader supports it)");
}

bool GPIOControl::readWakePin() {
    PinConfig wakePin = m_pins[PIN_WAKE];
    int state = digitalRead(wakePin.gpio);

    return (state == wakePin.activeLevel);
}

int GPIOControl::testPin(uint8_t gpio) {
    // Reject UART pins — these are owned by HardwareSerial; toggling
    // them with pinMode() would break serial TX/RX until reboot.
    if (gpio == m_pins[PIN_UART_TX].gpio || gpio == m_pins[PIN_UART_RX].gpio) {
        DEBUG_PRINTF("[GPIO] Test GPIO%d BLOCKED — pin is in use by UART\n", gpio);
        return -1;
    }

    pinMode(gpio, OUTPUT);
    digitalWrite(gpio, HIGH);
    delay(100);
    digitalWrite(gpio, LOW);
    delay(100);

    pinMode(gpio, INPUT);
    int state = digitalRead(gpio);

    DEBUG_PRINTF("[GPIO] Test GPIO%d: state=%d\n", gpio, state);

    return state;
}

void GPIOControl::loadConfig() {
    if (LittleFS.exists(PATH_PINS)) {
        File file = LittleFS.open(PATH_PINS, "r");
        if (file) {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, file);
            file.close();

            if (!error) {
                // Parse JSON configuration
                for (int i = 0; i < PIN_FUNCTION_COUNT; i++) {
                    if (doc[String(i)].is<JsonObject>()) {
                        JsonObject pin = doc[String(i)];
                        m_pins[i].gpio = pin["gpio"] | 0;
                        m_pins[i].mode = pin["mode"] | INPUT;
                        m_pins[i].activeLevel = pin["activeLevel"] | LOW;
                        m_pins[i].function = pin["function"] | "";
                    }
                }

                DEBUG_PRINTLN("[GPIO] Configuration loaded from file");
                return;
            }
        }
    }

    // Use defaults if no saved config
    setDefaults();
    DEBUG_PRINTLN("[GPIO] Using default configuration");
}

void GPIOControl::saveConfig() {
    JsonDocument doc;

    for (int i = 0; i < PIN_FUNCTION_COUNT; i++) {
        JsonObject pin = doc[String(i)].to<JsonObject>();
        pin["gpio"] = m_pins[i].gpio;
        pin["mode"] = m_pins[i].mode;
        pin["activeLevel"] = m_pins[i].activeLevel;
        pin["function"] = m_pins[i].function;
    }

    File file = LittleFS.open(PATH_PINS, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
        DEBUG_PRINTLN("[GPIO] Configuration saved to file");
    } else {
        DEBUG_PRINTLN("[GPIO] ERROR: Failed to save configuration");
    }
}

// Convert Arduino pin mode constant to string for JSON API
static const char* pinModeToString(uint8_t mode) {
    switch (mode) {
        case OUTPUT:         return "OUTPUT";
        case INPUT_PULLUP:   return "INPUT_PULLUP";
        case INPUT_PULLDOWN: return "INPUT_PULLDOWN";
        default:             return "INPUT";
    }
}

// Convert string pin mode back to Arduino constant
static uint8_t stringToPinMode(const char* str) {
    if (strcmp(str, "OUTPUT") == 0)         return OUTPUT;
    if (strcmp(str, "INPUT_PULLUP") == 0)   return INPUT_PULLUP;
    if (strcmp(str, "INPUT_PULLDOWN") == 0) return INPUT_PULLDOWN;
    return INPUT;
}

String GPIOControl::getConfigJSON() {
    JsonDocument doc;

    for (int i = 0; i < PIN_FUNCTION_COUNT; i++) {
        JsonObject pin = doc[String(i)].to<JsonObject>();
        pin["gpio"] = m_pins[i].gpio;
        pin["mode"] = pinModeToString(m_pins[i].mode);
        pin["activeLevel"] = m_pins[i].activeLevel == HIGH ? "HIGH" : "LOW";
        pin["function"] = m_pins[i].function;
    }

    String json;
    serializeJson(doc, json);
    return json;
}

bool GPIOControl::setConfigJSON(const String& json) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, json);

    if (error) {
        DEBUG_PRINTF("[GPIO] JSON parse error: %s\n", error.c_str());
        return false;
    }

    for (int i = 0; i < PIN_FUNCTION_COUNT; i++) {
        if (doc[String(i)].is<JsonObject>()) {
            JsonObject pin = doc[String(i)];
            m_pins[i].gpio = pin["gpio"];

            // Accept both string ("OUTPUT") and numeric (2) mode values
            if (pin["mode"].is<const char*>()) {
                m_pins[i].mode = stringToPinMode(pin["mode"].as<const char*>());
            } else {
                m_pins[i].mode = pin["mode"] | INPUT;
            }

            // Accept both string ("HIGH") and numeric (1) activeLevel values
            if (pin["activeLevel"].is<const char*>()) {
                m_pins[i].activeLevel = strcmp(pin["activeLevel"].as<const char*>(), "HIGH") == 0 ? HIGH : LOW;
            } else {
                m_pins[i].activeLevel = pin["activeLevel"] | LOW;
            }

            m_pins[i].function = pin["function"] | "";

            // Skip UART pins — managed by HardwareSerial
            if (i != PIN_UART_TX && i != PIN_UART_RX) {
                applyPinMode(m_pins[i]);
            }
        }
    }

    saveConfig();
    return true;
}

void GPIOControl::applyPinMode(const PinConfig& config) {
    pinMode(config.gpio, config.mode);

    // Set initial state for outputs
    if (config.mode == OUTPUT) {
        // For active-low signals, default to HIGH (inactive)
        digitalWrite(config.gpio, config.activeLevel == LOW ? HIGH : LOW);
    }

    DEBUG_PRINTF("[GPIO] GPIO%d configured as %s (%s active)\n",
                 config.gpio,
                 config.mode == OUTPUT ? "OUTPUT" : "INPUT",
                 config.activeLevel == HIGH ? "HIGH" : "LOW");
}

void GPIOControl::setDefaults() {
    m_pins[PIN_WAKE] = {DEFAULT_PIN_WAKE, INPUT, HIGH, "Wake Signal"};
    m_pins[PIN_RESET] = {DEFAULT_PIN_RESET, OUTPUT, LOW, "RAK Reset"};
    m_pins[PIN_UART_TX] = {DEFAULT_PIN_UART_TX, OUTPUT, HIGH, "UART TX"};
    m_pins[PIN_UART_RX] = {DEFAULT_PIN_UART_RX, INPUT, HIGH, "UART RX"};
    m_pins[PIN_SWD_CLK] = {DEFAULT_PIN_SWD_CLK, OUTPUT, HIGH, "SWD Clock"};
    m_pins[PIN_SWD_IO] = {DEFAULT_PIN_SWD_IO, OUTPUT, HIGH, "SWD Data"};

    saveConfig();
}
