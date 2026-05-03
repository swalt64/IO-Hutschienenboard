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

// Converts UTF-8 string to Adafruit GFX built-in font encoding (CP437).
// Handles German umlauts; other non-ASCII characters become '?'.
static void utf8ToDisplay(const char* src, char* dst, size_t maxLen) {
    size_t out = 0;
    for (size_t i = 0; src[i] && out < maxLen - 1; ) {
        uint8_t c = (uint8_t)src[i];
        if (c == 0xC3 && src[i + 1]) {
            switch ((uint8_t)src[i + 1]) {
                case 0x84: dst[out++] = '\x8E'; break; // Ä
                case 0x96: dst[out++] = '\x99'; break; // Ö
                case 0x9C: dst[out++] = '\x9A'; break; // Ü
                case 0x9F: dst[out++] = '\xE1'; break; // ß
                case 0xA4: dst[out++] = '\x84'; break; // ä
                case 0xB6: dst[out++] = '\x94'; break; // ö
                case 0xBC: dst[out++] = '\x81'; break; // ü
                default:   dst[out++] = '?';    break;
            }
            i += 2;
        } else if (c < 0x80) {
            dst[out++] = (char)c;
            i++;
        } else {
            // Skip continuation bytes of unsupported sequences
            i++;
            while (src[i] && ((uint8_t)src[i] & 0xC0) == 0x80) i++;
        }
    }
    dst[out] = '\0';
}

// ---- Scroll state -------------------------------------------------------

static const uint32_t SCROLL_STEP_MS    = 400;  // ms per scroll step
static const uint8_t  SCROLL_PAUSE_TICKS = 3;   // pause steps at each end

static struct {
    uint8_t  ch;
    char     name[64];
    bool     relayOn;
    uint32_t remainSecs;
    bool     hasTimer;
    uint16_t stateMask;
    uint8_t  offset;      // current scroll offset in characters
    int8_t   dir;         // +1 = scroll left, -1 = scroll right
    uint8_t  maxOffset;   // 0 = no scroll needed
    uint8_t  pauseCount;  // pause ticks remaining at current endpoint
    uint32_t lastMs;      // millis() of last scroll step
} S;

// ---- Internal draw ------------------------------------------------------

static void drawDisplay() {
    char shortName[14];
    strncpy(shortName, S.name + S.offset, 13);
    shortName[13] = '\0';

    char line1[22];
    snprintf(line1, sizeof(line1), "%02u/12  %s", (unsigned)(S.ch + 1), shortName);

    char timeStr[12];
    if (S.remainSecs < 3600) {
        snprintf(timeStr, sizeof(timeStr), "%02u:%02u",
                 (unsigned)(S.remainSecs / 60), (unsigned)(S.remainSecs % 60));
    } else {
        snprintf(timeStr, sizeof(timeStr), "%02u:%02u:%02u",
                 (unsigned)(S.remainSecs / 3600),
                 (unsigned)((S.remainSecs % 3600) / 60),
                 (unsigned)(S.remainSecs % 60));
    }

    char remainLine[24];
    if (!S.relayOn) {
        remainLine[0] = '\0';
    } else if (!S.hasTimer) {
        strcpy(remainLine, "Dauerbetrieb");
    } else if (S.remainSecs > 0) {
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
    oled->print(S.relayOn ? "EIN" : "AUS");

    oled->setTextSize(1);
    oled->setCursor(0, 46);
    oled->print(remainLine);

    // 12-Kanal-Statusleiste unten (y=57..63)
    static const int BW = 8, BH = 6, GAP = 1;
    static const int START_X = (128 - (12 * BW + 11 * GAP)) / 2;
    static const int START_Y = 57;
    for (uint8_t i = 0; i < 12; i++) {
        int x = START_X + i * (BW + GAP);
        bool on  = (S.stateMask >> i) & 1u;
        bool cur = (i == S.ch);
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

// ---- Public API ---------------------------------------------------------

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

    char convName[64];
    utf8ToDisplay(name, convName, sizeof(convName));

    bool resetScroll = (ch != S.ch) || (strcmp(convName, S.name) != 0);

    S.ch        = ch;
    strncpy(S.name, convName, sizeof(S.name) - 1);
    S.name[sizeof(S.name) - 1] = '\0';
    S.relayOn   = relayOn;
    S.remainSecs = remainSecs;
    S.hasTimer  = hasTimer;
    S.stateMask = stateMask;

    if (resetScroll) {
        size_t len  = strlen(S.name);
        S.maxOffset = (len > 13) ? (uint8_t)(len - 13) : 0;
        S.offset    = 0;
        S.dir       = 1;
        S.pauseCount = SCROLL_PAUSE_TICKS;
        S.lastMs    = millis();
    }

    drawDisplay();
}

// Called periodically (e.g. every 250 ms) to advance the scroll animation.
void tick(uint32_t nowMs) {
    if (!oled || S.maxOffset == 0) return;
    if (nowMs - S.lastMs < SCROLL_STEP_MS) return;
    S.lastMs = nowMs;

    if (S.pauseCount > 0) {
        S.pauseCount--;
        return;
    }

    if (S.dir > 0) {
        S.offset++;
        if (S.offset >= S.maxOffset) {
            S.offset    = S.maxOffset;
            S.dir       = -1;
            S.pauseCount = SCROLL_PAUSE_TICKS;
        }
    } else {
        S.offset--;
        if (S.offset == 0) {
            S.dir       = 1;
            S.pauseCount = SCROLL_PAUSE_TICKS;
        }
    }

    drawDisplay();
}

} // namespace display
