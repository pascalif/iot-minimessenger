// ================================================================================
// contacts.ino — Dynamic remote-contact tracking (up to MAX_CONTACTS slots) driven by MQTT liveness pings.
// ================================================================================
//
// Auto-discovered: any deviceId seen on `admin/live` claims a slot; `admin/dead` (Last Will) or an applicative timeout releases it. The two
// "friend present" LEDs and the contact silhouettes on the top status bar are derived from the active slot count — LED_FRIEND_1 lights when at
// least one contact is online, LED_FRIEND_2 when at least two are, regardless of which physical contacts they are. The previous static
// g_deviceIdFriend1/2 pair (one fixed friend per LED, declared in identifyDevice()) is gone.
//
// Concatenation order: this file lands after minimessenger.ino, bars.ino and commands.ino, before mqtt.ino. We therefore see all the layout /
// LED constants and globals defined in minimessenger.ino (DEVICE_ID_UNSET, LED_FRIEND_1, LED_STATE_ON, g_deviceData, g_statusBarDirty) and the
// ledSetState() prototype is auto-emitted by the Arduino builder. The only #include we need is mqtt.h for MQTT_KEEPALIVE_INTERVAL_MS — it lets
// us keep our timeout derived from the keepalive cadence in a single place rather than duplicating the number here.
//
// Exposed to other files purely via the accessor contactGetActiveCount() (read by bars.ino) and the entry point onReceivedContactOnline()
// (called by mqtt.ino's incoming-message dispatcher via auto-prototype). No contacts.h — same "light case" pattern as wifi.h.

#include "mqtt.h"
#include "mm_log.h"         // ESP_LOGI / ESP_LOGW + TAG_MM — par cohérence avec commands.ino, mqtt.ino et wifi.ino qui font le même include explicite.
#include "contacts.h"       // DeviceDataEntry + DeviceDataEntry::findByMac / ::findById — résolution pseudo / namePrefix / screen du contact distant.
#include "personal-data.h"  // matérialise les tableaux COMPILED_DEVICE_DATA_ENTRIES + COMPILED_WIFI_DEFAULTS — DeviceDataEntry (contacts.h) et
                            // CompiledWifiEntry (wifi.h) sont déjà en scope via les includes de minimessenger.ino concatenés en tête de TU.

// Device-table count derived ici — premier point de la TU où le tableau a sa pleine définition. wifi.ino se contente de définir le count wifi.
const size_t COMPILED_DEVICE_DATA_ENTRIES_COUNT = sizeof(COMPILED_DEVICE_DATA_ENTRIES) / sizeof(COMPILED_DEVICE_DATA_ENTRIES[0]);


#define MAX_CONTACTS 5

// Applicative timeout: we consider a contact offline when no keepalive has been seen for longer than one keepalive interval plus a 5 s grace window
// (network jitter + a bit of slack for the emitter's own scheduling). Tunable independently of MQTT's TCP keepalive — this one is purely about
// presence display, not transport health.
#define CONTACT_TIMEOUT_MS (MQTT_KEEPALIVE_INTERVAL_MS + 5000)


// ================================================================================
// DeviceDataEntry — static find* method bodies
// ================================================================================
//
// Out-of-class inline definitions for the find methods declared in contacts.h. Bodies live here so the iteration logic sits next to the rest of
// the contact-table code; the struct itself stays in contacts.h because minimessenger.ino (concatenated first) needs the type in scope to declare
// `DeviceDataEntry g_deviceData` and to read its fields.

inline const DeviceDataEntry* DeviceDataEntry::findByMac(const char* mac) {
    for (size_t i = 0; i < COMPILED_DEVICE_DATA_ENTRIES_COUNT; i++) {
        if (strcmp(COMPILED_DEVICE_DATA_ENTRIES[i].mac, mac) == 0) {
            return &COMPILED_DEVICE_DATA_ENTRIES[i];
        }
    }
    return nullptr;
}

inline const DeviceDataEntry* DeviceDataEntry::findById(byte deviceId) {
    for (size_t i = 0; i < COMPILED_DEVICE_DATA_ENTRIES_COUNT; i++) {
        if (COMPILED_DEVICE_DATA_ENTRIES[i].deviceId == deviceId) {
            return &COMPILED_DEVICE_DATA_ENTRIES[i];
        }
    }
    return nullptr;
}


