// ================================================================================
// wifi.ino — WiFi onboarding and runtime connection state machine
// ================================================================================
//
// All WiFi-related code lives here. The Arduino IDE concatenates this file with minimessenger.ino into a single translation unit (the main .ino
// comes first because it shares the sketch folder name, then files in alphabetical order), so globals declared below are visible from
// minimessenger.ino without extern. The shared enum WifiState and the function prototypes that minimessenger.ino calls are in wifi_state.h
// (included by both .ino files) so the main file doesn't need to know about WiFiMulti / WiFiManager / Preferences.
//
// State machine (see wifi_state.h for the enum):
//   BOOTING → TRYING_KNOWN          when setupWifi() finishes loading NVS into WiFiMulti
//   TRYING_KNOWN → CONNECTED        when WiFiMulti.run() returns WL_CONNECTED
//   TRYING_KNOWN → PORTAL           after WIFI_TRYING_KNOWN_TIMEOUT_MS with no success
//   PORTAL → CONNECTED              when the user submits credentials via the captive portal page
//   PORTAL → TRYING_KNOWN           after WIFI_PORTAL_TIMEOUT_MS without configuration (retry known nets)
//   CONNECTED → LOST                when WiFi.status() != WL_CONNECTED
//   LOST → CONNECTED                when WiFiMulti.run() succeeds again
//   LOST → PORTAL                   when LOST persists more than WIFI_LOST_TO_PORTAL_MS
//
// NVS layout (namespace "wifi"):
//   - "count" (uint8): index of the first free slot (0..MAX_WIFI_NETWORKS). Slots before it MAY contain empty strings if a /wifi forget left a
//                      hole; wifiLoadNVSAndCompiledIntoMulti skips empties. Treat "count" as "high-water mark", not strict size.
//   - "ssid_N" / "pwd_N" (String): for N in [0, count). Empty SSID = unused slot.
//
// Pink-banner rendering helpers (drawPortalInstructions) live here too so minimessenger.ino keeps its info-screen renderer agnostic.

#include <WiFi.h>
#include <WiFiMulti.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include "wifi_state.h"
#include "compiled_wifi.h"
#include "mm_log.h"

// ================================================================================
// Constants
// ================================================================================

#define MAX_WIFI_NETWORKS                   5          // hard cap of slot count in NVS. Bumping requires no migration since slots are key-indexed.
#define WIFI_TRYING_KNOWN_RETRY_INTERVAL_MS 1'000      // how often we call WiFiMulti.run() inside the TRYING_KNOWN state.
// After this much time in TRYING_KNOWN with no success, fall through to the captive portal. Set to 45 s (not 15) because a single failed association
// on a marginal AP burns ~11-13 s before ASSOC_EXPIRE fires; with 15 s we only got one attempt in before bailing, and the second attempt would often
// have worked. 45 s lets WiFiMulti retry 3-4 times before giving up. Tradeoff: if the known AP is truly gone the user waits 45 s before the portal
// appears instead of 15 s — acceptable because "known AP transiently unreachable" is more common than "known AP definitely gone".
#define WIFI_TRYING_KNOWN_TIMEOUT_MS        45'000
#define WIFI_PORTAL_TIMEOUT_MS              300'000UL  // 5 min: portal auto-closes and the state machine reverts to TRYING_KNOWN for one more pass.
#define WIFI_LOST_RETRY_INTERVAL_MS         5'000      // how often we call WiFiMulti.run() inside the LOST state.
#define WIFI_LOST_TO_PORTAL_MS              60'000     // if the connection has been LOST for more than this, drop into PORTAL (router probably gone).

#define WIFI_PORTAL_AP_SSID "minimsg-cfg"  // generic on purpose so the user knows what SSID to look for in the phone's WiFi list.
// WPA2 password for the captive portal AP. Set to a real string (8+ chars) rather than "" (open AP) because:
//   - arduino-esp32 3.x defaults pmf_cfg.capable=true on AP, which some Android 12+ phones refuse silently when paired with WIFI_AUTH_OPEN.
//   - Android also restricts auto-connect on open networks for privacy (no pop-up, just silent refuse).
//   - 8 chars is the WPA2 minimum (esp_wifi rejects shorter as "passphrase too short").
// Pick something memorable; users only see this once during onboarding.
#define WIFI_PORTAL_AP_PASS "11110000"

