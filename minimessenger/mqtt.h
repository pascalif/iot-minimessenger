#pragma once

// MQTT layer — constants and prototypes shared between mqtt.ino (definitions, MQTT functions, broker plumbing) and minimessenger.ino (which still
// drives the reconnect loop, status bar, info screen and setRecipient/setCallback wiring). The IDE concatenates the .ino files alphabetically after
// the main sketch, so mqtt.ino's definitions land AFTER minimessenger.ino in the single translation unit — this header is what lets minimessenger.ino
// see the MQTT globals/constants without the compiler tripping on "use before declaration". Same pattern as wifi_state.h vs wifi.ino.

// PubSubClient (Library Manager: "PubSubClient" 2.8). Brought in here so anyone including mqtt.h gets the type of g_mqttClient — no separate include
// needed in minimessenger.ino.
#include <PubSubClient.h>

// ================================================================================
// Topic names — shared so that incoming-message dispatch in mqtt.ino and the broker-connect / subscribe code can refer to the same strings.
// ================================================================================
extern const char* g_mqttOutgoingTopicLogs;       // admin/logs    — reserved, no consumer yet.
extern const char* g_mqttOutgoingTopicLive;       // admin/live    — retained "I'm alive" pings (boot / reconnect / 30 s keepalive).
extern const char* g_mqttOutgoingTopicWill;       // admin/dead    — MQTT Last Will published on disconnect.
extern const char* g_mqttIncomingTopicBroadcast;  // msg/broadcast — subscribed by all devices; the "everyone" channel.
//                                                   msg/unicast/<deviceId> — built at runtime in setRecipient().

// ================================================================================
// Timing / protocol constants
// ================================================================================

// Period between sending 2 "keepalive" messages on admin/live.
#define MQTT_KEEPALIVE_INTERVAL_MS 120'000
// Period between retrying connection to MQTT broker after a failure.
#define MQTT_CONNECT_RETRY_INTERVAL_MS 5'000

// Publish flags — readable names for PubSubClient::publish() / connect()'s boolean args.
#define MQTT_MSG_RETAINED     true
#define MQTT_MSG_NOT_RETAINED false

#define MQTT_SESSION_VOLATILE  true
#define MQTT_SESSION_PERSISTED false

#define MQTT_QOS_0 0
#define MQTT_QOS_1 1
#define MQTT_QOS_2 2


// ================================================================================
// Buffers
// ================================================================================

// Size includes all standard fields plus user's payload.
#define MSG_BUFFER_SIZE 500
// Max length of "msg/unicast/<id>" — id is a single byte so ≤ 3 digits, plus "msg/unicast/" prefix.
#define MQTT_TOPIC_SIZE 30

// ================================================================================
// Runtime globals defined in mqtt.ino
// ================================================================================
extern const char*   g_hiveMQRootCA;                        // HiveMQ Cloud root CA (ISRG Root X1). Wired into g_wifiClient.setCACert() in setup().
extern PubSubClient  g_mqttClient;                          // PubSubClient bound to the WiFiClientSecure declared in minimessenger.ino.
extern int           g_mqttConnectionId;                    // monotonically incremented on each successful (re)connect. -1 before the first.
extern unsigned int  g_mqttOutputMsgId;                     // monotonic id appended to every outgoing payload (### msgId:<n>).
extern bool          g_mqttWasConnected;                    // edge-detection latch — true while connected, falls to false on the first loop iteration that sees the link down.
extern unsigned long g_mqttLastReconnectTryTimestampMs;     // millis() of the last mqttReconnect() attempt — used to throttle retries.
extern unsigned long g_mqttPreviousKeepAliveTimestampMs;    // millis() of the last admin/live keepalive publish.
extern char          g_mqttOutgoingMsg[MSG_BUFFER_SIZE];    // scratch buffer used by mqttPushFormattedMessage() to assemble the payload + trailer.
extern char          g_mqttOutoingRecipientTopic[MQTT_TOPIC_SIZE];  // current unicast recipient topic — written by setRecipient(), read by routeMessage().


// ================================================================================
// Functions defined in mqtt.ino, callable from minimessenger.ino
// ================================================================================
bool mqttReconnect();                                                         // Attempts a TLS+MQTT (re)connect; returns true on success. Driven by the loop()-level reconnect gate.
void mqttSendAlive(int liveType);                                             // Publish an admin/live keepalive. liveType: 0=boot, 1=reco, 2=keep.
bool mqttPushFormattedMessage(const char* topic, const char* payload);        // Append the "### ts:… deviceId:… msgId:…" trailer and publish. Returns the PubSubClient publish() result.
void onMqttIncomingMessage(char* topic, byte* payload, unsigned int length);  // PubSubClient subscribe callback — dispatches by topic prefix.
