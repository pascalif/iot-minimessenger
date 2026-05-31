// Library Nimble-Arduino 2.5.0
#include <Arduino.h>
#include <NimBLEDevice.h>


// ----------------------------------------------------------------------------
// Standard Bluetooth SIG 16-bit identifiers used by this project.
//
// The hex suffix in each constant name mirrors the SIG-assigned value so the
// identifier remains self-documenting at the call site — `BT_SERVICE_HID_1812`
// reads as "the HID GATT service, assigned number 0x1812" without forcing the
// reader to look up the spec. Source: Bluetooth SIG "Assigned Numbers" document
// — see https://www.bluetooth.com/specifications/assigned-numbers/ if you need
// to add more.
// ----------------------------------------------------------------------------
constexpr uint16_t BT_SERVICE_HID_1812 = 0x1812;          // HID over GATT — top-level service exposed by all BLE keyboards / mice / gamepads.
constexpr uint16_t BT_CHAR_HID_REPORT_2A4D = 0x2A4D;      // HID Report — there is ONE characteristic instance per Report ID (input / output / feature).
constexpr uint16_t BT_APPEARANCE_KEYBOARD_03C1 = 0x03C1;  // Generic HID → Keyboard. Optional in adv data but a strong positive signal when present.
constexpr uint16_t BT_APPEARANCE_MOUSE_03C2 = 0x03C2;     // Generic HID → Mouse. Not used to filter today, kept for symmetry / future negative filtering.


typedef std::function<void(bool isFullyConnected)> mm_btkb_on_connection_callback;

// Original notify_callback is defined in NimBLE-Arduino as:
// typedef std::function<void(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool)> notify_callback;
// Here is a simplified version exposed to the upper layer:
typedef std::function<void(uint8_t* pData, size_t length)> mm_btkb_on_keystroke_callback;


/**
 * Class for handling bluetooth connection with a BLE keyboard
 * (detection, connection+bonding, reconnection, transmitting keystrokes to upper layer)
 *
 * Scan filter is now based on the HID GATT service UUID (BT_SERVICE_HID_1812) instead of the advertised device name. Any keyboard that exposes
 * HID-over-GATT is accepted — regardless of vendor name, model, or whether the firmware advertises a name at all. The trade-off is that any HID
 * peripheral in pairing mode and in range can match (mice, gamepads). Mouse reports are 3–4 bytes and are silently dropped by decodeHIDReport's
 * 8-byte length guard, so an accidental mouse bond is harmless apart from squatting the BLE slot until `cmd bonds` is sent.
 *
 * NimBLE-Arduino merges the security callbacks (passkey, auth complete) into NimBLEClientCallbacks, so we only need to inherit two callback bases
 * instead of the three required by Bluedroid. We use Just Works pairing (IO_NO_INPUT_OUTPUT), so onPassKeyEntry / onConfirmPasskey are never invoked
 * by the stack and are not overridden here — the base class no-op defaults are sufficient.
 */
class MiniMessengerBLEKeyboardInterface
  : public NimBLEScanCallbacks,
    public NimBLEClientCallbacks {

public:
  bool setup(
    bool clearExistingBonds,
    mm_btkb_on_connection_callback onConnectionCallback,
    mm_btkb_on_keystroke_callback onKeystrokeCallback);

  void clearAllExistingBonds();

  void tryToMaintainConnection();

  bool isFullyConnected();


protected:
  uint8_t m_scanningDurationSec = 30;

  bool connectToServer(const NimBLEAddress& address);

  // NimBLEScanCallbacks
  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
  void onScanEnd(const NimBLEScanResults& scanResults, int reason) override;

  // NimBLEClientCallbacks
  void onConnect(NimBLEClient* pClient) override;
  void onDisconnect(NimBLEClient* pClient, int reason) override;
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;

private:
  NimBLEAddress* pServerAddress = nullptr;
  bool m_connectionDone = false;
  bool doConnect = false;
  bool doScan = true;

  static void bleNotifyCallback(
    NimBLERemoteCharacteristic* pChar,
    uint8_t* pData,
    size_t length,
    bool isNotify);

  mm_btkb_on_connection_callback m_clientOnConnectionCallback = nullptr;
};
