#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <lwip/ip4_addr.h>
#include <dhcpserver/dhcpserver.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h>
#include <ESPmDNS.h>
#if !SIMULATE_HW
#include <Adafruit_MCP23X17.h>
#endif
#include "pin_config.h"
#include "swtools.h"
#include "statusled.h"
#include "display.h"
#include "version.h"

using namespace dbg;

#ifndef OLED_HARDTEST
#define OLED_HARDTEST 0
#endif

#if OLED_HARDTEST
static bool probeI2CAddress(uint8_t addr);

static void oledCmd1(uint8_t addr, uint8_t cmd) {
    Wire.beginTransmission(addr);
    Wire.write(0x00);
    Wire.write(cmd);
    Wire.endTransmission();
}

static void oledCmd2(uint8_t addr, uint8_t cmd1, uint8_t cmd2) {
    Wire.beginTransmission(addr);
    Wire.write(0x00);
    Wire.write(cmd1);
    Wire.write(cmd2);
    Wire.endTransmission();
}

static void runOledHardTest() {
    const uint8_t addr = OLED_ADDR;

    dbg::info(CAT_SYSTEM, "=== OLED HARDTEST ===");
    dbg::info(CAT_SYSTEM, "OLED probe 0x3C: %s", probeI2CAddress(0x3C) ? "ACK" : "kein ACK");
    dbg::info(CAT_SYSTEM, "OLED probe 0x3D: %s", probeI2CAddress(0x3D) ? "ACK" : "kein ACK");

    if (!probeI2CAddress(addr)) {
        dbg::warn(CAT_SYSTEM, "OLED antwortet nicht auf 0x%02X", addr);
        return;
    }

    oledCmd1(addr, 0xAE);
    oledCmd2(addr, 0xD5, 0x80);
    oledCmd2(addr, 0xA8, 0x3F);
    oledCmd2(addr, 0xD3, 0x00);
    oledCmd1(addr, 0x40);
    oledCmd2(addr, 0x8D, 0x14);
    oledCmd2(addr, 0x20, 0x00);
    oledCmd1(addr, 0xA1);
    oledCmd1(addr, 0xC8);
    oledCmd2(addr, 0xDA, 0x12);
    oledCmd2(addr, 0x81, 0xCF);
    oledCmd2(addr, 0xD9, 0xF1);
    oledCmd2(addr, 0xDB, 0x40);
    oledCmd1(addr, 0xA4);
    oledCmd1(addr, 0xA6);
    oledCmd1(addr, 0xAF);
    delay(100);

    oledCmd1(addr, 0xA5);
    dbg::warn(CAT_SYSTEM, "OLED Hardtest aktiv: ALL PIXELS ON auf 0x%02X", addr);
}
#endif

// ============================================================
// WiFi Configuration - AP mode for initial setup
// ============================================================
const char* AP_SSID = "IO-Hutschiene";
const char* AP_PASS = "12345678";   // HINWEIS: Standard-AP-Passwort – für Produktiveinsatz ändern
const IPAddress AP_IP(192, 168, 50, 1);
const IPAddress AP_GATEWAY(192, 168, 50, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);
const bool ENABLE_DHCP_DIAG = false;

String sta_ssid = "";
String sta_pass = "";
String ui_user  = "admin";   // Produktiveinsatz: in Web-UI ändern
String ui_pass  = "admin";   // Produktiveinsatz: in Web-UI ändern
String ota_pass = "admin";   // Produktiveinsatz: muss mit --auth in platformio.ini übereinstimmen

// ============================================================
// Global State
// ============================================================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences prefs;

static AsyncCallbackWebHandler* g_apiHandler    = nullptr;
static AsyncCallbackWebHandler* g_cmdHandler    = nullptr;
static AsyncStaticWebHandler*   g_staticHandler = nullptr;

static void applyWebAuth() {
    if (g_apiHandler)    g_apiHandler->setAuthentication(ui_user.c_str(), ui_pass.c_str());
    if (g_cmdHandler)    g_cmdHandler->setAuthentication(ui_user.c_str(), ui_pass.c_str());
    if (g_staticHandler) g_staticHandler->setAuthentication(ui_user.c_str(), ui_pass.c_str());
    ws.setAuthentication(ui_user.c_str(), ui_pass.c_str());
    ElegantOTA.setAuth(ui_user.c_str(), ui_pass.c_str());
}

#if !SIMULATE_HW
Adafruit_MCP23X17 mcp[2];
Adafruit_MCP23X17 mcpTop;
#endif
bool mcpReady[2]  = {false, false};
bool mcpTopReady  = false;

volatile bool topBtnFlag = false;
volatile bool pendingAllOff = false;
void IRAM_ATTR topBtnISR() { topBtnFlag = true; }

bool   relayState[NUM_CHANNELS] = {false};
bool   inputState[NUM_CHANNELS] = {false};
time_t lastInputTime[NUM_CHANNELS]  = {0};  // Unix-Timestamp letzte Betätigung
time_t lastOutputTime[NUM_CHANNELS] = {0};  // Unix-Timestamp letzte Relais-Aktion

unsigned long inputLastEdgeMs[NUM_CHANNELS]  = {};
bool          inputRawPrev[NUM_CHANNELS]     = {};
unsigned long pressStartMs[NUM_CHANNELS]     = {};
bool          signalActivePrev[NUM_CHANNELS] = {};
bool          longPressFired[NUM_CHANNELS]   = {};

uint16_t inputMapping[NUM_CHANNELS]; // Bitmask: Bit j = Eingang steuert Ausgang j
uint8_t  outputMode[NUM_CHANNELS]  = {0}; // 0=Toggle, 1=Taster
uint8_t  inputMode[NUM_CHANNELS]   = {0}; // 0=Toggle, 1=Taster

uint32_t autoOffSeconds[NUM_CHANNELS] = {0};
unsigned long relayOnTimestamp[NUM_CHANNELS] = {0};
int32_t tempTimeAdjustMs[NUM_CHANNELS] = {0};
unsigned long timerFreezeStart[NUM_CHANNELS]  = {0};  // Freeze-Beginn (0=inaktiv)
unsigned long timerFreezeExpiry[NUM_CHANNELS] = {0};  // Freeze-Ablauf

static inline unsigned long getTimerEffectiveNow(uint8_t ch) {
    return (timerFreezeStart[ch] > 0) ? timerFreezeStart[ch] : millis();
}

char channelNames[NUM_CHANNELS][CH_NAME_MAX_LEN + 1];
char inputNames[NUM_CHANNELS][CH_NAME_MAX_LEN + 1];
uint8_t displayChannel = 0;

uint8_t outDisplayOrder[NUM_CHANNELS] = {0,1,2,3,4,5,6,7,8,9,10,11};
bool    outChEnabled[NUM_CHANNELS]    = {true,true,true,true,true,true,true,true,true,true,true,true};
bool    outRemoteProtected[NUM_CHANNELS] = {};
// true = Ausgang wird bei "Alle Ausgaenge AUS" mit ausgeschaltet (Eingangs-Langdruck,
// Web-UI, App). false = Ausgang bleibt unberuehrt, z.B. Klimaanlage.
bool    outAllOffEnabled[NUM_CHANNELS] = {true,true,true,true,true,true,true,true,true,true,true,true};

uint32_t getRemainingAutoOffSeconds(uint8_t ch, unsigned long nowMs) {
    if (ch >= NUM_CHANNELS) return 0;
    if (!relayState[ch]) return 0;
    if (relayOnTimestamp[ch] == 0) return 0;

    const int64_t totalMs = (int64_t)autoOffSeconds[ch] * 1000LL + tempTimeAdjustMs[ch];
    if (totalMs <= 0) return 0;

    const unsigned long elapsedMs = nowMs - relayOnTimestamp[ch];
    if (elapsedMs >= (unsigned long)totalMs) return 0;

    const unsigned long remainingMs = (unsigned long)totalMs - elapsedMs;
    return (remainingMs + 999UL) / 1000UL;
}

static int32_t btnStepMs(uint32_t remainSecs) {
    if (remainSecs <=   900) return    60000;  // bis 15 min  →  1 min
    if (remainSecs <=  3600) return   300000;  // bis 1h      →  5 min
    if (remainSecs <=  7200) return   600000;  // bis 2h      → 10 min
    if (remainSecs <= 21600) return  1800000;  // bis 6h      → 30 min
    if (remainSecs <= 86400) return  3600000;  // bis 24h     →  1 h
    if (remainSecs <= 172800) return 21600000; // bis 48h     →  6 h
    return 21600000;                           // über 48h    →  6 h
}

// ============================================================
// Display-Aktualisierung für den aktuell angezeigten Kanal
// ============================================================
void updateDisplay() {
    uint8_t ch = displayChannel;
    uint16_t mask = 0;
    for (uint8_t i = 0; i < NUM_CHANNELS; i++)
        if (relayState[i]) mask |= (1u << i);
    display::show(ch, channelNames[ch], relayState[ch],
                  getRemainingAutoOffSeconds(ch, getTimerEffectiveNow(ch)),
                  autoOffSeconds[ch] > 0 || tempTimeAdjustMs[ch] > 0,
                  mask);
}

