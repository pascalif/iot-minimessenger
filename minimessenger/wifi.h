#pragma once

// Compile-time WiFi credentials row. Used by wifi.ino's `wifiLoadNVSAndCompiledIntoMulti` and materialised as the COMPILED_WIFI_DEFAULTS table in
// personal-data.h. Hosted in this shared header so the table definition (which lives in personal-data.h, included from contacts.ino in the
// concatenation order) sees a complete element type when it is processed.
struct CompiledWifiEntry {
    const char* ssid;
    const char* pwd;
};

extern const CompiledWifiEntry COMPILED_WIFI_DEFAULTS[];
extern const size_t            COMPILED_WIFI_DEFAULTS_COUNT;

// Connection state machine driven by wifi.ino. Read by minimessenger.ino (status screen rendering) and mutated by wifi.ino (wifiTick).
// Kept in its own tiny header so both translation units see the same complete enum definition without dragging in the WiFiManager / WiFiMulti
// headers from wifi.ino into the main .ino.
enum class WifiState {
    WIFI_BOOTING,       // setup() not finished — NVS not read yet, WiFiMulti not populated.
    WIFI_TRYING_KNOWN,  // WiFiMulti is iterating over the known networks (NVS +compiled defaults).
                        // Retries every WIFI_TRYING_KNOWN_RETRY_INTERVAL_MS.
    WIFI_PORTAL,        // No known network reachable; WiFiManager captive WIFI_PORTAL is open at
                        // AP "minimessenger-config" → http://192.168.4.1.
    WIFI_CONNECTED,     // STA associated. NTP and MQTT can run. UI may transition to
                        // conversation mode once MQTT is also up.
    WIFI_LOST           // Was WIFI_CONNECTED, now disconnected. WiFiMulti.run() called periodically;
                        // falls back to WIFI_PORTAL after WIFI_LOST_TO_PORTAL_MS.
};

// Window during which setup() pumps wifiTick() before launching the BLE keyboard. On ESP32 the WiFi and BLE radios share the 2.4 GHz front-end via
// time-slicing, and the BLE keyboard's reconnect storm (scan + connect + auth + 3× subscribe = ~2 s of intense radio activity right after boot) was
// starving the first WiFi association attempt — the 802.11 management frames missed enough ACK windows for the AP to give up, and the driver then
// waited ~11 s in ASSOC_EXPIRE before WiFiMulti could retry. By blocking setupKeyboard() until either WiFi.status() == WL_CONNECTED or this timeout
// elapses, the radio is exclusively WiFi during the critical first assoc window. Tradeoff: in the worst case (AP unreachable) the BLE keyboard is
// delayed by this whole duration before it even starts scanning — bump it if your AP is healthy and you want a generous safety margin, shrink it if
// BLE keyboard responsiveness at cold boot matters more than WiFi speed. Range that makes practical sense: 1'000 .. 10'000.
//
// Defined in this header (not in wifi.ino) because the Arduino IDE concatenates minimessenger.ino BEFORE wifi.ino (sketch-name file comes first), so
// a #define inside wifi.ino is not yet visible when setup() in minimessenger.ino is compiled.
#define WIFI_BOOT_EXCLUSIVE_GRACE_MS 5'000

// Adafruit_ST7789 forward declaration so we can take a pointer in the WIFI_PORTAL renderer's signature without pulling Adafruit_GFX into wifi.h.
class Adafruit_ST7789;

// === Functions defined in wifi.ino, callable from minimessenger.ino ===
// setup()-time: full WiFi bringup — driver init (mode + hostname) + NVS seed/load + WiFiManager config + state machine kick. Non-blocking.
void setupWifi();

// loop()-time: drive the state machine (retry / WIFI_PORTAL / process).
void wifiTick(unsigned long currentMillis);

// Inspect the current state — used by showUpdatedInfoScreen() to pick the right row set.
WifiState wifiGetState();

// NVS-backed network list. wifiSaveToNvs / wifiForgetFromNvs return true on success, false if the list was full or the SSID was absent.
bool wifiSaveToNvs(const char* ssid, const char* pwd);
bool wifiForgetFromNvs(const char* ssid);
void wifiClearNvs();

// Print the known SSID list directly into the conversation area as pink help-coloured blocks. Defined in wifi.ino so it can iterate NVS without
// returning a std::vector<String> across the .ino boundary (which would complicate the forward declaration).
void wifiPrintListToConversation();

// Append every known WiFi credential as lines "\n<ssid>|<pwd>" into `buffer`, starting at offset `used` (which is grown in place). Pulls from both
// sources that wifiLoadNVSAndCompiledIntoMulti feeds into WiFiMulti at boot: first the NVS-stored entries, then the COMPILED_WIFI_DEFAULTS rows
// whose SSID hasn't already been emitted from NVS (same NVS-wins precedence rule as the boot loader). Stops cleanly without truncating mid-line if
// the next pair wouldn't fit within `cap`, setting `outSaturated = true`. Returns the number of pairs successfully appended. Cleartext passwords —
// this helper exists to let /wifi pub assemble a payload for OTA bootstrap; the MQTT publishing itself lives in commands.ino
// (cmdWifiPublishNetworksToMQTTPeer) so the NVS-access and compile-table knowledge stays encapsulated here.
int wifiAppendKnownCredentialsToBuffer(char* buffer, size_t cap, size_t& used, bool& outSaturated);

// Force the state machine into WIFI_PORTAL right now (used by /wifi WIFI_PORTAL). Closes any active STA attempt and opens the captive AP.
void wifiForcePortal();

// Render the WIFI_PORTAL instructions block on the info screen. Caller advances nextY itself based on the lines added (the function updates it via the
// reference parameter so the caller can keep flowing rows below the WIFI_PORTAL block — currently the HELP row is appended after).
void drawPortalInstructions(Adafruit_ST7789* pDisp, int& nextY, int colHeaders, int colValues, int lineHeight);
