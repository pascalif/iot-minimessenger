#ifndef MM_LOG_H
#define MM_LOG_H

// Lightweight logging facade — output format:
//
//   HH:MM:SS <L> <TAG> <func________>: <message>
//
//   15:41:44 I MQTT mqttPushForm   : Published #8 to [admin/live]
//   15:42:07 I MQTT onMqttIncom    : Incoming message [bbb]
//
// Why we don't use esp_log_set_vprintf(): in arduino-esp32 3.x, ESP_LOGx
// expands to log_x() → log_printf() → ets_printf() — direct UART write,
// never via esp_log_writev(). A vprintf hook is therefore dead code for
// arduhal output. We bypass arduhal entirely: ESP_LOGx is redefined below
// to call mm_log_emit() which writes the message to Serial in our format.
//
// Per-tag severity is still configured via esp_log_level_set() because we
// query the same level table via esp_log_level_get() before emitting. Calls
// to setupLogging() (from setup()) populate the policy.
//
// Tags used across the project:
//   TAG_MM    — general minimessenger (boot, display, NTP, identity, ...)
//   TAG_WIFI  — WiFi connection lifecycle
//   TAG_MQTT  — MQTT broker connect / publish / receive
//   TAG_BTKB  — BLE keyboard (scan, connect, HID reports)
//
// Severity macros (printf-style format) — keep using the standard names:
//   ESP_LOGE  error
//   ESP_LOGW  warning
//   ESP_LOGI  info       ← default visible level
//   ESP_LOGD  debug
//   ESP_LOGV  verbose

#include <Arduino.h>
#include <esp_log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG_MM   "MM__"
#define TAG_WIFI "WIFI"
#define TAG_MQTT "MQTT"
#define TAG_BTKB "BTKB"

// Column widths. Tags happen to be 4 chars; func is space-padded if shorter
// and truncated if longer.
#define MM_LOG_TAG_W  4
#define MM_LOG_FUNC_W 30

// Set to 1 to insert "<file>:<line> " between the TAG and func columns.
#define MM_LOG_SHOW_FILE 1
#define MM_LOG_FILE_W    18

// Single-line emit helper. Builds the full line in one buffer before writing
// to Serial so it lands atomically (a second log call from another task
// can't interleave bytes mid-line). Called only after the level check passes,
// so there is no early-out here.
static inline void mm_log_emit(char level, const char* tag, const char* func, const char* file, int line, const char* fmt, ...) {
    time_t    now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    // Truncate func into a fixed-width NUL-terminated buffer.
    char   funcBuf[MM_LOG_FUNC_W + 1] = { 0 };
    size_t fnLen                      = strlen(func);
    memcpy(funcBuf, func, fnLen < MM_LOG_FUNC_W ? fnLen : MM_LOG_FUNC_W);

    char buf[320];
    int  n;
#if MM_LOG_SHOW_FILE
    // Strip directory components from __FILE__.
    const char* slash                      = strrchr(file, '/');
    const char* fileShort                  = slash ? slash + 1 : file;
    char        fileBuf[MM_LOG_FILE_W + 1] = { 0 };
    size_t      fLen                       = strlen(fileShort);
    memcpy(fileBuf, fileShort, fLen < MM_LOG_FILE_W ? fLen : MM_LOG_FILE_W);

    n = snprintf(buf,
                 sizeof(buf),
                 "%02d:%02d:%02d %c  %-*s  %-*s:%-5d  %-*s: ",
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 tm_info.tm_sec,
                 level,
                 MM_LOG_TAG_W,
                 tag,
                 MM_LOG_FILE_W,
                 fileBuf,
                 line,
                 MM_LOG_FUNC_W,
                 funcBuf);
#else
    (void)file;
    (void)line;
    n = snprintf(buf,
                 sizeof(buf),
                 "%02d:%02d:%02d %c  %-*s  %-*s: ",
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 tm_info.tm_sec,
                 level,
                 MM_LOG_TAG_W,
                 tag,
                 MM_LOG_FUNC_W,
                 funcBuf);
#endif
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(buf)) {
        n = sizeof(buf) - 1;
    }

    va_list args;
    va_start(args, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, args);
    va_end(args);
    if (m < 0) {
        m = 0;
    }

    size_t total = (size_t)n + (size_t)m;
    if (total > sizeof(buf) - 2) {
        total = sizeof(buf) - 2;
    }
    buf[total++] = '\r';
    buf[total++] = '\n';

    Serial.write((const uint8_t*)buf, total);
}

// rem  utilisation du do/while : idiome C/C++ classique pour les macros multi-statements.
#define MM_LOG_AT(LVL, CH, TAG, FMT, ...)                                                                                                                      \
    do {                                                                                                                                                       \
        if (esp_log_level_get(TAG) >= (LVL)) {                                                                                                                 \
            mm_log_emit((CH), (TAG), __FUNCTION__, __FILE__, __LINE__, (FMT), ##__VA_ARGS__);                                                                  \
        }                                                                                                                                                      \
    } while (0)

// Drop the arduhal redefinitions of ESP_LOGx (from esp32-hal-log.h, included
// transitively via Arduino.h) and route them to our emitter. Existing call
// sites keep working unchanged.
#undef ESP_LOGE
#undef ESP_LOGW
#undef ESP_LOGI
#undef ESP_LOGD
#undef ESP_LOGV
#define ESP_LOGE(tag, fmt, ...) MM_LOG_AT(ESP_LOG_ERROR, 'E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) MM_LOG_AT(ESP_LOG_WARN, 'W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) MM_LOG_AT(ESP_LOG_INFO, 'I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) MM_LOG_AT(ESP_LOG_DEBUG, 'D', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) MM_LOG_AT(ESP_LOG_VERBOSE, 'V', tag, fmt, ##__VA_ARGS__)

// Call once at the very beginning of setup() (after Serial.begin) to install
// the per-tag log level policy. Default = INFO for everything except BTKB
// which is at DEBUG so HID keystroke reception stays visible while
// developing. Levels are queried by MM_LOG_AT() before every emit.
inline void setupLogging() {
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG_BTKB, ESP_LOG_DEBUG);

    // NB: do NOT add esp_log_level_set("NimBLEXxx", ...) here. NimBLE-Arduino gates its NIMBLE_LOGD/I/W/E macros at compile time
    // via   #if CONFIG_NIMBLE_CPP_LOG_LEVEL >= N   (see src/NimBLELog.h), and emits through console_printf, which bypasses esp_log
    // entirely. The runtime per-tag table populated here is never consulted by NimBLE — any such call would be silently ineffective.
    // To silence NimBLE chatter, lower CONFIG_NIMBLE_CPP_LOG_LEVEL — easiest path is Tools → Core Debug Level → "Warning" in the Arduino IDE,
    // since NimBLELog.h falls back to CORE_DEBUG_LEVEL when the define is unset.
    // Example of a call that would NOT work:
    //   esp_log_level_set("NimBLEClient", ESP_LOG_WARN);
}

#endif