// ============================================================
// Top Board: Status-LEDs aktualisieren (GPB0-2, aktiv-low)
// ============================================================
void updateTopLeds() {
#if !SIMULATE_HW
    if (!mcpTopReady) return;

    bool staConnected = (WiFi.status() == WL_CONNECTED);
    bool anyRelayOn = false;
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        if (relayState[i]) { anyRelayOn = true; break; }
    }
    bool systemReady = (mcpReady[0] && mcpReady[1]);

    // WLAN-LED (2s-Periode):
    //   kein STA          → aus
    //   STA, keine IP     → 10/90 (200ms an, 1800ms aus)
    //   IP, RSSI < 50%    → 80/20 (1600ms an,  400ms aus)
    //   IP, RSSI >= 50%   → Dauerlicht
    bool hasIP = staConnected && (WiFi.localIP() != IPAddress(0, 0, 0, 0));
    bool wlanOn = false;
    if (hasIP) {
        int pct = min(100, max(0, 2 * (WiFi.RSSI() + 100)));
        wlanOn = (pct >= 50) ? true : ((millis() % 2000) < 1600);
    } else if (staConnected) {
        wlanOn = (millis() % 2000) < 200;
    }
    mcpTop.digitalWrite(LED_WLAN_PIN, wlanOn ? LOW : HIGH);
    mcpTop.digitalWrite(LED_OUTPUT_PIN, anyRelayOn ? LOW : HIGH);
    // RUN-LED: 2s-Blinker (1s an, 1s aus) wenn System bereit
    if (systemReady) {
        bool runOn = ((millis() / 1000) % 2) == 0;
        mcpTop.digitalWrite(LED_RUN_PIN, runOn ? LOW : HIGH);
    } else {
        mcpTop.digitalWrite(LED_RUN_PIN, HIGH);  // aus
    }
#endif
}

void saveConfig();  // forward declaration

// handleButton() ist nach onWebSocketEvent() definiert (benötigt sendState etc.)
void handleButton(uint8_t btn);

// ============================================================
// Helper: determine correct LED state based on system status
// ============================================================
void updateLedState() {
#if !SIMULATE_HW
    if (!mcpReady[0] && !mcpReady[1]) {
        statusled::setState(statusled::ST_MCP_ERROR);
        return;
    }
#endif

    bool anyRelayOn = false;
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        if (relayState[i]) { anyRelayOn = true; break; }
    }

    bool wsClients = ws.count() > 0;

    if (anyRelayOn) {
        statusled::setState(statusled::ST_RELAY_ACTIVE);
    } else if (wsClients) {
        statusled::setState(statusled::ST_WS_CLIENT);
    } else if (WiFi.status() == WL_CONNECTED && dbg::isTimeSynced()) {
        statusled::setState(statusled::ST_READY);
    } else if (WiFi.status() == WL_CONNECTED) {
        statusled::setState(statusled::ST_WIFI_NO_NTP);
    } else {
        statusled::setState(statusled::ST_AP_ONLY);
    }

    updateTopLeds();
}

// ============================================================
// MCP23017 Init
// ============================================================
// I2C-Bus-Recovery: SDA nach Soft-Reset stuck → 9 SCL-Pulse + STOP-Condition
static void i2cBusRecover() {
    pinMode(I2C_SCL_PIN, OUTPUT);
    pinMode(I2C_SDA_PIN, OUTPUT);
    digitalWrite(I2C_SDA_PIN, HIGH);
    for (int i = 0; i < 9; i++) {
        digitalWrite(I2C_SCL_PIN, HIGH); delayMicroseconds(5);
        digitalWrite(I2C_SCL_PIN, LOW);  delayMicroseconds(5);
    }
    // STOP condition: SDA LOW → SCL HIGH → SDA HIGH
    digitalWrite(I2C_SDA_PIN, LOW);  delayMicroseconds(5);
    digitalWrite(I2C_SCL_PIN, HIGH); delayMicroseconds(5);
    digitalWrite(I2C_SDA_PIN, HIGH); delayMicroseconds(5);
    // Pins auf INPUT zurück damit Wire sie übernehmen kann
    pinMode(I2C_SCL_PIN, INPUT);
    pinMode(I2C_SDA_PIN, INPUT);
}

static bool probeI2CAddress(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

void setupMCP() {
#if SIMULATE_HW
    i2cBusRecover();
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(10);   // 10ms statt 1000ms — verhindert 145s Init-Delay beim SSD1306
    Wire.setClock(100000); // explizit 100kHz
    mcpReady[0]  = true;
    mcpReady[1]  = true;
    mcpTopReady  = true;
    dbg::warn(CAT_MCP, "*** SIMULATE_HW: MCP23017 simuliert ***");
#else
    i2cBusRecover();
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(10);
    Wire.setClock(100000);

    // --- Relais-MCPs (0x21 SET, 0x20 RESET, per Platinen-Verdrahtung) ---
    const uint8_t addrs[2] = {MCP_ADDR_SET, MCP_ADDR_RESET};
    const char* names[2]   = {"SET (0x21)", "RESET (0x20)"};
    for (uint8_t m = 0; m < 2; m++) {
        if (mcp[m].begin_I2C(addrs[m], &Wire)) {
            mcpReady[m] = true;
            dbg::info(CAT_MCP, "MCP23017 %s OK", names[m]);
            for (uint8_t p = 0; p < 16; p++) {
                mcp[m].pinMode(p, OUTPUT);
                mcp[m].digitalWrite(p, LOW);
            }
        } else {
            dbg::error(CAT_MCP, "MCP23017 %s NICHT GEFUNDEN!", names[m]);
        }
    }

    // --- Top Board MCP (0x22): Taster GPA0-4, LEDs GPB0-2 ---
    if (mcpTop.begin_I2C(MCP_ADDR_TOPBOARD, &Wire)) {
        mcpTopReady = true;
        dbg::info(CAT_MCP, "MCP23017 TopBoard (0x22) OK");

        // GPA0-4: Taster-Eingaenge, interne Pull-ups aktiv
        for (uint8_t p = BTN_ENTER; p <= BTN_RIGHT; p++) {
            mcpTop.pinMode(p, INPUT_PULLUP);
        }
        // GPA5-7: ungenutzt, als Eingang mit Pull-up
        for (uint8_t p = 5; p <= 7; p++) {
            mcpTop.pinMode(p, INPUT_PULLUP);
        }
        // GPB0-2: LED-Ausgaenge, aktiv-low → HIGH = aus
        for (uint8_t p = LED_WLAN_PIN; p <= LED_RUN_PIN; p++) {
            mcpTop.pinMode(p, OUTPUT);
            mcpTop.digitalWrite(p, HIGH);
        }
        // GPB3-7: ungenutzt, HIGH
        for (uint8_t p = 11; p <= 15; p++) {
            mcpTop.pinMode(p, OUTPUT);
            mcpTop.digitalWrite(p, HIGH);
        }

        // INTA konfigurieren: kein Mirror, Push-Pull, aktiv-low
        mcpTop.setupInterrupts(false, false, LOW);
        for (uint8_t p = BTN_ENTER; p <= BTN_RIGHT; p++) {
            mcpTop.setupInterruptPin(p, CHANGE);
        }

        // GPIO13 als Interrupt-Eingang (INTA, aktiv-low)
        pinMode(MCP_INT_PIN, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(MCP_INT_PIN), topBtnISR, FALLING);
    } else {
        dbg::error(CAT_MCP, "MCP23017 TopBoard (0x22) NICHT GEFUNDEN!");
    }
#endif
}

// ============================================================
// Relay Control via MCP23017
// ============================================================
void setRelay(uint8_t ch, bool on, bool saveTimerAdjust = false) {
    if (ch >= NUM_CHANNELS) return;

#if SIMULATE_HW
    dbg::debug(CAT_RELAY, "[SIM] Relais %d: Puls auf %s-Pin", ch + 1, on ? "SET" : "RESET");
#else
    uint8_t mcpIdx = on ? MCP_SET : MCP_RESET;
    uint8_t pin    = RELAY_PINS[ch];

    if (!mcpReady[mcpIdx]) {
        dbg::error(CAT_RELAY, "Relais %d: MCP23017 %s nicht bereit!",
                   ch + 1, on ? "SET" : "RESET");
        return;
    }

    mcp[mcpIdx].digitalWrite(pin, HIGH);
    delay(RELAY_PULSE_MS);
    mcp[mcpIdx].digitalWrite(pin, LOW);
#endif

    // Beim manuellen Ausschalten: per Taste eingestellte Zeit dauerhaft speichern
    if (!on && saveTimerAdjust && tempTimeAdjustMs[ch] != 0) {
        int64_t totalMs = (int64_t)autoOffSeconds[ch] * 1000 + tempTimeAdjustMs[ch];
        autoOffSeconds[ch] = totalMs > 0 ? (uint32_t)(totalMs / 1000) : 0;
        saveConfig();
    }
    tempTimeAdjustMs[ch]  = 0;
    timerFreezeStart[ch]  = 0;
    timerFreezeExpiry[ch] = 0;
    relayState[ch] = on;
    relayOnTimestamp[ch] = on ? millis() : 0;
    lastOutputTime[ch] = time(nullptr);
    dbg::info(CAT_RELAY, "Relais %d: %s", ch + 1, on ? "EIN" : "AUS");

    updateLedState();
    if (ch == displayChannel) updateDisplay();
}

void toggleRelay(uint8_t ch) {
    setRelay(ch, !relayState[ch], relayState[ch]);  // speichert nur beim Ausschalten
}

// Schaltet alle Ausgaenge aus, die fuer "Alles aus" freigegeben sind.
// Ausgaenge mit outAllOffEnabled[i] == false bleiben unberuehrt (z.B. Klimaanlage).
void allOutputsOff(const char* reason) {
    uint8_t skipped = 0;
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        if (!relayState[i]) continue;
        if (!outAllOffEnabled[i]) { skipped++; continue; }
        setRelay(i, false, true);
    }
    dbg::info(CAT_RELAY, "%s: alle Ausgaenge AUS (%u ausgenommen)", reason, skipped);
}

