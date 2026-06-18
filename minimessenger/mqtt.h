#pragma once

/*
Console HiveMQ :
- https://console.hivemq.cloud/clusters
- l'identifiant du cluster est dans personal-data.h (g_mqttServerInfo.server).

Tester manuellement depuis le web-client du broker :

  - Notifier qu'un device 1 est LIVE :    topic admin/liveness/1   payload "LIVE
1717459200"   (retained=true — voir ../docs/howto_mqtt.md)
  - Notifier qu'un device 1 est mort :    topic admin/liveness/1   payload
"DEAD"              (retained=true — tombstone d'état)
  - Envoyer un message à tous :           topic msg/broadcast      payload
"hello"             (retained=false : évènement de chat)
*/

// MQTT layer — constants and prototypes shared between mqtt.ino (definitions, MQTT functions, broker plumbing) and minimessenger.ino (which still
// drives the reconnect loop, status bar, info screen and setRecipient/setCallback wiring). The IDE concatenates the .ino files alphabetically after
// the main sketch, so mqtt.ino's definitions land AFTER minimessenger.ino in the single translation unit — this header is what lets minimessenger.ino
// see the MQTT globals/constants without the compiler tripping on "use before declaration". Same pattern as wifi.h vs wifi.ino.

// PubSubClient (Library Manager: "PubSubClient" 2.8). Brought in here so anyone including mqtt.h gets the type of g_mqttClient — no separate include
// needed in minimessenger.ino.
#include <PubSubClient.h>

// ================================================================================
// Topic names — shared so that incoming-message dispatch in mqtt.ino and the broker-connect / subscribe code can refer to the same strings.
// ================================================================================
extern const char* g_mqttIncomingTopicBroadcast;  // msg/broadcast — subscribed by all devices;
                                                  // the "everyone" channel.
//                                                   msg/unicast/<deviceId> — built at runtime in setRecipient().

// admin/liveness/<deviceId> — retained state topic, ONE per device. Carries every flavour of liveness signal (BOOT / RECO / LIVE / DEAD) so the
// broker's retained store always reflects "what each device believes is its current state". Per-device topic lets a fresh subscriber to
// admin/liveness/+ get one retained per peer in a single shot (a shared topic would only retain the last publisher). The DEAD payload doubles as
// the MQTT Last Will, set in connect() with retained=true so the broker-detected disconnect leaves a tombstone visible to future subscribers. The
// admin/dead topic that used to handle the Will is gone. See ../docs/howto_mqtt.md for the design rationale.
#define MQTT_LIVENESS_TOPIC_PREFIX     "admin/liveness/"
#define MQTT_LIVENESS_TOPIC_PREFIX_LEN (sizeof(MQTT_LIVENESS_TOPIC_PREFIX) - 1)
#define MQTT_LIVENESS_TOPIC_WILDCARD   "admin/liveness/+"  // single-level wildcard — matches admin/liveness/<id> for  any id, not deeper paths.

// Liveness payload subtype — the leading word of an admin/liveness/<id> payload. Wire format: "<TYPE> <epochSeconds>" for BOOT/RECO/LIVE, just
// "DEAD" (no timestamp) for the Will. Epoch seconds rather than a formatted timestamp so the staleness check on the receiver side is a single
// integer subtract against time(nullptr). The four wire strings are exactly 4 chars each, which simplifies parsing.
//
// Strongly-typed enum (rather than 4 separate `#define ... "BOOT"` macros) so callers like mqttSendLiveness() take a typed argument instead of magic
// ints (0=boot, 1=reco, 2=keep), and the dispatcher's parse step returns a typed value the receiver can switch on. The wire mapping is one-way:
// each subtype has exactly one canonical 4-char string, set by mqttLivenessAsString() and recognised by parseMQTTLiveness().
enum class MQTTLiveness : uint8_t {
    BOOT,  // First publish after a fresh boot. Dispatches to ContactLiveness::LIVE on the receiver (peer is up).
    RECO,  // Publish on every successful reconnect after the first. Same dispatch as BOOT.
    LIVE,  // Periodic keepalive every MQTT_KEEPALIVE_INTERVAL_MS while connected. Same dispatch as BOOT.
    DEAD,  // Will payload. Fired by the broker on detected disconnect. No timestamp. Dispatches to ContactLiveness::DEAD.
};

// Wire string for a subtype. Returns a pointer to a static string literal (lifetime = forever), safe to pass directly to PubSubClient::connect()'s
// willMessage argument. Used by mqttSendLiveness() to assemble the payload and by mqttReconnectAttempt() to set the Will body.
const char* mqttLivenessAsString(MQTTLiveness subtype);

// Parse the leading 4-char TYPE word of an admin/liveness/<id> payload. Returns true and fills `outSubtype` on a clean match (the 4 chars match a
// known type AND the next char is either '\0' or ' '), false otherwise. The trailing-char check avoids matching "DEADX" as DEAD.
bool parseMQTTLiveness(const char* payload, MQTTLiveness& outSubtype);

