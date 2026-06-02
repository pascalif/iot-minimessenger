// ================================================================================
// mqtt.ino — MQTT broker plumbing: topics, globals, reconnect, publish, incoming dispatch, TLS root CA
// ================================================================================
//
// All MQTT-specific code lives here. The Arduino IDE concatenates this file with minimessenger.ino into a single translation unit (alphabetical order
// after the main sketch), so anything declared in mqtt.h is visible from both files. Same split philosophy as wifi.ino / commands.ino: keep the main
// .ino focused on orchestration (status bar, loop reconnect gate, setRecipient, etc.), push the cross-cutting MQTT plumbing into its own file.
//
// Stays in minimessenger.ino on purpose:
//   - mqtt_server / mqtt_port / mqtt_user / mqtt_password — broker credentials, kept near the rest of the sensitive config (see CLAUDE.md "Secrets").
//   - the loop()-level reconnect gate and the status-bar / info-screen indicators — those are display/orchestration concerns, not MQTT plumbing.
//   - setRecipient(), onMQTTReconnected() — conversation logic and display transitions, not MQTT internals.

#include "mqtt.h"
#include "mm_log.h"
#include "symbols.h"
#include <WiFiClientSecure.h>

// Broker credentials defined in minimessenger.ino — declared here so the connect() call below resolves them. Kept in the main .ino because they
// belong with the "Secrets" block per CLAUDE.md.
extern const char* mqtt_server;
extern const int   mqtt_port;
extern const char* mqtt_user;
extern const char* mqtt_password;

// WiFiClientSecure instance lives in minimessenger.ino (declared before mqtt.ino in the concatenation order, so the constructor below sees it). The
// extern here is purely for clarity / so this file reads standalone.
extern WiFiClientSecure g_wifiClient;

// Identity strings used by mqttReconnect (client id / will message) and mqttSendAlive — declared in minimessenger.ino, see identifyDevice().
extern char g_deviceName[];
extern char g_deviceIdChars[];
extern byte g_deviceIdMe;

// LED + display helpers called from this file (LED on after a successful connect, message routing for incoming chat messages, contact liveness LEDs).
// Forward-declared so the auto-prototype ordering does not bite us.
extern void ledSetState(int pin, int requiredState);
extern void routeMessage(const String& message, MessageSource source);
extern void onReceivedContactOnline(int remoteDeviceId, bool isLive);
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

char g_mqttOutgoingMsg[MSG_BUFFER_SIZE];
char g_mqttOutoingRecipientTopic[MQTT_TOPIC_SIZE];


// ================================================================================
// MQTT — connect / publish / receive
// ================================================================================

// Return true is reconnection is successfull
bool mqttReconnect() {
    // Pour voir s'il y a assez de bloc memoire pour la connection TLS.
    // mbedtls handshake = ~38-40 KB contigus (16 KB IN + 16 KB OUT + ~6 KB SSL ctx).
    // Heap dispo (largest block) observé :
    //   - Bluedroid : ~24 KB → insuffisant, rc=-2
    //   - NimBLE    : ~60-70 KB attendus → handshake OK
    ESP_LOGI(TAG_MQTT, "Attempting connection... heap free=%u largest block=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    unsigned long t0              = millis();
    bool          isMQTTConnected = g_mqttClient.connect(g_deviceName,
                                                mqtt_user,
                                                mqtt_password,
                                                g_mqttOutgoingTopicWill,
                                                MQTT_QOS_0,
                                                MQTT_MSG_NOT_RETAINED,
                                                g_deviceIdChars,
                                                MQTT_SESSION_VOLATILE);
    ESP_LOGI(TAG_MQTT, "connect() returned %d after %lums, rc=%d", isMQTTConnected ? 1 : 0, millis() - t0, g_mqttClient.state());

    if (isMQTTConnected) {
        ESP_LOGI(TAG_MQTT, "isMQTTConnected, MQTT_MAX_PACKET_SIZE=%d", MQTT_MAX_PACKET_SIZE);

        g_mqttClient.subscribe(g_mqttIncomingTopicBroadcast, MQTT_QOS_1);

        String myUnicastTopic = String("msg/unicast/") + g_deviceIdMe;
        g_mqttClient.subscribe(myUnicastTopic.c_str(), MQTT_QOS_1);
        g_mqttClient.subscribe(g_mqttOutgoingTopicLive, MQTT_QOS_0);
        g_mqttClient.subscribe(g_mqttOutgoingTopicWill, MQTT_QOS_0);

        g_mqttWasConnected = true;
        g_mqttConnectionId++;
        ledSetState(LED_STATUS, LED_STATE_ON);

        // Send public liveness
        mqttSendAlive((g_mqttConnectionId == 0 ? 0 : 1));

        return true;
    } else {
        // rc=-4 : MQTT_CONNECTION_REFUSED_BAD_USERNAME_OR_PASSWORD (or not using WiFiClientSecure)
        // rc=-2 : MQTT_CONNECTION_REFUSED_SERVER_UNAVAILABLE
        ESP_LOGE(TAG_MQTT, "Connect failed (rc=%d), retrying in %dms", g_mqttClient.state(), MQTT_CONNECT_RETRY_INTERVAL_MS);
        return false;
    }
}

// 0: boot, 1:reco, 2:keepalive
void mqttSendAlive(int liveType) {
    char payload[MSG_BUFFER_SIZE];
    snprintf(payload,
             MSG_BUFFER_SIZE,
             "%d %s mac:%s ssid:%s ip:%s recoId:%d",
             g_deviceIdMe,
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
    snprintf(g_mqttOutgoingMsg, MSG_BUFFER_SIZE, "%s ### ts:%s deviceId:%d msgId:%d", payload, getCurrentDateTime(), g_deviceIdMe, g_mqttOutputMsgId);

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
        int remoteDeviceId = atoi(message.c_str());
        onReceivedContactOnline(remoteDeviceId, true);
    } else if (strcmp(topic, g_mqttOutgoingTopicWill) == 0) {
        int remoteDeviceId = atoi(message.c_str());
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


// ================================================================================
// TLS root CA — used by g_wifiClient.setCACert() in setup() to validate the HiveMQ broker certificate during the TLS handshake.
// ================================================================================

// Root CA used to validate the HiveMQ Cloud broker certificate. This is the ISRG Root X1 (Let's Encrypt) certificate — HiveMQ Cloud's serverside cert
// chains up to it. Source: https://community.hivemq.com/t/frequently-asked-questions-hivemq-cloud/514
const char* g_hiveMQRootCA = "-----BEGIN CERTIFICATE-----\n"
                             "MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
                             "TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
                             "cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
                             "WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
                             "ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
                             "MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
                             "h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
                             "0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
                             "A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
                             "T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
                             "B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
                             "B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
                             "KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
                             "OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
                             "jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
                             "qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
                             "rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
                             "HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
                             "hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
                             "ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
                             "3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
                             "NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
                             "ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
                             "TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
                             "jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
                             "oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
                             "4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
                             "mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
                             "emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
                             "-----END CERTIFICATE-----\n";
