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
    showBoot("");   // version wird von außen nachgereicht
    return true;
}

uint8_t address() {
    return activeAddr;
}

const char* controller() {
    return "SSD1306";
}

void showBoot(const char* version) {
    if (!oled) return;

    oled->clearDisplay();
    oled->setTextColor(SSD1306_WHITE);

    oled->setTextSize(1);
    oled->setCursor(0, 0);
    oled->print("IO-Hutschienenboard");
    oled->drawLine(0, 10, 127, 10, SSD1306_WHITE);

    oled->setTextSize(2);
    oled->setCursor(16, 16);
    oled->print("v");
    oled->print(version && version[0] ? version : "?.?.?");

    oled->setTextSize(1);
    oled->setCursor(0, 46);
    oled->print("OLED: SSD1306 0x");
    oled->print(activeAddr, HEX);

    oled->display();
}

void showMessage(const char* title, const char* line1, const char* line2) {
    if (!oled) return;

    oled->clearDisplay();
    oled->setTextColor(SSD1306_WHITE);

    oled->setTextSize(1);
    oled->setCursor(0, 0);
    oled->print(title);
    oled->drawLine(0, 10, 127, 10, SSD1306_WHITE);

    oled->setCursor(0, 18);
    oled->print(line1);

    if (line2 && line2[0]) {
        oled->setCursor(0, 32);
        oled->print(line2);
    }

    oled->display();
}

void showIP(const char* apIP, const char* staIP) {
    if (!oled) return;

    oled->clearDisplay();
    oled->setTextColor(SSD1306_WHITE);

    oled->setTextSize(1);
    oled->setCursor(0, 0);
    oled->print("Netzwerk");
    oled->drawLine(0, 10, 127, 10, SSD1306_WHITE);

    oled->setCursor(0, 16);
    oled->print("AP:  ");
    oled->print(apIP);

    oled->setCursor(0, 30);
    oled->print("STA: ");
    bool staOk = staIP && staIP[0] && strcmp(staIP, "0.0.0.0") != 0;
    oled->print(staOk ? staIP : "nicht verbunden");

    oled->display();
}

void show(uint8_t ch, const char* name, bool relayOn,
          uint32_t remainSecs, bool hasTimer, uint16_t stateMask) {
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
        remainLine[0] = '\0';
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

    // 12-Kanal-Statusleiste unten (y=57..63)
    static const int BW = 8, BH = 6, GAP = 1;
    static const int START_X = (128 - (12 * BW + 11 * GAP)) / 2;
    static const int START_Y = 57;
    for (uint8_t i = 0; i < 12; i++) {
        int x = START_X + i * (BW + GAP);
        bool on  = (stateMask >> i) & 1u;
        bool cur = (i == ch);
        if (cur) {
            oled->fillRect(x - 1, START_Y - 1, BW + 2, BH + 2, SSD1306_WHITE);
            if (!on) oled->fillRect(x, START_Y, BW, BH, SSD1306_BLACK);
        } else {
            if (on) oled->fillRect(x, START_Y, BW, BH, SSD1306_WHITE);
            else    oled->drawRect(x, START_Y, BW, BH, SSD1306_WHITE);
        }
    }

    oled->display();
}

} // namespace display
