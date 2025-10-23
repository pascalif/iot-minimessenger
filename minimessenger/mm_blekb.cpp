#include "mm_blekb.h"


// This one cannot be a static class member for some C++ reason
static mm_btkb_on_keystroke_callback g_clientOnKeystrokeCallback = nullptr;


bool MiniMessengerBLEKeyboardInterface::isFullyConnected() {
  return m_connectionDone;
}

void MiniMessengerBLEKeyboardInterface::clearAllExistingBonds() {
  int dev_num = esp_ble_get_bond_device_num();
  if (dev_num == 0) {
    Serial.println("No bonded devices to clear.");
    return;
  }

  esp_ble_bond_dev_t* dev_list = (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * dev_num);
  if (!dev_list) {
    Serial.println("Failed to allocate memory for bond list.");
    return;
  }

  esp_ble_get_bond_device_list(&dev_num, dev_list);

  for (int i = 0; i < dev_num; i++) {
    esp_ble_remove_bond_device(dev_list[i].bd_addr);
    char bda_str[18];
    sprintf(bda_str, "%02X:%02X:%02X:%02X:%02X:%02X",
            dev_list[i].bd_addr[0], dev_list[i].bd_addr[1], dev_list[i].bd_addr[2],
            dev_list[i].bd_addr[3], dev_list[i].bd_addr[4], dev_list[i].bd_addr[5]);
    Serial.print("Removed bonded device: ");
    Serial.println(bda_str);
  }

  free(dev_list);
  Serial.println("BTKB: 🧹 All bonds cleared.");
}

bool MiniMessengerBLEKeyboardInterface::connectToServer(BLEAddress pAddress) {
  BLEClient* pClient = BLEDevice::createClient();
  Serial.println("BTKB: Connecting to server...");

  pClient->setClientCallbacks(this);

  if (!pClient->connect(pAddress)) {
    Serial.println("BTKB: Connection failed!");
    return false;
  }

  Serial.println("BTKB: Connected");

  BLERemoteService* pRemoteService = pClient->getService(BLEUUID((uint16_t)0x1812));
  if (pRemoteService == nullptr) {
    Serial.println("BTKB: HID service 0x1812 not found!");
    return false;
  }

  pRemoteCharacteristic = pRemoteService->getCharacteristic(BLEUUID((uint16_t)0x2A4D));  // HID Report
  if (pRemoteCharacteristic == nullptr) {
    Serial.println("BTKB: HID Report characteristic 0x2A4D not found!");
    return false;
  }

  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(this->bleNotifyCallback);
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
// Scan callbacks
//class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
// ============================================================================

void MiniMessengerBLEKeyboardInterface::onResult(BLEAdvertisedDevice advertisedDevice) {
  if (advertisedDevice.haveName() && advertisedDevice.getName() == m_expectedKeyboardBluetoothName) {
    Serial.print("BTKB: BLEAdvertisedDeviceCallbacks::onResult() - ✅ Found target keyboard [");
    Serial.print(advertisedDevice.toString().c_str());
    Serial.println(']');

    advertisedDevice.getScan()->stop();
    pServerAddress = new BLEAddress(advertisedDevice.getAddress());

    doConnect = true;
    doScan = false;
  }
}
//};


// ============================================================================
// Security callbacks (PIN / bonding) from BLESecurityCallbacks
//
// None of the 4 following methods will be called because we don't request this kind of security
// ============================================================================
uint32_t MiniMessengerBLEKeyboardInterface::onPassKeyRequest() {
  Serial.printf("BTKB: BLESecurityCallbacks::onPassKeyRequest() not implemented...");
  return 123456;
}
void MiniMessengerBLEKeyboardInterface::onPassKeyNotify(uint32_t pass_key) {
  Serial.printf("BTKB: BLESecurityCallbacks::onPassKeyNotify(), PassKey: %06u\n", pass_key);
}
bool MiniMessengerBLEKeyboardInterface::onSecurityRequest() {
  Serial.printf("BTKB: BLESecurityCallbacks::onSecurityRequest() called");
  return true;
}
bool MiniMessengerBLEKeyboardInterface::onConfirmPIN(uint32_t pin) {
  Serial.printf("BTKB: BLESecurityCallbacks::onConfirmPIN() not implemented...");
  return true;
}

void MiniMessengerBLEKeyboardInterface::onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) {
  if (auth_cmpl.success) {
    Serial.println("BTKB: BLESecurityCallbacks::onAuthenticationComplete - Bonding success ✅ (saved in NVS).");
  } else {
    Serial.printf("BTKB: BLESecurityCallbacks::onAuthenticationComplete - Bonding failed ❌, reason: %d\n", auth_cmpl.fail_reason);
  }
}

// ============================================================================
// Client callbacks from BLEClientCallbacks
// ============================================================================

void MiniMessengerBLEKeyboardInterface::onConnect(BLEClient* pClient) {
  Serial.println("BTKB: BLEClientCallbacks - Client connected.");
}

void MiniMessengerBLEKeyboardInterface::onDisconnect(BLEClient* pClient) {
  Serial.println("BTKB: BLEClientCallbacks - ⚠️ Client disconnected. Restarting scan...");
  m_connectionDone = false;
  m_clientOnConnectionCallback(m_connectionDone);

  doScan = true;
}

// ============================================================================


// ============================================================================

void MiniMessengerBLEKeyboardInterface::bleNotifyCallback(
  BLERemoteCharacteristic* pChar,
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

  BLEDevice::init("");
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  BLEDevice::setSecurityCallbacks(this);
  // Vu dans chatgpt, utile un jour ?
  //BLEDevice::setSecurityAuth(true, true, true);
  //BLEDevice::setSecurityPasskey(0);

  // Clear bonding info if you want fresh pairing
  if (clearExistingBonds) clearAllExistingBonds();

  BLESecurity* pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);
  pSecurity->setCapability(ESP_IO_CAP_NONE);
  pSecurity->setKeySize(16);
  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  Serial.printf("BLKB: Starting scan for keyboard named [%s]\n...", keyboardBluetoothName.c_str());
  this->m_expectedKeyboardBluetoothName = keyboardBluetoothName;

  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(this);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(m_scanningDurationSec);
  doScan = true;

  m_clientOnConnectionCallback = onConnectionCallback;
  g_clientOnKeystrokeCallback = onKeystrokeCallback;

  return true;
}


void MiniMessengerBLEKeyboardInterface::tryToMaintainConnection() {

  if (doConnect) {
    if (connectToServer(*pServerAddress)) {
      Serial.println("BTKB: We are now connected");
    } else {
      Serial.println("BTKB: Failed to connect");
      doScan = true;  // retry scan if connect failed
    }
    doConnect = false;
  }

  if (doScan) {
    Serial.println("BLKB: Rescanning...");
    BLEDevice::getScan()->start(m_scanningDurationSec, false);  // relaunch scan
  }

  // delay should not be done here to not affect in main loop
  //delay(1000);
}