// ================================================================================
// Dynamic peer table
// ================================================================================

struct ContactLiveness {
    byte          deviceId;    // DEVICE_ID_UNSET = slot libre, sinon ID du contact distant.
    unsigned long lastSeenMs;  // millis() de la dernière liveness reçue pour ce contact.
};

static ContactLiveness g_contacts[MAX_CONTACTS];


// Number of slots currently occupied. Read every ~500 ms by redrawStatusBar() in bars.ino to pick between 0/1/2-icon layouts on the top bar, and
// also used internally by contactsApplyState() / the +/-/timeout log lines to surface the post-change count.
int contactGetActiveCount() {
    int n = 0;
    for (int i = 0; i < MAX_CONTACTS; i++) {
        if (g_contacts[i].deviceId != DEVICE_ID_UNSET) {
            n++;
        }
    }
    return n;
}

// Refresh derived UI state after any add / remove. Two FRIEND LEDs on the device + a forced top-bar repaint at the next status-bar poll: the bar
// reads contactGetActiveCount() in redrawStatusBar() and short-circuits when the count is unchanged, so the dirty flag is what makes the new count
// actually paint instead of getting cached out.
static void contactsApplyState() {
    const int count = contactGetActiveCount();
    ledSetState(LED_FRIEND_1, (count >= 1) ? LED_STATE_ON : LED_STATE_OFF);
    ledSetState(LED_FRIEND_2, (count >= 2) ? LED_STATE_ON : LED_STATE_OFF);
    g_statusBarDirty = true;
}

// Pseudo for logging — never null. Returns the pseudo from personal-data.h if the deviceId is declared there, else a literal "?" so format strings stay
// well-formed (snprintf with "%s" on nullptr is undefined behavior on some toolchains).
static const char* pseudoOrPlaceholder(byte deviceId) {
    const DeviceDataEntry* entry = DeviceDataEntry::findById(deviceId);
    return (entry != nullptr) ? entry->pseudo : "?";
}

// Push a banner into the conversation buffer when a contact transitions online / offline. Uses the declared pseudo when available, otherwise falls
// back to "device #<n>" so unknown peers still get a visible transition (just without their friendly name). Color = green on connect, red on
// disconnect, matching CONVO_INFO_COLOR / CONVO_ERROR_COLOR used elsewhere for ready / lost-server banners. Called on transitions only (add via
// admin/live, remove via admin/dead, remove via applicative timeout) — never on the periodic refresh of an already-known contact.
static void announceContactTransition(byte deviceId, bool isLive) {
    const DeviceDataEntry* entry = DeviceDataEntry::findById(deviceId);
    String                         label = (entry != nullptr) ? String(entry->pseudo) : (String("device #") + deviceId);
    label += isLive ? " connected" : " disconnected";
    addConversationBlock("", label, isLive ? CONVO_INFO_COLOR : CONVO_ERROR_COLOR, CENTER);
}


// Reset the whole table to "empty" at boot. Called once from setup(). We can't rely on zero-init giving us DEVICE_ID_UNSET (which is 0xFF, not 0), so
// the explicit pass is mandatory.
void contactsSetup() {
    for (int i = 0; i < MAX_CONTACTS; i++) {
        g_contacts[i].deviceId   = DEVICE_ID_UNSET;
        g_contacts[i].lastSeenMs = 0;
    }
}