// ============================================================
// Configuration persistence (NVS)
// ============================================================
void loadConfig() {
    prefs.begin("io-config", true);
    sta_ssid = prefs.getString("ssid", "");
    sta_pass = prefs.getString("pass", "");
    ui_user  = prefs.getString("ui_user", "admin");
    ui_pass  = prefs.getString("ui_pass", "admin");
    ota_pass = prefs.getString("ota_pass", "admin");

    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        String key = "mm" + String(i);   // "mm" = multi-map bitmask (uint16)
        inputMapping[i] = prefs.getUShort(key.c_str(), 0);
        key = "auto" + String(i);
        autoOffSeconds[i] = prefs.getUInt(key.c_str(), 0);
        key = "outmode" + String(i);
        outputMode[i] = prefs.getUChar(key.c_str(), 0);
        key = "inmode" + String(i);
        inputMode[i] = prefs.getUChar(key.c_str(), 0);
        key = "ord" + String(i);
        outDisplayOrder[i] = prefs.getUChar(key.c_str(), i);
        if (outDisplayOrder[i] >= NUM_CHANNELS) outDisplayOrder[i] = i;
        key = "ena" + String(i);
        outChEnabled[i] = prefs.getUChar(key.c_str(), 1) != 0;
        key = "prot" + String(i);
        outRemoteProtected[i] = prefs.getUChar(key.c_str(), 0) != 0;
        key = "aoff" + String(i);
        outAllOffEnabled[i] = prefs.getUChar(key.c_str(), 1) != 0;  // Default: mit ausschalten
        key = "name" + String(i);
        String defaultName = "Ausgang " + String(i + 1);
        String name = prefs.getString(key.c_str(), defaultName);
        strncpy(channelNames[i], name.c_str(), CH_NAME_MAX_LEN);
        channelNames[i][CH_NAME_MAX_LEN] = '\0';

        key = "iname" + String(i);
        String defaultIName = "Eingang " + String(i + 1);
        String iname = prefs.getString(key.c_str(), defaultIName);
        strncpy(inputNames[i], iname.c_str(), CH_NAME_MAX_LEN);
        inputNames[i][CH_NAME_MAX_LEN] = '\0';
    }
    prefs.end();
    dbg::info(CAT_CONFIG, "Konfiguration geladen (SSID: '%s')", sta_ssid.c_str());
}

void saveConfig() {
    prefs.begin("io-config", false);
    prefs.putString("ssid", sta_ssid);
    prefs.putString("pass", sta_pass);
    prefs.putString("ui_user", ui_user);
    prefs.putString("ui_pass", ui_pass);
    prefs.putString("ota_pass", ota_pass);

    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        String key = "mm" + String(i);
        prefs.putUShort(key.c_str(), inputMapping[i]);
        key = "auto" + String(i);
        prefs.putUInt(key.c_str(), autoOffSeconds[i]);
        key = "outmode" + String(i);
        prefs.putUChar(key.c_str(), outputMode[i]);
        key = "inmode" + String(i);
        prefs.putUChar(key.c_str(), inputMode[i]);
        key = "ord" + String(i);
        prefs.putUChar(key.c_str(), outDisplayOrder[i]);
        key = "ena" + String(i);
        prefs.putUChar(key.c_str(), outChEnabled[i] ? 1 : 0);
        key = "prot" + String(i);
        prefs.putUChar(key.c_str(), outRemoteProtected[i] ? 1 : 0);
        key = "aoff" + String(i);
        prefs.putUChar(key.c_str(), outAllOffEnabled[i] ? 1 : 0);
    }
    prefs.end();
    dbg::debug(CAT_CONFIG, "Konfiguration gespeichert");
}

// ============================================================
// WebSocket
// ============================================================
String buildStateJson() {
    JsonDocument doc;
    JsonArray inputs = doc["inputs"].to<JsonArray>();
    JsonArray outputs = doc["outputs"].to<JsonArray>();
    JsonArray mappings = doc["mappings"].to<JsonArray>();
    JsonArray timers = doc["timers"].to<JsonArray>();
    JsonArray remaining = doc["remaining"].to<JsonArray>();
    JsonArray names = doc["names"].to<JsonArray>();
    JsonArray inames = doc["input_names"].to<JsonArray>();
    JsonArray lastIn    = doc["last_input"].to<JsonArray>();
    JsonArray lastOut   = doc["last_output"].to<JsonArray>();
    JsonArray outModes    = doc["out_modes"].to<JsonArray>();
    JsonArray inModes     = doc["in_modes"].to<JsonArray>();
    JsonArray outOrder    = doc["out_order"].to<JsonArray>();
    JsonArray outEnabledA = doc["out_enabled"].to<JsonArray>();
    JsonArray outProtectedA = doc["out_protected"].to<JsonArray>();
    JsonArray outAllOffA  = doc["out_alloff"].to<JsonArray>();
    JsonArray mcpStatus   = doc["mcp"].to<JsonArray>();
    unsigned long now = millis();

    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        inputs.add(inputState[i]);
        outputs.add(relayState[i]);
        mappings.add((uint16_t)inputMapping[i]);
        timers.add(autoOffSeconds[i]);
        remaining.add(getRemainingAutoOffSeconds(i, now));
        names.add(channelNames[i]);
        inames.add(inputNames[i]);
        lastIn.add((uint32_t)lastInputTime[i]);
        lastOut.add((uint32_t)lastOutputTime[i]);
        outModes.add(outputMode[i]);
        inModes.add(inputMode[i]);
        outOrder.add(outDisplayOrder[i]);
        outEnabledA.add(outChEnabled[i]);
        outProtectedA.add(outRemoteProtected[i]);
        outAllOffA.add(outAllOffEnabled[i]);
    }
    mcpStatus.add(mcpReady[0]);
    mcpStatus.add(mcpReady[1]);
    doc["time"] = dbg::getTimestamp();
    doc["ntp"] = dbg::isTimeSynced();
    if (WiFi.status() == WL_CONNECTED) {
        doc["wifi_rssi"]    = (int8_t)WiFi.RSSI();
        doc["wifi_bssid"]   = WiFi.BSSIDstr();
        doc["wifi_channel"] = WiFi.channel();
    }
#if SIMULATE_HW
    doc["sim"] = true;
#endif

    String json;
    serializeJson(doc, json);
    return json;
}

void sendState() {
    ws.textAll(buildStateJson());
}

static bool isRemoteProtected(uint8_t ch, bool armed) {
    return ch < NUM_CHANNELS && outRemoteProtected[ch] && !armed;
}

void onWebSocketEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        dbg::info(CAT_WEB, "WebSocket Client #%u verbunden", client->id());
        client->text(buildStateJson());
        updateLedState();
    } else if (type == WS_EVT_DISCONNECT) {
        dbg::info(CAT_WEB, "WebSocket Client #%u getrennt", client->id());
        updateLedState();
    } else if (type == WS_EVT_DATA) {
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) return;

        const char* cmd = doc["cmd"];
        if (!cmd) return;

        dbg::debug(CAT_WEB, "WS Kommando: %s", cmd);

        bool doSendState = true;

        if (strcmp(cmd, "toggle") == 0) {
            uint8_t ch = doc["ch"];
            if (ch < NUM_CHANNELS) {
                if (isRemoteProtected(ch, doc["armed"] | false)) {
                    dbg::warn(CAT_WEB, "Ausgang A%d geschuetzt: Toggle ignoriert", ch + 1);
                } else if (outputMode[ch] == 0) {
                    toggleRelay(ch);
                } else {
                    dbg::warn(CAT_WEB, "Toggle fuer A%d ignoriert (Ausgang im Taster-Modus)", ch + 1);
                }
            }
        } else if (strcmp(cmd, "set") == 0) {
            uint8_t ch = doc["ch"];
            bool val = doc["val"];
            if (ch < NUM_CHANNELS) {
                if (isRemoteProtected(ch, doc["armed"] | false)) {
                    dbg::warn(CAT_WEB, "Ausgang A%d geschuetzt: Set ignoriert", ch + 1);
                } else {
                    setRelay(ch, val, !val);
                }
            }
        } else if (strcmp(cmd, "map") == 0) {
            uint8_t input  = doc["input"];
            uint8_t output = doc["output"];
            bool    active = doc["active"] | false;
            if (input < NUM_CHANNELS && output < NUM_CHANNELS) {
                if (active) {
                    inputMapping[input] |=  (1u << output);
                    dbg::info(CAT_CONFIG, "Mapping E%d +-> A%d", input + 1, output + 1);
                } else {
                    inputMapping[input] &= ~(1u << output);
                    dbg::info(CAT_CONFIG, "Mapping E%d -/- A%d", input + 1, output + 1);
                }
                saveConfig();
            }
        } else if (strcmp(cmd, "timer") == 0) {
            uint8_t ch = doc["ch"];
            int32_t secsRaw = doc["secs"] | 0;
            uint32_t secs = (secsRaw < 0) ? 0 : (uint32_t)secsRaw;
            if (secs > 604800) secs = 604800;  // max. 1 Woche
            if (ch < NUM_CHANNELS) {
                autoOffSeconds[ch]    = secs;
                tempTimeAdjustMs[ch]  = 0;
                timerFreezeStart[ch]  = 0;
                timerFreezeExpiry[ch] = 0;
                if (relayState[ch]) relayOnTimestamp[ch] = millis();  // Timer ab jetzt
                dbg::info(CAT_TIMER, "Auto-Aus A%d: %u s", ch + 1, secs);
                saveConfig();
            }
        } else if (strcmp(cmd, "set_web_creds") == 0) {
            String newUser = doc["ui_user"].as<String>();
            String newPass = doc["ui_pass"].as<String>();
            if (newUser.length() > 0) ui_user = newUser;
            if (newPass.length() > 0) ui_pass = newPass;
            saveConfig();
            applyWebAuth();
            dbg::info(CAT_CONFIG, "Web-Zugangsdaten geaendert: '%s'", ui_user.c_str());
            client->text("{\"type\":\"creds_updated\"}");
            doSendState = false;
        } else if (strcmp(cmd, "set_ota_pass") == 0) {
            String newOta = doc["ota_pass"].as<String>();
            if (newOta.length() > 0) {
                ota_pass = newOta;
                saveConfig();
                ArduinoOTA.setPassword(ota_pass.c_str());
                dbg::info(CAT_CONFIG, "OTA-Passwort geaendert");
                client->text("{\"type\":\"ota_pass_saved\"}");
            }
            doSendState = false;
        } else if (strcmp(cmd, "out_mode") == 0) {
            uint8_t ch   = doc["ch"]   | 0;
            uint8_t mode = doc["mode"] | 0;
            if (ch < NUM_CHANNELS && mode <= 1) {
                outputMode[ch] = mode;
                prefs.begin("io-config", false);
                prefs.putUChar(("outmode" + String(ch)).c_str(), outputMode[ch]);
                prefs.end();
                dbg::info(CAT_CONFIG, "Ausgangs-Modus A%d: %s", ch + 1, mode ? "Taster" : "Toggle");
            }
        } else if (strcmp(cmd, "in_mode") == 0) {
            uint8_t ch   = doc["ch"]   | 0;
            uint8_t mode = doc["mode"] | 0;
            if (ch < NUM_CHANNELS && mode <= 1) {
                inputMode[ch] = mode;
                // Zustand zurücksetzen damit kein Restaustand bleibt
                signalActivePrev[ch] = false;
                longPressFired[ch]   = false;
                inputState[ch]       = false;
                prefs.begin("io-config", false);
                prefs.putUChar(("inmode" + String(ch)).c_str(), inputMode[ch]);
                prefs.end();
                dbg::info(CAT_CONFIG, "Eingangs-Modus E%d: %s", ch + 1, mode ? "Taster" : "Toggle");
            }
        } else if (strcmp(cmd, "set_out_order") == 0) {
            JsonArray arr = doc["order"];
            if (arr.size() == NUM_CHANNELS) {
                bool used[NUM_CHANNELS] = {};
                bool valid = true;
                for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                    uint8_t v = arr[i].as<uint8_t>();
                    if (v >= NUM_CHANNELS || used[v]) { valid = false; break; }
                    used[v] = true;
                }
                if (valid) {
                    prefs.begin("io-config", false);
                    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                        outDisplayOrder[i] = arr[i].as<uint8_t>();
                        prefs.putUChar(("ord" + String(i)).c_str(), outDisplayOrder[i]);
                    }
                    prefs.end();
                    dbg::info(CAT_CONFIG, "Ausgangs-Reihenfolge aktualisiert");
                }
            }
        } else if (strcmp(cmd, "set_out_enabled") == 0) {
            uint8_t ch  = doc["ch"]      | 0;
            bool    ena = doc["enabled"] | true;
            if (ch < NUM_CHANNELS) {
                outChEnabled[ch] = ena;
                prefs.begin("io-config", false);
                prefs.putUChar(("ena" + String(ch)).c_str(), ena ? 1 : 0);
                prefs.end();
                dbg::info(CAT_CONFIG, "Ausgang A%d: %s", ch + 1, ena ? "aktiv" : "deaktiviert");
            }
        } else if (strcmp(cmd, "set_out_protected") == 0) {
            uint8_t ch  = doc["ch"]      | 0;
            bool    ena = doc["enabled"] | false;
            if (ch < NUM_CHANNELS) {
                outRemoteProtected[ch] = ena;
                prefs.begin("io-config", false);
                prefs.putUChar(("prot" + String(ch)).c_str(), ena ? 1 : 0);
                prefs.end();
                dbg::info(CAT_CONFIG, "Ausgang A%d Fernschutz: %s", ch + 1, ena ? "aktiv" : "aus");
            }
        } else if (strcmp(cmd, "set_out_alloff") == 0) {
            uint8_t ch  = doc["ch"]      | 0;
            bool    ena = doc["enabled"] | false;
            if (ch < NUM_CHANNELS) {
                outAllOffEnabled[ch] = ena;
                prefs.begin("io-config", false);
                prefs.putUChar(("aoff" + String(ch)).c_str(), ena ? 1 : 0);
                prefs.end();
                dbg::info(CAT_CONFIG, "Ausgang A%d bei 'Alles aus': %s",
                          ch + 1, ena ? "ausschalten" : "ausgenommen");
            }
        } else if (strcmp(cmd, "wifi") == 0) {
            String newSsid = doc["ssid"].as<String>();
            if (newSsid.length() > 0) sta_ssid = newSsid;
            String newPass = doc["pass"].as<String>();
            if (newPass.length() > 0) sta_pass = newPass;
            dbg::info(CAT_WIFI, "WiFi-Konfiguration geaendert: '%s'", sta_ssid.c_str());
            saveConfig();
            dbg::warn(CAT_SYSTEM, "Neustart in 1s...");
            statusled::setState(statusled::ST_BOOTING);
            statusled::update();
            delay(1000);
            ESP.restart();
        } else if (strcmp(cmd, "alloff") == 0) {
            pendingAllOff = true;  // in loop() ausführen; sendState() folgt dort
            doSendState = false;
        } else if (strcmp(cmd, "load_config") == 0) {
            // Komplette E/A-Konfiguration laden (Namen, Zuordnungen, Timer)
            JsonArray jNames    = doc["names"];
            JsonArray jINames   = doc["input_names"];
            JsonArray jMappings = doc["mappings"];
            JsonArray jTimers   = doc["timers"];
            JsonArray jOutModes   = doc["out_modes"];
            JsonArray jInModes    = doc["in_modes"];
            JsonArray jOutOrder   = doc["out_order"];
            JsonArray jOutEnabled = doc["out_enabled"];
            JsonArray jOutProtected = doc["out_protected"];
            JsonArray jOutAllOff    = doc["out_alloff"];

            if (jNames.size() < NUM_CHANNELS || jINames.size() < NUM_CHANNELS ||
                jMappings.size() < NUM_CHANNELS || jTimers.size() < NUM_CHANNELS) {
                dbg::warn(CAT_CONFIG, "load_config abgelehnt: unvollstaendige E/A-Konfiguration");
            } else {
                const uint16_t validMappingMask = (uint16_t)((1u << NUM_CHANNELS) - 1u);
                prefs.begin("io-config", false);
                for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                    const char* outName = jNames[i];
                    if (outName) {
                        strncpy(channelNames[i], outName, CH_NAME_MAX_LEN);
                        channelNames[i][CH_NAME_MAX_LEN] = '\0';
                        prefs.putString(("name" + String(i)).c_str(), channelNames[i]);
                    }

                    const char* inName = jINames[i];
                    if (inName) {
                        strncpy(inputNames[i], inName, CH_NAME_MAX_LEN);
                        inputNames[i][CH_NAME_MAX_LEN] = '\0';
                        prefs.putString(("iname" + String(i)).c_str(), inputNames[i]);
                    }

                    // Bits auf NUM_CHANNELS begrenzen
                    int32_t rawMapping = jMappings[i].as<int32_t>();
                    inputMapping[i] = rawMapping > 0 ? ((uint16_t)rawMapping & validMappingMask) : 0;
                    prefs.putUShort(("mm" + String(i)).c_str(), inputMapping[i]);

                    int64_t rawSecs = jTimers[i].as<int64_t>();
                    uint32_t secs = 0;
                    if (rawSecs > 0) secs = rawSecs > 604800 ? 604800 : (uint32_t)rawSecs;
                    autoOffSeconds[i]   = secs;
                    tempTimeAdjustMs[i] = 0;
                    timerFreezeStart[i]  = 0;
                    timerFreezeExpiry[i] = 0;
                    if (relayState[i]) relayOnTimestamp[i] = millis();  // Timer ab jetzt
                    prefs.putUInt(("auto" + String(i)).c_str(), secs);

                    uint8_t outMode = jOutModes.size() > i ? (uint8_t)(jOutModes[i].as<uint8_t>() > 0 ? 1 : 0) : outputMode[i];
                    uint8_t inMode  = jInModes.size()  > i ? (uint8_t)(jInModes[i].as<uint8_t>()  > 0 ? 1 : 0) : inputMode[i];
                    outputMode[i] = outMode;
                    inputMode[i]  = inMode;
                    prefs.putUChar(("outmode" + String(i)).c_str(), outputMode[i]);
                    prefs.putUChar(("inmode"  + String(i)).c_str(), inputMode[i]);

                    if (jOutOrder.size() == NUM_CHANNELS) {
                        uint8_t v = jOutOrder[i].as<uint8_t>();
                        outDisplayOrder[i] = (v < NUM_CHANNELS) ? v : i;
                        prefs.putUChar(("ord" + String(i)).c_str(), outDisplayOrder[i]);
                    }
                    if (jOutEnabled.size() == NUM_CHANNELS) {
                        outChEnabled[i] = jOutEnabled[i].as<bool>();
                        prefs.putUChar(("ena" + String(i)).c_str(), outChEnabled[i] ? 1 : 0);
                    }
                    if (jOutProtected.size() == NUM_CHANNELS) {
                        outRemoteProtected[i] = jOutProtected[i].as<bool>();
                        prefs.putUChar(("prot" + String(i)).c_str(), outRemoteProtected[i] ? 1 : 0);
                    }
                    if (jOutAllOff.size() == NUM_CHANNELS) {
                        outAllOffEnabled[i] = jOutAllOff[i].as<bool>();
                        prefs.putUChar(("aoff" + String(i)).c_str(), outAllOffEnabled[i] ? 1 : 0);
                    }
                }
                prefs.end();
                dbg::info(CAT_CONFIG, "E/A-Konfiguration via load_config geladen");
            }
        } else if (strcmp(cmd, "reset_io") == 0) {
            // Werkseinstellung E/A: alle Relais aus, dann Namen/Zuordnungen/Timer zurücksetzen
            for (uint8_t i = 0; i < NUM_CHANNELS; i++)
                if (relayState[i]) setRelay(i, false);
            prefs.begin("io-config", false);
            for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                inputMapping[i]    = 0;
                autoOffSeconds[i]  = 0;
                outputMode[i]      = 0;
                inputMode[i]       = 0;
                tempTimeAdjustMs[i] = 0;
                outDisplayOrder[i] = i;
                outChEnabled[i]    = true;
                outRemoteProtected[i] = false;
                outAllOffEnabled[i]   = true;
                snprintf(channelNames[i], CH_NAME_MAX_LEN + 1, "Ausgang %d", i + 1);
                snprintf(inputNames[i],   CH_NAME_MAX_LEN + 1, "Eingang %d", i + 1);
                prefs.putUShort(("mm"    + String(i)).c_str(), 0);
                prefs.putUInt  (("auto"  + String(i)).c_str(), 0);
                prefs.putUChar (("outmode" + String(i)).c_str(), 0);
                prefs.putUChar (("inmode"  + String(i)).c_str(), 0);
                prefs.putUChar (("ord"   + String(i)).c_str(), i);
                prefs.putUChar (("ena"   + String(i)).c_str(), 1);
                prefs.putUChar (("prot"  + String(i)).c_str(), 0);
                prefs.putUChar (("aoff"  + String(i)).c_str(), 1);
                prefs.putString(("name"  + String(i)).c_str(), channelNames[i]);
                prefs.putString(("iname" + String(i)).c_str(), inputNames[i]);
            }
            prefs.end();
            dbg::info(CAT_CONFIG, "Werkseinstellung E/A durchgeführt");
        } else if (strcmp(cmd, "name") == 0) {
            uint8_t ch = doc["ch"];
            const char* nm = doc["name"];
            if (ch < NUM_CHANNELS && nm) {
                strncpy(channelNames[ch], nm, CH_NAME_MAX_LEN);
                channelNames[ch][CH_NAME_MAX_LEN] = '\0';
                dbg::info(CAT_CONFIG, "Kanalname %d: '%s'", ch + 1, channelNames[ch]);
                prefs.begin("io-config", false);
                prefs.putString(("name" + String(ch)).c_str(), channelNames[ch]);
                prefs.end();
                if (ch == displayChannel) updateDisplay();
            }
        } else if (strcmp(cmd, "iname") == 0) {
            uint8_t ch = doc["ch"];
            const char* nm = doc["name"];
            if (ch < NUM_CHANNELS && nm) {
                strncpy(inputNames[ch], nm, CH_NAME_MAX_LEN);
                inputNames[ch][CH_NAME_MAX_LEN] = '\0';
                dbg::info(CAT_CONFIG, "Eingangsname %d: '%s'", ch + 1, inputNames[ch]);
                prefs.begin("io-config", false);
                prefs.putString(("iname" + String(ch)).c_str(), inputNames[ch]);
                prefs.end();
            }
        }

        if (doSendState) sendState();
    }
}

