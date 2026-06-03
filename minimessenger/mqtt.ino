// ================================================================================
// mqtt.ino — MQTT broker plumbing: topics, globals, reconnect, publish, incoming dispatch, TLS root CA
// ================================================================================
//
// All MQTT-specific code lives here. The Arduino IDE concatenates this file with minimessenger.ino into a single translation unit (alphabetical order
// after the main sketch), so anything declared in mqtt.h is visible from both files. Same split philosophy as wifi.ino / commands.ino: keep the main
// .ino focused on orchestration (status bar, loop reconnect gate, setRecipient, etc.), push the cross-cutting MQTT plumbing into its own file.
//
// Stays in minimessenger.ino on purpose:
//   - the loop()-level reconnect gate and the status-bar / info-screen indicators — those are display/orchestration concerns, not MQTT plumbing.
//   - setRecipient(), onMQTTReconnected() — conversation logic and display transitions, not MQTT internals.
//   - the setupMqtt() call sequence (setCACert / setInsecure / setServer / setCallback) — lives in setup() right after setupWifi() so the
//     boot sequence stays linear and readable in one place.
//
// Broker credentials (server, port, user, password) + TLS root CA are no longer here either — they're grouped in g_mqttServerInfo (declared in
// mqtt.h, defined in personal-data.h). The connect() call below reads them through that struct.

#include "mqtt.h"
#include "mm_log.h"
#include "symbols.h"
#include <WiFiClientSecure.h>

// WiFiClientSecure instance lives in minimessenger.ino (declared before mqtt.ino in the concatenation order, so the constructor below sees it). The
// extern here is purely for clarity / so this file reads standalone.
extern WiFiClientSecure g_wifiClient;

// Device identity (g_deviceData, with `g_deviceData.name()` for the formatted "namePrefix_NNN" string) is defined in minimessenger.ino /
// personal-data.h. The .ino files are concatenated into a single TU by the Arduino IDE, so no extern declaration is needed here. See
// identifyDevice() for the init path.

// LED + display helpers called from this file (LED on after a successful connect, message routing for incoming chat messages, contact liveness LEDs).
// Forward-declared so the auto-prototype ordering does not bite us.
extern void  ledSetState(int pin, int requiredState);
extern void  routeMessage(const String& message, MessageSource source);
extern void  onReceivedContactOnline(int remoteDeviceId, bool isLive);
extern char* getCurrentDateTime();

// ================================================================================
// Topic strings
// ================================================================================
const char* g_mqttOutgoingTopicLogs      = "admin/logs";
const char* g_mqttOutgoingTopicLive      = "admin/live";
const char* g_mqttOutgoingTopicWill      = "admin/dead";
const char* g_mqttIncomingTopicBroadcast = "msg/broadcast";
//                                         "msg/unicast/12"

// ================================================================================
// Runtime globals
// ================================================================================
PubSubClient  g_mqttClient(g_wifiClient);  // a WiFiClientSecure instance is needed for HiveMQ connection
int           g_mqttConnectionId                 = -1;
unsigned int  g_mqttOutputMsgId                  = 0;
bool          g_mqttWasConnected                 = false;
unsigned long g_mqttLastReconnectTryTimestampMs  = 0;
unsigned long g_mqttPreviousKeepAliveTimestampMs = 0;
uint8_t       g_mqttReconnectAttempts            = 0;

char g_mqttOutgoingMsg[MSG_BUFFER_SIZE];
char g_mqttOutgoingRecipientTopic[MQTT_TOPIC_SIZE];


// ================================================================================
// MQTT — connect / publish / receive
// ================================================================================

