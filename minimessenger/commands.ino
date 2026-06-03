// ================================================================================
// commands.ino — Local command vocabulary, dispatchers, and help printers
// ================================================================================
//
// This file owns everything related to the "/cmd" surface area: the CMD_*/GROUP_* constants, the top-level dispatcher processPayloadAsCommand,
// the per-group sub-dispatchers (/wifi *, /mqtt *, /bt *, /dbg *), and the help printers (global + one partial per group).
//
// It does NOT own the funnel — routeMessage() in minimessenger.ino is the single insertion point for any payload arriving over MQTT, Serial or
// the BLE keyboard, and it calls processPayloadAsCommand() as one of its steps. See the header comment in minimessenger.ino around routeMessage
// for the funnel's three-step contract (wake screen → interpret as command → otherwise route to display + MQTT).
//
// Arduino IDE concatenates this file with the main .ino into a single translation unit (in alphabetical order after the sketch-named file).
// commands.ino therefore sees all of minimessenger.ino above it and can call addConversationBlock, dumpChipInfo, etc. without forward decls.
// Symbols defined in wifi.ino (concatenated later) are reached via the forward declarations in wifi.h, which we include below.

#include <Arduino.h>
#include "wifi.h"
#include "mm_log.h"
#include "mqtt.h"  // /mqtt drop calls g_mqttClient.disconnect(); the symbol is defined in mqtt.ino which is concatenated AFTER commands.ino.

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
//      "/bt clean" wipes BLE bonds without leaking that string to other devices.
//   3. Otherwise route to display: LEFT for REMOTE, RIGHT for LOCAL — and for LOCAL, also publish to peers over MQTT so they receive the text.
//
// Why ONE funnel rather than scattering the wake / interpret logic across each channel's handler: it keeps the command vocabulary in one place,
// and any future channel just has to call routeMessage() with the right source to inherit all three steps.
// ----------------------------------------------------------------------------

// Orphan commands — shown directly in the global /help listing. Pure top-level verbs, no group structure (yet).
const char* const CMD_HELP   = "/help";
const char* const CMD_STATUS = "/status";
const char* const CMD_CLEAR  = "/clear";

// Group prefixes. Typing the bare prefix (e.g. just "/wifi") prints the group's partial help. Subcommands use a SPACE separator, not a hyphen:
// "/wifi drop" instead of "/wifi-drop". This lets the dispatcher branch on `startsWith("/wifi ")` and parse the remaining argv with simple
// substring math. The bare prefixes themselves are listed in the global /help so users discover the groups exist.
// /mqtt and /bt currently host one subcommand each (drop / clean); the group skeleton is in place so additional verbs slot in without re-routing.
const char* const GROUP_WIFI = "/wifi";
const char* const GROUP_DBG  = "/dbg";
const char* const GROUP_MQTT = "/mqtt";
const char* const GROUP_BT   = "/bt";

// WiFi subcommands. /wifi forget takes an SSID argument so it's matched via startsWith() with a trailing space rather than equality.
const char* const CMD_WIFI_DROP   = "/wifi drop";
const char* const CMD_WIFI_CLEAN  = "/wifi clean";
const char* const CMD_WIFI_LIST   = "/wifi list";
const char* const CMD_WIFI_FORGET = "/wifi forget";  // payload: "/wifi forget <ssid>"
const char* const CMD_WIFI_PORTAL = "/wifi portal";

// Debug / diagnostic subcommands. Visual recovery (redraw) and diagnostic dumps (chip, mem).
const char* const CMD_DBG_CHIP   = "/dbg chip";
const char* const CMD_DBG_MEM    = "/dbg mem";
const char* const CMD_DBG_REDRAW = "/dbg redraw";

// MQTT subcommands.
const char* const CMD_MQTT_DROP = "/mqtt drop";

// BLE subcommands.
const char* const CMD_BT_CLEAN = "/bt clean";


// === Help printers ============================================================
// Each one lists the entries left-aligned in pink so it stands out from normal traffic. The global /help only shows orphan commands and the
// group prefixes ("/wifi *", "/dbg *"); subcommands are listed only by their group's partial help. This keeps the global view short as new
// debug or wifi-management commands are added.

