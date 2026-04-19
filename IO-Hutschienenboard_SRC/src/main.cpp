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
#if !SIMULATE_HW
#include <Adafruit_MCP23X17.h>
#endif
#include "pin_config.h"
#include "swtools.h"
#include "statusled.h"
#include "display.h"

using namespace dbg;

#define FW_VERSION "1.1.0"

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
const char* AP_PASS = "12345678";
const IPAddress AP_IP(192, 168, 50, 1);
const IPAddress AP_GATEWAY(192, 168, 50, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);
const bool ENABLE_DHCP_DIAG = false;

String sta_ssid = "";
String sta_pass = "";

// ============================================================
// Global State
// ============================================================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Preferences prefs;

#if !SIMULATE_HW
Adafruit_MCP23X17 mcp[2];
Adafruit_MCP23X17 mcpTop;
#endif
bool mcpReady[2]  = {false, false};
bool mcpTopReady  = false;

volatile bool topBtnFlag = false;
void IRAM_ATTR topBtnISR() { topBtnFlag = true; }

bool relayState[NUM_CHANNELS] = {false};
bool inputState[NUM_CHANNELS] = {false};
bool inputStatePrev[NUM_CHANNELS] = {false};

int8_t inputMapping[NUM_CHANNELS];

uint32_t autoOffSeconds[NUM_CHANNELS] = {0};
unsigned long relayOnTimestamp[NUM_CHANNELS] = {0};
int32_t tempTimeAdjustMs[NUM_CHANNELS] = {0};  // temporäre Zeitkorrektur via Taster (kein NVS)

char channelNames[NUM_CHANNELS][CH_NAME_MAX_LEN + 1];
uint8_t displayChannel = 0;

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
    if (remainSecs <= 900)  return 60000;        // bis 15 min → 1 min
    if (remainSecs <= 3600) return 300000;       // bis 1h    → 5 min
    if (remainSecs <= 21600) return 900000;      // bis 6h    → 15 min
    return 1800000;                              // darüber   → 30 min
}

// ============================================================
// Display-Aktualisierung für den aktuell angezeigten Kanal
// ============================================================
void updateDisplay() {
    uint8_t ch = displayChannel;
    display::show(ch, channelNames[ch], relayState[ch],
                  getRemainingAutoOffSeconds(ch, millis()),
                  autoOffSeconds[ch] > 0 || tempTimeAdjustMs[ch] > 0);
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

    // aktiv-low: LOW = LED an, HIGH = LED aus
    bool wlanActive = staConnected || (WiFi.getMode() & WIFI_AP);
    mcpTop.digitalWrite(LED_WLAN_PIN,   wlanActive   ? LOW : HIGH);
    mcpTop.digitalWrite(LED_OUTPUT_PIN, anyRelayOn   ? LOW : HIGH);
    mcpTop.digitalWrite(LED_RUN_PIN,    systemReady  ? LOW : HIGH);
#endif
}

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

    // --- Relais-MCPs (0x20 SET, 0x21 RESET) ---
    const uint8_t addrs[2] = {MCP_ADDR_SET, MCP_ADDR_RESET};
    const char* names[2]   = {"SET (0x20)", "RESET (0x21)"};
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
void setRelay(uint8_t ch, bool on) {
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

    tempTimeAdjustMs[ch] = 0;
    relayState[ch] = on;
    relayOnTimestamp[ch] = on ? millis() : 0;
    dbg::info(CAT_RELAY, "Relais %d: %s", ch + 1, on ? "EIN" : "AUS");

    updateLedState();
    if (ch == displayChannel) updateDisplay();
}

void toggleRelay(uint8_t ch) {
    setRelay(ch, !relayState[ch]);
}

