// ================================================================================
// time.ino — System clock, NTP sync, date/time formatters, timezone label
// ================================================================================
//
// Owns everything related to wall-clock time: the POSIX TZ rule for Europe/Paris, the SNTP boot/resync routine called once WiFi reaches CONNECTED,
// and the small set of formatters that produce the strings shown on screen (status info row "NTP:", per-message timestamp prefix) and embedded in
// MQTT payload trailers (### ts:YYYY-MM-DD|HH:MM:SS). Arduino IDE concatenates this file with minimessenger.ino into a single translation unit
// (alphabetical order after the sketch-named file), so the globals it touches (none today — the clock state lives in the ESP-IDF SNTP module
// directly, accessed through libc's time()/localtime_r()) and the libc time.h header pulled in from minimessenger.ino are visible here without
// forward decls.
//
// What stays elsewhere:
//   - `time_t g_lastShownTsEpoch` and `CONVO_TS_HIDE_THRESHOLD_S` live in display.ino, next to their sole consumer addConversationBlockImpl — they're
//     conversation-clustering state, not generic wall-clock helpers.
//   - Per-call `time(nullptr)` reads in display.ino / mqtt.ino are plain libc calls; no wrapper is provided since each call site needs a different
//     downstream formatting (epoch comparison for clustering, epoch trailer for MQTT, etc.).
//   - MQTT keepalive / retry intervals are MQTT-specific and stay in mqtt.h.

// POSIX timezone string for Paris (Europe/Paris). Decoded:
//   CET-1     standard time name = CET, POSIX offset -1 → human UTC+1
//   CEST      daylight time name = CEST (implicit offset = CET + 1h = UTC+2)
//   M3.5.0    DST starts: Month 3 (March), week 5 (= last), day 0 (Sunday) → last Sunday of March
//   M10.5.0/3 DST ends:   last Sunday of October at 03:00 local
// With this in configTzTime(), the libc handles DST automatically; tm.tm_isdst tells us which side we are on.
#define TZ_PARIS "CET-1CEST,M3.5.0,M10.5.0/3"


void setupNTP() {
    ESP_LOGI(TAG_MM, "Init NTP...");
    // configTzTime (vs configTime with a fixed offset) installs a POSIX TZ rule that auto-switches between CET and CEST. localtime_r will then
    // populate tm.tm_isdst correctly twice a year without us touching anything.
    configTzTime(TZ_PARIS, "europe.pool.ntp.org", "pool.ntp.org");

    // Block until SNTP returns a plausible epoch (after 2023-11) so the first
    // TLS handshake doesn't run with a 1970 clock and reject the broker cert.
    time_t now   = 0;
    int    tries = 0;
    while ((now = time(nullptr)) < 1700000000 && tries++ < 30) {
        delay(500);
    }
    ESP_LOGI(TAG_MM, "NTP synced after %d tries, epoch=%ld", tries, (long)now);

    // Push a refresh to the info screen if it's shown — the TIME row label is computed from the local TZ which is only valid post-NTP.
    refreshInfoScreenIfShown();
}

char* getCurrentDateTime() {
    // Function-local static buffer, .bss-resident, zero heap.
    // Size: "YYYY-MM-DD|HH:MM:SS" = 19 chars + NUL = 20 bytes.
    static char gs_buf[20];

    time_t    epochTime = time(nullptr);
    struct tm timeInfo;
    localtime_r(&epochTime, &timeInfo);

    snprintf(gs_buf,
             sizeof(gs_buf),
             "%04d-%02d-%02d|%02d:%02d:%02d",
             timeInfo.tm_year + 1900,
             timeInfo.tm_mon + 1,
             timeInfo.tm_mday,
             timeInfo.tm_hour,
             timeInfo.tm_min,
             timeInfo.tm_sec);

    return gs_buf;
}

char* getCurrentTime() {
    // Function-local static buffer.
    // Size: "HH:MM:SS" = 8 chars + NUL = 9.
    static char gs_buf[9];

    time_t    epochTime = time(nullptr);
    struct tm timeInfo;
    localtime_r(&epochTime, &timeInfo);

    snprintf(gs_buf, sizeof(gs_buf), "%02d:%02d:%02d", timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

    return gs_buf;
}

// Returns "Paris (UTC+1)" in winter (CET) or "Paris (UTC+2)" in summer (CEST). Relies on the POSIX TZ rule installed by setupNTP() via
// configTzTime(TZ_PARIS, ...): the libc populates tm.tm_isdst correctly and we just translate it to the human label.
const char* getTimezoneLabel() {
    time_t    epochTime = time(nullptr);
    struct tm timeInfo;
    localtime_r(&epochTime, &timeInfo);
    // tm_isdst > 0 → DST in effect (CEST, UTC+2). 0 → standard (CET, UTC+1). < 0 → unknown (would happen if TZ isn't set; we still default to UTC+1).
    return (timeInfo.tm_isdst > 0) ? "Paris (UTC+2)" : "Paris (UTC+1)";
}
