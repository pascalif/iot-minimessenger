#pragma once

/*
https://console.hivemq.cloud/clusters/8f76c91610f343c2b6795974c58861c7/web-client

Notifier qu'un device est LIVE:
admin/live
1 keep

Notifier qu'un device n'est plus connecté au broker MQTT:
admin/dead
1

Envoyer un message à tous (pas d'identification possible de l'émetteur avec cette version)
msg/broadcast
hello
*/


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

// Reconnect cadence — exponential backoff with cap. First retry after BASE ms, then doubles every failed attempt until it saturates at MAX ms. The
// counter (g_mqttReconnectAttempts) is reset to 0 on a successful reconnect, so a single transient broker outage doesn't permanently slow the
// device down. No random jitter — broker is a single HiveMQ cluster, fleet stays small.
#define MQTT_CONNECT_RETRY_BASE_MS 5'000
#define MQTT_CONNECT_RETRY_MAX_MS  60'000

// Minimum largest contiguous heap block required to attempt the mbedtls TLS handshake. mbedtls allocates roughly 16 KB IN buffer + 16 KB OUT buffer
// + ~6 KB SSL context in one shot; below ~38 KB the handshake silently fails with rc=-2. We pad to 50 KB so a transient allocation spike during
// connect doesn't push us into the failure zone. Compared against ESP.getMaxAllocHeap() in mqttReconnectAttempt() — if the heap is too fragmented we skip
// the connect attempt and surface the condition on screen instead of burning ~40 KB on a doomed handshake.
#define MQTT_TLS_MIN_FREE_HEAP_B 50'000

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
extern const char*   g_hiveMQRootCA;      // HiveMQ Cloud root CA (ISRG Root X1). Wired into g_wifiClient.setCACert() in setup().
extern PubSubClient  g_mqttClient;        // PubSubClient bound to the WiFiClientSecure declared in minimessenger.ino.
extern int           g_mqttConnectionId;  // monotonically incremented on each successful (re)connect. -1 before the first.
extern unsigned int  g_mqttOutputMsgId;   // monotonic id appended to every outgoing payload (### msgId:<n>).
extern bool          g_mqttWasConnected;  // edge-detection latch — true while connected, falls to false on the first loop iteration that sees the link down.
extern unsigned long g_mqttLastReconnectTryTimestampMs;             // millis() of the last mqttReconnectAttempt() attempt — used to throttle retries.
extern uint8_t       g_mqttReconnectAttempts;                       // failed attempts since the last successful (re)connect; drives mqttReconnectDelayMs().
extern unsigned long g_mqttPreviousKeepAliveTimestampMs;            // millis() of the last admin/live keepalive publish.
extern char          g_mqttOutgoingMsg[MSG_BUFFER_SIZE];            // scratch buffer used by mqttPushFormattedMessage() to assemble the payload + trailer.
extern char          g_mqttOutgoingRecipientTopic[MQTT_TOPIC_SIZE];  // current unicast recipient topic — written by setRecipient(), read by routeMessage().


// ================================================================================
// Functions defined in mqtt.ino, callable from minimessenger.ino
// ================================================================================
bool          mqttReconnectAttempt();       // Attempts a TLS+MQTT (re)connect; returns true on success. Driven by the loop()-level reconnect gate.
unsigned long mqttReconnectDelayMs();       // Current reconnect-throttle interval (exponential backoff capped at MQTT_CONNECT_RETRY_MAX_MS).
void          mqttSendAlive(int liveType);  // Publish an admin/live keepalive. liveType: 0=boot, 1=reco, 2=keep.
bool          mqttPushFormattedMessage(const char* topic,
                                       const char* payload);  // Append the "### ts:… deviceId:… msgId:…" trailer and publish. Returns the PubSubClient publish() result.
void          onMqttIncomingMessage(char* topic, byte* payload, unsigned int length);  // PubSubClient subscribe callback — dispatches by topic prefix.