#define WIFI_PREFS_NAMESPACE "wifi"

// ================================================================================
// Globals
// ================================================================================

WifiState     g_wifiState            = WifiState::BOOTING;
unsigned long g_wifiStateEnteredMs   = 0;  // millis() snapshot when we transitioned into g_wifiState. Drives per-state timeouts.
unsigned long g_wifiLastTryConnectMs = 0;  // last time WiFiMulti.run() was called inside TRYING_KNOWN / LOST — used to throttle the retry cadence.

// To try connection among a list of known SSIDs and passwords
WiFiMulti g_wifiMulti;

// When no possible connection, create an AP + login portal
WiFiManager g_wifiManager;

Preferences g_wifiPrefs;

// True once we've called setupNTP() at least once after the first CONNECTED transition. Subsequent CONNECTED transitions skip it (configTzTime
// is idempotent and re-issuing it would just re-arm SNTP without value).
bool g_wifiNtpDoneOnce = false;

// Edge-detection flag for the "WiFi back" / "WiFi lost" banners: set true on CONNECTED entry, reset false on LOST entry. Lets us print each
// banner exactly once per LOST↔CONNECTED transition (the state machine wouldn't fire transitions redundantly today, but the flag also gates the
// "WiFi lost" banner against the boot path where we've never been connected yet — see the LOST branch in wifiTransitionTo).
bool g_wifiWasConnected = false;

// Set true by wifiOnPortalSave() when WiFiManager hands back control after the user submitted credentials via the web UI and STA association
// succeeded. In that path WiFiManager runs its own shutdownConfigPortal() internally (deletes the WebServer / DNSServer / netif callbacks)
// BEFORE returning, so wifiStopPortal() must NOT call g_wifiManager.stopConfigPortal() a second time — doing so dereferences NULL pointers
// inside WM and panics (LoadProhibited, EXCVADDR=0). Reset to false at the end of wifiStopPortal() so the next portal entry starts fresh.
static bool g_portalSelfClosed = false;

// ================================================================================
// Forward declarations of internal helpers
// ================================================================================

static void wifiTransitionTo(WifiState newState);
static int  wifiLoadNVSAndCompiledIntoMulti();
static void wifiStartPortal();
static void wifiStopPortal();
static void wifiOnPortalSave();
static void wifiOnConnected();

// setupNTP() lives in minimessenger.ino — declared here so wifiOnConnected() can call it.
void setupNTP();

// ================================================================================
// setupWifi() — full WiFi bringup, called once from setup()
// ================================================================================
//
// Initialises the WiFi driver (mode + hostname), loads both the NVS-saved networks and the compile-time defaults into WiFiMulti (NVS wins on
// duplicate SSID — see wifiLoadNVSAndCompiledIntoMulti), configures the captive portal callbacks, and kicks the state machine into TRYING_KNOWN
// (or PORTAL when both lists are empty so nothing can be tried). Non-blocking: wifiTick() drives the actual connection attempts from loop().
// identifyDevice() already called WiFi.mode(WIFI_STA) earlier; we re-call it for safety in case a disconnect(true,true) wiped the state.