// ================================================================================
// Timing / protocol constants
// ================================================================================

// Period between sending 2 "keepalive" messages on admin/liveness/<id>. Doubles as the upper bound on "how stale can a retained admin/liveness/<id>
// payload be" before the subscriber considers it a leftover (a peer that crashed, didn't fire its Will, and whose retained LIVE was never overwritten
// by DEAD). The dispatcher in mqtt.ino enforces this: payload epoch older than `MQTT_KEEPALIVE_INTERVAL_MS/1000 + 5 s` → ignored.
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

// Sentinel that separates the user payload from the metadata trailer ("ts:… deviceId:… msgId:…") appended by mqttPushFormattedMessage() to every
// outgoing chat message. The string MUST stay in sync between the writer side (the snprintf format in mqtt.ino) and the parser side
// (extractSenderAndStripTrailer in mqtt.ino) — that's why it lives here as a single source of truth. The leading + trailing spaces are part of the
// sentinel; they make accidental collisions with user-typed content less likely and visually break the trailer in serial logs. Defined as a string
// literal so it can be concatenated directly into a snprintf format ("%s" MQTT_TRAILER_SENTINEL "ts:%s …").
#define MQTT_TRAILER_SENTINEL " # "


// ================================================================================
// Buffers
// ================================================================================

// Max length of "msg/unicast/<id>" — id is a single byte so ≤ 3 digits, plus "msg/unicast/" prefix.
#define MQTT_TOPIC_SIZE 30

// ================================================================================
// Per-deployment broker description — defined ONCE in personal-data.h (gitignored)
// ================================================================================
//
// Groups the 4 broker credentials + the TLS root CA that used to be 5 separate top-level globals scattered between minimessenger.ino and mqtt.ino.
// Putting them in personal-data.h keeps the gitignored surface area minimal: a fresh repo clone has no broker baked in, and a contributor only ever
// edits one file to point at their own broker.
//
// The rootCA field is intentionally nullable. When non-null, setup() calls g_wifiClient.setCACert(rootCA) and mbedtls verifies the broker's cert
// chain against it. When null, setup() falls back to g_wifiClient.setInsecure() — TLS still negotiates, but mbedtls accepts whatever cert the broker
// presents. Calling NEITHER would leave WiFiClientSecure in its strict-no-CA default and the handshake would systematically fail with
// MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, which is why the null-branch is wired explicitly to setInsecure() rather than just skipping setCACert().
// The "no verification" mode is for testing against a local broker (Mosquitto on the LAN, dev cluster) that doesn't expose a CA-signed cert.
struct MQTTServerInfo {
    const char* server;
    int         port;
    const char* user;
    const char* password;
    const char* rootCA;
};

extern const MQTTServerInfo g_mqttServerInfo;


// ================================================================================
// Runtime globals defined in mqtt.ino
// ================================================================================
extern PubSubClient g_mqttClient;                                    // PubSubClient bound to the WiFiClientSecure declared in minimessenger.ino.
extern int          g_mqttConnectionId;                              // monotonically incremented on each successful (re)connect. -1 before the first.
extern unsigned int g_mqttOutputMsgNextId;                           // monotonic id appended to every outgoing payload (### msgId:<n>).
extern bool         g_mqttWasConnected;                              // edge-detection latch — true while connected, falls to false on the first loop iteration
                                                                     // that sees the link down.
extern unsigned long g_mqttLastReconnectTryTimestampMs;              // millis() of the last mqttReconnectAttempt() attempt — used to throttle retries.
extern uint8_t       g_mqttReconnectAttempts;                        // failed attempts since the last successful (re)connect; drives mqttReconnectDelayMs().
extern unsigned long g_mqttPreviousKeepAliveTimestampMs;             // millis() of the last admin/liveness/<id>  keepalive publish.
extern char          g_mqttOutgoingRecipientTopic[MQTT_TOPIC_SIZE];  // current unicast recipient topic — written by setRecipient(), read by routeMessage().

// ================================================================================
// Functions defined in mqtt.ino, callable from minimessenger.ino
// ================================================================================
void setupMQTT();
bool mqttReconnectAttempt();                  // Attempts a TLS+MQTT (re)connect; returns true on
                                              // success. Driven by the loop()-level reconnect
                                              // gate.
unsigned long mqttReconnectDelayMs();         // Current reconnect-throttle interval (exponential
                                              // backoff capped at MQTT_CONNECT_RETRY_MAX_MS).
void mqttSendLiveness(MQTTLiveness subtype);  // Publish a retained "<subtype> <epoch>" on
                                              // admin/liveness/<id>. Accepts BOOT/RECO/LIVE (DEAD is
                                              // reserved for the Will, see mqttReconnectAttempt()).
// Append the "### ts:… deviceId:… msgId:…" trailer and publish. Returns the PubSubClient publish() result.
bool mqttPushFormattedMessage(const char* topic, const char* payload);
void onMqttIncomingMessage(char* topic, byte* payload,
                           unsigned int length);  // PubSubClient subscribe callback — dispatches by
                                                  // topic prefix.