// ============================================================
// Configuration persistence (NVS)
// ============================================================
void loadConfig() {
    prefs.begin("io-config", true);
    sta_ssid = prefs.getString("ssid", "");
    sta_pass = prefs.getString("pass", "");

    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        String key = "map" + String(i);
        inputMapping[i] = prefs.getChar(key.c_str(), -1);
        key = "auto" + String(i);
        autoOffSeconds[i] = prefs.getUInt(key.c_str(), 0);
        key = "name" + String(i);
        String defaultName = "Ausgang " + String(i + 1);
        String name = prefs.getString(key.c_str(), defaultName);
        strncpy(channelNames[i], name.c_str(), CH_NAME_MAX_LEN);
        channelNames[i][CH_NAME_MAX_LEN] = '\0';
    }
    prefs.end();
    dbg::info(CAT_CONFIG, "Konfiguration geladen (SSID: '%s')", sta_ssid.c_str());
}

void saveConfig() {
    prefs.begin("io-config", false);
    prefs.putString("ssid", sta_ssid);
    prefs.putString("pass", sta_pass);

    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        String key = "map" + String(i);
        prefs.putChar(key.c_str(), inputMapping[i]);
        key = "auto" + String(i);
        prefs.putUInt(key.c_str(), autoOffSeconds[i]);
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
    JsonArray mcpStatus = doc["mcp"].to<JsonArray>();
    unsigned long now = millis();

    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        inputs.add(inputState[i]);
        outputs.add(relayState[i]);
        mappings.add(inputMapping[i]);
        timers.add(autoOffSeconds[i]);
        remaining.add(getRemainingAutoOffSeconds(i, now));
        names.add(channelNames[i]);
    }
    mcpStatus.add(mcpReady[0]);
    mcpStatus.add(mcpReady[1]);
    doc["time"] = dbg::getTimestamp();
    doc["ntp"] = dbg::isTimeSynced();
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

        if (strcmp(cmd, "toggle") == 0) {
            uint8_t ch = doc["ch"];
            if (ch < NUM_CHANNELS) toggleRelay(ch);
        } else if (strcmp(cmd, "set") == 0) {
            uint8_t ch = doc["ch"];
            bool val = doc["val"];
            if (ch < NUM_CHANNELS) setRelay(ch, val);
        } else if (strcmp(cmd, "map") == 0) {
            uint8_t input = doc["input"];
            int8_t output = doc["output"];
            if (input < NUM_CHANNELS && output < (int8_t)NUM_CHANNELS) {
                inputMapping[input] = output;
                dbg::info(CAT_CONFIG, "Mapping E%d -> A%d", input + 1, output + 1);
                saveConfig();
            }
        } else if (strcmp(cmd, "timer") == 0) {
            uint8_t ch = doc["ch"];
            uint32_t secs = doc["secs"];
            if (ch < NUM_CHANNELS) {
                autoOffSeconds[ch] = secs;
                dbg::info(CAT_TIMER, "Auto-Aus A%d: %u s", ch + 1, secs);
                saveConfig();
            }
        } else if (strcmp(cmd, "wifi") == 0) {
            sta_ssid = doc["ssid"].as<String>();
            sta_pass = doc["pass"].as<String>();
            dbg::info(CAT_WIFI, "WiFi-Konfiguration geaendert: '%s'", sta_ssid.c_str());
            saveConfig();
            dbg::warn(CAT_SYSTEM, "Neustart in 1s...");
            statusled::setState(statusled::ST_BOOTING);
            statusled::update();
            delay(1000);
            ESP.restart();
        } else if (strcmp(cmd, "alloff") == 0) {
            dbg::info(CAT_RELAY, "Alle Relais AUS");
            for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
                if (relayState[i]) setRelay(i, false);
            }
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
        }

        sendState();
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
    } else if (btn == BTN_RIGHT) {
        uint8_t ch = displayChannel;
        if (relayState[ch]) {
            if (autoOffSeconds[ch] == 0 && tempTimeAdjustMs[ch] == 0)
                tempTimeAdjustMs[ch] = 86400000L;  // Default 24h
            uint32_t rem = getRemainingAutoOffSeconds(ch, now);
            int32_t step = btnStepMs(rem);
            if (rem < 86400) tempTimeAdjustMs[ch] += step;
            updateDisplay();
            sendState();
        }
    } else if (btn == BTN_LEFT) {
        uint8_t ch = displayChannel;
        if (relayState[ch]) {
            if (autoOffSeconds[ch] == 0 && tempTimeAdjustMs[ch] == 0)
                tempTimeAdjustMs[ch] = 86400000L;  // Default 24h
            uint32_t rem = getRemainingAutoOffSeconds(ch, now);
            int32_t step = btnStepMs(rem);
            if (rem <= (uint32_t)(step / 1000)) {
                setRelay(ch, false);
            } else {
                tempTimeAdjustMs[ch] -= step;
                updateDisplay();
            }
            sendState();
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

        WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());
        dbg::info(CAT_WIFI, "Verbinde mit '%s'...", sta_ssid.c_str());

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            statusled::update();
            delay(100);
        }

        if (WiFi.status() == WL_CONNECTED) {
            dbg::info(CAT_WIFI, "WiFi verbunden! IP: %s", WiFi.localIP().toString().c_str());
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
    server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        JsonArray inputs = doc["inputs"].to<JsonArray>();
        JsonArray outputs = doc["outputs"].to<JsonArray>();
        JsonArray mappings = doc["mappings"].to<JsonArray>();
        JsonArray timers = doc["timers"].to<JsonArray>();
        JsonArray remaining = doc["remaining"].to<JsonArray>();
        JsonArray names = doc["names"].to<JsonArray>();
        unsigned long now = millis();
        for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
            inputs.add(inputState[i]);
            outputs.add(relayState[i]);
            mappings.add(inputMapping[i]);
            timers.add(autoOffSeconds[i]);
            remaining.add(getRemainingAutoOffSeconds(i, now));
            names.add(channelNames[i]);
        }
        doc["ap_ip"] = WiFi.softAPIP().toString();
        doc["sta_ip"] = WiFi.localIP().toString();
        doc["sta_ssid"] = sta_ssid;
        doc["sta_pass"] = sta_pass;
        doc["version"] = FW_VERSION;
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
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(204);
    });

    ElegantOTA.begin(&server);
    ElegantOTA.setAutoReboot(true);

    ws.onEvent(onWebSocketEvent);
    server.addHandler(&ws);

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    server.begin();
    dbg::info(CAT_WEB, "Webserver gestartet auf Port 80");
}

