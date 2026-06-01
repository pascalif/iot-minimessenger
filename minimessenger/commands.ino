// ================================================================================
// commands.ino — Local command vocabulary, dispatchers, and help printers
// ================================================================================
//
// This file owns everything related to the "/cmd" surface area: the CMD_*/GROUP_* constants, the top-level dispatcher processPayloadAsCommand,
// the per-group sub-dispatchers (/wifi *, /dbg *), and the three help printers (global + per-group partial).
//
// It does NOT own the funnel — routeMessage() in minimessenger.ino is the single insertion point for any payload arriving over MQTT, Serial or
// the BLE keyboard, and it calls processPayloadAsCommand() as one of its steps. See the header comment in minimessenger.ino around routeMessage
// for the funnel's three-step contract (wake screen → interpret as command → otherwise route to display + MQTT).
//
// Arduino IDE concatenates this file with the main .ino into a single translation unit (in alphabetical order after the sketch-named file).
// commands.ino therefore sees all of minimessenger.ino above it and can call addConversationBlock, dumpChipInfo, etc. without forward decls.
// Symbols defined in wifi.ino (concatenated later) are reached via the forward declarations in wifi_state.h, which we include below.

#include <Arduino.h>
#include "wifi_state.h"
#include "mm_log.h"

// ----------------------------------------------------------------------------
// Local commands + message funnel
//
// routeMessage() (in minimessenger.ino) is the SINGLE insertion point for any complete payload landing in this device, whether it arrived over
// MQTT (REMOTE) or was just composed locally on the serial monitor / BLE keyboard (LOCAL). Both channels converge there so the same three steps
// happen exactly once:
//
//   1. Wake the screen via noteUserActivity() — every payload is a real user-initiated event and must be visible, regardless of dim/off state.
//   2. Try to interpret the payload as a local command (CMD_* / GROUP_* below). Commands run locally and are NEITHER displayed in the conversation
//      NOR republished to peers, so an MQTT "/wifi drop" cleanly disconnects the recipient without polluting anyone's screen, and a serial
//      "/bt-clean" wipes BLE bonds without leaking that string to other devices.
//   3. Otherwise route to display: LEFT for REMOTE, RIGHT for LOCAL — and for LOCAL, also publish to peers over MQTT so they receive the text.
//
// Why ONE funnel rather than scattering the wake / interpret logic across each channel's handler: it keeps the command vocabulary in one place,
// and any future channel just has to call routeMessage() with the right source to inherit all three steps.
// ----------------------------------------------------------------------------

// Orphan commands — shown directly in the global /help listing. /mqtt-drop and /bt-clean stay top-level for now (no /mqtt * or /bt * group
// would have a second subcommand to justify it); promote them into a group later if more verbs land in those families.
const char* const CMD_HELP      = "/help";
const char* const CMD_STATUS    = "/status";
const char* const CMD_CLEAR     = "/clear";
const char* const CMD_MQTT_DROP = "/mqtt-drop";
const char* const CMD_BT_CLEAN  = "/bt-clean";

// Group prefixes. Typing the bare prefix (e.g. just "/wifi") prints the group's partial help. Subcommands use a SPACE separator, not a hyphen:
// "/wifi drop" instead of "/wifi-drop". This lets the dispatcher branch on `startsWith("/wifi ")` and parse the remaining argv with simple
// substring math. The bare prefixes themselves are listed in the global /help so users discover the groups exist.
const char* const GROUP_WIFI = "/wifi";
const char* const GROUP_DBG  = "/dbg";

// WiFi subcommands. /wifi forget takes an SSID argument so it's matched via startsWith() with a trailing space rather than equality.
const char* const CMD_WIFI_DROP   = "/wifi drop";
const char* const CMD_WIFI_CLEAN  = "/wifi clean";
const char* const CMD_WIFI_LIST   = "/wifi list";
const char* const CMD_WIFI_FORGET = "/wifi forget";  // payload: "/wifi forget <ssid>"
const char* const CMD_WIFI_PORTAL = "/wifi portal";

