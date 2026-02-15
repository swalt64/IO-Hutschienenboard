#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>

// ============================================================
// Debug output via COM port (Serial0 = CH343 UART)
// with NTP timestamps and per-category filtering
//
// Usage:
//   dbg::begin();
//   dbg::catEnable(dbg::CAT_RELAY, true);
//   dbg::catEnable(dbg::CAT_WIFI, false);   // WIFI stumm schalten
//   dbg::info(CAT_RELAY, "Relais %d: EIN", ch);
//   dbg::error(CAT_MCP, "MCP nicht gefunden!");
//   dbg::enableAll();                        // alle Kategorien an
//   dbg::disableAll();                       // alle stumm (ausser ERROR)
// ============================================================

namespace dbg {

// --- Log Levels ---
enum Level : uint8_t {
    LVL_DEBUG = 0,
    LVL_INFO  = 1,
    LVL_WARN  = 2,
    LVL_ERROR = 3,
    LVL_NONE  = 4
};

// --- Debug Categories (bitmask) ---
enum Category : uint16_t {
    CAT_SYSTEM = (1 << 0),   // General system messages
    CAT_WIFI   = (1 << 1),   // WiFi connection, AP
    CAT_NTP    = (1 << 2),   // NTP time sync
    CAT_MCP    = (1 << 3),   // MCP23017 I/O expander
    CAT_RELAY  = (1 << 4),   // Relay switching
    CAT_INPUT  = (1 << 5),   // Input edge detection
    CAT_WEB    = (1 << 6),   // WebServer, WebSocket
    CAT_CONFIG = (1 << 7),   // Configuration load/save
    CAT_TIMER  = (1 << 8),   // Auto-off timers
    CAT_ALL    = 0xFFFF
};

// Initialize debug serial (Serial0, 115200)
void begin(Level minLevel = LVL_DEBUG, uint16_t enabledCategories = CAT_ALL);

// --- Category control ---
void catEnable(Category cat, bool enable);
void catSet(uint16_t mask);       // Set entire category bitmask
uint16_t catGet();                // Get current bitmask
void enableAll();                 // Enable all categories
void disableAll();                // Disable all (ERROR still passes through)

// --- NTP ---
void ntpSync(const char* timezone = "CET-1CEST,M3.5.0,M10.5.0/3",
             const char* ntpServer1 = "pool.ntp.org",
             const char* ntpServer2 = "time.nist.gov");
bool isTimeSynced();

// --- Logging with category ---
void debug(Category cat, const char* fmt, ...);
void info(Category cat, const char* fmt, ...);
void warn(Category cat, const char* fmt, ...);
void error(Category cat, const char* fmt, ...);

// --- Level control ---
void setLevel(Level level);
Level getLevel();

// --- Utilities ---
String getTimestamp();
const char* catName(Category cat);

} // namespace dbg