// ============================================================
// Top Board: Taster-Aktion verarbeiten
// ============================================================
void handleButton(uint8_t btn) {
    static const char* const btnNames[] = {"ENTER", "UP", "DOWN", "LEFT", "RIGHT"};
    dbg::info(CAT_SYSTEM, "Taster: %s", btnNames[btn]);

    unsigned long now = millis();

    if (btn == BTN_UP) {
        displayChannel = (displayChannel + 1) % NUM_CHANNELS;
        updateDisplay();
    } else if (btn == BTN_DOWN) {
        displayChannel = (displayChannel + NUM_CHANNELS - 1) % NUM_CHANNELS;
        updateDisplay();
    } else if (btn == BTN_ENTER) {
        toggleRelay(displayChannel);
        sendState();
    } else if (btn == BTN_RIGHT || btn == BTN_LEFT) {
        uint8_t ch = displayChannel;
        if (relayState[ch]) {
            if (autoOffSeconds[ch] == 0 && tempTimeAdjustMs[ch] == 0) {
                // Erster Tastendruck: Startwert LEFT=1h, RIGHT=6h
                tempTimeAdjustMs[ch] = (btn == BTN_LEFT) ? 3600000L : 21600000L;
                if (timerFreezeStart[ch] == 0) timerFreezeStart[ch] = now;
                timerFreezeExpiry[ch] = now + 5000;
                updateDisplay();
                sendState();
            } else {
                // Freeze aktivieren/verlängern
                if (timerFreezeStart[ch] == 0) timerFreezeStart[ch] = now;
                timerFreezeExpiry[ch] = now + 5000;

                uint32_t rem  = getRemainingAutoOffSeconds(ch, timerFreezeStart[ch]);
                uint32_t step = (uint32_t)(btnStepMs(rem) / 1000);

                uint32_t newRem;
                if (btn == BTN_RIGHT) {
                    newRem = (rem / step + 1) * step;
                    if (newRem > 604800) newRem = 604800;
                } else {
                    uint32_t snapped = (rem / step) * step;
                    newRem = (snapped < rem) ? snapped : (snapped >= step ? snapped - step : 0);
                }

                if (newRem == 0) {
                    timerFreezeStart[ch]  = 0;
                    timerFreezeExpiry[ch] = 0;
                    setRelay(ch, false);
                } else {
                    tempTimeAdjustMs[ch] += (int32_t)(newRem - rem) * 1000;
                    updateDisplay();
                }
                sendState();
            }
        }
    }
}

// ============================================================
// WiFi Setup
// ============================================================
void onDhcpLeaseAssigned(uint8_t client_ip[4]) {
    dbg::info(CAT_WIFI, "DHCPS Callback: Lease vergeben -> %u.%u.%u.%u",
              client_ip[0], client_ip[1], client_ip[2], client_ip[3]);
}

void setupWiFiEvents() {
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_AP_START:
                dbg::info(CAT_WIFI, "WiFi Event: AP gestartet");
                break;
            case ARDUINO_EVENT_WIFI_AP_STOP:
                dbg::warn(CAT_WIFI, "WiFi Event: AP gestoppt");
                break;
            case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
                dbg::info(CAT_WIFI,
                          "WiFi Event: Station verbunden (AID=%u, MAC=%02X:%02X:%02X:%02X:%02X:%02X)",
                          info.wifi_ap_staconnected.aid,
                          info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
                          info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
                          info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
                break;
            case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
                dbg::warn(CAT_WIFI,
                          "WiFi Event: Station getrennt (AID=%u, MAC=%02X:%02X:%02X:%02X:%02X:%02X)",
                          info.wifi_ap_stadisconnected.aid,
                          info.wifi_ap_stadisconnected.mac[0], info.wifi_ap_stadisconnected.mac[1],
                          info.wifi_ap_stadisconnected.mac[2], info.wifi_ap_stadisconnected.mac[3],
                          info.wifi_ap_stadisconnected.mac[4], info.wifi_ap_stadisconnected.mac[5]);
                break;
            case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
                dbg::info(CAT_WIFI, "WiFi Event: DHCP Lease vergeben -> " IPSTR,
                          IP2STR(&info.wifi_ap_staipassigned.ip));
                break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                dbg::info(CAT_WIFI, "WiFi Event: STA hat IP -> %s",
                          IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
                dbg::ntpSync("CET-1CEST,M3.5.0,M10.5.0/3");
                statusled::setState(statusled::ST_WIFI_NO_NTP);
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                dbg::warn(CAT_WIFI, "WiFi Event: STA getrennt (Reason=%u)",
                          info.wifi_sta_disconnected.reason);
                break;
            default:
                break;
        }
    });
}

bool startAccessPoint() {
    WiFi.softAPdisconnect(false);
    delay(100);

    bool cfgOk = WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    dbg::info(CAT_WIFI, "softAPConfig(): %s", cfgOk ? "OK" : "FEHLER");
    if (!cfgOk) {
        return false;
    }

    bool apOk = WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 4);
    dbg::info(CAT_WIFI, "softAP(): %s", apOk ? "OK" : "FEHLER");
    if (apOk) {
        dhcps_set_new_lease_cb(onDhcpLeaseAssigned);
        dbg::info(CAT_WIFI, "AP gestartet: %s -> %s", AP_SSID, WiFi.softAPIP().toString().c_str());
    }
    return apOk;
}

void ensureApDhcpServer() {
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        dbg::error(CAT_WIFI, "DHCP-Pruefung fehlgeschlagen: AP netif nicht gefunden");
        return;
    }

    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    esp_err_t statusErr = esp_netif_dhcps_get_status(ap_netif, &status);
    if (statusErr != ESP_OK) {
        dbg::error(CAT_WIFI, "DHCP-Status kann nicht gelesen werden: %d", statusErr);
        return;
    }

    if (status == ESP_NETIF_DHCP_STARTED) {
        return;
    }

    dbg::warn(CAT_WIFI, "DHCP Server nicht aktiv - starte manuell...");
    esp_netif_dhcps_stop(ap_netif);
    delay(100);
    esp_err_t startErr = esp_netif_dhcps_start(ap_netif);
    dbg::info(CAT_WIFI, "DHCP manueller Start: %s (err=%d)", startErr == ESP_OK ? "OK" : "FEHLER", startErr);
}

