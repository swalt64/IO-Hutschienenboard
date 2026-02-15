#include "swtools.h"
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"

// Debug output goes to Serial0 = CH343 COM port (UART0, GPIO43/44)
// With ARDUINO_USB_CDC_ON_BOOT=1, Serial = USB-CDC, Serial0 = UART0
// UART0 TX/RX pins must be set explicitly on ESP32-S3
#define DBG_SERIAL Serial0
#define DBG_BAUD   115200
#define DBG_TX_PIN 43
#define DBG_RX_PIN 44
#define DBG_BUFSIZE 256

namespace dbg {

static Level s_minLevel = LVL_DEBUG;
static uint16_t s_catMask = CAT_ALL;
static bool s_initialized = false;
static bool s_timeSynced = false;

// --- Level helpers ---

static const char* levelStr(Level lvl) {
    switch (lvl) {
        case LVL_DEBUG: return "DBG";
        case LVL_INFO:  return "INF";
        case LVL_WARN:  return "WRN";
        case LVL_ERROR: return "ERR";
        default:        return "???";
    }
}

static const char* levelColor(Level lvl) {
    switch (lvl) {
        case LVL_DEBUG: return "\033[36m";  // Cyan
        case LVL_INFO:  return "\033[32m";  // Green
        case LVL_WARN:  return "\033[33m";  // Yellow
        case LVL_ERROR: return "\033[31m";  // Red
        default:        return "\033[0m";
    }
}

// --- Category helpers ---

const char* catName(Category cat) {
    switch (cat) {
        case CAT_SYSTEM: return "SYS";
        case CAT_WIFI:   return "WIFI";
        case CAT_NTP:    return "NTP";
        case CAT_MCP:    return "MCP";
        case CAT_RELAY:  return "RELAY";
        case CAT_INPUT:  return "INPUT";
        case CAT_WEB:    return "WEB";
        case CAT_CONFIG: return "CONF";
        case CAT_TIMER:  return "TIMER";
        default:         return "???";
    }
}

// --- Init ---

void begin(Level minLevel, uint16_t enabledCategories) {
    s_minLevel = minLevel;
    s_catMask = enabledCategories;
    if (!s_initialized) {
        DBG_SERIAL.begin(DBG_BAUD, SERIAL_8N1, DBG_RX_PIN, DBG_TX_PIN);
        s_initialized = true;
        delay(100);
    }
}

// --- Category control ---

void catEnable(Category cat, bool enable) {
    if (enable) {
        s_catMask |= (uint16_t)cat;
    } else {
        s_catMask &= ~(uint16_t)cat;
    }
}

void catSet(uint16_t mask) {
    s_catMask = mask;
}

uint16_t catGet() {
    return s_catMask;
}

void enableAll() {
    s_catMask = CAT_ALL;
}

void disableAll() {
    s_catMask = 0;
}

// --- NTP ---

static void ntpSyncCallback(struct timeval* tv) {
    s_timeSynced = true;
}

void ntpSync(const char* timezone, const char* ntpServer1, const char* ntpServer2) {
    configTzTime(timezone, ntpServer1, ntpServer2);
    sntp_set_time_sync_notification_cb(ntpSyncCallback);
    info(CAT_NTP, "NTP sync gestartet (TZ: %s)", timezone);
}

bool isTimeSynced() {
    return s_timeSynced;
}

// --- Timestamp ---

String getTimestamp() {
    struct tm ti;
    if (getLocalTime(&ti, 0)) {
        char buf[24];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
        return String(buf);
    }
    unsigned long ms = millis();
    unsigned long s = ms / 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu.%03lu", s, ms % 1000);
    return String(buf);
}

// --- Level control ---

void setLevel(Level level) {
    s_minLevel = level;
}

Level getLevel() {
    return s_minLevel;
}

// --- Core log function ---

static void logMsg(Level lvl, Category cat, const char* fmt, va_list args) {
    if (!s_initialized) return;
    if (lvl < s_minLevel) return;

    // ERROR always passes through, others check category mask
    if (lvl != LVL_ERROR && !(s_catMask & (uint16_t)cat)) return;

    char msg[DBG_BUFSIZE];
    vsnprintf(msg, sizeof(msg), fmt, args);

    String ts = getTimestamp();

    // Format: [timestamp] LVL CATEGORY | message
    DBG_SERIAL.printf("%s[%s] %s %-5s | %s\033[0m\r\n",
                      levelColor(lvl), ts.c_str(), levelStr(lvl), catName(cat), msg);
}

// --- Public log functions ---

void debug(Category cat, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logMsg(LVL_DEBUG, cat, fmt, args);
    va_end(args);
}

void info(Category cat, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logMsg(LVL_INFO, cat, fmt, args);
    va_end(args);
}

void warn(Category cat, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logMsg(LVL_WARN, cat, fmt, args);
    va_end(args);
}

void error(Category cat, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logMsg(LVL_ERROR, cat, fmt, args);
    va_end(args);
}

} // namespace dbg