// Debug / diagnostic subcommands. Kept under /dbg because they're either visual recovery (redraw) or diagnostic dumps (chip, mem). User-facing
// connectivity actions (/mqtt-drop, /bt-clean) live as orphans above.
const char* const CMD_DBG_CHIP   = "/dbg chip";
const char* const CMD_DBG_MEM    = "/dbg mem";
const char* const CMD_DBG_REDRAW = "/dbg redraw";


// === Help printers ============================================================
// Each one lists the entries left-aligned in pink so it stands out from normal traffic. The global /help only shows orphan commands and the
// group prefixes ("/wifi *", "/dbg *"); subcommands are listed only by their group's partial help. This keeps the global view short as new
// debug or wifi-management commands are added.

void printHelpGlobal() {
    ESP_LOGI(TAG_MM, "Listing global commands");
    printInfoLine("Commands:");
    printInfoLine("/help",      "list cmds");
    printInfoLine("/status",    "info screen");
    printInfoLine("/clear",     "wipe history");
    printInfoLine("/mqtt-drop", "drop MQTT");
    printInfoLine("/bt-clean",  "clear bonds");
    printInfoLine("/wifi *",    "WiFi mgmt");
    printInfoLine("/dbg *",     "diagnostics");
}

void printHelpWifi() {
    ESP_LOGI(TAG_MM, "Listing /wifi subcommands");
    printInfoLine("/wifi subcmds:");
    printInfoLine("- drop",   "drop link");
    printInfoLine("- clean",  "wipe NVS");
    printInfoLine("- list",   "known nets");
    printInfoLine("- forget", "<ssid>");
    printInfoLine("- portal", "open portal");
}

void printHelpDbg() {
    ESP_LOGI(TAG_MM, "Listing /dbg subcommands");
    printInfoLine("/dbg subcmds:");
    printInfoLine("- chip",   "chip + MACs");
    printInfoLine("- mem",    "heap + stack");
    printInfoLine("- redraw", "full repaint");
}


// === Dispatchers ==============================================================

// Top-level command dispatcher. Two orphan commands (/help, /status) and two prefix groups (/wifi *, /dbg *). For groups, the bare prefix
// (e.g. "/wifi" alone with no subcommand) prints the group's partial help; a typo subcommand (e.g. "/wifi xyzzy") prints an "Unknown" banner
// followed by the same partial help. Returns true if the message was consumed as a command (caller skips display + republish).
bool processPayloadAsCommand(const String& message) {
    if (message == CMD_HELP) {
        printHelpGlobal();
        return true;
    }
    if (message == CMD_STATUS) {
        ESP_LOGI(TAG_MM, "Command [%s] — info screen overlay for %ums", CMD_STATUS, (unsigned)STATUS_SCREEN_DURATION_MS);
        showUpdatedInfoScreen();
        g_statusScreenEndMs = millis() + STATUS_SCREEN_DURATION_MS;
        return true;
    }
    if (message == CMD_CLEAR) {
        ESP_LOGI(TAG_MM, "Command [%s] — wiping conversation history + scroll area", CMD_CLEAR);
        clearConversationHistory();
        return true;
    }
    if (message == CMD_MQTT_DROP) {
        ESP_LOGI(TAG_MM, "Command [%s] — disconnecting MQTT", CMD_MQTT_DROP);
        g_mqttClient.disconnect();
        return true;
    }
    if (message == CMD_BT_CLEAN) {
        // Delegated to the keyboard wrapper so the NimBLEDevice API stays encapsulated in mm_blekb. The wrapper emits its own "All bonds cleared"
        // log under TAG_BTKB for traceability.
        ESP_LOGI(TAG_MM, "Command [%s] — clearing all BLE bonds", CMD_BT_CLEAN);
        g_kb.clearAllExistingBonds();
        return true;
    }
    // Group routing: the bare prefix OR the prefix followed by a space. The trailing-space check rules out false positives like "/wifix" (no
    // such command, must not be misrouted into the WiFi dispatcher).
    if (message == GROUP_WIFI || message.startsWith(String(GROUP_WIFI) + " ")) {
        return processWifiSubcommand(message);
    }
    if (message == GROUP_DBG || message.startsWith(String(GROUP_DBG) + " ")) {
        return processDbgSubcommand(message);
    }
    return false;
}

