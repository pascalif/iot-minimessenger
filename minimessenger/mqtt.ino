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

#include "contacts.h"  // ContactLiveness enum for the onReceivedContactOnline() signature.
#include "mm_log.h"
#include "mqtt.h"
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
extern void  routeMessage(const String& message, MessageSource source, byte senderDeviceId);
extern void  onReceivedContactOnline(int remoteDeviceId, ContactLiveness liveness);
extern char* getCurrentDateTime();

// ================================================================================
// Topic strings
// ================================================================================
const char* g_mqttIncomingTopicBroadcast = "msg/broadcast";
//                                         "msg/unicast/12"
// admin/liveness/<id> — built dynamically in mqttSendLiveness() / mqttReconnectAttempt() via MQTT_LIVENESS_TOPIC_PREFIX (declared in mqtt.h). Not a
// single global because the topic varies per publisher; the dispatch / subscribe paths use MQTT_LIVENESS_TOPIC_PREFIX / MQTT_LIVENESS_TOPIC_WILDCARD
// respectively. The Will targets this same per-device topic with payload MQTTLiveness::DEAD, retained=true — see mqttReconnectAttempt() below.

// ================================================================================
// Runtime globals
// ================================================================================
PubSubClient g_mqttClient(g_wifiClient);  // a WiFiClientSecure instance is
                                          // needed for HiveMQ connection
int           g_mqttConnectionId                 = -1;
unsigned int  g_mqttOutputMsgNextId              = 0;
bool          g_mqttWasConnected                 = false;
unsigned long g_mqttLastReconnectTryTimestampMs  = 0;
unsigned long g_mqttPreviousKeepAliveTimestampMs = 0;
uint8_t       g_mqttReconnectAttempts            = 0;

// Pas partagé, mais permet d'allouer une fois pour toute dans la mémoire .bss
char g_mqttOutgoingMsg[MSG_BUFFER_SIZE];

char g_mqttOutgoingRecipientTopic[MQTT_TOPIC_SIZE];

// ================================================================================
// Liveness subtype wire mapping
// ================================================================================

const char* mqttLivenessAsString(MQTTLiveness subtype) {
    switch (subtype) {
    case MQTTLiveness::BOOT:
        return "BOOT";
    case MQTTLiveness::RECO:
        return "RECO";
    case MQTTLiveness::LIVE:
        return "LIVE";
    case MQTTLiveness::DEAD:
        return "DEAD";
    }
    return "????";  // unreachable in practice — all enum values handled above.
                    // Placeholder kept so the compiler doesn't warn about missing
                    // return.
}