void setupWifi() {
    ESP_LOGI(TAG_WIFI, "setupWifi() — bringing up driver and seeding state machine");

    // Cold-boot path: arduino-esp32 has its own auto-reconnect that fires as soon as WIFI_STA is up. It re-uses the SSID/password persisted in the
    // driver's NVS from the previous boot and runs a single, very long association attempt (we observed ~11 s until ASSOC_EXPIRE on a marginal RSSI).
    // That attempt happens *before* WiFiMulti gets a chance to run, and during it the state machine sits idle in TRYING_KNOWN. To skip that whole
    // detour and let WiFiMulti drive from the start (it scans first, picks the best RSSI from our list, retries every 1 s) we wipe the driver-cached
    // credentials with disconnect(true, true): first true = also stop the WiFi radio if it's already trying, second true = erase the persisted
    // SSID/pwd so auto-reconnect has nothing to chew on. Then a short settle delay and a clean WIFI_STA mode flip. Net effect: ~10 s shaved off cold
    // boot when the AP is marginal — no impact when the AP is healthy (assoc completes in <1 s either way).
    //
    // We INIT the driver (WiFi.mode(WIFI_STA)) BEFORE calling disconnect — otherwise disconnect() fails with ESP_ERR_WIFI_NOT_INIT (observed when
    // identifyDevice() failed to bring up WIFI_STA earlier, leaving the radio uninitialised). When disconnect() fails the cached creds stay in NVS,
    // the driver auto-reconnect races WiFiMulti, and the resulting collisions cause ASSOC_EXPIRE on the first attempt. The first WiFi.mode() call is
    // idempotent if the driver is already up — costs nothing on the happy path.
    WiFi.mode(WIFI_STA);
    delay(50);
    WiFi.disconnect(true, true);
    delay(50);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_deviceName);

    // Populate WiFiMulti with every known network: NVS entries first (priority on duplicate SSID), then compile-time defaults from compiled_wifi.h
    // as a permanent safety net. /wifi clean only wipes NVS, so the device can always come back up on a compiled default without a reflash.
    int n = wifiLoadNVSAndCompiledIntoMulti();

    // Configure WiFiManager once for later PORTAL usage. setConfigPortalBlocking(false) means startConfigPortal() returns immediately; we drive it
    // with g_wifiManager.process() inside wifiTick. The save callback fires when the user submits credentials via the web UI.
    g_wifiManager.setConfigPortalBlocking(false);
    g_wifiManager.setSaveConfigCallback(wifiOnPortalSave);
    // Temporarily enable WiFiManager's own verbose log to see what happens at /wifi portal: phones can see the SSID but fail to associate without
    // any user-visible error, and we need the lib's internal trace (AP setup, DHCP, DNS catcher, HTTP server) to know which layer is broken.
    // Flip back to false (or the single-arg form) once the connect path is confirmed stable. setDebugOutput overload: (enabled, level).
    g_wifiManager.setDebugOutput(true, WM_DEBUG_VERBOSE);

    if (n == 0) {
        // No networks known at all → skip the TRYING_KNOWN dance, go straight to portal.
        ESP_LOGW(TAG_WIFI, "No known networks — opening portal immediately");
        wifiTransitionTo(WifiState::PORTAL);
    } else {
        wifiTransitionTo(WifiState::TRYING_KNOWN);
    }
}

// ================================================================================
// wifiTick() — drives the state machine, called every loop() iteration
// ================================================================================