void printHelpGlobal() {
    ESP_LOGI(TAG_MM, "Listing global commands");
    printInfoLine("Commands:");
    printInfoLine("/help", "list cmds");
    printInfoLine("/status", "info screen");
    printInfoLine("/clear", "wipe history");
    printInfoLine("/wifi *", "WiFi mgmt");
    printInfoLine("/mqtt *", "MQTT mgmt");
    printInfoLine("/bt *", "BLE mgmt");
    printInfoLine("/dbg *", "diagnostics");
}

void printHelpWifi() {
    ESP_LOGI(TAG_MM, "Listing /wifi subcommands");
    printInfoLine("/wifi subcmds:");
    printInfoLine("- drop", "drop link");
    printInfoLine("- clean", "wipe NVS");
    printInfoLine("- list", "known nets");
    printInfoLine("- forget", "<ssid>");
    printInfoLine("- portal", "open portal");
}

void printHelpMqtt() {
    ESP_LOGI(TAG_MM, "Listing /mqtt subcommands");
    printInfoLine("/mqtt subcmds:");
    printInfoLine("- drop", "drop MQTT");
}

void printHelpBt() {
    ESP_LOGI(TAG_MM, "Listing /bt subcommands");
    printInfoLine("/bt subcmds:");
    printInfoLine("- clean", "clear bonds");
}

void printHelpDbg() {
    ESP_LOGI(TAG_MM, "Listing /dbg subcommands");
    printInfoLine("/dbg subcmds:");
    printInfoLine("- chip", "chip + MACs");
    printInfoLine("- mem", "heap + stack");
    printInfoLine("- redraw", "full repaint");
}


// === Dispatchers ==============================================================

// Top-level command dispatcher. Three orphan commands (/help, /status, /clear) and four prefix groups (/wifi *, /mqtt *, /bt *, /dbg *). For groups,
// the bare prefix (e.g. "/wifi" alone with no subcommand) prints the group's partial help; a typo subcommand (e.g. "/wifi xyzzy") prints an "Unknown"
// banner followed by the same partial help. Returns true if the message was consumed as a command (caller skips display + republish).
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
    // Group routing: the bare prefix OR the prefix followed by a space. The trailing-space check rules out false positives like "/wifix" (no
    // such command, must not be misrouted into the WiFi dispatcher).
    if (message == GROUP_WIFI || message.startsWith(String(GROUP_WIFI) + " ")) {
        return processWifiSubcommand(message);
    }
    if (message == GROUP_MQTT || message.startsWith(String(GROUP_MQTT) + " ")) {
        return processMqttSubcommand(message);
    }
    if (message == GROUP_BT || message.startsWith(String(GROUP_BT) + " ")) {
        return processBtSubcommand(message);
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
        // Wipes the entire NVS WiFi namespace. Compile-time defaults in personal-data.h still re-feed WiFiMulti at the next boot, so the device can
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
    printInfoLine("Unknown /wifi cmd", CONVO_ERROR_COLOR);
    printHelpWifi();
    return true;
}

bool processMqttSubcommand(const String& message) {
    if (message == GROUP_MQTT) {
        printHelpMqtt();
        return true;
    }
    if (message == CMD_MQTT_DROP) {
        ESP_LOGI(TAG_MM, "Command [%s] — disconnecting MQTT", CMD_MQTT_DROP);
        g_mqttClient.disconnect();
        return true;
    }
    ESP_LOGW(TAG_MM, "Unknown /mqtt subcommand: [%s]", message.c_str());
    printInfoLine("Unknown /mqtt cmd", CONVO_ERROR_COLOR);
    printHelpMqtt();
    return true;
}

bool processBtSubcommand(const String& message) {
    if (message == GROUP_BT) {
        printHelpBt();
        return true;
    }
    if (message == CMD_BT_CLEAN) {
        // Delegated to the keyboard wrapper so the NimBLEDevice API stays encapsulated in mm_blekb. The wrapper emits its own "All bonds cleared"
        // log under TAG_BTKB for traceability.
        ESP_LOGI(TAG_MM, "Command [%s] — clearing all BLE bonds", CMD_BT_CLEAN);
        g_kb.clearAllExistingBonds();
        printInfoLine("NVS BT bonds cleared");
        return true;
    }
    ESP_LOGW(TAG_MM, "Unknown /bt subcommand: [%s]", message.c_str());
    printInfoLine("Unknown /bt cmd", CONVO_ERROR_COLOR);
    printHelpBt();
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
    printInfoLine("Unknown /dbg cmd", CONVO_ERROR_COLOR);
    printHelpDbg();
    return true;
}