// ============================================================
// Input Pins
// ============================================================
void setupInputPins() {
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        pinMode(INPUT_PINS[i], INPUT);
        inputStatePrev[i] = digitalRead(INPUT_PINS[i]);
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
    } else {
        dbg::warn(CAT_SYSTEM, "OLED Display nicht gefunden (nur 0x%02X getestet)", OLED_ADDR);
    }
    loadConfig();
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

    ArduinoOTA.setHostname("io-hutschiene");
    ArduinoOTA.setPassword("ota1234");
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

    // Read inputs with rising edge detection (StromstoÃŸschalter-Logik)
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        bool current = digitalRead(INPUT_PINS[i]);
        if (current && !inputStatePrev[i]) {
            inputState[i] = true;
            dbg::debug(CAT_INPUT, "Eingang %d: steigende Flanke", i + 1);
            if (inputMapping[i] >= 0 && inputMapping[i] < NUM_CHANNELS) {
                toggleRelay(inputMapping[i]);
            }
            stateChanged = true;
        } else if (!current && inputStatePrev[i]) {
            inputState[i] = false;
            stateChanged = true;
        }
        inputStatePrev[i] = current;
    }

    // Auto-off timer check (berücksichtigt tempTimeAdjustMs)
    unsigned long now = millis();
    for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
        if (relayState[i] && (autoOffSeconds[i] > 0 || tempTimeAdjustMs[i] > 0) && relayOnTimestamp[i] > 0) {
            if (getRemainingAutoOffSeconds(i, now) == 0) {
                dbg::info(CAT_TIMER, "Auto-Aus: Relais %d", i + 1);
                setRelay(i, false);
                stateChanged = true;
            }
        }
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

    delay(10);
}
