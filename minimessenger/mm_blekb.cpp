#include "mm_blekb.h"
#include "mm_log.h"


// This one cannot be a static class member for some C++ reason
static mm_btkb_on_keystroke_callback g_clientOnKeystrokeCallback = nullptr;


bool MiniMessengerBLEKeyboardInterface::isFullyConnected() {
    return m_connectionDone;
}

void MiniMessengerBLEKeyboardInterface::clearAllExistingBonds() {
    NimBLEDevice::deleteAllBonds();
    ESP_LOGI(TAG_BTKB, "All bonds cleared");
}

bool MiniMessengerBLEKeyboardInterface::connectToServer(const NimBLEAddress& address) {
    NimBLEClient* pClient = NimBLEDevice::createClient();
    ESP_LOGI(TAG_BTKB, "Connecting to server...");

    pClient->setClientCallbacks(this, false);  // false = do NOT delete `this` when client is destroyed

    if (!pClient->connect(address)) {
        ESP_LOGE(TAG_BTKB, "Connection failed");
        return false;
    }

    ESP_LOGI(TAG_BTKB, "Connected");

    NimBLERemoteService* pRemoteService = pClient->getService(NimBLEUUID(BT_SERVICE_HID_1812));
    if (pRemoteService == nullptr) {
        ESP_LOGE(TAG_BTKB, "HID service 0x1812 not found");
        return false;
    }

    // HID over GATT keyboards expose MULTIPLE characteristics with UUID BT_CHAR_HID_REPORT_2A4D inside the HID service — one per Report ID (keyboard
    // input report, LEDs output report, possibly feature reports, etc.). We don't know upfront which one is the keyboard input report, so we iterate
    // the whole list and subscribe to every 0x2A4D that has notify capability. The non-input reports' callbacks simply stay silent in practice.
    //
    // Picking only the first match (the old `getCharacteristic(0x2A4D)` shortcut) is fragile: Bluedroid often returned the input report first by chance,
    // NimBLE may not.
    const NimBLEUUID                                hidReportUuid(BT_CHAR_HID_REPORT_2A4D);
    const std::vector<NimBLERemoteCharacteristic*>& chars      = pRemoteService->getCharacteristics(true);
    int                                             subscribed = 0;
    for (NimBLERemoteCharacteristic* pChar : chars) {
        if (pChar->getUUID() == hidReportUuid && pChar->canNotify()) {
            bool ok = pChar->subscribe(true, MiniMessengerBLEKeyboardInterface::bleNotifyCallback);
            ESP_LOGI(TAG_BTKB, "subscribe(handle=%u) -> %s", pChar->getHandle(), ok ? "OK" : "FAIL");
            if (ok) {
                subscribed++;
            }
        }
    }
    if (subscribed == 0) {
        ESP_LOGE(TAG_BTKB, "No notifiable HID Report (0x2A4D) characteristic found");
        return false;
    }
    ESP_LOGI(TAG_BTKB, "%d HID Report characteristic(s) subscribed", subscribed);

    m_connectionDone = true;
    m_clientOnConnectionCallback(m_connectionDone);

    return true;
}

// ============================================================================
// Scan callbacks (NimBLEScanCallbacks)
// ============================================================================

