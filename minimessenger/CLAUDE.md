# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A small Arduino sketch turning an ESP32 (or ESP8266 D1 mini) into a self-contained "messenger" appliance: BLE keyboard for input, ST7789 240×320 TFT for display, and MQTT over TLS (HiveMQ Cloud) as the transport between paired devices. There is no host-side build system — everything is a `.ino` plus headers compiled by the Arduino IDE.

## Build / flash

There is no Makefile, PlatformIO config, CI, or test harness. Build and flash via the Arduino IDE:

- Open `minimessenger.ino`.
- For ESP32: keep `#define PAC_ON_ESP32` at the top of the `.ino` (default). Tools → Boards: select an ESP32 board; Tools → Partition Scheme: **"Huge App" (1.9 MB / 320 KB SPIFFS)** — the default partition is too small.
- For ESP8266 D1 mini: switch to `#define PAC_ON_D1MINI`. Tools → Boards: "LOLIN(WEMOS) D1 R2 & mini".
- Serial monitor: 115200 baud.

Required libraries (Library Manager, exact versions known to work in comments):
- PubSubClient 2.8
- Adafruit ST7735 and ST7789 1.11.0 (pulls in Adafruit_GFX)
- NTPClient 3.2.1
- BLE / WiFi / WiFiClientSecure / SPI / Wire ship with the ESP32 / ESP8266 board packages.

## Per-device identity

`identifyDevice()` in `minimessenger.ino` is the source of truth for device behaviour. It branches on `WiFi.macAddress()` and assigns: `g_deviceIdMe`, `g_deviceName` (e.g. `D1M_001`, `E32_004`), `g_userPseudo`, the two `g_deviceIdFriend*` IDs, the default recipient, and `g_displayType`. **Adding or moving a physical device requires editing this function** — there is no config file. An unknown MAC falls back to a random ID and pseudo "JohnDoe".

On ESP32 the bootstrap sequence calls `WiFi.begin()` once *before* reading the MAC; without that call, the MAC reads as `00:00:00:00:00:00`. Don't reorder `identifyDevice()` relative to that.

## MQTT topology

All devices share one HiveMQ Cloud broker (credentials and root CA are hardcoded in the sketch). Topics:

- `msg/broadcast` — subscribed by all devices.
- `msg/unicast/<deviceId>` — each device subscribes to its own; outgoing messages target one peer via `g_mqttOutoingRecipientTopic`, set by `setRecipient(int)`.
- `admin/live` — retained "I'm alive" pings (boot / reconnect / 30 s keepalive). Drives the friend-presence LEDs via `onLiveness()`.
- `admin/dead` — MQTT Last Will published on disconnect; same liveness handler treats it as `isLive=false`.
- `admin/logs` — currently unused for output but reserved.

`mqttPushFormattedMessage()` appends a trailer `### ts:<...> deviceId:<n> msgId:<n>` to every payload. Incoming messages are dispatched in `onMqttIncomingMessage()` by topic prefix (`'m'` = a `msg/...` topic). The string `"dis"` received as a message triggers a deliberate MQTT disconnect — useful for testing reconnection.

## Display / conversation buffer

`display.h` defines `TextLine`, a single conversation entry holding both an optional timestamp and the message, each with their own font, colour, size, X position, and pre-computed `getTextBounds` rectangle. `lines[MAX_LINES]` (40) is a ring-ish buffer: when full or when the next line would run off-screen, `addConversationBlock()` shifts the array down by one and decreases `g_nextTextTopY` by the freed height, then `redrawAllConversations()` repaints from scratch. There is no partial-redraw / hardware-scroll path — every new line redraws the full screen.

Coordinate quirk worth knowing: with GFX fonts (`FreeSans9pt7b`), `getTextBounds` returns negative `y` for the bounding-box top because the cursor sits on the baseline. The drawing code compensates with `setCursor(x - bounds[BOX_X], y - bounds[BOX_Y])`. There's a long French comment in `addConversationBlock` explaining this — keep it; it documents non-obvious behaviour of the upstream library.

`g_displayType` exists to support an alternate `OLEDSHIELD` (SSD1306) target, but only the `ST7789` branch is implemented. `setupDisplay`, `showSplashScreen`, `redrawAllConversations`, etc. all log "DISPLAY_TYPE_NOT_CONFIGURED" for the OLED branch.

## BLE keyboard

`mm_blekb.{h,cpp}` wraps the ESP32 BLE stack into `MiniMessengerBLEKeyboardInterface`, which multi-inherits `BLEAdvertisedDeviceCallbacks`, `BLESecurityCallbacks`, and `BLEClientCallbacks`. The expected keyboard MAC is `KEYBOARD_MAC_ADDRESS` in the sketch. Bonding is persisted in NVS; pass `true` to `setup()`'s `clearExistingBonds` to wipe and re-pair.

`tryToMaintainConnection()` is called every `loop()` iteration and drives the scan → connect → re-scan state machine; do not block in `loop()` longer than a couple of seconds or BLE reconnection stalls.

HID reports arrive in `decodeHIDReport()`, which maintains `kbIsCapsLockOn` locally (toggled on the keyboard's CapsLock keypress, since we don't read the LED state back from the keyboard) and uses `keymapLower` / `keymapUpper` lookup tables keyed by HID codes from `hid_keys.h`. Currently keystrokes accumulate into `g_currentMsg` but pressing Enter only logs "TODO Send message" — this path is not yet wired to MQTT publishing; serial input *is* wired (via `FLAG_READ_SERIAL_INPUTS`).

## LEDs

Three LEDs (`LED_STATUS`, `LED_FRIEND_1`, `LED_FRIEND_2`) are managed via a tiny non-blocking state machine: `g_ledRequiredState[pin]` holds OFF / ON / BLINK_FAST / BLINK_SLOW, and the `loop()` tail toggles blinking pins based on `LED_BLINK_*_DURATION`. Use `ledSetState(pin, state)` rather than `digitalWrite` directly so blink state stays consistent. The `LED_QTY` (17) array size is sized for raw GPIO numbers, not LED count.

## Logging

Custom variadic templates `hlog` / `hlogn` / `log` / `logn` (in the sketch) prefix output with `g_deviceName`. All logging is gated by `#define WITH_LOGS`; removing it is the production path (see TODO list at top of `.ino`). Don't replace these with bare `Serial.print` — you lose the device-name prefix that's essential when watching multiple devices on one serial console.

## Secrets

WiFi SSID/password, MQTT user/password, and the HiveMQ Let's Encrypt root CA are hardcoded in `minimessenger.ino`. Treat the file as sensitive when sharing diffs externally; don't commit alternate credentials without the user's say-so.
