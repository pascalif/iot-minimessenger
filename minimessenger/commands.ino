// ================================================================================
// commands.ino — Local command vocabulary, dispatchers, and help printers
// ================================================================================
//
// This file owns everything related to the "/cmd" surface area: the CMD_*/GROUP_* constants, the top-level dispatcher processPayloadAsCommand,
// the per-group sub-dispatchers (/wifi *, /mqtt *, /bt *, /dbg *), and the help printers (global + one partial per group).

#include "mm_log.h"
#include "mqtt.h"  // /mqtt drop calls g_mqttClient.disconnect(); the symbol is defined in mqtt.ino which is concatenated AFTER commands.ino.
#include "wifi.h"
#include <Arduino.h>

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
// /wifi pub — publishes the device's NVS-stored WiFi credentials (SSID|PWD per line) back to the sender via msg/unicast/<senderId>. Only valid when
// invoked from MQTT with a parseable senderDeviceId in the trailer; ignored (warning log) from serial or BLE keyboard since there is no return path.
const char* const CMD_WIFI_PUB = "/wifi pub";

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
    printCmdInfo("Commands:");
    printCmdInfo("/help", "list cmds");
    printCmdInfo("/status", "info screen");
    printCmdInfo("/clear", "wipe history");
    printCmdInfo("/wifi *", "WiFi mgmt");
    printCmdInfo("/mqtt *", "MQTT mgmt");
    printCmdInfo("/bt *", "BLE mgmt");
    printCmdInfo("/dbg *", "diagnostics");
}

void printHelpWifi() {
    ESP_LOGI(TAG_MM, "Listing /wifi subcommands");
    printCmdInfo("/wifi subcmds:");
    printCmdInfo("- drop", "drop link");
    printCmdInfo("- clean", "wipe NVS");
    printCmdInfo("- list", "known nets");
    printCmdInfo("- forget", "<ssid>");
    printCmdInfo("- portal", "open portal");
    printCmdInfo("- pub", "publish");
}

void printHelpMqtt() {
    ESP_LOGI(TAG_MM, "Listing /mqtt subcommands");
    printCmdInfo("/mqtt subcmds:");
    printCmdInfo("- drop", "drop MQTT");
}

void printHelpBt() {
    ESP_LOGI(TAG_MM, "Listing /bt subcommands");
    printCmdInfo("/bt subcmds:");
    printCmdInfo("- clean", "clear bonds");
}

void printHelpDbg() {
    ESP_LOGI(TAG_MM, "Listing /dbg subcommands");
    printCmdInfo("/dbg subcmds:");
    printCmdInfo("- chip", "chip + MACs");
    printCmdInfo("- mem", "heap + stack");
    printCmdInfo("- redraw", "full repaint");
}

// === Command implementations ==================================================