void wifiTick(unsigned long currentMillis) {
    switch (g_wifiState) {
    case WifiState::BOOTING:
        // Should never linger here — setup() calls setupWifi() which transitions out. If we see BOOTING in loop(), something is wrong upstream.
        return;

    case WifiState::TRYING_KNOWN: {
        // Throttle WiFiMulti.run() — calling it every loop iteration is wasteful, once per second is plenty since each call internally takes
        // hundreds of ms to scan / try to associate.
        if (currentMillis - g_wifiLastTryConnectMs >= WIFI_TRYING_KNOWN_RETRY_INTERVAL_MS) {  // 1 second
            g_wifiLastTryConnectMs = currentMillis;
            if (g_wifiMulti.run() == WL_CONNECTED) {
                wifiTransitionTo(WifiState::CONNECTED);
                return;
            }
        }
        // Still not connected after the timeout → escalate to portal.
        if (currentMillis - g_wifiStateEnteredMs >= WIFI_TRYING_KNOWN_TIMEOUT_MS) {  // 15 seconds
            ESP_LOGW(TAG_WIFI, "TRYING_KNOWN timeout (%lu ms) — no known network responded, opening portal", (unsigned long)WIFI_TRYING_KNOWN_TIMEOUT_MS);
            wifiTransitionTo(WifiState::PORTAL);
        }
        return;
    }

    case WifiState::PORTAL: {
        // Pump the WiFiManager HTTP / DNS / portal logic. process() returns true when the user has just successfully configured a network — the
        // saveConfigCallback already added it to NVS / WiFiMulti for us, so we just have to flip the state.
        bool justConnected = g_wifiManager.process();
        if (justConnected || WiFi.status() == WL_CONNECTED) {
            ESP_LOGI(TAG_WIFI, "Portal closed — STA is now connected");
            wifiStopPortal();
            wifiTransitionTo(WifiState::CONNECTED);
            return;
        }
        // Portal idle timeout: WiFiManager's own setConfigPortalTimeout is ignored in non-blocking mode (per its header comment), so we track time
        // ourselves. After WIFI_PORTAL_TIMEOUT_MS without a successful save, close the portal and give the known networks one more chance.
        if (currentMillis - g_wifiStateEnteredMs >= WIFI_PORTAL_TIMEOUT_MS) {
            ESP_LOGW(TAG_WIFI, "Portal timeout (%lu ms) — closing portal and retrying known networks", (unsigned long)WIFI_PORTAL_TIMEOUT_MS);
            wifiStopPortal();
            wifiTransitionTo(WifiState::TRYING_KNOWN);
        }
        return;
    }

    case WifiState::CONNECTED: {
        if (WiFi.status() != WL_CONNECTED) {
            ESP_LOGW(TAG_WIFI, "Connection lost (was CONNECTED) — entering LOST state");
            wifiTransitionTo(WifiState::LOST);
        }
        return;
    }

    // LOST is intentionally distinct from TRYING_KNOWN even though they share the same control-flow shape (retry WiFiMulti, escalate to PORTAL on
    // timeout). The split lets us pick different timing for the "freshly booted, never connected" case (TRYING_KNOWN: aggressive 1s retry, 15s
    // before opening the portal — user is watching the screen and wants fast feedback) vs the "was online, just dropped" case (LOST: gentler 5s
    // retry, 60s grace period — typical router reboots resolve in 30-60s, no point flipping the screen to portal mode for nothing).
    case WifiState::LOST: {
        // Retry WiFiMulti every WIFI_LOST_RETRY_INTERVAL_MS. The same WiFiMulti that succeeded before will try all known networks in order of RSSI.
        if (currentMillis - g_wifiLastTryConnectMs >= WIFI_LOST_RETRY_INTERVAL_MS) {
            g_wifiLastTryConnectMs = currentMillis;
            if (g_wifiMulti.run() == WL_CONNECTED) {
                wifiTransitionTo(WifiState::CONNECTED);
                return;
            }
        }
        // After WIFI_LOST_TO_PORTAL_MS of failed retries, the AP is likely gone for good (moved house, router replaced, …). Open the portal so the
        // user can add a new network without rebuilding firmware.
        if (currentMillis - g_wifiStateEnteredMs >= WIFI_LOST_TO_PORTAL_MS) {
            ESP_LOGW(TAG_WIFI, "LOST for %lu ms — falling back to portal", (unsigned long)WIFI_LOST_TO_PORTAL_MS);
            wifiTransitionTo(WifiState::PORTAL);
        }
        return;
    }
    }
}

// ================================================================================
// State transitions
// ================================================================================

static void wifiTransitionTo(WifiState newState) {
    if (newState == g_wifiState) {
        return;
    }

    ESP_LOGI(TAG_WIFI, "State: %d → %d", (int)g_wifiState, (int)newState);
    g_wifiState            = newState;
    g_wifiStateEnteredMs   = millis();
    g_wifiLastTryConnectMs = 0;  // force an immediate retry attempt on entry to a state that polls WiFiMulti.

    switch (newState) {
    case WifiState::TRYING_KNOWN:
        // No explicit action — wifiTick will call WiFiMulti.run() in the next iteration.
        break;

    case WifiState::PORTAL:
        wifiStartPortal();
        break;

    case WifiState::CONNECTED:
        wifiOnConnected();
        break;

    case WifiState::LOST:
        if (g_wifiWasConnected) {
            addConversationBlock("", "WiFi lost - Retrying...", CONVO_ERROR_COLOR, CENTER);
        }
        g_wifiWasConnected = false;  // re-arm so the next CONNECTED prints a banner.
        break;

    case WifiState::BOOTING:
        // We never transition back to BOOTING; this branch is just for completeness.
        break;
    }
}