bool parseMQTTLiveness(const char* payload, MQTTLiveness& outSubtype) {
    if (payload == nullptr || strlen(payload) < 4) {
        return false;
    }
    // Require a delimiter (or end-of-string) right after the 4-char word so "DEADBEEF" doesn't match DEAD.
    const char tail = payload[4];
    if (tail != '\0' && tail != ' ') {
        return false;
    }
    if (memcmp(payload, "BOOT", 4) == 0) {
        outSubtype = MQTTLiveness::BOOT;
        return true;
    }
    if (memcmp(payload, "RECO", 4) == 0) {
        outSubtype = MQTTLiveness::RECO;
        return true;
    }
    if (memcmp(payload, "LIVE", 4) == 0) {
        outSubtype = MQTTLiveness::LIVE;
        return true;
    }
    if (memcmp(payload, "DEAD", 4) == 0) {
        outSubtype = MQTTLiveness::DEAD;
        return true;
    }
    return false;
}

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
        ESP_LOGW(TAG_MQTT,
                 "No root CA configured (g_mqttServerInfo.rootCA == nullptr) — "
                 "falling back to setInsecure() (no server-cert verification)");
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
        ESP_LOGE(TAG_MQTT,
                 "Insufficient heap for TLS handshake: largest block=%u < "
                 "threshold=%u — skipping connect",
                 largestBlock,
                 MQTT_TLS_MIN_FREE_HEAP_B);
        if (g_inConversationMode) {
            char banner[64];
            snprintf(banner, sizeof(banner), "Low heap %u B - MQTT skipped", (unsigned)largestBlock);
            printCmdError(banner);
        }
        refreshInfoScreenIfShown();
        return false;
    }

    // Pre-flight: refuse to connect until NTP has synced. Two reasons coupled here:
    //   1. The admin/liveness/<id> payload embeds an epoch — connecting before SNTP completes would publish "BOOT 22" (boot-relative seconds returned
    //      by time() prior to first NTP response) and pollute the broker's retained store with a meaningless timestamp that the staleness check
    //      on every peer would then have to filter out.
    //   2. TLS cert verification is currently bypassed by setInsecure() (g_mqttServerInfo.rootCA == nullptr), which is why the connect can succeed
    //      with a 1970 clock today. If setCACert() is re-enabled later, the same gate covers the "device too old to validate the cert" failure path.
    // Same threshold (1.7e9 ≈ Nov 2023) as setupNTP() uses to decide that the SNTP burst has succeeded.
    const time_t nowEpoch = time(nullptr);
    if (nowEpoch < 1'700'000'000L) {
        ESP_LOGI(TAG_MQTT,
                 "Postponing connect: NTP not synced yet (time=%ld). Retrying on "
                 "the next reconnect gate.",
                 (long)nowEpoch);
        return false;
    }

    // Will = "DEAD" retained on admin/liveness/<myId>. When the broker detects our disconnect, it publishes this Will payload AND retains it,
    // overwriting whatever LIVE/BOOT/RECO retained we had pushed earlier. Any future subscriber to admin/liveness/+ then sees us as DEAD immediately.
    // The retained DEAD is self-cleaned on our next successful reconnect (mqttSendLiveness() overwrites with BOOT/RECO). willTopic must live for the
    // span of connect() only — PubSubClient copies both willTopic and willMessage into its CONNECT packet buffer before returning, so the stack
    // buffer below is sufficient. Worst case "admin/liveness/999\0" = 19 bytes — well within MQTT_TOPIC_SIZE.
    char willTopic[MQTT_TOPIC_SIZE];
    snprintf(willTopic, MQTT_TOPIC_SIZE, MQTT_LIVENESS_TOPIC_PREFIX "%d", g_deviceData.deviceId);

    unsigned long t0              = millis();
    bool          isMQTTConnected = g_mqttClient.connect(g_deviceData.name(),
                                                g_mqttServerInfo.user,
                                                g_mqttServerInfo.password,
                                                willTopic,
                                                MQTT_QOS_0,
                                                MQTT_MSG_RETAINED,
                                                mqttLivenessAsString(MQTTLiveness::DEAD),
                                                MQTT_SESSION_VOLATILE);
    ESP_LOGI(TAG_MQTT, "connect() returned %d after %lums, rc=%d", isMQTTConnected ? 1 : 0, millis() - t0, g_mqttClient.state());

    if (isMQTTConnected) {
        ESP_LOGI(TAG_MQTT, "isMQTTConnected, MQTT_MAX_PACKET_SIZE=%d", MQTT_MAX_PACKET_SIZE);

        // Subscribes
        // - a/ msg/broadcast
        g_mqttClient.subscribe(g_mqttIncomingTopicBroadcast, MQTT_QOS_1);

        // - b/ msg/unicast/<me>
        String myUnicastTopic = String("msg/unicast/") + g_deviceData.deviceId;
        g_mqttClient.subscribe(myUnicastTopic.c_str(), MQTT_QOS_1);

        // -c/ admin/liveness/+
        // wildcard subscription so a fresh boot receives the retained liveness state of every peer in a single shot. Per-device
        // topics avoid the retained-collision that a single shared admin/liveness would have (broker keeps one retained per topic, not one per
        // publisher). Carries BOOT/RECO/LIVE/DEAD; the Will lives on this same topic family, so a single subscribe covers both "alive" and "dead" events
        g_mqttClient.subscribe(MQTT_LIVENESS_TOPIC_WILDCARD, MQTT_QOS_0);

        g_mqttWasConnected      = true;
        g_mqttReconnectAttempts = 0;  // success — reset the backoff so a future outage starts at BASE_MS
                                      // again instead of inheriting the previous wait.
        g_mqttConnectionId++;
        ledSetState(LED_STATUS, LED_STATE_ON);

        // Send public liveness. First successful connect since boot → BOOT; any subsequent reconnect → RECO. The keepalive ticks fired from the loop()
        // gate use LIVE — see minimessenger.ino.
        mqttSendLiveness(g_mqttConnectionId == 0 ? MQTTLiveness::BOOT : MQTTLiveness::RECO);

        // Push a refresh to the info screen if it's currently shown — the MQTT row flips from "NOT OK" to "OK" on this connect.
        refreshInfoScreenIfShown();

        return true;
    } else {
        // Codes utiles renvoyés par g_mqttClient.state() après un connect() raté
        // (source : PubSubClient.h, #define MQTT_*) :
        //   rc=-4  MQTT_CONNECTION_TIMEOUT    — pas de réponse du broker dans les
        //   MQTT_SOCKET_TIMEOUT (15 s par défaut).
        //                                       En pratique sur ce projet :
        //                                       handshake mbedtls qui patine (heap
        //                                       fragmenté → cf. pré-check
        //                                       MQTT_TLS_MIN_FREE_HEAP_B
        //                                       au-dessus), ou tentative de
        //                                       connexion sur 8883 sans TLS.
        //   rc=-3  MQTT_CONNECTION_LOST       — TCP RST / WiFi tombé pendant ou
        //   juste après le handshake. rc=-2  MQTT_CONNECT_FAILED        — pas de
        //   TCP du tout : DNS qui échoue, port fermé, broker URL erronée dans
        //   personal-data.h. rc=-1  MQTT_DISCONNECTED          — on a appelé
        //   disconnect() nous-mêmes (cf. /mqtt drop). rc=4
        //   MQTT_CONNECT_BAD_CREDENTIALS — CONNACK du broker : user/password
        //   incorrect dans g_mqttServerInfo (personal-data.h). rc=5
        //   MQTT_CONNECT_UNAUTHORIZED   — CONNACK : creds OK mais ACL refuse cette
        //   opération (ex. topic non autorisé pour ce user).
        // Note : ne pas confondre rc=-4 (timeout transport) et rc=4 (broker
        // refusant les creds) — le signe compte.
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

// Publish to admin/liveness/<myId> in retained mode. Payload is the minimal "<TYPE> <epochSeconds>" form (no trailer, no mac/ssid/ip): the topic
// suffix carries the deviceId, the subtype carries the transition flavour, and the epoch lets the receiver detect a stale retained leftover.
// DEAD is never sent from here — it rides the Will mechanism set up in mqttReconnectAttempt() and only fires server-side when the broker detects
// our disconnect. Passing DEAD here would be a programming error and is logged + dropped rather than published.
void mqttSendLiveness(MQTTLiveness subtype) {
    if (subtype == MQTTLiveness::DEAD) {
        ESP_LOGW(TAG_MQTT,
                 "mqttSendLiveness: DEAD is reserved for the Will, not a "
                 "self-publish — skipping");
        return;
    }

    char topic[MQTT_TOPIC_SIZE];
    snprintf(topic, MQTT_TOPIC_SIZE, MQTT_LIVENESS_TOPIC_PREFIX "%d", g_deviceData.deviceId);

    // "TYPE epochSeconds" — max length "RECO 9999999999\0" = 16 bytes. 32 is comfortable headroom.
    char payload[32];
    snprintf(payload, sizeof(payload), "%s %ld", mqttLivenessAsString(subtype), (long)time(nullptr));

    bool ok = g_mqttClient.publish(topic, payload, MQTT_MSG_RETAINED);
    if (ok) {
        ESP_LOGI(TAG_MQTT, "Liveness published [%s] -> [%s]", topic, payload);
    } else {
        ESP_LOGE(TAG_MQTT, "Liveness publish FAILED for [%s] (state=%d size=%u) : [%s]", topic, g_mqttClient.state(), (unsigned)strlen(payload), payload);
    }
}

// Returns true if the publish was accepted by PubSubClient (sent on the wire — no broker ACK at QoS 0 so this is best-effort). Callers that need
// to react to the failure (e.g. routeMessage tagging the local message with "[ERROR] ") should check the return value; keepalive callers can
// safely ignore it — the next interval will retry. The `retained` flag is forwarded to the broker as-is: pass MQTT_MSG_RETAINED only for state
// topics, MQTT_MSG_NOT_RETAINED for events (chat messages). See ../docs/howto_mqtt.md. Note: liveness publishes go through mqttSendLiveness() with a
// dedicated minimal payload (no trailer), they don't call this function.
bool mqttPushFormattedMessage(const char* topic, const char* payload) {
    // The sentinel between user payload and trailer is the shared MQTT_TRAILER_SENTINEL constant from mqtt.h — same string is matched by
    // extractSenderAndStripTrailer() on the receiver side. The C string concatenation glues it into the snprintf format literal at compile time.
    snprintf(g_mqttOutgoingMsg,
             MSG_BUFFER_SIZE,
             "%s" MQTT_TRAILER_SENTINEL "ts:%s deviceId:%d msgId:%d",
             payload,
             getCurrentDateTime(),
             g_deviceData.deviceId,
             g_mqttOutputMsgNextId);

    // Publishing. Only QoS 0 is possible at publish time with PubSubClient
    bool ok = g_mqttClient.publish(topic, g_mqttOutgoingMsg, MQTT_MSG_NOT_RETAINED);
    if (ok) {
        ESP_LOGI(TAG_MQTT, "Published #%u to [%s]: [%s]", g_mqttOutputMsgNextId, topic, g_mqttOutgoingMsg);
    } else {
        // On failure, include state() and the payload size so we can tell apart the four PubSubClient failure modes (see CLAUDE.md / discussions):
        //   state =  0 (MQTT_CONNECTED)         → write to the socket failed mid-send (TCP buffer full, TLS error, link dropped between connected()
        //                                          check and write). If size > ~240 bytes, may also be MQTT_MAX_PACKET_SIZE = 256 rejecting it.
        //   state = -1 (MQTT_DISCONNECTED)      → we called disconnect() ourselves.
        //   state = -3 (MQTT_CONNECTION_LOST)   → broker / WiFi dropped; connected() detected it on this call.
        //   state = -4 (MQTT_CONNECTION_TIMEOUT)→ TCP-level timeout. Mostly during a fresh connect, not on publish.
        ESP_LOGE(TAG_MQTT,
                 "Publish FAILED for #%u to [%s] state=%d size=%u : [%s]",
                 g_mqttOutputMsgNextId,
                 topic,
                 g_mqttClient.state(),
                 (unsigned)strlen(g_mqttOutgoingMsg),
                 g_mqttOutgoingMsg);
    }

    g_mqttOutputMsgNextId++;
    return ok;
}

// Parses the leading positive integer in `str` and returns true iff it is a valid deviceId in [1..254]. 0 and 255 are reserved (DEVICE_ID_UNSET).
// Used to extract the id from the admin/liveness/<id> topic suffix (after the prefix). Anything malformed (empty, non-numeric, out of range, or
// followed by an unexpected non-separator character) is rejected so a peer cannot spoof "device 0" via a crafted topic.
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
    // Accept either end-of-string (topic suffix "admin/liveness/<id>" parses to <id> + '\0') or a whitespace separator.
    const char trailing = *endptr;
    if (trailing != '\0' && trailing != ' ' && trailing != '\t' && trailing != '\r' && trailing != '\n') {
        return false;
    }
    outDeviceId = (int)val;
    return true;
}