void MiniMessengerBLEKeyboardInterface::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    // Filter by HID-over-GATT service UUID rather than by advertised device name. This means we accept any vendor/model whose firmware exposes the
    // standard HID service — Logitech, Apple, generic ChiCony, you name it — without recompiling. The previous name filter ("Bluetooth Keyboard")
    // worked only for the one specific keyboard model that was set up at flashing time, which blocked any swap to a differently-named device.
    //
    // When active scan is enabled (first onboarding — see setup() below), NimBLE issues a SCAN_REQ on every adv it sees and merges the SCAN_RSP
    // payload into the NimBLEAdvertisedDevice, so even keyboards that advertise the HID UUID only in the scan response are detected here. On
    // reconnect after a bond, we drop to passive scan to free the radio for WiFi: that path assumes the bonded keyboard puts the HID UUID in its
    // primary adv data (the case for sane HID stacks). isAdvertisingService() inspects the merged service list in either mode.
    //
    // Trade-off: this also matches BLE mice / gamepads / etc. that happen to be in pairing mode within range. In practice such collisions are rare
    // (the user usually puts only their target keyboard into pairing mode), and even if a mouse bonds by accident, its 3–4-byte HID reports are
    // silently dropped by decodeHIDReport's `length < 8` guard. The slot can be freed afterwards with the `cmd bonds` command.
    if (!advertisedDevice->isAdvertisingService(NimBLEUUID(BT_SERVICE_HID_1812))) {
        return;
    }

    // Negative filter on Appearance: if the device explicitly advertises itself as a Mouse, skip it without attempting a connection. Appearance is
    // OPTIONAL in BLE adv data — many devices don't publish it, in which case haveAppearance() is false and we fall through to connect. We only act
    // on a POSITIVE signal that the device is a mouse; absence of Appearance is never used as evidence of "this is a keyboard". The same pattern
    // could be extended later to BT_APPEARANCE_JOYSTICK_03C3 / GAMEPAD_03C4 if those start showing up in your environment.
    if (advertisedDevice->haveAppearance() && advertisedDevice->getAppearance() == BT_APPEARANCE_MOUSE_03C2) {
        ESP_LOGI(TAG_BTKB, "Skipping mouse (appearance 0x%04X) [%s]", BT_APPEARANCE_MOUSE_03C2, advertisedDevice->toString().c_str());
        return;
    }

    ESP_LOGI(TAG_BTKB, "Found HID device [%s]", advertisedDevice->toString().c_str());

    NimBLEDevice::getScan()->stop();

    // Free any address kept from a previous scan/disconnect cycle before allocating a new one — otherwise repeated re-pairings leak ~24 bytes
    // of heap per cycle. `delete nullptr` is a no-op, so this is safe on the first call too.
    delete pServerAddress;
    pServerAddress = new NimBLEAddress(advertisedDevice->getAddress());

    doConnect = true;
    doScan    = false;
}

void MiniMessengerBLEKeyboardInterface::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
    // NimBLE scans are asynchronous: the start() call returns immediately and this fires when the scan window elapses. Re-arm doScan so the next
    // tryToMaintainConnection() iteration kicks off a new round — unless paused (e.g. during WiFi portal, see pauseScan()).
    ESP_LOGI(TAG_BTKB, "Scan ended (reason=%d, %d devices seen) — %s", reason, scanResults.getCount(), m_scanPaused ? "paused, not rescanning" : "will rescan");
    if (!m_connectionDone && !m_scanPaused) {
        doScan = true;
    }
}


// ============================================================================
// Client callbacks (NimBLEClientCallbacks)
// ============================================================================

void MiniMessengerBLEKeyboardInterface::onConnect(NimBLEClient* pClient) {
    ESP_LOGI(TAG_BTKB, "Client connected");
}

void MiniMessengerBLEKeyboardInterface::onDisconnect(NimBLEClient* pClient, int reason) {
    ESP_LOGE(TAG_BTKB, "Client disconnected (reason=%d) — restarting scan", reason);
    m_connectionDone = false;
    m_clientOnConnectionCallback(m_connectionDone);

    doScan = true;
}

void MiniMessengerBLEKeyboardInterface::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    if (connInfo.isEncrypted()) {
        ESP_LOGI(TAG_BTKB, "Authentication complete — bonding saved in NVS");
    } else {
        ESP_LOGE(TAG_BTKB, "Authentication failed");
    }
}


// ============================================================================

void MiniMessengerBLEKeyboardInterface::bleNotifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    g_clientOnKeystrokeCallback(pData, length);
}