// /wifi pub — assemble every known WiFi credential (NVS + COMPILED_WIFI_DEFAULTS, NVS wins on duplicate SSID) as a multi-line payload "SSID|PWD"
// per line, and publish it back to the sender via msg/unicast/<recipientDeviceId>. Mirrors the boot-time merge done by
// wifiLoadNVSAndCompiledIntoMulti so the published set matches exactly what WiFiMulti has loaded, including the compile-time entries that a freshly
// flashed device with an empty NVS would otherwise hide. The MQTT publishing logic lives here in the command layer; the NVS + compile-table access
// details stay encapsulated in wifi.ino's wifiAppendKnownCredentialsToBuffer helper (exposed via wifi.h). The unicast topic is built into a local
// buffer so g_mqttOutgoingRecipientTopic (the chat recipient) is left untouched. Cleartext passwords on purpose — the whole point is to give an OTA
// operator the credentials they need to join the same WiFi network as the device.
static void cmdWifiPublishNetworksToMQTTPeer(byte recipientDeviceId) {
    char unicastTopic[MQTT_TOPIC_SIZE];
    snprintf(unicastTopic, MQTT_TOPIC_SIZE, "msg/unicast/%u", (unsigned)recipientDeviceId);

    // Payload budget: MSG_BUFFER_SIZE (500) is also the size of g_mqttOutgoingMsg, into which mqttPushFormattedMessage will copy `payload` AND append
    // the "### ts:… deviceId:… msgId:…" trailer (~60 chars worst case). We reserve 64 chars of headroom so the trailer fits without truncation.
    char         payload[MSG_BUFFER_SIZE];
    const size_t budget  = MSG_BUFFER_SIZE - 64;
    size_t       used    = 0;
    int          written = snprintf(payload, sizeof(payload), "wifi pub:");
    if (written > 0) {
        used = (size_t)written;
    }

    bool saturated = false;
    int  published = wifiAppendKnownCredentialsToBuffer(payload, budget, used, saturated);

    if (published == 0) {
        snprintf(payload, sizeof(payload), "wifi pub: (none)");
    }

    bool ok = mqttPushFormattedMessage(unicastTopic, payload);
    if (ok) {
        ESP_LOGI(TAG_MM, "Published %d WiFi entries to [%s]%s", published, unicastTopic, saturated ? " (buffer saturated, list truncated)" : "");
    } else {
        ESP_LOGW(TAG_MM, "Failed to publish WiFi entries to [%s]", unicastTopic);
    }
    if (saturated) {
        ESP_LOGW(TAG_MM,
                 "Payload buffer saturated — some NVS WiFi entries were omitted "
                 "from the [%s] publication",
                 unicastTopic);
    }
}