void diagnoseDHCP() {
    if (!ENABLE_DHCP_DIAG) {
        return;
    }

    dbg::info(CAT_WIFI, "--- DHCP Server Diagnose ---");

    // AP IP prÃ¼fen
    IPAddress apIP = WiFi.softAPIP();
    dbg::info(CAT_WIFI, "softAPIP(): %s", apIP.toString().c_str());

    // AP Netif holen
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        dbg::error(CAT_WIFI, "AP netif NICHT GEFUNDEN!");
        return;
    }
    dbg::info(CAT_WIFI, "AP netif Handle: OK");

    // Netif IP-Info
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (err == ESP_OK) {
        dbg::info(CAT_WIFI, "Netif IP:      " IPSTR, IP2STR(&ip_info.ip));
        dbg::info(CAT_WIFI, "Netif Gateway: " IPSTR, IP2STR(&ip_info.gw));
        dbg::info(CAT_WIFI, "Netif Netmask: " IPSTR, IP2STR(&ip_info.netmask));
    } else {
        dbg::error(CAT_WIFI, "esp_netif_get_ip_info Fehler: %d", err);
    }

    // DHCP Server Status
    esp_netif_dhcp_status_t dhcp_status;
    err = esp_netif_dhcps_get_status(ap_netif, &dhcp_status);
    if (err == ESP_OK) {
        const char* statusStr = "UNBEKANNT";
        switch (dhcp_status) {
            case ESP_NETIF_DHCP_INIT:    statusStr = "INIT"; break;
            case ESP_NETIF_DHCP_STARTED: statusStr = "GESTARTET"; break;
            case ESP_NETIF_DHCP_STOPPED: statusStr = "GESTOPPT"; break;
        }
        dbg::info(CAT_WIFI, "DHCP Server Status: %s (%d)", statusStr, dhcp_status);
    } else {
        dbg::error(CAT_WIFI, "DHCP Status Fehler: %d", err);
    }

    // DHCP Lease Range prÃ¼fen
    dhcps_lease_t lease;
    lease.enable = true;
    err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_GET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease));
    if (err == ESP_OK) {
        dbg::info(CAT_WIFI, "DHCP Lease Start: " IPSTR, IP2STR(&lease.start_ip));
        dbg::info(CAT_WIFI, "DHCP Lease End:   " IPSTR, IP2STR(&lease.end_ip));
    } else {
        dbg::warn(CAT_WIFI, "DHCP Lease Info nicht verfuegbar: %d", err);
    }

    // WiFi Modus
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    dbg::info(CAT_WIFI, "WiFi Modus: %d (1=STA, 2=AP, 3=AP+STA)", mode);

    // Anzahl verbundener Stationen
    dbg::info(CAT_WIFI, "Verbundene Stationen: %d", WiFi.softAPgetStationNum());

    dbg::info(CAT_WIFI, "--- Ende DHCP Diagnose ---");
}

void setupWiFi() {
    // WiFi komplett initialisieren
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

    // Modus setzen: AP oder AP+STA
    if (sta_ssid.length() > 0) {
        WiFi.mode(WIFI_AP_STA);
        dbg::info(CAT_WIFI, "WiFi Modus: AP+STA");
    } else {
        WiFi.mode(WIFI_AP);
        dbg::info(CAT_WIFI, "WiFi Modus: AP");
    }
    delay(100);

    // AP starten
    bool apOk = startAccessPoint();
    delay(1000);

    // DHCP Server Diagnose
    diagnoseDHCP();

    // Falls DHCP nicht laeuft, manuell starten
    ensureApDhcpServer();
    if (apOk) {
        diagnoseDHCP();
    }

    // Optional STA-Verbindung
    if (sta_ssid.length() > 0) {
        statusled::setState(statusled::ST_WIFI_CONNECTING);
        statusled::update();

        WiFi.setHostname("HS-IO");

        // Ohne diese beiden Zeilen nutzt der ESP32 WIFI_FAST_SCAN und verbindet sich
        // mit dem ERSTEN gefundenen AP dieser SSID, nicht mit dem staerksten. Bei
        // mehreren APs (Repeater/Mesh) landet das Board sonst nach jedem Neustart
        // zufaellig auf einem weit entfernten AP. Kostet ~1-2s zusaetzliche Bootzeit.
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
        WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

        WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());
        dbg::info(CAT_WIFI, "Verbinde mit '%s'...", sta_ssid.c_str());

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            statusled::update();
            delay(100);
        }

        if (WiFi.status() == WL_CONNECTED) {
            dbg::info(CAT_WIFI, "WiFi verbunden! IP: %s, AP: %s, Kanal %d, RSSI %d dBm (%d%%)",
                      WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str(),
                      WiFi.channel(), WiFi.RSSI(),
                      min(100, max(0, 2 * (WiFi.RSSI() + 100))));
            // NTP-Sync erfolgt im GOT_IP-Event-Handler (vermeidet Doppelaufruf)
            statusled::setState(statusled::ST_WIFI_NO_NTP);
        } else {
            // Timeout aber im AP+STA-Modus bleiben — ESP32 verbindet asynchron weiter.
            // NTP-Sync erfolgt im GOT_IP-Event-Handler sobald Verbindung steht.
            dbg::warn(CAT_WIFI, "WiFi-Verbindungsversuch laeuft im Hintergrund weiter...");
            statusled::setState(statusled::ST_AP_ONLY);
        }

        // Nochmal DHCP-Status pruefen nach STA-Verbindungsversuch
        if (ENABLE_DHCP_DIAG) {
            dbg::info(CAT_WIFI, "DHCP Status nach STA-Versuch:");
            diagnoseDHCP();
        }
    } else {
        dbg::info(CAT_WIFI, "Kein WiFi konfiguriert, nur AP-Modus");
        statusled::setState(statusled::ST_AP_ONLY);
    }
}

// ============================================================
// Web Server
// ============================================================
void setupWebServer() {
    g_apiHandler = &server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        JsonArray inputs = doc["inputs"].to<JsonArray>();
        JsonArray outputs = doc["outputs"].to<JsonArray>();
        JsonArray mappings = doc["mappings"].to<JsonArray>();
        JsonArray timers = doc["timers"].to<JsonArray>();
        JsonArray remaining = doc["remaining"].to<JsonArray>();
        JsonArray names = doc["names"].to<JsonArray>();
        JsonArray inames = doc["input_names"].to<JsonArray>();
        JsonArray outModes    = doc["out_modes"].to<JsonArray>();
        JsonArray inModes     = doc["in_modes"].to<JsonArray>();
        JsonArray outOrderA   = doc["out_order"].to<JsonArray>();
        JsonArray outEnabledA = doc["out_enabled"].to<JsonArray>();
        JsonArray outProtectedA = doc["out_protected"].to<JsonArray>();
        JsonArray outAllOffA  = doc["out_alloff"].to<JsonArray>();
        unsigned long now = millis();
        for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
            inputs.add(inputState[i]);
            outputs.add(relayState[i]);
            mappings.add((uint16_t)inputMapping[i]);
            timers.add(autoOffSeconds[i]);
            remaining.add(getRemainingAutoOffSeconds(i, now));
            names.add(channelNames[i]);
            inames.add(inputNames[i]);
            outModes.add(outputMode[i]);
            inModes.add(inputMode[i]);
            outOrderA.add(outDisplayOrder[i]);
            outEnabledA.add(outChEnabled[i]);
            outProtectedA.add(outRemoteProtected[i]);
            outAllOffA.add(outAllOffEnabled[i]);
        }
        doc["ap_ip"] = WiFi.softAPIP().toString();
        doc["sta_ip"] = WiFi.localIP().toString();
        doc["sta_ssid"] = sta_ssid;
        doc["sta_pass"] = sta_pass;
        doc["version"] = FW_VERSION;
        if (WiFi.status() == WL_CONNECTED) {
            doc["wifi_rssi"]    = (int8_t)WiFi.RSSI();
            doc["wifi_bssid"]   = WiFi.BSSIDstr();
            doc["wifi_channel"] = WiFi.channel();
        }
        doc["mcp1"] = mcpReady[0];
        doc["mcp2"] = mcpReady[1];
        doc["time"] = dbg::getTimestamp();
        doc["ntp"] = dbg::isTimeSynced();
#if SIMULATE_HW
        doc["sim"] = true;
#endif

        String json;
        serializeJson(doc, json);
        req->send(200, "application/json", json);
    });
    g_apiHandler->setAuthentication(ui_user.c_str(), ui_pass.c_str());

    g_cmdHandler = &server.on("/api/cmd", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("cmd")) {
            req->send(400, "application/json", "{\"ok\":false,\"error\":\"missing cmd\"}");
            return;
        }

        String cmd = req->getParam("cmd")->value();
        bool ok = true;

        if (cmd == "toggle") {
            if (!req->hasParam("ch")) {
                ok = false;
            } else {
                uint8_t ch = (uint8_t)req->getParam("ch")->value().toInt();
                bool armed = req->hasParam("armed") && req->getParam("armed")->value().toInt() != 0;
                if (ch < NUM_CHANNELS && isRemoteProtected(ch, armed)) {
                    ok = false;
                } else if (ch < NUM_CHANNELS && outputMode[ch] == 0) {
                    toggleRelay(ch);
                } else {
                    ok = false;
                }
            }
        } else if (cmd == "set") {
            if (!req->hasParam("ch") || !req->hasParam("val")) {
                ok = false;
            } else {
                uint8_t ch = (uint8_t)req->getParam("ch")->value().toInt();
                bool val = req->getParam("val")->value().toInt() != 0;
                bool armed = req->hasParam("armed") && req->getParam("armed")->value().toInt() != 0;
                if (ch < NUM_CHANNELS && !isRemoteProtected(ch, armed)) setRelay(ch, val, !val);
                else ok = false;
            }
        } else if (cmd == "alloff") {
            pendingAllOff = true;
        } else {
            ok = false;
        }

        if (ok) {
            sendState();
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(400, "application/json", "{\"ok\":false}");
        }
    });
    g_cmdHandler->setAuthentication(ui_user.c_str(), ui_pass.c_str());

    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(204);
    });

    ElegantOTA.begin(&server);
    ElegantOTA.setAutoReboot(true);
    ElegantOTA.setAuth(ui_user.c_str(), ui_pass.c_str());

    ws.onEvent(onWebSocketEvent);
    ws.setAuthentication(ui_user.c_str(), ui_pass.c_str());
    server.addHandler(&ws);

    g_staticHandler = &server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    g_staticHandler->setAuthentication(ui_user.c_str(), ui_pass.c_str());

    server.begin();
    dbg::info(CAT_WEB, "Webserver gestartet auf Port 80");
}