static void wifiStartPortal() {
    ESP_LOGI(TAG_WIFI, "Starting captive portal AP [%s]", WIFI_PORTAL_AP_SSID);

    // Pause the BLE keyboard scan before touching the WiFi mode. On ESP32 the BLE and WiFi radios share the same 2.4 GHz front-end; an active
    // BLE scan at ~100 % duty cycle (which our default setActiveScan(true) + 30 s window + immediate re-arm produces) starves the WiFi RX path.
    // The symptom is: AP beacons go out fine (so the phone sees the SSID in its list and prompts for the password), but auth/association frames
    // from the phone never reach the ESP32 — NUM CLIENTS stays at 0 forever, Android pops "Impossible de se connecter au réseau Wi-Fi" with no
    // further detail. Suspending the scan during the portal phase frees the RX path. Resumed in wifiStopPortal().
    g_kb.pauseScan();

    // Hard reset of the WiFi driver BEFORE handing over to WiFiManager. Symmetric to the cleanup we already do in wifiStopPortal() (see the long
    // comment there): coming out of TRYING_KNOWN means the STA side was scanning / connecting, the radio is busy on a random channel, and the
    // softAP that startConfigPortal() raises ends up broadcasting beacons that phones SEE in the network list but cannot associate to (no error
    // bubbles up — the phone retries silently then gives up). disconnect(true,true) stops the radio AND wipes in-RAM creds, the 100 ms delay
    // lets the async STA_STOP / scan-abort events drain, then WiFiManager.startConfigPortal() puts us into a clean AP_STA from a known-quiet state.
    WiFi.disconnect(true, true);
    delay(100);

    // WIFI_AP_STA is implicit when startConfigPortal switches the chip into AP mode. The previous STA association (if any) is dropped.
    g_wifiManager.startConfigPortal(WIFI_PORTAL_AP_SSID, WIFI_PORTAL_AP_PASS);
}

static void wifiStopPortal() {
    ESP_LOGI(TAG_WIFI, "Stopping captive portal (selfClosed=%d)", g_portalSelfClosed);

    // Only call stopConfigPortal() if WiFiManager hasn't already torn itself down. When the user submits credentials on the portal page and STA
    // association succeeds, WM closes the portal on its own and frees the WebServer/DNSServer objects; a second stopConfigPortal() then deref's
    // freed memory → LoadProhibited panic. g_portalSelfClosed is set by wifiOnPortalSave() in that path. The other path (timeout-based fall-through
    // from PORTAL back to TRYING_KNOWN) has WM still alive, so we must call stop() ourselves there.
    if (!g_portalSelfClosed) {
        g_wifiManager.stopConfigPortal();
    }
    g_portalSelfClosed = false;

    // AP → STA transition is delicate on arduino-esp32: after a long stretch in WIFI_AP_STA mode (5 min of captive portal, by default), the radio
    // keeps internal residues (AP context, beaconing state, netif callbacks). Just calling WiFi.mode(WIFI_STA) is not enough — the next few
    // scanNetworks() calls return 0 results and WiFiMulti logs "no matching wifi found!" even when our SSID is right there. The fix is a hard reset
    // sequence: disconnect(true, true) stops the radio AND erases the in-RAM driver creds, the 100 ms delay lets the driver-side async events
    // (STA_STOP, AP_STOP, …) drain, then we flip into pure STA. Bloquant 100 ms mais une seule fois par fermeture de portail, et de toute façon il
    // n'y a rien à faire pendant ce temps (par définition WiFi est down, donc MQTT est gated, et le BLE keyboard tourne sur son propre task).
    WiFi.disconnect(true, true);

    delay(100);

    WiFi.mode(WIFI_STA);

    // Resume the BLE keyboard scan now that the radio is back to STA-only. Mirrors the pauseScan() call in wifiStartPortal().
    g_kb.resumeScan();
}

void wifiForcePortal() {
    if (g_wifiState == WifiState::PORTAL) {
        ESP_LOGI(TAG_WIFI, "wifiForcePortal: already in PORTAL — no-op");
        return;
    }
    ESP_LOGI(TAG_WIFI, "wifiForcePortal: forcing PORTAL transition");
    wifiTransitionTo(WifiState::PORTAL);
}

// ================================================================================
// CONNECTED entry-point side effects: NTP + UI banner
// ================================================================================