bool MiniMessengerBLEKeyboardInterface::setup(bool                           clearExistingBonds,
                                              mm_btkb_on_connection_callback onConnectionCallback,
                                              mm_btkb_on_keystroke_callback  onKeystrokeCallback) {
    ESP_LOGI(TAG_BTKB, "setup()...");

    NimBLEDevice::init("");

    // Bonding=true, MITM=false, Secure Connections=false. IO_NO_INPUT_OUTPUT → "Just Works" pairing (no PIN exchange, no user confirmation).
    // That's why onPassKeyEntry / onConfirmPasskey are not overridden in this class — the stack never calls them in this mode.
    NimBLEDevice::setSecurityAuth(true, false, false);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    // Clear bonding info if you want fresh pairing
    if (clearExistingBonds) {
        clearAllExistingBonds();
    }

    // Active scan triggers a SCAN_REQ/SCAN_RSP exchange on every adv received: NimBLE merges the SCAN_RSP payload (where many keyboards put their
    // service UUIDs) into the NimBLEAdvertisedDevice before onResult() runs, which is what makes the HID-UUID filter reliable for first onboarding.
    // The trade-off is that active scan sits at ~100 % duty cycle on the shared 2.4 GHz front-end and starves WiFi RX (see pauseScan() + the
    // portal-time mitigation in wifi.ino). Once a keyboard is bonded its address is known and connectToServer() will only complete the pairing-secured
    // link with that specific peer — the UUID filter doesn't need to round-trip a SCAN_RSP to be useful, so we drop to passive scan and free the radio
    // for WiFi. NB: this relies on the bonded keyboard advertising its HID UUID in the primary adv data (the case for sane HID stacks); if a future
    // model only puts it in the SCAN_RSP, force a re-bond via g_kb.setup(true, ...) to get active scan back.
    const int  bondedCount     = NimBLEDevice::getNumBonds();
    const bool hasExistingBond = (bondedCount > 0);
    ESP_LOGI(TAG_BTKB,
             "Starting scan for any device advertising HID service 0x%04X (activeScan=%d, bondedCount=%d)",
             BT_SERVICE_HID_1812,
             hasExistingBond ? 0 : 1,
             bondedCount);

    NimBLEScan* pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setScanCallbacks(this, false);
    pBLEScan->setActiveScan(!hasExistingBond);
    // NimBLE-Arduino 2.x: scan duration is in MILLISECONDS (1.x was in seconds)
    pBLEScan->start(m_scanningDurationSec * 1000);
    // Scan already running; onScanEnd() will re-arm doScan if it ends without finding the keyboard.
    doScan = false;

    m_clientOnConnectionCallback = onConnectionCallback;
    g_clientOnKeystrokeCallback  = onKeystrokeCallback;

    return true;
}


void MiniMessengerBLEKeyboardInterface::tryToMaintainConnection() {
    if (doConnect) {
        if (connectToServer(*pServerAddress)) {
            ESP_LOGI(TAG_BTKB, "tryToMaintainConnection: connected");
        } else {
            ESP_LOGE(TAG_BTKB, "tryToMaintainConnection: failed to connect");
            doScan = true;  // retry scan if connect failed
        }
        doConnect = false;
    }

    if (doScan && !m_scanPaused) {
        ESP_LOGI(TAG_BTKB, "Rescanning...");
        NimBLEDevice::getScan()->start(m_scanningDurationSec * 1000, false);
        // NimBLE scan is async — flag it as "in flight" so we don't restart it every loop iteration. onScanEnd() will re-arm doScan when the
        // window elapses without finding the keyboard (unless m_scanPaused).
        doScan = false;
    }
}

// Pause the BLE scan loop. Stops any active scan synchronously (NimBLE's stop() blocks until the radio is freed) and inhibits onScanEnd from
// re-arming doScan. Safe to call multiple times. Used by the WiFi portal entry to free the 2.4 GHz radio for WiFi RX — see header comment.
void MiniMessengerBLEKeyboardInterface::pauseScan() {
    ESP_LOGI(TAG_BTKB, "pauseScan() — stopping scan and inhibiting re-arm");
    m_scanPaused = true;
    NimBLEDevice::getScan()->stop();
    doScan = false;
}

// Resume the BLE scan loop. If the keyboard isn't currently connected, the next tryToMaintainConnection() iteration will kick off a new scan.
void MiniMessengerBLEKeyboardInterface::resumeScan() {
    ESP_LOGI(TAG_BTKB, "resumeScan() — scan loop re-enabled");
    m_scanPaused = false;
    if (!m_connectionDone) {
        doScan = true;
    }
}