void setupMQTT() {
    // The verification mode is driven by g_mqttServerInfo.rootCA: non-null → strict verification against that CA, null → setInsecure() so mbedtls still
    // negotiates TLS but accepts whatever cert the broker presents. Calling NEITHER would leave WiFiClientSecure in its strict-no-CA default state
    // and the handshake would systematically fail with MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, so the null branch is wired explicitly to setInsecure().
    if (g_mqttServerInfo.rootCA != nullptr) {
        g_wifiClient.setCACert(g_mqttServerInfo.rootCA);
    } else {
        g_wifiClient.setInsecure();
        ESP_LOGW(TAG_MQTT, "No root CA configured (g_mqttServerInfo.rootCA == nullptr) — falling back to setInsecure() (no server-cert verification)");
    }
    g_mqttClient.setServer(g_mqttServerInfo.server, g_mqttServerInfo.port);
    g_mqttClient.setCallback(onMqttIncomingMessage);
}

// Current wait interval between two reconnect attempts. Exponential backoff: BASE * 2^attempts, capped at MAX. Cap k early so the shift never
// overflows unsigned long (2^32 = 4 GB ms — we hit MAX_MS long before that, but a defensive cap keeps the math obvious).
unsigned long mqttReconnectDelayMs() {
    uint8_t       k     = (g_mqttReconnectAttempts > 16) ? 16 : g_mqttReconnectAttempts;
    unsigned long delay = (unsigned long)MQTT_CONNECT_RETRY_BASE_MS << k;
    if (delay > (unsigned long)MQTT_CONNECT_RETRY_MAX_MS) {
        delay = (unsigned long)MQTT_CONNECT_RETRY_MAX_MS;
    }
    return delay;
}

// Return true is reconnection is successfull
bool mqttReconnectAttempt() {
    // Pour voir s'il y a assez de bloc memoire pour la connection TLS.
    // mbedtls handshake = ~38-40 KB contigus (16 KB IN + 16 KB OUT + ~6 KB SSL ctx).
    // Heap dispo (largest block) observé :
    //   - Bluedroid : ~24 KB → insuffisant, rc=-2
    //   - NimBLE    : ~60-70 KB attendus → handshake OK
    const uint32_t largestBlock = ESP.getMaxAllocHeap();
    ESP_LOGI(TAG_MQTT, "Attempting connection... required=%u largest_free_block=%u heap_free=%u", MQTT_TLS_MIN_FREE_HEAP_B, largestBlock, ESP.getFreeHeap());

    // Pre-flight: skip the connect entirely when the largest contiguous heap block is too small to host the mbedtls TLS buffers + context. Going
    // through the connect() call anyway burns ~40 KB and ends up with rc=-2 — better to surface the condition immediately so the user sees something
    // changed, and so the retry loop doesn't keep producing identical opaque failures every interval. Failure counter still bumps so the next
    // attempt waits with the exponential backoff.
    if (largestBlock < (uint32_t)MQTT_TLS_MIN_FREE_HEAP_B) {
        ESP_LOGE(TAG_MQTT, "Insufficient heap for TLS handshake: largest block=%u < threshold=%u — skipping connect", largestBlock, MQTT_TLS_MIN_FREE_HEAP_B);
        if (g_inConversationMode) {
            char banner[64];
            snprintf(banner, sizeof(banner), "Low heap %u B - MQTT skipped", (unsigned)largestBlock);
            printInfoLine(banner, CONVO_ERROR_COLOR);
        }
        refreshInfoScreenIfShown();
        return false;
    }


    // Will message = decimal string of deviceId (e.g. "4"). Built on the stack right before connect() — PubSubClient copies willMessage into its own
    // CONNECT packet buffer before returning, so the lifetime of willMsg only needs to span this call. Stack buffer over heap (no String alloc, no
    // helper static): zero heap fragmentation on the MQTT reconnect path, which matters on a device that already lives close to the mbedtls TLS
    // handshake heap budget. Worst case "999\0" (4 bytes) — see audit EDGE-001 for the exact-fit caveat.
    char willMsg[4];
    snprintf(willMsg, sizeof(willMsg), "%d", g_deviceData.deviceId);

    unsigned long t0              = millis();
    bool          isMQTTConnected = g_mqttClient.connect(g_deviceData.name(),
                                                g_mqttServerInfo.user,
                                                g_mqttServerInfo.password,
                                                g_mqttOutgoingTopicWill,
                                                MQTT_QOS_0,
                                                MQTT_MSG_NOT_RETAINED,
                                                willMsg,
                                                MQTT_SESSION_VOLATILE);
    ESP_LOGI(TAG_MQTT, "connect() returned %d after %lums, rc=%d", isMQTTConnected ? 1 : 0, millis() - t0, g_mqttClient.state());

    if (isMQTTConnected) {
        ESP_LOGI(TAG_MQTT, "isMQTTConnected, MQTT_MAX_PACKET_SIZE=%d", MQTT_MAX_PACKET_SIZE);

        g_mqttClient.subscribe(g_mqttIncomingTopicBroadcast, MQTT_QOS_1);

        String myUnicastTopic = String("msg/unicast/") + g_deviceData.deviceId;
        g_mqttClient.subscribe(myUnicastTopic.c_str(), MQTT_QOS_1);
        g_mqttClient.subscribe(g_mqttOutgoingTopicLive, MQTT_QOS_0);
        g_mqttClient.subscribe(g_mqttOutgoingTopicWill, MQTT_QOS_0);

        g_mqttWasConnected      = true;
        g_mqttReconnectAttempts = 0;  // success — reset the backoff so a future outage starts at BASE_MS again instead of inheriting the previous wait.
        g_mqttConnectionId++;
        ledSetState(LED_STATUS, LED_STATE_ON);

        // Send public liveness
        mqttSendAlive((g_mqttConnectionId == 0 ? 0 : 1));

        // Push a refresh to the info screen if it's currently shown — the MQTT row flips from "NOT OK" to "OK" on this connect.
        refreshInfoScreenIfShown();

        return true;
    } else {
        // rc=-4 : MQTT_CONNECTION_REFUSED_BAD_USERNAME_OR_PASSWORD (or not using WiFiClientSecure)
        // rc=-2 : MQTT_CONNECTION_REFUSED_SERVER_UNAVAILABLE
        if (g_mqttReconnectAttempts < UINT8_MAX) {
            g_mqttReconnectAttempts++;
        }
        ESP_LOGE(TAG_MQTT,
                 "Connect failed (rc=%d), attempt #%u, retrying in %lums",
                 g_mqttClient.state(),
                 (unsigned)g_mqttReconnectAttempts,
                 mqttReconnectDelayMs());
        return false;
    }
}