// ============================================================
// Input Pins
// ============================================================
void setupInputPins() {
    unsigned long now = millis();
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        pinMode(INPUT_PINS[i], INPUT);
        bool r = digitalRead(INPUT_PINS[i]);
        inputRawPrev[i]      = r;
        inputLastEdgeMs[i]   = now - 200;   // Holdoff-Fenster ab erstem loop() abgelaufen
        signalActivePrev[i]  = !r;          // initiales signalActive vorab setzen
    }
}

// ============================================================
// Main
// ============================================================
void setup() {
    dbg::begin(dbg::LVL_DEBUG, dbg::CAT_ALL);

#if OLED_HARDTEST
    pinMode(RESET_PERIPHERIE_PIN, OUTPUT);
    digitalWrite(RESET_PERIPHERIE_PIN, HIGH);
    delay(10);

    i2cBusRecover();
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setTimeOut(10);
    Wire.setClock(100000);
    runOledHardTest();
    return;
#endif

    pinMode(RESET_PERIPHERIE_PIN, OUTPUT);
    digitalWrite(RESET_PERIPHERIE_PIN, HIGH);
    delay(10);

    statusled::begin(20);
    statusled::setState(statusled::ST_BOOTING);

    dbg::info(CAT_SYSTEM, "=== IO-Hutschienenboard ===");
    dbg::info(CAT_SYSTEM, "12-Kanal I/O mit MCP23017");
#if SIMULATE_HW
    dbg::warn(CAT_SYSTEM, "*** SIMULATIONSMODUS - keine echte Hardware ***");
#endif

    setupInputPins();
    setupMCP();
    if (display::begin(I2C_SDA_PIN, I2C_SCL_PIN, OLED_ADDR)) {
        dbg::info(CAT_SYSTEM, "OLED Display OK (0x%02X, %s)", display::address(), display::controller());
        display::showBoot(FW_VERSION);
    } else {
        dbg::warn(CAT_SYSTEM, "OLED Display nicht gefunden (nur 0x%02X getestet)", OLED_ADDR);
    }
    loadConfig();

    // Credential-Reset: ENTER beim Boot 5s gehalten → ui_user/ui_pass auf Standard zurücksetzen
    if (mcpTopReady) {
        uint16_t gpioVal = mcpTop.readGPIOAB();
        if (!((gpioVal >> BTN_ENTER) & 1u)) {  // ENTER aktiv-low
            bool held = true;
            for (int s = 5; s > 0 && held; s--) {
                char buf[22];
                snprintf(buf, sizeof(buf), "ENTER halten: %d s", s);
                display::showMessage("Passwort-Reset", buf, "Loslassen = Abbruch");
                unsigned long t = millis();
                while (millis() - t < 1000) {
                    if ((mcpTop.readGPIOAB() >> BTN_ENTER) & 1u) { held = false; break; }
                    delay(50);
                }
            }
            if (held) {
                ui_user  = "admin";
                ui_pass  = "admin";
                ota_pass = "admin";
                saveConfig();
                dbg::warn(CAT_CONFIG, "Alle Credentials zurueckgesetzt auf admin");
                display::showMessage("Passwort-Reset", "Zurueckgesetzt!", "admin / admin / admin");
                delay(2000);
            }
        }

        // E/A-Werkseinstellung: DOWN beim Boot 5s gehalten → Namen, Zuordnungen, Timer zurücksetzen
        gpioVal = mcpTop.readGPIOAB();
        if (!((gpioVal >> BTN_DOWN) & 1u)) {  // DOWN aktiv-low
            bool held = true;
            for (int s = 5; s > 0 && held; s--) {
                char buf[22];
                snprintf(buf, sizeof(buf), "DOWN halten: %d s", s);
                display::showMessage("E/A-Reset", buf, "Loslassen = Abbruch");
                unsigned long t = millis();
                while (millis() - t < 1000) {
                    if ((mcpTop.readGPIOAB() >> BTN_DOWN) & 1u) { held = false; break; }
                    delay(50);
                }
            }
            if (held) {
                prefs.begin("io-config", false);
                for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                    inputMapping[i]   = 0;
                    autoOffSeconds[i] = 0;
                    snprintf(channelNames[i], CH_NAME_MAX_LEN + 1, "Ausgang %d", i + 1);
                    snprintf(inputNames[i],   CH_NAME_MAX_LEN + 1, "Eingang %d", i + 1);
                    prefs.putUShort(("mm"    + String(i)).c_str(), 0);
                    prefs.putUInt  (("auto"  + String(i)).c_str(), 0);
                    prefs.putString(("name"  + String(i)).c_str(), channelNames[i]);
                    prefs.putString(("iname" + String(i)).c_str(), inputNames[i]);
                }
                prefs.end();
                dbg::warn(CAT_CONFIG, "E/A-Werkseinstellung durchgefuehrt");
                display::showMessage("E/A-Reset", "Zurueckgesetzt!", "Namen + Zuordnungen");
                delay(2000);
            }
        }
    }

    setupWiFiEvents();

    if (!LittleFS.begin(true)) {
        dbg::error(CAT_SYSTEM, "LittleFS mount fehlgeschlagen!");
        statusled::setState(statusled::ST_CONFIG_ERROR);
        statusled::update();
    } else {
        dbg::info(CAT_SYSTEM, "LittleFS OK");
    }

    setupWiFi();
    setupWebServer();

    if (MDNS.begin("HS-IO")) {
        MDNS.addService("http", "tcp", 80);
        dbg::info(CAT_WIFI, "mDNS: hs-io.local");
    } else {
        dbg::warn(CAT_WIFI, "mDNS start fehlgeschlagen");
    }

    ArduinoOTA.setHostname("HS-IO");
    ArduinoOTA.setPassword(ota_pass.c_str());
    ArduinoOTA.onStart([]() { dbg::info(CAT_SYSTEM, "ArduinoOTA Start"); });
    ArduinoOTA.onEnd([]()   { dbg::info(CAT_SYSTEM, "ArduinoOTA Ende"); });
    ArduinoOTA.onError([](ota_error_t e) { dbg::error(CAT_SYSTEM, "ArduinoOTA Fehler: %u", e); });
    ArduinoOTA.begin();
    dbg::info(CAT_SYSTEM, "ArduinoOTA bereit (Port 3232)");

    // Reset all relays to OFF on startup (RESET-Impuls auf MCP 0x21)
#if !SIMULATE_HW
    if (mcpReady[MCP_RESET]) {
        for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
            mcp[MCP_RESET].digitalWrite(RELAY_PINS[i], HIGH);
        }
        delay(RELAY_PULSE_MS);
        for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
            mcp[MCP_RESET].digitalWrite(RELAY_PINS[i], LOW);
        }
    }
#endif
    dbg::info(CAT_RELAY, "Alle Relais zurueckgesetzt");

    updateLedState();
    display::showIP(WiFi.softAPIP().toString().c_str(),
                    WiFi.localIP().toString().c_str());
    delay(3000);
    updateDisplay();
    dbg::info(CAT_SYSTEM, "Setup abgeschlossen - System bereit");
}