// Called from onMqttIncomingMessage() (mqtt.ino) for every `admin/live` (isLive=true) or `admin/dead` (isLive=false) payload received. Filtering self
// is critical because admin/live is retained: when we (re)subscribe after a MQTT reconnect, the broker replays our own last keepalive back at us, and
// without the guard we would self-allocate a slot on every reconnect.
//
// isLive=true: refresh an existing slot if we already know this id, otherwise allocate the first free slot. Table-full → log + ignore (per design
// decision: a 6th contact is silently dropped until a timeout frees a slot, no LRU eviction).
//
// isLive=false: locate the slot and release it immediately, without waiting for CONTACT_TIMEOUT_MS. Last Will gives us a fast path on clean shutdowns.
//
// Caveat: a peer that crashed without publishing admin/dead leaves its retained admin/live in place on the broker. At our next subscribe we'll get
// that retained payload and treat the peer as alive — the CONTACT_TIMEOUT_MS window will eventually evict it (~125 s with MQTT_KEEPALIVE_INTERVAL_MS
// = 120 s), so the UI corrects itself but lags by up to one timeout window after a startup-resume scenario.
void onReceivedContactOnline(int remoteDeviceId, bool isLive) {
    if (remoteDeviceId == g_deviceData.deviceId) {
        return;
    }

    const byte          id  = (byte)remoteDeviceId;
    const unsigned long now = millis();

    int slotIdxIfKnown = -1;
    int slotIdxIfFree  = -1;
    for (int i = 0; i < MAX_CONTACTS; i++) {
        if (g_contacts[i].deviceId == id) {
            slotIdxIfKnown = i;
            break;
        }
        if (slotIdxIfFree == -1 && g_contacts[i].deviceId == DEVICE_ID_UNSET) {
            slotIdxIfFree = i;
        }
    }

    if (isLive) {
        if (slotIdxIfKnown >= 0) {
            g_contacts[slotIdxIfKnown].lastSeenMs = now;
            return;  // déjà connu, juste un refresh : ni LED ni redraw ni bannière.
        }
        if (slotIdxIfFree < 0) {
            ESP_LOGW(TAG_MM, "CONTACT overflow, ignoring id=%d (%s, table full)", remoteDeviceId, pseudoOrPlaceholder(id));
            return;
        }
        g_contacts[slotIdxIfFree].deviceId   = id;
        g_contacts[slotIdxIfFree].lastSeenMs = now;
        ESP_LOGI(TAG_MM, "CONTACT + id=%d (%s) slot=%d count=%d ts=%lu", remoteDeviceId, pseudoOrPlaceholder(id), slotIdxIfFree, contactGetActiveCount(), now);
        announceContactTransition(id, true);
        contactsApplyState();
    } else {
        if (slotIdxIfKnown < 0) {
            return;  // dead pour un contact qu'on ne suivait pas — rien à faire.
        }
        g_contacts[slotIdxIfKnown].deviceId   = DEVICE_ID_UNSET;
        g_contacts[slotIdxIfKnown].lastSeenMs = 0;
        ESP_LOGI(TAG_MM, "CONTACT -dead id=%d (%s) slot=%d count=%d", remoteDeviceId, pseudoOrPlaceholder(id), slotIdxIfKnown, contactGetActiveCount());
        announceContactTransition(id, false);
        contactsApplyState();
    }
}

// Called every loop() iteration from minimessenger.ino. Walks the table and releases any slot whose last keepalive is older than CONTACT_TIMEOUT_MS.
// Only repaints the derived state (LEDs + status bar) if at least one slot was actually freed by this pass, to keep the periodic work cheap when
// nothing is changing.
//
// Reads millis() locally instead of taking a currentMillis parameter. Reason: onReceivedContactOnline() (called from the MQTT callback in the same
// loop iteration) stamps lastSeenMs with its own fresh millis(), which can be a few ms later than the currentMillis captured at the top of loop().
// Comparing a stale currentMillis with a fresher lastSeenMs underflows the unsigned subtraction and instantly evicts the contact we just received.
// Keeping the clock read in here (matching what onReceivedContactOnline already does) makes the two sites use the same time reference.
void contactsTick() {
    const unsigned long now      = millis();
    bool                anyFreed = false;
    for (int i = 0; i < MAX_CONTACTS; i++) {
        if (g_contacts[i].deviceId == DEVICE_ID_UNSET) {
            continue;
        }
        if (now - g_contacts[i].lastSeenMs > CONTACT_TIMEOUT_MS) {
            const byte expiredId = g_contacts[i].deviceId;
            ESP_LOGI(TAG_MM,
                     "CONTACT -timeout id=%d (%s) slot=%d ageMs=%lu lastSeen=%lu now=%lu",
                     expiredId,
                     pseudoOrPlaceholder(expiredId),
                     i,
                     now - g_contacts[i].lastSeenMs,
                     g_contacts[i].lastSeenMs,
                     now);
            g_contacts[i].deviceId   = DEVICE_ID_UNSET;
            g_contacts[i].lastSeenMs = 0;
            anyFreed                 = true;
            announceContactTransition(expiredId, false);
        }
    }
    if (anyFreed) {
        contactsApplyState();
    }
}