static void wifiOnConnected() {
    String ip = WiFi.localIP().toString();
    ESP_LOGI(TAG_WIFI, "Connected to [%s], IP=%s, RSSI=%d", WiFi.SSID().c_str(), ip.c_str(), WiFi.RSSI());

    // NTP needs WiFi up and is idempotent. We run it once per boot only — subsequent reconnects don't need to re-arm SNTP.
    if (!g_wifiNtpDoneOnce) {
        setupNTP();
        g_wifiNtpDoneOnce = true;
    }

    // Banner on the rising edge (first CONNECTED after a LOST or initial connect).
    if (!g_wifiWasConnected) {
        //addConversationBlock("", "WiFi back", CONVO_INFO_COLOR, CENTER);
        g_wifiWasConnected = true;
    }
}

// ================================================================================
// Portal save callback — fires when the user submits credentials via the web UI
// ================================================================================

static void wifiOnPortalSave() {
    String ssid = WiFi.SSID();
    String pwd  = WiFi.psk();
    ESP_LOGI(TAG_WIFI, "Portal saved credentials: SSID=[%s] (pwd %u chars)", ssid.c_str(), (unsigned)pwd.length());

    if (ssid.length() == 0) {
        ESP_LOGW(TAG_WIFI, "Portal save callback fired but WiFi.SSID() is empty — skipping NVS write");
        return;
    }

    if (wifiSaveToNvs(ssid.c_str(), pwd.c_str())) {
        g_wifiMulti.addAP(ssid.c_str(), pwd.c_str());
        ESP_LOGI(TAG_WIFI, "Added [%s] to NVS and WiFiMulti", ssid.c_str());
    } else {
        ESP_LOGW(TAG_WIFI, "wifiSaveToNvs failed for [%s] (list full?) — credentials live only until reboot", ssid.c_str());
    }

    // Signal to wifiStopPortal() that WiFiManager will tear down the portal on its own (it does so right after this callback returns when STA
    // association succeeded). Prevents the double-stopConfigPortal() that previously caused a LoadProhibited panic.
    g_portalSelfClosed = true;
}

// ================================================================================
// Public state getter
// ================================================================================

WifiState wifiGetState() {
    return g_wifiState;
}

// ================================================================================
// NVS persistence
// ================================================================================

// Loads every known network into WiFiMulti: first the NVS-saved ones (user-managed, password may have been updated via portal), then the
// compile-time defaults from compiled_wifi.h. Deduplication is by SSID with NVS winning: if "SatelliteThree" is both in NVS and in the compiled
// defaults, only the NVS entry is used — its password is potentially fresher (user reconfigured via portal after changing the home router pass).
// The compile-time list therefore acts as a permanent safety net: a `/wifi clean` wipes NVS but the defaults still get loaded at the next boot,
// so the device can still come up on a known network without recompiling.
// Returns the total number of unique networks loaded into WiFiMulti.

static int wifiLoadNVSAndCompiledIntoMulti() {
    // Track SSIDs already added so the compile-default pass can skip duplicates. Capped at MAX_WIFI_NETWORKS — same cap as NVS storage.
    String nvsSsids[MAX_WIFI_NETWORKS];
    int    nvsSsidCount = 0;

    // === Pass 1: NVS (priority) ===
    g_wifiPrefs.begin(WIFI_PREFS_NAMESPACE, true);
    uint8_t count     = g_wifiPrefs.getUChar("count", 0);
    int     loadedNvs = 0;
    for (uint8_t i = 0; i < count && i < MAX_WIFI_NETWORKS; i++) {
        String ssidKey = "ssid_" + String((int)i);
        String pwdKey  = "pwd_" + String((int)i);
        String ssid    = g_wifiPrefs.getString(ssidKey.c_str(), "");
        String pwd     = g_wifiPrefs.getString(pwdKey.c_str(), "");
        if (ssid.length() == 0) {
            continue;  // forgotten slot, skip
        }
        g_wifiMulti.addAP(ssid.c_str(), pwd.c_str());
        if (nvsSsidCount < MAX_WIFI_NETWORKS) {
            nvsSsids[nvsSsidCount++] = ssid;
        }
        ESP_LOGD(TAG_WIFI, "  NVS  → addAP([%s], %u chars)", ssid.c_str(), (unsigned)pwd.length());
        loadedNvs++;
    }
    g_wifiPrefs.end();

    // === Pass 2: compile-time defaults, skipping SSIDs already pulled from NVS ===
    int loadedCompiled  = 0;
    int skippedCompiled = 0;
    for (size_t i = 0; i < COMPILED_WIFI_DEFAULTS_COUNT; i++) {
        const CompiledWifiEntry& e = COMPILED_WIFI_DEFAULTS[i];
        if (e.ssid == nullptr || e.ssid[0] == '\0') {
            continue;
        }

        bool dup = false;
        for (int k = 0; k < nvsSsidCount; k++) {
            if (nvsSsids[k] == e.ssid) {
                dup = true;
                break;
            }
        }
        if (dup) {
            ESP_LOGD(TAG_WIFI, "  COMP → skip [%s] (NVS entry wins)", e.ssid);
            skippedCompiled++;
            continue;
        }

        g_wifiMulti.addAP(e.ssid, e.pwd ? e.pwd : "");
        ESP_LOGD(TAG_WIFI, "  COMP → addAP([%s])", e.ssid);
        loadedCompiled++;
    }

    ESP_LOGI(TAG_WIFI,
             "Loaded into WiFiMulti: %d from NVS + %d from compile defaults (%d compiled skipped as duplicates)",
             loadedNvs,
             loadedCompiled,
             skippedCompiled);
    return loadedNvs + loadedCompiled;
}