void loop() {
#if OLED_HARDTEST
    delay(1000);
    return;
#endif

    ArduinoOTA.handle();
    ElegantOTA.loop();

    if (pendingAllOff) {
        pendingAllOff = false;
        allOutputsOff("Alles-Aus-Kommando");
        sendState();
    }

    ws.cleanupClients();
    statusled::update();

    // Top Board: Taster via Polling (50ms-Takt) + INTA-Interrupt als Trigger
#if !SIMULATE_HW
    if (mcpTopReady) {
        static unsigned long lastPollMs  = 0;
        static uint8_t       lastGpa     = 0xFF;
        static uint8_t       heldBtn     = 0xFF;   // aktuell gehaltene Taste
        static unsigned long heldSince   = 0;       // wann gedrückt
        static unsigned long lastRepeatMs = 0;      // letzter Repeat-Feuerzeitpunkt
        static const uint32_t REPEAT_DELAY_MS  = 600;
        static const uint32_t REPEAT_PERIOD_MS = 150;

        unsigned long nowBtn = millis();
        if (topBtnFlag || (nowBtn - lastPollMs >= 50)) {
            topBtnFlag = false;
            lastPollMs = nowBtn;
            uint16_t gpioVal = mcpTop.readGPIOAB();
            uint8_t  gpa     = gpioVal & 0xFF;
            uint8_t  held    = (~gpa) & 0x1F;   // aktuell gedrückte Bits (alle 5 Tasten)
            uint8_t  pressed = (~gpa) & lastGpa; // nur neue Flanken
            lastGpa = gpa;

            // Flanken: sofort auslösen
            if (pressed & (1 << BTN_ENTER)) handleButton(BTN_ENTER);
            if (pressed & (1 << BTN_UP))    handleButton(BTN_UP);
            if (pressed & (1 << BTN_DOWN))  handleButton(BTN_DOWN);
            if (pressed & (1 << BTN_LEFT))  { handleButton(BTN_LEFT);  heldBtn = BTN_LEFT;  heldSince = nowBtn; lastRepeatMs = nowBtn; }
            if (pressed & (1 << BTN_RIGHT)) { handleButton(BTN_RIGHT); heldBtn = BTN_RIGHT; heldSince = nowBtn; lastRepeatMs = nowBtn; }

            // Auto-Repeat für LEFT / RIGHT
            if (heldBtn != 0xFF) {
                if (held & (1 << heldBtn)) {
                    // Taste noch gehalten
                    if ((nowBtn - heldSince >= REPEAT_DELAY_MS) &&
                        (nowBtn - lastRepeatMs >= REPEAT_PERIOD_MS)) {
                        lastRepeatMs = nowBtn;
                        handleButton(heldBtn);
                    }
                } else {
                    heldBtn = 0xFF; // losgelassen
                }
            }
        }
    }
#endif

    bool stateChanged = false;

    // Eingangserkennung: einheitliche Signal-Erkennung für DC und 8VAC (50Hz)
    // signalActive = HIGH (DC) ODER letzte Flanke < 30ms her (AC-Halbwellen-Lücke)
    // Gemeinsame Press-State-Machine für Toggle- und Taster-Modus:
    //   Steigende Flanke  → pressStartMs merken, longPressFired löschen
    //   Während aktiv     → nach 2s alle Ausgänge AUS (Langdruck), einmalig
    //   Fallende Flanke   → Toggle: OR-Toggle; Taster: Ausgänge AUS (nur wenn kein Langdruck)
    static const uint32_t IN_ACTIVE_HOLD_MS = 30;    // Halbwellen-Lücke AC
    static const uint32_t IN_LONGPRESS_MS   = 2000;  // Langdruck-Schwelle
    unsigned long nowIn = millis();
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        bool raw  = digitalRead(INPUT_PINS[i]);
        bool edge = (raw != inputRawPrev[i]);
        inputRawPrev[i] = raw;

        if (edge) inputLastEdgeMs[i] = nowIn;

        // GPIO ist aktiv-LOW (Schmitt-Inverter invertiert Optokoppler-Ausgang)
        bool signalActive = !raw || (nowIn - inputLastEdgeMs[i] < IN_ACTIVE_HOLD_MS);
        bool risingEdge   =  signalActive && !signalActivePrev[i];
        bool fallingEdge  = !signalActive &&  signalActivePrev[i];
        signalActivePrev[i] = signalActive;

        // UI: Eingangs-LED folgt signalActive
        if (signalActive != inputState[i]) {
            inputState[i] = signalActive;
            stateChanged  = true;
        }

        if (risingEdge) {
            pressStartMs[i]   = nowIn;
            longPressFired[i] = false;
            lastInputTime[i]  = time(nullptr);
            if (inputMode[i] == 1) {
                // Eingangs-Tastermodus: alle gemappten Ausgänge folgen dem Eingang.
                for (uint8_t j = 0; j < NUM_CHANNELS; j++) {
                    if (inputMapping[i] & (1u << j)) setRelay(j, true);
                }
                stateChanged = true;
            } else {
                // Eingangs-Togglemodus: Ausgänge im Tastermodus folgen trotzdem dem Eingang.
                for (uint8_t j = 0; j < NUM_CHANNELS; j++) {
                    if ((inputMapping[i] & (1u << j)) && outputMode[j] == 1) {
                        setRelay(j, true);
                    }
                }
                stateChanged = true;
            }
        }

        // Langdruck nur im Eingangs-Togglemodus: Taster-Eingaenge duerfen
        // gehalten werden, ohne global alle Ausgaenge auszuschalten.
        if (inputMode[i] == 0 && signalActive && !longPressFired[i] &&
            (nowIn - pressStartMs[i] >= IN_LONGPRESS_MS)) {
            char reason[32];
            snprintf(reason, sizeof(reason), "Eingang %d Langdruck", i + 1);
            allOutputsOff(reason);
            longPressFired[i] = true;
            stateChanged = true;
        }

        if (fallingEdge && !longPressFired[i]) {
            if (inputMode[i] == 1) {
                // Eingangs-Tastermodus: alle gemappten Ausgänge AUS.
                for (uint8_t j = 0; j < NUM_CHANNELS; j++) {
                    if (inputMapping[i] & (1u << j)) setRelay(j, false);
                }
            } else {
                // Ausgänge im Tastermodus AUS; nur Toggle-Ausgänge werden getoggelt.
                for (uint8_t j = 0; j < NUM_CHANNELS; j++) {
                    if ((inputMapping[i] & (1u << j)) && outputMode[j] == 1) {
                        setRelay(j, false);
                    }
                }

                // Toggle: OR-Logik über Toggle-Ausgänge (irgendeiner AN → alle AUS, sonst alle AN)
                if (inputMapping[i]) {
                    bool anyOn = false;
                    for (uint8_t j = 0; j < NUM_CHANNELS; j++) {
                        if ((inputMapping[i] & (1u << j)) && outputMode[j] == 0 && relayState[j]) {
                            anyOn = true;
                            break;
                        }
                    }
                    bool newState = !anyOn;
                    dbg::info(CAT_INPUT, "Eingang %d Toggle → %s", i+1, newState ? "EIN" : "AUS");
                    for (uint8_t j = 0; j < NUM_CHANNELS; j++) {
                        if ((inputMapping[i] & (1u << j)) && outputMode[j] == 0) {
                            setRelay(j, newState, !newState);
                        }
                    }
                }
            }
            stateChanged = true;
        }
    }

    // Timer-Freeze Ablauf prüfen → relayOnTimestamp verschieben
    unsigned long now = millis();
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        if (timerFreezeStart[i] > 0 && (long)(now - timerFreezeExpiry[i]) >= 0) {
            relayOnTimestamp[i] += timerFreezeExpiry[i] - timerFreezeStart[i];
            timerFreezeStart[i]  = 0;
            timerFreezeExpiry[i] = 0;
            stateChanged = true;
        }
    }

    // Auto-off timer check
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        if (relayState[i] && (autoOffSeconds[i] > 0 || tempTimeAdjustMs[i] > 0) && relayOnTimestamp[i] > 0) {
            if (getRemainingAutoOffSeconds(i, getTimerEffectiveNow(i)) == 0) {
                dbg::info(CAT_TIMER, "Auto-Aus: Relais %d", i + 1);
                setRelay(i, false);
                stateChanged = true;
            }
        }
    }

    // RUN-LED Blinker + Display-Scroll: alle 250ms aktualisieren
    static unsigned long lastLedTick = 0;
    if (now - lastLedTick >= 250) {
        lastLedTick = now;
        updateTopLeds();
        display::tick(now);
    }

    // Display jede Sekunde aktualisieren (Countdown)
    static unsigned long lastDisp = 0;
    if (now - lastDisp >= 1000) {
        lastDisp = now;
        updateDisplay();
    }

    // Update LED when NTP syncs
    static bool lastNtpState = false;
    if (dbg::isTimeSynced() && !lastNtpState) {
        dbg::info(CAT_NTP, "NTP synchronisiert: %s", dbg::getTimestamp().c_str());
        updateLedState();
        lastNtpState = true;
    }

    // Periodische AP-Station-Ãœberwachung (alle 5 Sekunden)
    static unsigned long lastStaCheck = 0;
    static uint8_t lastStaCount = 255;
    if (now - lastStaCheck >= 5000) {
        lastStaCheck = now;
        uint8_t staCount = WiFi.softAPgetStationNum();
        if (staCount != lastStaCount) {
            dbg::info(CAT_WIFI, "AP Stationen: %d (vorher: %d)", staCount, lastStaCount);
            lastStaCount = staCount;

            // Station-Details ausgeben (inkl. zugewiesener DHCP-IP)
            wifi_sta_list_t staList;
            if (esp_wifi_ap_get_sta_list(&staList) == ESP_OK) {
                for (int i = 0; i < staList.num; i++) {
                    ip4_addr_t clientIp;
                    bool hasIp = dhcp_search_ip_on_mac(staList.sta[i].mac, &clientIp);
                    if (hasIp) {
                        dbg::info(CAT_WIFI,
                            "  Station %d MAC: %02X:%02X:%02X:%02X:%02X:%02X IP: " IPSTR,
                            i + 1,
                            staList.sta[i].mac[0], staList.sta[i].mac[1],
                            staList.sta[i].mac[2], staList.sta[i].mac[3],
                            staList.sta[i].mac[4], staList.sta[i].mac[5],
                            IP2STR(&clientIp));
                    } else {
                        dbg::info(CAT_WIFI,
                            "  Station %d MAC: %02X:%02X:%02X:%02X:%02X:%02X IP: (noch keine DHCP-Lease)",
                            i + 1,
                            staList.sta[i].mac[0], staList.sta[i].mac[1],
                            staList.sta[i].mac[2], staList.sta[i].mac[3],
                            staList.sta[i].mac[4], staList.sta[i].mac[5]);
                    }
                }
            }
        }
    }

    if (stateChanged) {
        sendState();
    }

    // Periodischer State-Push (alle 30 s) für RSSI-Aktualisierung
    static unsigned long lastPeriodicPush = 0;
    if (now - lastPeriodicPush >= 30000) {
        lastPeriodicPush = now;
        sendState();
    }

    delay(10);
}
