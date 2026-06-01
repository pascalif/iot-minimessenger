#pragma once

// Connection state machine driven by wifi.ino. Read by minimessenger.ino (status screen rendering) and mutated by wifi.ino (wifiTick).
// Kept in its own tiny header so both translation units see the same complete enum definition without dragging in the WiFiManager / WiFiMulti
// headers from wifi.ino into the main .ino.
enum class WifiState {
    BOOTING,       // setup() not finished — NVS not read yet, WiFiMulti not populated.
    TRYING_KNOWN,  // WiFiMulti is iterating over the known networks (NVS + compiled defaults). Retries every WIFI_TRYING_KNOWN_RETRY_INTERVAL_MS.
    PORTAL,        // No known network reachable; WiFiManager captive portal is open at AP "minimessenger-config" → http://192.168.4.1.
    CONNECTED,     // STA associated. NTP and MQTT can run. UI may transition to conversation mode once MQTT is also up.
    LOST           // Was CONNECTED, now disconnected. WiFiMulti.run() called periodically; falls back to PORTAL after WIFI_LOST_TO_PORTAL_MS.
};

// Adafruit_ST7789 forward declaration so we can take a pointer in the portal renderer's signature without pulling Adafruit_GFX into wifi_state.h.
class Adafruit_ST7789;

// === Functions defined in wifi.ino, callable from minimessenger.ino ===
// setup()-time: full WiFi bringup — driver init (mode + hostname) + NVS seed/load + WiFiManager config + state machine kick. Non-blocking.
void setupWifi();

// loop()-time: drive the state machine (retry / portal / process).
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

// Force the state machine into PORTAL right now (used by /wifi portal). Closes any active STA attempt and opens the captive AP.
void wifiForcePortal();

// Render the portal instructions block on the info screen. Caller advances nextY itself based on the lines added (the function updates it via the
// reference parameter so the caller can keep flowing rows below the portal block — currently the HELP row is appended after).
void drawPortalInstructions(Adafruit_ST7789* pDisp, int& nextY, int colHeaders, int colValues, int lineHeight);