// PubSubClient subscribe callback. Two families of topics:
//   - admin/liveness/<id>     drives the friend-presence LEDs / contact silhouettes (BOOT/RECO/LIVE = alive, DEAD = offline).
//   - msg/broadcast | msg/unicast/<me>     chat traffic, funneled through routeMessage() for wake-on-input + /cmd interception + display.
//
// Liveness payload contract (cf. ../docs/howto_mqtt.md):
//   "DEAD"                                  → device is offline, no timestamp.
//   "<BOOT|RECO|LIVE> <epochSeconds>"       → device is alive at the given wall-clock epoch.
//
// Self-filter is critical because the broker replays our own retained admin/liveness/<myId> back at us when we subscribe to the wildcard.
//
// Staleness check covers the "peer crashed without firing its Will" edge: its retained LIVE sits at the broker forever, with a frozen timestamp.
// On subscribe we receive it, compare the embedded epoch against our local time(nullptr), and ignore it if older than the keepalive window + 5 s of
// margin. The check is gated by "our own clock is plausibly synced" (time(nullptr) > 1.7e9 — same threshold setupNTP uses) — early-boot devices
// without NTP yet can't compare, so they fall back to trusting the LIVE rather than locking out every peer.

// Trailer parser for chat payloads on msg/broadcast and msg/unicast/<id>. mqttPushFormattedMessage() appends "<userMsg> ### ts:<…> deviceId:<n>
// msgId:<n>" to every outgoing chat — we use that trailer here for self-echo filtering AND for author identification.
//
// Stripping policy — three cases:
//   1. Sentinel " ### " absent                     → message intact, return 0 (anonymous, e.g. web console / mosquitto_pub publishing raw text).
//   2. Sentinel present WITHOUT "deviceId:" field  → probably user-typed content that happened to contain " ### ". Don't touch the message, return 0.
//   3. Sentinel present AND "deviceId:" field      → it IS our format. ALWAYS strip the trailer, regardless of whether the deviceId field parses
//                                                    as a number in [1..254]. Otherwise a payload like "hello ### deviceId:Z" would display its
//                                                    whole mangled trailer on screen. Return the valid id, or 0 if non-parseable / out-of-range.
//
// Return values:
//   0      = no author identified (cases 1, 2, or 3-with-bad-id). Caller displays whatever remains with the "ext" prefix.
//   1..254 = valid sender deviceId. Caller looks up pseudo and (unless == us) prefixes it to the ts.
static byte extractSenderAndStripTrailer(String& message) {
    const int sentinelIdx = message.indexOf(MQTT_TRAILER_SENTINEL);
    if (sentinelIdx < 0) {
        return 0;
    }
    const int devIdx = message.indexOf("deviceId:", sentinelIdx);
    if (devIdx < 0) {
        return 0;  // sentinelle isolée — probablement du texte utilisateur, on
                   // n'altère pas le payload
    }

    // À partir d'ici on est confiants que c'est notre format de trailer : on strip TOUJOURS, même si le deviceId est non-parseable / hors plage.
    byte       senderId = 0;
    const long parsed   = strtol(message.c_str() + devIdx + 9 /* strlen("deviceId:") */, nullptr, 10);
    if (parsed >= 1 && parsed <= 254) {
        senderId = (byte)parsed;
    }
    message = message.substring(0, sentinelIdx);
    return senderId;
}