// /dbg chip — diagnostic dump of silicon identity (chip model, revision, cores, features, package, MACs, flash, IDF version, reset reason). Sent
// both to the serial log (multi-line, with structured fields) and to the conversation in compact form so the info is visible on the device too.
// See ../docs/howto_efuse.md for what each line means and which API gives it.
static void cmdDumpChipInfo() {
    esp_chip_info_t info;
    esp_chip_info(&info);

    ESP_LOGI(TAG_MM, "--- /dbg chip ---");
    ESP_LOGI(TAG_MM, "Chip: model=%d revision=%d cores=%d features=0x%lx", (int)info.model, info.revision, info.cores, (unsigned long)info.features);
    ESP_LOGI(TAG_MM,
             "Caps: WiFi:%s BT:%s BLE:%s EmbFlash:%s EmbPSRAM:%s",
             (info.features & CHIP_FEATURE_WIFI_BGN) ? "y" : "-",
             (info.features & CHIP_FEATURE_BT) ? "y" : "-",
             (info.features & CHIP_FEATURE_BLE) ? "y" : "-",
             (info.features & CHIP_FEATURE_EMB_FLASH) ? "y" : "-",
             (info.features & CHIP_FEATURE_EMB_PSRAM) ? "y" : "-");
    ESP_LOGI(TAG_MM, "CPU: %u MHz", ESP.getCpuFreqMHz());
    ESP_LOGI(TAG_MM, "Flash: size=%u mode=%d speed=%u Hz", ESP.getFlashChipSize(), ESP.getFlashChipMode(), ESP.getFlashChipSpeed());

    uint8_t mac[6];
    // Use ESP_MAC_EFUSE_FACTORY (direct silicon read) instead of esp_efuse_mac_get_default() — see identifyDevice() for the full explanation
    // of why the latter returns ESP_OK + zeros when called before the IDF base-MAC cache has been populated.
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    ESP_LOGI(TAG_MM, "MAC base/STA: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_read_mac(mac, ESP_MAC_BT);
    ESP_LOGI(TAG_MM, "MAC BT:       %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG_MM, "Sketch: size=%u free=%u", ESP.getSketchSize(), ESP.getFreeSketchSpace());
    ESP_LOGI(TAG_MM, "IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG_MM, "Last reset reason: %d", (int)esp_reset_reason());

    // Echo a compact summary into the conversation so it's visible on-device too.
    char line[64];
    snprintf(line, sizeof(line), "model=%d rev=%d cpu=%uMHz", (int)info.model, info.revision, ESP.getCpuFreqMHz());
    printCmdInfo(line);
    snprintf(line, sizeof(line), "flash=%uKB reset=%d", ESP.getFlashChipSize() / 1024, (int)esp_reset_reason());
    printCmdInfo(line);
}

// /dbg mem — diagnostic dump of heap state. "free" is the sum of all free bytes; "largest" is the biggest single contiguous run (the one mbedtls /
// TLS / SPI buffers need). "min ever" is the historical low-water mark — a value that keeps dropping over time is the classic signature of a leak.
// Sketch / partition counters are flash-side, not RAM, but useful in the same diagnostic dump.
static void cmdDumpMemInfo() {
    uint32_t totalHeap   = ESP.getHeapSize();
    uint32_t freeHeap    = ESP.getFreeHeap();
    uint32_t largest     = ESP.getMaxAllocHeap();
    uint32_t minEverFree = ESP.getMinFreeHeap();
    uint32_t usedHeap    = totalHeap - freeHeap;

    ESP_LOGI(TAG_MM, "--- /dbg mem ---");
    ESP_LOGI(TAG_MM, "Heap: total=%u used=%u free=%u largest=%u min-ever-free=%u", totalHeap, usedHeap, freeHeap, largest, minEverFree);
    ESP_LOGI(TAG_MM, "Heap fragmentation: %u%% (= 1 - largest/free)", freeHeap > 0 ? (100 - (largest * 100 / freeHeap)) : 0);
    // All byte-counts shown in KB on the device screen (integer division — values < 1 KB display as "0 KB", which is itself a useful red-flag signal
    // for the stack low-watermark). The serial log above keeps the raw byte counts for precise diagnostics.
    printCmdInfo("heap:");
    printCmdInfo("- free", String(freeHeap / 1024) + " KB");
    printCmdInfo("- max block", String(largest / 1024) + " KB");
    printCmdInfo("- min ever", String(minEverFree / 1024) + " KB");
    printCmdInfo("- frag", String(freeHeap > 0 ? (100 - (largest * 100 / freeHeap)) : 0) + " %");

    // PSRAM is only present on certain ESP32 variants (e.g. WROVER). On chips without PSRAM these calls return 0.
    if (ESP.getPsramSize() > 0) {
        ESP_LOGI(TAG_MM, "PSRAM: total=%u free=%u largest=%u", ESP.getPsramSize(), ESP.getFreePsram(), ESP.getMaxAllocPsram());
    } else {
        ESP_LOGI(TAG_MM, "PSRAM: none");
    }

    // Stack high-water-mark for the task currently running this code (typically the Arduino loop task). Lower number = closer to overflow.
    // Returns the minimum free stack the task has ever had since boot, in WORDS (uint32_t units on ESP32) — multiply by 4 for bytes.
    UBaseType_t stackHWMWords = uxTaskGetStackHighWaterMark(NULL);
    uint32_t    stackHWMBytes = (uint32_t)stackHWMWords * 4;
    ESP_LOGI(TAG_MM, "Loop task stack: min-ever-free=%u bytes", (unsigned)stackHWMBytes);
    printCmdInfo("stack:");
    printCmdInfo("- min ever", String(stackHWMBytes / 1024) + " KB");

    ESP_LOGI(TAG_MM, "Sketch: size=%u free=%u", ESP.getSketchSize(), ESP.getFreeSketchSpace());
    printCmdInfo("sketch:");
    printCmdInfo("- size", String(ESP.getSketchSize() / 1024) + " KB");
    printCmdInfo("- free", String(ESP.getFreeSketchSpace() / 1024) + " KB");
}

// === Dispatchers ==============================================================

// Top-level command dispatcher. Three orphan commands (/help, /status, /clear) and four prefix groups (/wifi *, /mqtt *, /bt *, /dbg *). For groups,
// the bare prefix (e.g. "/wifi" alone with no subcommand) prints the group's partial help; a typo subcommand (e.g. "/wifi xyzzy") prints an "Unknown"
// banner followed by the same partial help. Returns true if the message was consumed as a command (caller skips display + republish).
//
// `source` and `senderDeviceId` are threaded through so subcommands that need to reply via MQTT (currently only /wifi pub) can authenticate the
// origin and address the response. For local-only commands the parameters are simply ignored. Passing them down to every sub-dispatcher (rather than
// only /wifi) keeps the surface uniform — adding a future /mqtt-pub-style command won't require a signature change cascade.
bool processPayloadAsCommand(const String& message, MessageSource source, byte senderDeviceId) {
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
        return processWifiSubcommand(message, source, senderDeviceId);
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

bool processWifiSubcommand(const String& message, MessageSource source, byte senderDeviceId) {
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
        printCmdInfo("NVS WiFi cleared - reboot to re-seed");
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
            printCmdError("Usage: /wifi forget <ssid>");
        } else if (wifiForgetFromNvs(ssid.c_str())) {
            printCmdInfo(String("Forgot: ") + ssid);
        } else {
            printCmdError(String("Not found: ") + ssid);
        }
        return true;
    }
    if (message == CMD_WIFI_PORTAL) {
        ESP_LOGI(TAG_MM, "Command [%s] — forcing portal", CMD_WIFI_PORTAL);
        wifiForcePortal();
        return true;
    }
    if (message == CMD_WIFI_PUB) {
        // Guardrail: this command only makes sense over MQTT, because the response is a unicast publish back to the sender's deviceId — which is
        // only available when the incoming payload carried a parseable trailer (the dispatcher in mqtt.ino extracted it and routeMessage passed it
        // down to us). A serial-typed or BLE-typed "/wifi pub" has no return path → silent refusal with a warning log so the operator can see why
        // nothing happened. We also refuse senderDeviceId == 0 (means: MQTT message had no parseable deviceId trailer, e.g. mosquitto_pub from a
        // dev laptop without our trailer format) for the same reason: no valid return topic.
        if (source != MessageSource::REMOTE || senderDeviceId == 0) {
            ESP_LOGW(TAG_MM,
                     "Command [%s] ignored — only valid from MQTT with a parseable senderDeviceId (got source=%d senderId=%u)",
                     CMD_WIFI_PUB,
                     (int)source,
                     (unsigned)senderDeviceId);
            return true;
        }
        ESP_LOGI(TAG_MM, "Command [%s] from #%u — publishing NVS WiFi credentials", CMD_WIFI_PUB, (unsigned)senderDeviceId);
        cmdWifiPublishNetworksToMQTTPeer(senderDeviceId);
        return true;
    }
    // Unknown subcommand — show what's available so the user can correct.
    ESP_LOGW(TAG_MM, "Unknown /wifi subcommand: [%s]", message.c_str());
    printCmdError("Unknown /wifi cmd");
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
    printCmdError("Unknown /mqtt cmd");
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
        printCmdInfo("NVS BT bonds cleared");
        return true;
    }
    ESP_LOGW(TAG_MM, "Unknown /bt subcommand: [%s]", message.c_str());
    printCmdError("Unknown /bt cmd");
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
        cmdDumpChipInfo();
        return true;
    }
    if (message == CMD_DBG_MEM) {
        ESP_LOGI(TAG_MM, "Command [%s] — dumping memory info", CMD_DBG_MEM);
        cmdDumpMemInfo();
        return true;
    }
    ESP_LOGW(TAG_MM, "Unknown /dbg subcommand: [%s]", message.c_str());
    printCmdError("Unknown /dbg cmd");
    printHelpDbg();
    return true;
}