// 0: boot, 1:reco, 2:keepalive
void mqttSendAlive(int liveType) {
    char payload[MSG_BUFFER_SIZE];
    snprintf(payload,
             MSG_BUFFER_SIZE,
             "%d %s mac:%s ssid:%s ip:%s recoId:%d",
             g_deviceData.deviceId,
             (liveType == 0 ? "boot" : (liveType == 1 ? "reco" : "keep")),
             WiFi.macAddress().c_str(),
             WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(),
             g_mqttConnectionId);
    mqttPushFormattedMessage(g_mqttOutgoingTopicLive, payload);
}


// Returns true if the publish was accepted by PubSubClient (sent on the wire — no broker ACK at QoS 0 so this is best-effort). Callers that need
// to react to the failure (e.g. routeMessage tagging the local message with "[ERROR] ") should check the return value; keepalive callers can
// safely ignore it — the next interval will retry.
bool mqttPushFormattedMessage(const char* topic, const char* payload) {
    snprintf(g_mqttOutgoingMsg, MSG_BUFFER_SIZE, "%s ### ts:%s deviceId:%d msgId:%d", payload, getCurrentDateTime(), g_deviceData.deviceId, g_mqttOutputMsgId);

    // Publishing. Only QoS 0 is possible at publish time with PubSubClient
    bool ok = g_mqttClient.publish(topic, g_mqttOutgoingMsg, MQTT_MSG_RETAINED);
    if (ok) {
        ESP_LOGI(TAG_MQTT, "Published #%u to [%s]: [%s]", g_mqttOutputMsgId, topic, g_mqttOutgoingMsg);
    } else {
        // On failure, include state() and the payload size so we can tell apart the four PubSubClient failure modes (see CLAUDE.md / discussions):
        //   state =  0 (MQTT_CONNECTED)         → write to the socket failed mid-send (TCP buffer full, TLS error, link dropped between connected()
        //                                          check and write). If size > ~240 bytes, may also be MQTT_MAX_PACKET_SIZE = 256 rejecting it.
        //   state = -1 (MQTT_DISCONNECTED)      → we called disconnect() ourselves.
        //   state = -3 (MQTT_CONNECTION_LOST)   → broker / WiFi dropped; connected() detected it on this call.
        //   state = -4 (MQTT_CONNECTION_TIMEOUT)→ TCP-level timeout. Mostly during a fresh connect, not on publish.
        ESP_LOGE(TAG_MQTT,
                 "Publish FAILED for #%u to [%s] state=%d size=%u : [%s]",
                 g_mqttOutputMsgId,
                 topic,
                 g_mqttClient.state(),
                 (unsigned)strlen(g_mqttOutgoingMsg),
                 g_mqttOutgoingMsg);
    }

    g_mqttOutputMsgId++;
    return ok;
}


