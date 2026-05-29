// Library Nimble-Arduino 2.5.0
#include <Arduino.h>
#include <NimBLEDevice.h>


typedef std::function<void(boolean isFullyConnected)> mm_btkb_on_connection_callback;

// Original notify_callback is defined in NimBLE-Arduino as:
// typedef std::function<void(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool)> notify_callback;
// Here is a simplified version exposed to the upper layer:
typedef std::function<void(uint8_t* pData, size_t length)> mm_btkb_on_keystroke_callback;


/**
 * Class for handling bluetooth connection with a BLE keyboard
 * (detection, connection+bonding, reconnection, transmitting keystrokes to upper layer)
 *
 * NimBLE-Arduino merges the security callbacks (passkey, auth complete) into
 * NimBLEClientCallbacks, so we only need to inherit two callback bases instead
 * of the three required by Bluedroid.
 */
class MiniMessengerBLEKeyboardInterface
  : public NimBLEScanCallbacks,
    public NimBLEClientCallbacks {

public:
  bool setup(
    String keyboardBluetoothName,
    bool clearExistingBonds,
    mm_btkb_on_connection_callback onConnectionCallback,
    mm_btkb_on_keystroke_callback onKeystrokeCallback);

  void tryToMaintainConnection();

  bool isFullyConnected();


protected:
  uint8_t m_scanningDurationSec = 30;

  void clearAllExistingBonds();
  bool connectToServer(const NimBLEAddress& address);

  // NimBLEScanCallbacks
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
  void onScanEnd(const NimBLEScanResults& scanResults, int reason) override;

  // NimBLEClientCallbacks (connect + security callbacks merged)
  void onConnect(NimBLEClient* pClient) override;
  void onDisconnect(NimBLEClient* pClient, int reason) override;
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override;
  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override;
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;

private:
  NimBLEAddress* pServerAddress = nullptr;
  NimBLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
  boolean m_connectionDone = false;
  boolean doConnect = false;
  boolean doScan = true;

  static void bleNotifyCallback(
    NimBLERemoteCharacteristic* pChar,
    uint8_t* pData,
    size_t length,
    bool isNotify);

  mm_btkb_on_connection_callback m_clientOnConnectionCallback = nullptr;

  // Your keyboard's advertised name
  String m_expectedKeyboardBluetoothName;
};
