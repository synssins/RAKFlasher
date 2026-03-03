/**
 * @file sleep_manager.h
 * @brief Deep sleep management for ESP32-S3
 */
#pragma once

#include <Arduino.h>
#include <esp_sleep.h>
#include "config.h"

class SleepManager {
public:
    SleepManager();

    /**
     * @brief Enter deep sleep mode
     * Configures wake sources and enters deep sleep
     */
    void enterDeepSleep();

    /**
     * @brief Get the reason for waking from sleep
     * @return esp_sleep_wakeup_cause_t Wake-up cause
     */
    esp_sleep_wakeup_cause_t getWakeupReason();

    /**
     * @brief Configure wake sources
     * @param wakePin GPIO pin for external wake
     * @param wakeLevel HIGH or LOW for wake trigger
     */
    void configureWake(gpio_num_t wakePin, int wakeLevel);

    /**
     * @brief Enable timer wake
     * @param seconds Sleep duration in seconds
     */
    void enableTimerWake(uint64_t seconds);

private:
    gpio_num_t m_wakePin;
    int m_wakeLevel;
};