void onMqttIncomingMessage(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    message.trim();

    ESP_LOGD(TAG_MQTT, "Incoming message [%s] -> [%s]", topic, message.c_str());

    // admin/liveness/<id> — id from the topic suffix, payload from "TYPE [epoch]".
    if (strncmp(topic, MQTT_LIVENESS_TOPIC_PREFIX, MQTT_LIVENESS_TOPIC_PREFIX_LEN) == 0) {
        int remoteDeviceId;
        if (!parseLeadingDeviceId(topic + MQTT_LIVENESS_TOPIC_PREFIX_LEN, remoteDeviceId)) {
            ESP_LOGW(TAG_MQTT, "Ignoring malformed admin/liveness topic: [%s]", topic);
            return;
        }
        // Self-filter: the broker mirrors our own retained back when we subscribe to the wildcard.
        if (remoteDeviceId == g_deviceData.deviceId) {
            ESP_LOGD(TAG_MQTT, "Ignoring own liveness echo on [%s]", topic);
            return;
        }

        const char* msgC = message.c_str();

        // Parse the leading TYPE word. Rejects unknown / truncated payloads (e.g. somebody manually publishing "hello" on the topic).
        MQTTLiveness subtype;
        if (!parseMQTTLiveness(msgC, subtype)) {
            ESP_LOGW(TAG_MQTT, "Liveness payload has unknown leading TYPE for device %d: [%s]", remoteDeviceId, msgC);
            return;
        }

        // DEAD short-circuit — no timestamp to parse, mark offline immediately.
        if (subtype == MQTTLiveness::DEAD) {
            onReceivedContactOnline(remoteDeviceId, ContactLiveness::DEAD);
            return;
        }

        // BOOT/RECO/LIVE: payload must be "<TYPE> <epoch>". The TYPE is exactly 4 chars (validated above), so the epoch field starts at offset 5.
        const char* epochField   = msgC + 5;
        char*       endptr       = nullptr;
        long        payloadEpoch = strtol(epochField, &endptr, 10);
        if (endptr == epochField || payloadEpoch <= 0) {
            ESP_LOGW(TAG_MQTT, "Liveness epoch not numeric for device %d: [%s]", remoteDeviceId, msgC);
            return;
        }

        // Staleness check. Threshold = keepalive interval + 5 s margin, same window as contactsTick uses. Only enforced when our local clock looks
        // synced — otherwise we don't have a baseline to compare against.
        time_t nowEpoch = time(nullptr);
        if (nowEpoch > 1'700'000'000L) {
            long age = (long)nowEpoch - payloadEpoch;
            if (age > (long)(MQTT_KEEPALIVE_INTERVAL_MS / 1000) + 5) {
                ESP_LOGD(TAG_MQTT,
                         "Ignoring stale liveness retain for device %d (age %lds, "
                         "payload=[%s])",
                         remoteDeviceId,
                         age,
                         msgC);
                return;
            }
        }

        onReceivedContactOnline(remoteDeviceId, ContactLiveness::LIVE);
    }
    // msg/unicast/<me> or msg/broadcast
    else if (topic[0] == 'm') {
        // Trailer "… ### ts:<…> deviceId:<n> msgId:<n>" is added by mqttPushFormattedMessage() for every chat message published by our own code.
        // We use it for two things:
        //   1. Self-filter: when WE publish on msg/broadcast, the broker echoes our payload back to us through the same subscription. The trailer
        //      identifies the echo and we drop it silently (no double-display).
        //   2. Identify the author for the UI pseudo prefix — passed through to routeMessage and ultimately to addConversationBlock.
        // Messages from a web-MQTT console / mosquitto_pub have no trailer; they fall through with senderId=0 and are displayed as "ext".
        byte senderId = extractSenderAndStripTrailer(message);

        if (senderId != 0 && senderId == g_deviceData.deviceId) {
            ESP_LOGD(TAG_MQTT, "Ignoring self-echo on [%s]", topic);
            return;
        }

        // Route through the common funnel: wakes the screen, intercepts CMD_*
        // commands, otherwise renders LEFT.
        routeMessage(message, MessageSource::REMOTE, senderId);

    } else {
        ESP_LOGW(TAG_MQTT, "Message received in unknown topic [%s]: [%s]", topic, message.c_str());
    }
}

// TLS root CA has moved out of this file — it now lives in personal-data.h as the HIVEMQ_ROOT_CA static array, referenced by g_mqttServerInfo.rootCA.
// setup() in minimessenger.ino reads it via that struct and wires it into g_wifiClient.setCACert() (or falls back to setInsecure() if rootCA is null).