// Parses the leading positive integer in `str` (admin/dead is just the deviceId, admin/live is "<deviceId> boot mac:..." — both share the leading-int
// shape) and returns true iff it is a valid deviceId in [1..254]. 0 and 255 are reserved (DEVICE_ID_UNSET). Anything malformed (empty, non-numeric,
// out of range, or followed by an unexpected non-separator character) is rejected so a peer cannot spoof "device 0" via a crafted payload.
static bool parseLeadingDeviceId(const char* str, int& outDeviceId) {
    if (str == nullptr) {
        return false;
    }
    char* endptr = nullptr;
    long  val    = strtol(str, &endptr, 10);
    if (endptr == str) {
        return false;  // no digits parsed at the start
    }
    if (val < 1 || val > 254) {
        return false;
    }
    // Accept either end-of-string (admin/dead payload) or a whitespace separator before the rest of the alive payload.
    const char trailing = *endptr;
    if (trailing != '\0' && trailing != ' ' && trailing != '\t' && trailing != '\r' && trailing != '\n') {
        return false;
    }
    outDeviceId = (int)val;
    return true;
}


// PubSubClient subscribe callback — dispatches by topic. admin/live + admin/dead drive the friend-presence LEDs; msg/* topics route through the
// shared routeMessage() funnel which handles screen wake / /cmd interception / display.
void onMqttIncomingMessage(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    message.trim();

    ESP_LOGD(TAG_MQTT, "Incoming message [%s] -> [%s]", topic, message.c_str());

    if (strcmp(topic, g_mqttOutgoingTopicLive) == 0) {
        int remoteDeviceId;
        if (!parseLeadingDeviceId(message.c_str(), remoteDeviceId)) {
            ESP_LOGW(TAG_MQTT, "Ignoring malformed admin/live payload: [%s]", message.c_str());
            return;
        }
        onReceivedContactOnline(remoteDeviceId, true);
    } else if (strcmp(topic, g_mqttOutgoingTopicWill) == 0) {
        int remoteDeviceId;
        if (!parseLeadingDeviceId(message.c_str(), remoteDeviceId)) {
            ESP_LOGW(TAG_MQTT, "Ignoring malformed admin/dead payload: [%s]", message.c_str());
            return;
        }
        onReceivedContactOnline(remoteDeviceId, false);
    }
    // msg/unicast/<me> or msg/broadcast
    else if (topic[0] == 'm') {
        // Route through the common funnel: wakes the screen, intercepts CMD_*
        // commands, otherwise renders LEFT.
        routeMessage(message, MessageSource::REMOTE);

    } else {
        ESP_LOGW(TAG_MQTT, "Message received in unknown topic [%s]: [%s]", topic, message.c_str());
    }
}


// TLS root CA has moved out of this file — it now lives in personal-data.h as the HIVEMQ_ROOT_CA static array, referenced by g_mqttServerInfo.rootCA.
// setup() in minimessenger.ino reads it via that struct and wires it into g_wifiClient.setCACert() (or falls back to setInsecure() if rootCA is null).