bool wifiSaveToNvs(const char* ssid, const char* pwd) {
    if (ssid == nullptr || ssid[0] == '\0') {
        return false;
    }

    g_wifiPrefs.begin(WIFI_PREFS_NAMESPACE, false);
    uint8_t count = g_wifiPrefs.getUChar("count", 0);

    // First pass: is this SSID already saved? If yes, overwrite the password in place.
    for (uint8_t i = 0; i < count && i < MAX_WIFI_NETWORKS; i++) {
        String ssidKey  = "ssid_" + String((int)i);
        String existing = g_wifiPrefs.getString(ssidKey.c_str(), "");
        if (existing == ssid) {
            String pwdKey = "pwd_" + String((int)i);
            g_wifiPrefs.putString(pwdKey.c_str(), pwd ? pwd : "");
            g_wifiPrefs.end();
            ESP_LOGI(TAG_WIFI, "Updated password for existing SSID [%s] in slot %u", ssid, (unsigned)i);
            return true;
        }
    }

    // Second pass: find a free slot (empty SSID from a previous /wifi forget).
    for (uint8_t i = 0; i < count && i < MAX_WIFI_NETWORKS; i++) {
        String ssidKey  = "ssid_" + String((int)i);
        String existing = g_wifiPrefs.getString(ssidKey.c_str(), "");
        if (existing.length() == 0) {
            String pwdKey = "pwd_" + String((int)i);
            g_wifiPrefs.putString(ssidKey.c_str(), ssid);
            g_wifiPrefs.putString(pwdKey.c_str(), pwd ? pwd : "");
            g_wifiPrefs.end();
            ESP_LOGI(TAG_WIFI, "Saved [%s] in reclaimed slot %u", ssid, (unsigned)i);
            return true;
        }
    }

    // Third pass: append a new slot if we haven't reached MAX yet.
    if (count < MAX_WIFI_NETWORKS) {
        String ssidKey = "ssid_" + String((int)count);
        String pwdKey  = "pwd_" + String((int)count);
        g_wifiPrefs.putString(ssidKey.c_str(), ssid);
        g_wifiPrefs.putString(pwdKey.c_str(), pwd ? pwd : "");
        g_wifiPrefs.putUChar("count", count + 1);
        g_wifiPrefs.end();
        ESP_LOGI(TAG_WIFI, "Saved [%s] in new slot %u", ssid, (unsigned)count);
        return true;
    }

    g_wifiPrefs.end();
    ESP_LOGW(TAG_WIFI, "wifiSaveToNvs: list full (count=%u, max=%d) — [%s] not saved", (unsigned)count, MAX_WIFI_NETWORKS, ssid);
    return false;
}

