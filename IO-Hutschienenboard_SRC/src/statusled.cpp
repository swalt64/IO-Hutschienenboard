#include "statusled.h"
#include "Freenove_WS2812_Lib_for_ESP32.h"

#define LED_PIN    48
#define LED_COUNT  1
#define CHANNEL    0

namespace statusled {

// --- Color definitions (R, G, B) ---
struct Color { uint8_t r, g, b; };

static const Color COL_OFF     = {  0,   0,   0};
static const Color COL_WHITE   = {255, 255, 255};
static const Color COL_GREEN   = {  0, 255,   0};
static const Color COL_RED     = {255,   0,   0};
static const Color COL_BLUE    = {  0,   0, 255};
static const Color COL_CYAN    = {  0, 255, 255};
static const Color COL_YELLOW  = {255, 180,   0};
static const Color COL_MAGENTA = {255,   0, 255};

// --- Pattern types ---
enum Pattern : uint8_t {
    PAT_SOLID,      // Dauerlicht
    PAT_BLINK,      // An/Aus im Takt
    PAT_PULSE,      // Sanftes Auf-/Abdimmen
    PAT_FLASH,      // Kurzer Blitz mit langer Pause
};

// --- Pattern definition per state ---
struct StateDef {
    Color   color;
    Pattern pattern;
    uint16_t periodMs;   // Gesamtzyklus in ms
    uint16_t onMs;       // Nur fuer PAT_FLASH: Dauer des Blitzes
};

static const StateDef STATES[] = {
    // ST_OFF
    {COL_OFF,     PAT_SOLID,  0,    0},
    // ST_BOOTING:          Weiss, schnell blinken 100ms
    {COL_WHITE,   PAT_BLINK,  200,  0},
    // ST_AP_ONLY:          Gelb, langsam pulsieren 2s
    {COL_YELLOW,  PAT_PULSE,  2000, 0},
    // ST_WIFI_CONNECTING:  Blau, schnell blinken 200ms
    {COL_BLUE,    PAT_BLINK,  400,  0},
    // ST_WIFI_NO_NTP:      Cyan, Dauerlicht
    {COL_CYAN,    PAT_SOLID,  0,    0},
    // ST_READY:            Gruen, Dauerlicht
    {COL_GREEN,   PAT_SOLID,  0,    0},
    // ST_RELAY_ACTIVE:     Gruen, langsam blinken 1s
    {COL_GREEN,   PAT_BLINK,  2000, 0},
    // ST_MCP_ERROR:        Rot, schnell blinken 200ms
    {COL_RED,     PAT_BLINK,  400,  0},
    // ST_CONFIG_ERROR:     Rot, Dauerlicht
    {COL_RED,     PAT_SOLID,  0,    0},
    // ST_OTA_UPDATE:       Magenta, schnell pulsieren 500ms
    {COL_MAGENTA, PAT_PULSE,  500,  0},
    // ST_WS_CLIENT:        Gruen, kurzer Blitz 50ms alle 3s
    {COL_GREEN,   PAT_FLASH,  3000, 50},
};

static Freenove_ESP32_WS2812 strip(LED_COUNT, LED_PIN, CHANNEL, TYPE_GRB);
static State s_state = ST_OFF;
static unsigned long s_cycleStart = 0;
static uint8_t s_brightness = 20;

// --- Init ---

void begin(uint8_t brightness) {
    s_brightness = brightness;
    strip.begin();
    strip.setBrightness(s_brightness);
    strip.setLedColorData(0, 0, 0, 0);
    strip.show();
}

// --- State ---

void setState(State state) {
    if (state == s_state) return;
    s_state = state;
    s_cycleStart = millis();
}

State getState() {
    return s_state;
}

void setBrightness(uint8_t brightness) {
    s_brightness = brightness;
    strip.setBrightness(s_brightness);
}

// --- Set pixel color with optional dimming factor (0-255) ---
static void setColor(const Color& c, uint8_t dim = 255) {
    uint8_t r = ((uint16_t)c.r * dim) >> 8;
    uint8_t g = ((uint16_t)c.g * dim) >> 8;
    uint8_t b = ((uint16_t)c.b * dim) >> 8;
    strip.setLedColorData(0, r, g, b);
    strip.show();
}

// --- Non-blocking update (call from loop) ---

void update() {
    const StateDef& sd = STATES[s_state];
    unsigned long now = millis();
    unsigned long elapsed = now - s_cycleStart;

    switch (sd.pattern) {

    case PAT_SOLID:
        setColor(sd.color);
        break;

    case PAT_BLINK: {
        uint16_t half = sd.periodMs / 2;
        uint16_t pos = elapsed % sd.periodMs;
        if (pos < half) {
            setColor(sd.color);
        } else {
            setColor(COL_OFF);
        }
        break;
    }

    case PAT_PULSE: {
        // Sine-like pulse: 0 -> max -> 0 over periodMs
        uint16_t pos = elapsed % sd.periodMs;
        float phase = (float)pos / (float)sd.periodMs;
        // Triangle wave: 0->1->0
        float brightness;
        if (phase < 0.5f) {
            brightness = phase * 2.0f;
        } else {
            brightness = (1.0f - phase) * 2.0f;
        }
        // Apply gamma for smoother visual
        brightness = brightness * brightness;
        uint8_t dim = (uint8_t)(brightness * 255.0f);
        setColor(sd.color, dim);
        break;
    }

    case PAT_FLASH: {
        uint16_t pos = elapsed % sd.periodMs;
        if (pos < sd.onMs) {
            setColor(sd.color);
        } else {
            setColor(COL_OFF);
        }
        break;
    }
    }
}

} // namespace statusled
