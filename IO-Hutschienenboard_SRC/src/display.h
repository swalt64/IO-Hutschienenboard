#pragma once
#include <stdint.h>

namespace display {
    bool begin(uint8_t sda, uint8_t scl, uint8_t addr);
    uint8_t address();
    const char* controller();

    void showBoot(const char* version = "");
    void showIP(const char* apIP, const char* staIP);
    void showMessage(const char* title, const char* line1, const char* line2 = "");

    void show(uint8_t ch, const char* name, bool relayOn,
              uint32_t remainSecs, bool hasTimer, uint16_t stateMask = 0);
}