bool wifiForgetFromNvs(const char* ssid) {
    if (ssid == nullptr || ssid[0] == '\0') {
        return false;
    }

    g_wifiPrefs.begin(WIFI_PREFS_NAMESPACE, false);
    uint8_t count   = g_wifiPrefs.getUChar("count", 0);
    bool    removed = false;
    for (uint8_t i = 0; i < count && i < MAX_WIFI_NETWORKS; i++) {
        String ssidKey  = "ssid_" + String((int)i);
        String existing = g_wifiPrefs.getString(ssidKey.c_str(), "");
        if (existing == ssid) {
            String pwdKey = "pwd_" + String((int)i);
            g_wifiPrefs.putString(ssidKey.c_str(), "");
            g_wifiPrefs.putString(pwdKey.c_str(), "");
            removed = true;
            ESP_LOGI(TAG_WIFI, "Forgot SSID [%s] (slot %u left as a hole; will be reclaimed on next save)", ssid, (unsigned)i);
            break;
        }
    }
    g_wifiPrefs.end();
    if (!removed) {
        ESP_LOGW(TAG_WIFI, "wifiForgetFromNvs: SSID [%s] not found", ssid);
    }
    return removed;
}

void wifiClearNvs() {
    g_wifiPrefs.begin(WIFI_PREFS_NAMESPACE, false);
    g_wifiPrefs.clear();
    g_wifiPrefs.end();
    ESP_LOGI(TAG_WIFI, "Cleared NVS namespace [%s] — next boot will re-seed from compile-time defaults if any", WIFI_PREFS_NAMESPACE);
}

void wifiPrintListToConversation() {
    g_wifiPrefs.begin(WIFI_PREFS_NAMESPACE, true);
    uint8_t count = g_wifiPrefs.getUChar("count", 0);

    printInfoLine("Saved WiFi networks:");
    int shown = 0;
    for (uint8_t i = 0; i < count && i < MAX_WIFI_NETWORKS; i++) {
        String ssidKey = "ssid_" + String((int)i);
        String ssid    = g_wifiPrefs.getString(ssidKey.c_str(), "");
        if (ssid.length() == 0) {
            continue;
        }
        String line = "  " + ssid;
        printInfoLine( line);
        shown++;
    }
    if (shown == 0) {
        printInfoLine( "  (none)");
    }
    g_wifiPrefs.end();
}

// ================================================================================
// Portal instructions renderer for the info screen
// ================================================================================
//
// Called from showUpdatedInfoScreen() in minimessenger.ino when g_wifiState == PORTAL. The caller has already drawn ID / Name / MAC / BTKB rows
// and passes the current nextY by reference; we advance it for each row we paint so it can continue the layout (HELP row gets appended below).

void drawPortalInstructions(Adafruit_ST7789* pDisp, int& nextY, int colHeaders, int colValues, int lineHeight) {
    pDisp->setTextSize(2);

    // First a one-line header, red on its own (no value column), to visually break from the meta rows above.
    pDisp->setCursor(colHeaders, nextY);
    pDisp->setTextColor(ST77XX_RED);
    pDisp->print("** WiFi setup **");
    nextY += lineHeight;

    // Three instruction rows: AP name, URL, action. Each uses the standard header/value column layout so it stays consistent visually with the
    // rest of the info screen even though the meaning is different.
    pDisp->setCursor(colHeaders, nextY);
    pDisp->setTextColor(ST77XX_RED);
    pDisp->print("AP:");
    pDisp->setCursor(colValues, nextY);
    pDisp->setTextColor(ST77XX_WHITE);
    pDisp->print(WIFI_PORTAL_AP_SSID);
    nextY += lineHeight;

    pDisp->setCursor(colHeaders, nextY);
    pDisp->setTextColor(ST77XX_RED);
    pDisp->print("PASS:");
    pDisp->setCursor(colValues, nextY);
    pDisp->setTextColor(ST77XX_WHITE);
    pDisp->print(WIFI_PORTAL_AP_PASS);
    nextY += lineHeight;

    pDisp->setCursor(colHeaders, nextY);
    pDisp->setTextColor(ST77XX_RED);
    pDisp->print("URL:");
    pDisp->setCursor(colValues, nextY);
    pDisp->setTextColor(ST77XX_WHITE);
    pDisp->print("192.168.4.1");
    nextY += lineHeight;

    pDisp->setCursor(colHeaders, nextY);
    pDisp->setTextColor(ST77XX_RED);
    pDisp->print("THEN:");
    pDisp->setCursor(colValues, nextY);
    pDisp->setTextColor(ST77XX_WHITE);
    pDisp->print("pick WiFi");
    nextY += lineHeight;
}
