#include "display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>

namespace display {

static Adafruit_SSD1306* oled = nullptr;
static uint8_t activeAddr = 0;

bool begin(uint8_t sda, uint8_t scl, uint8_t addr) {
    (void)sda;
    (void)scl;

    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) {
        activeAddr = 0;
        return false;
    }

    if (oled) {
        delete oled;
        oled = nullptr;
    }

    oled = new Adafruit_SSD1306(128, 64, &Wire, -1);
    if (!oled) {
        activeAddr = 0;
        return false;
    }

    activeAddr = addr;
    if (!oled->begin(SSD1306_SWITCHCAPVCC, addr, false, false)) {
        delete oled;
        oled = nullptr;
        activeAddr = 0;
        return false;
    }

    oled->clearDisplay();
    oled->display();
    showBoot();
    return true;
}

uint8_t address() {
    return activeAddr;
}

const char* controller() {
    return "SSD1306";
}

void showBoot() {
    if (!oled) return;

    oled->clearDisplay();
    oled->setTextColor(SSD1306_WHITE);
    oled->setTextSize(1);
    oled->setCursor(24, 20);
    oled->print("OLED OK");
    oled->setCursor(12, 36);
    oled->print("SSD1306 / 0x");
    oled->print(activeAddr, HEX);
    oled->display();
}

void show(uint8_t ch, const char* name, bool relayOn,
          uint32_t remainSecs, bool hasTimer) {
    if (!oled) return;

    char shortName[14];
    strncpy(shortName, name, 13);
    shortName[13] = '\0';

    char line1[22];
    snprintf(line1, sizeof(line1), "%02u/12  %s", (unsigned)(ch + 1), shortName);

    char timeStr[12];
    if (remainSecs < 3600) {
        snprintf(timeStr, sizeof(timeStr), "%02u:%02u",
                 (unsigned)(remainSecs / 60), (unsigned)(remainSecs % 60));
    } else {
        snprintf(timeStr, sizeof(timeStr), "%02u:%02u:%02u",
                 (unsigned)(remainSecs / 3600),
                 (unsigned)((remainSecs % 3600) / 60),
                 (unsigned)(remainSecs % 60));
    }

    char remainLine[24];
    if (!relayOn) {
        strcpy(remainLine, "--");
    } else if (!hasTimer) {
        strcpy(remainLine, "Dauerbetrieb");
    } else if (remainSecs > 0) {
        snprintf(remainLine, sizeof(remainLine), "Noch: %s", timeStr);
    } else {
        strcpy(remainLine, "--");
    }

    oled->clearDisplay();
    oled->setTextColor(SSD1306_WHITE);

    oled->setTextSize(1);
    oled->setCursor(0, 1);
    oled->print(line1);
    oled->drawLine(0, 11, 127, 11, SSD1306_WHITE);

    oled->setTextSize(2);
    oled->setCursor(34, 20);
    oled->print(relayOn ? "EIN" : "AUS");

    oled->setTextSize(1);
    oled->setCursor(0, 46);
    oled->print(remainLine);

    oled->setCursor(0, 56);
    oled->print("^v Ch  OK Tog");

    oled->display();
}

} // namespace display
