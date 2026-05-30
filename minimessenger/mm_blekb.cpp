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

  NimBLERemoteService* pRemoteService = pClient->getService(NimBLEUUID((uint16_t)0x1812));
  if (pRemoteService == nullptr) {
    ESP_LOGE(TAG_BTKB, "HID service 0x1812 not found");
    return false;
  }

  // HID over GATT keyboards expose MULTIPLE characteristics with UUID 0x2A4D
  // (HID Report) inside the HID service — one per Report ID (keyboard input
  // report, LEDs output report, possibly feature reports, etc.). We don't
  // know upfront which one is the keyboard input report, so we iterate the
  // whole list and subscribe to every 0x2A4D that has notify capability.
  // The non-input reports' callbacks simply stay silent in practice.
  //
  // Picking only the first match (the old `getCharacteristic(0x2A4D)`
  // shortcut) is fragile: Bluedroid often returned the input report first
  // by chance, NimBLE may not.
  const NimBLEUUID hidReportUuid((uint16_t)0x2A4D);
  const std::vector<NimBLERemoteCharacteristic*>& chars =
      pRemoteService->getCharacteristics(true);
  int subscribed = 0;
  for (NimBLERemoteCharacteristic* pChar : chars) {
    if (pChar->getUUID() == hidReportUuid && pChar->canNotify()) {
      bool ok = pChar->subscribe(true, MiniMessengerBLEKeyboardInterface::bleNotifyCallback);
      ESP_LOGI(TAG_BTKB, "subscribe(handle=%u) -> %s",
               pChar->getHandle(), ok ? "OK" : "FAIL");
      if (ok) subscribed++;
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
  if (advertisedDevice->haveName()
      && String(advertisedDevice->getName().c_str()) == m_expectedKeyboardBluetoothName) {
    ESP_LOGI(TAG_BTKB, "Found target keyboard [%s]", advertisedDevice->toString().c_str());

    NimBLEDevice::getScan()->stop();

    // Free any address kept from a previous scan/disconnect cycle before
    // allocating a new one — otherwise repeated re-pairings leak ~24 bytes
    // of heap per cycle. `delete nullptr` is a no-op, so this is safe on the
    // first call too.
    delete pServerAddress;
    pServerAddress = new NimBLEAddress(advertisedDevice->getAddress());

    doConnect = true;
    doScan = false;
  }
}

void MiniMessengerBLEKeyboardInterface::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
  // NimBLE scans are asynchronous: the start() call returns immediately and
  // this fires when the scan window elapses. Re-arm doScan so the next
  // tryToMaintainConnection() iteration kicks off a new round.
  ESP_LOGI(TAG_BTKB, "Scan ended (reason=%d, %d devices seen) — will rescan", reason, scanResults.getCount());
  if (!m_connectionDone) {
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

void MiniMessengerBLEKeyboardInterface::bleNotifyCallback(
  NimBLERemoteCharacteristic* pChar,
  uint8_t* pData,
  size_t length,
  bool isNotify) {
  g_clientOnKeystrokeCallback(pData, length);
}


bool MiniMessengerBLEKeyboardInterface::setup(
  String keyboardBluetoothName,
  bool clearExistingBonds,
  mm_btkb_on_connection_callback onConnectionCallback,
  mm_btkb_on_keystroke_callback onKeystrokeCallback) {
  ESP_LOGI(TAG_BTKB, "setup()...");

  NimBLEDevice::init("");

  // Bonding=true, MITM=false, Secure Connections=false.
  // IO_NO_INPUT_OUTPUT → "Just Works" pairing (no PIN exchange, no user
  // confirmation). That's why onPassKeyEntry / onConfirmPasskey are not
  // overridden in this class — the stack never calls them in this mode.
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  // Clear bonding info if you want fresh pairing
  if (clearExistingBonds) clearAllExistingBonds();

  ESP_LOGI(TAG_BTKB, "Starting scan for keyboard named [%s]", keyboardBluetoothName.c_str());
  this->m_expectedKeyboardBluetoothName = keyboardBluetoothName;

  NimBLEScan* pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(this, false);
  pBLEScan->setActiveScan(true);
  // NimBLE-Arduino 2.x: scan duration is in MILLISECONDS (1.x was in seconds)
  pBLEScan->start(m_scanningDurationSec * 1000);
  // Scan already running; onScanEnd() will re-arm doScan if it ends without
  // finding the keyboard.
  doScan = false;

  m_clientOnConnectionCallback = onConnectionCallback;
  g_clientOnKeystrokeCallback = onKeystrokeCallback;

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

  if (doScan) {
    ESP_LOGI(TAG_BTKB, "Rescanning...");
    NimBLEDevice::getScan()->start(m_scanningDurationSec * 1000, false);
    // NimBLE scan is async — flag it as "in flight" so we don't restart it
    // every loop iteration. onScanEnd() will re-arm doScan when the window
    // elapses without finding the keyboard.
    doScan = false;
  }
}
