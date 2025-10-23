#include "BLEDevice.h"

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEAddress.h>
#include <BLEScan.h>
#include <BLESecurity.h>


typedef std::function<void(boolean isFullyConnected)> mm_btkb_on_connection_callback;

// Original notify_callback is defined in
// ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/libraries/BLE/src/BLERemoteCharacteristic.h
// as:
// typedef std::function<void(BLERemoteCharacteristic *pBLERemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify)> notify_callback;
// Here is a simplified version:
typedef std::function<void(uint8_t* pData, size_t length)> mm_btkb_on_keystroke_callback;


/**
 * Class for handling bluetooth connection with a BLE keyboard
 * (detection, connection+bonding, reconnection, transmitting keystrokes to upper layer
 */
class MiniMessengerBLEKeyboardInterface
  : public BLEAdvertisedDeviceCallbacks,
    public BLESecurityCallbacks,
    public BLEClientCallbacks {

public:
  // Configure stuff and start scanning for keyboards.
  bool setup(
    String keyboardBluetoothName,
    bool clearExistingBonds,
    mm_btkb_on_connection_callback onConnectionCallback,
    mm_btkb_on_keystroke_callback onKeystrokeCallback);

  // Equivalent of a loop() function
  void tryToMaintainConnection();

  bool isFullyConnected();


protected:
  uint8_t m_scanningDurationSec = 30;

  void clearAllExistingBonds();
  bool connectToServer(BLEAddress pAddress);

  // BLEAdvertisedDeviceCallbacks
  virtual void onResult(BLEAdvertisedDevice advertisedDevice) override;

  // BLESecurityCallbacks
  virtual uint32_t onPassKeyRequest() override;
  virtual void onPassKeyNotify(uint32_t pass_key) override;
  virtual bool onSecurityRequest() override;
  virtual bool onConfirmPIN(uint32_t pin) override;
  virtual void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override;

  // BLEClientCallbacks
  virtual void onConnect(BLEClient* pClient) override;
  virtual void onDisconnect(BLEClient* pClient) override;

private:
  BLEAddress* pServerAddress = nullptr;
  BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
  boolean m_connectionDone = false;
  boolean doConnect = false;
  boolean doScan = true;

  static void bleNotifyCallback(
    BLERemoteCharacteristic* pChar,
    uint8_t* pData,
    size_t length,
    bool isNotify);

  mm_btkb_on_connection_callback m_clientOnConnectionCallback = nullptr;

  // Your keyboard's advertised name
  String m_expectedKeyboardBluetoothName;
};
