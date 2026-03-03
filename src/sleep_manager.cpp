/**
 * @file sleep_manager.cpp
 * @brief Implementation of deep sleep management
 */

#include "sleep_manager.h"

SleepManager::SleepManager()
    : m_wakePin((gpio_num_t)DEFAULT_PIN_WAKE)
    , m_wakeLevel(WAKE_GPIO_LEVEL) {
}

void SleepManager::enterDeepSleep() {
    DEBUG_PRINTLN("[SLEEP] Preparing for deep sleep...");

    // Configure wake source - EXT0 for single GPIO
    // ESP32-S3 supports wake on any RTC GPIO (GPIO0-21)
    esp_sleep_enable_ext0_wakeup(m_wakePin, m_wakeLevel);

    DEBUG_PRINTF("[SLEEP] Wake configured on GPIO%d (level: %s)\n",
                 m_wakePin, m_wakeLevel == HIGH ? "HIGH" : "LOW");

    // Flush serial before sleep
    Serial.flush();

    delay(100);

    // Enter deep sleep
    esp_deep_sleep_start();
}

esp_sleep_wakeup_cause_t SleepManager::getWakeupReason() {
    return esp_sleep_get_wakeup_cause();
}

void SleepManager::configureWake(gpio_num_t wakePin, int wakeLevel) {
    m_wakePin = wakePin;
    m_wakeLevel = wakeLevel;
}

void SleepManager::enableTimerWake(uint64_t seconds) {
    esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);  // Convert to microseconds
    DEBUG_PRINTF("[SLEEP] Timer wake enabled for %llu seconds\n", seconds);
}