bool processWifiSubcommand(const String& message) {
    if (message == GROUP_WIFI) {
        printHelpWifi();
        return true;
    }
    if (message == CMD_WIFI_DROP) {
        ESP_LOGI(TAG_MM, "Command [%s] — disconnecting WiFi", CMD_WIFI_DROP);
        WiFi.disconnect();
        return true;
    }
    if (message == CMD_WIFI_CLEAN) {
        // Wipes the entire NVS WiFi namespace. Compile-time defaults in compiled_wifi.h still re-feed WiFiMulti at the next boot, so the device can
        // come up on a known network without reflashing. The current STA connection survives this call — only future boots are affected.
        ESP_LOGI(TAG_MM, "Command [%s] — clearing NVS WiFi list", CMD_WIFI_CLEAN);
        wifiClearNvs();
        printInfoLine("NVS WiFi cleared - reboot to re-seed");
        return true;
    }
    if (message == CMD_WIFI_LIST) {
        ESP_LOGI(TAG_MM, "Command [%s] — listing saved WiFi networks", CMD_WIFI_LIST);
        wifiPrintListToConversation();
        return true;
    }
    if (message.startsWith(String(CMD_WIFI_FORGET) + " ")) {
        String ssid = message.substring(strlen(CMD_WIFI_FORGET) + 1);
        ssid.trim();
        ESP_LOGI(TAG_MM, "Command [%s] — forgetting SSID [%s]", CMD_WIFI_FORGET, ssid.c_str());
        if (ssid.length() == 0) {
            printInfoLine("Usage: /wifi forget <ssid>", CONVO_ERROR_COLOR);
        } else if (wifiForgetFromNvs(ssid.c_str())) {
            printInfoLine(String("Forgot: ") + ssid);
        } else {
            printInfoLine(String("Not found: ") + ssid, CONVO_ERROR_COLOR);
        }
        return true;
    }
    if (message == CMD_WIFI_PORTAL) {
        ESP_LOGI(TAG_MM, "Command [%s] — forcing portal", CMD_WIFI_PORTAL);
        wifiForcePortal();
        return true;
    }
    // Unknown subcommand — show what's available so the user can correct.
    ESP_LOGW(TAG_MM, "Unknown /wifi subcommand: [%s]", message.c_str());
    printInfoLine("Unknown /wifi cmd:", CONVO_ERROR_COLOR);
    printHelpWifi();
    return true;
}

bool processDbgSubcommand(const String& message) {
    if (message == GROUP_DBG) {
        printHelpDbg();
        return true;
    }
    if (message == CMD_DBG_REDRAW) {
        // Full repaint: status bar + footer + scroll area refilled from the ring buffer. Same path as the auto-revert from /status so users have
        // one consistent recovery command after visual glitches or framebuffer/state drift.
        ESP_LOGI(TAG_MM, "Command [%s] — full redraw of the 3 zones", CMD_DBG_REDRAW);
        returnToConversationsScreen();
        return true;
    }
    if (message == CMD_DBG_CHIP) {
        ESP_LOGI(TAG_MM, "Command [%s] — dumping chip info", CMD_DBG_CHIP);
        dumpChipInfo();
        return true;
    }
    if (message == CMD_DBG_MEM) {
        ESP_LOGI(TAG_MM, "Command [%s] — dumping memory info", CMD_DBG_MEM);
        dumpMemInfo();
        return true;
    }
    ESP_LOGW(TAG_MM, "Unknown /dbg subcommand: [%s]", message.c_str());
    printInfoLine("Unknown /dbg cmd:", CONVO_ERROR_COLOR);
    printHelpDbg();
    return true;
}
