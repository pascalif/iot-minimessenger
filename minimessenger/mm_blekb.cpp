#include "mm_blekb.h"


// This one cannot be a static class member for some C++ reason
static mm_btkb_on_keystroke_callback g_clientOnKeystrokeCallback = nullptr;


bool MiniMessengerBLEKeyboardInterface::isFullyConnected() {
  return m_connectionDone;
}

void MiniMessengerBLEKeyboardInterface::clearAllExistingBonds() {
  NimBLEDevice::deleteAllBonds();
  Serial.println("BTKB: 🧹 All bonds cleared.");
}

bool MiniMessengerBLEKeyboardInterface::connectToServer(const NimBLEAddress& address) {
  NimBLEClient* pClient = NimBLEDevice::createClient();
  Serial.println("BTKB: Connecting to server...");

  pClient->setClientCallbacks(this, false);  // false = do NOT delete `this` when client is destroyed

  if (!pClient->connect(address)) {
    Serial.println("BTKB: Connection failed!");
    return false;
  }

  Serial.println("BTKB: Connected");

  NimBLERemoteService* pRemoteService = pClient->getService(NimBLEUUID((uint16_t)0x1812));
  if (pRemoteService == nullptr) {
    Serial.println("BTKB: HID service 0x1812 not found!");
    return false;
  }

  pRemoteCharacteristic = pRemoteService->getCharacteristic(NimBLEUUID((uint16_t)0x2A4D));  // HID Report
  if (pRemoteCharacteristic == nullptr) {
    Serial.println("BTKB: HID Report characteristic 0x2A4D not found!");
    return false;
  }

  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->subscribe(true, MiniMessengerBLEKeyboardInterface::bleNotifyCallback);
    Serial.println("BTKB: Keystrokes callback registered");
  } else {
    Serial.println("BTKB: Keystrokes callback not registered");
    return false;
  }

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
    Serial.print("BTKB: NimBLEScanCallbacks::onResult() - ✅ Found target keyboard [");
    Serial.print(advertisedDevice->toString().c_str());
    Serial.println(']');

    NimBLEDevice::getScan()->stop();
    pServerAddress = new NimBLEAddress(advertisedDevice->getAddress());

    doConnect = true;
    doScan = false;
  }
}

void MiniMessengerBLEKeyboardInterface::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
  // NimBLE scans are asynchronous: the start() call returns immediately and
  // this fires when the scan window elapses. Re-arm doScan so the next
  // tryToMaintainConnection() iteration kicks off a new round.
  Serial.printf("BTKB: onScanEnd() reason=%d, %d devices seen — will rescan\n",
                reason, scanResults.getCount());
  if (!m_connectionDone) {
    doScan = true;
  }
}


// ============================================================================
// Security callbacks (merged into NimBLEClientCallbacks).
// None of these will be called: we use IO_NO_INPUT_OUTPUT → "Just Works" pairing.
// ============================================================================

void MiniMessengerBLEKeyboardInterface::onPassKeyEntry(NimBLEConnInfo& connInfo) {
  Serial.printf("BTKB: onPassKeyEntry() not implemented, injecting dummy passkey\n");
  NimBLEDevice::injectPassKey(connInfo, 123456);
}

void MiniMessengerBLEKeyboardInterface::onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) {
  Serial.printf("BTKB: onConfirmPasskey() not implemented, accepting %06u\n", pin);
  NimBLEDevice::injectConfirmPasskey(connInfo, true);
}

void MiniMessengerBLEKeyboardInterface::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
  if (connInfo.isEncrypted()) {
    Serial.println("BTKB: Authentication complete - Bonding success ✅ (saved in NVS).");
  } else {
    Serial.println("BTKB: Authentication failed ❌");
  }
}

// ============================================================================
// Client callbacks (NimBLEClientCallbacks)
// ============================================================================

void MiniMessengerBLEKeyboardInterface::onConnect(NimBLEClient* pClient) {
  Serial.println("BTKB: Client connected.");
}

void MiniMessengerBLEKeyboardInterface::onDisconnect(NimBLEClient* pClient, int reason) {
  Serial.printf("BTKB: ⚠️ Client disconnected (reason=%d). Restarting scan...\n", reason);
  m_connectionDone = false;
  m_clientOnConnectionCallback(m_connectionDone);

  doScan = true;
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
  Serial.println("MiniMessengerBLEKeyboardInterface::setup()...");

  NimBLEDevice::init("");

  // Bonding=true, MITM=false, Secure Connections=false.
  // IO_NO_INPUT_OUTPUT → "Just Works" pairing (no PIN exchange).
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  // Clear bonding info if you want fresh pairing
  if (clearExistingBonds) clearAllExistingBonds();

  Serial.printf("BLKB: Starting scan for keyboard named [%s]\n...", keyboardBluetoothName.c_str());
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
      Serial.println("BTKB - tryToMaintainConnection() : We are now connected");
    } else {
      Serial.println("BTKB - tryToMaintainConnection() : Failed to connect");
      doScan = true;  // retry scan if connect failed
    }
    doConnect = false;
  }

  if (doScan) {
    Serial.println("BLKB: Rescanning...");
    NimBLEDevice::getScan()->start(m_scanningDurationSec * 1000, false);
    // NimBLE scan is async — flag it as "in flight" so we don't restart it
    // every loop iteration. onScanEnd() will re-arm doScan when the window
    // elapses without finding the keyboard.
    doScan = false;
  }

  // delay should not be done here to not affect in main loop
  //delay(1000);
}
