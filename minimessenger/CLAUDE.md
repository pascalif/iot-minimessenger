# CLAUDE.md

## Code style — comment formatting

Wrap source-code comments at **up to 160 characters per line** (not the conventional 80/100). Use the full available width so each comment line carries more context, and **never break a sentence in the middle just to satisfy the wrap** — finish the sentence on the same line if it fits within 160, otherwise break at a natural clause boundary (after a comma, before a conjunction, etc.). Aim for prose that reads as continuous English/French paragraphs rather than fragmented snippets. Apply this to all `.ino`, `.h`, `.cpp` files in the project.

## UI strings — command listing convention

Commands are invoked by typing **`/<verb>`** (slash prefix, no space) — e.g. `/wifi`, `/mqtt`, `/help`, `/status`. The `CMD_*` constants and the matching in `processPayloadAsCommand` hold the full `"/verb"` string. When rendering the help listing on the device screen, **omit the leading `/`** from the displayed strings — each line shows just the bare verb (`help`, `wifi`, `mqtt`, `bonds`, `redraw`, `status`, …) followed by its description. The slash is implied: it's part of the input syntax, not the UI vocabulary. (Historically the prefix was `cmd `; it was swapped to `/` to feel more like a chat-slash-command. The "strip the prefix from display" rule survives the swap.)

## What this is

An Arduino sketch turning an ESP32 (or ESP8266 D1 mini) into a self-contained "messenger" appliance: BLE keyboard for input, ST7789 240×320 TFT for display, and MQTT over TLS (HiveMQ Cloud) as the transport between paired devices. There is no host-side build system — everything is a `.ino` plus headers compiled by the Arduino IDE.

The goal is to provide a way for multiple people to communicate together as long as they can connect to a WiFi network.
It's kind of a very small "whatsapp"-like application.
Messages can be sent to a single person or to all persons using the devices.
Messages are received then displayed.
MQTT server is used to manage storage and transfer of messages.

## Build / flash

There is no Makefile, PlatformIO config, CI, or test harness. Build and flash via the Arduino IDE:

- Open `minimessenger.ino`.
- For ESP32: keep `#define PAC_ON_ESP32` at the top of the `.ino` (default). Tools → Boards: select an ESP32 board; Tools → Partition Scheme: **"Huge App" (1.9 MB / 320 KB SPIFFS)** — the default partition is too small.
- For ESP8266 D1 mini: switch to `#define PAC_ON_D1MINI`. Tools → Boards: "LOLIN(WEMOS) D1 R2 & mini".
- Serial monitor: 115200 baud.

Required libraries (Library Manager, exact versions known to work in comments):
- PubSubClient 2.8
- Adafruit ST7735 and ST7789 1.11.0 (pulls in Adafruit_GFX)
- **NimBLE-Arduino 2.5.0** (by h2zero) — replaces the bundled Bluedroid BLE library. Required to free enough heap (~50 KB) for the mbedtls TLS handshake to HiveMQ; with Bluedroid, the handshake fails with rc=-2 (only ~24 KB contiguous heap, mbedtls needs ~38 KB for default 16 KB IN + 16 KB OUT + SSL ctx). Do **not** also `#include <BLEDevice.h>` — calling Bluedroid's `BLEDevice::init()` alongside NimBLE wastes the savings.
- WiFi / WiFiClientSecure / SPI / Wire ship with the ESP32 / ESP8266 board packages. Time comes from the core's `configTime()` SNTP (no third-party NTP lib).

## Per-device identity

`identifyDevice()` in `minimessenger.ino` is the source of truth for device behaviour. It branches on the device's MAC and assigns: `g_deviceIdMe`, `g_deviceName` (e.g. `D1M_001`, `E32_004`), `g_userPseudo`, the two `g_deviceIdFriend*` IDs, the default recipient, and `g_displayType`. **Adding or moving a physical device requires editing this function** — there is no config file. An unknown MAC falls back to a random ID and pseudo "JohnDoe".

On ESP32 (arduino-esp32 3.3.8) reading the MAC reliably requires the WiFi driver to be in STA mode first. `identifyDevice()` therefore calls `WiFi.mode(WIFI_STA)` before `WiFi.macAddress(buffer)` — this brings up the netif without connecting and lets the MAC read return the real address. `setupWifi()` later issues `WiFi.disconnect(true,true) + mode + begin` on top of that already-initialised STA mode, which is well-defined. We initially tried `esp_read_mac(macBytes, ESP_MAC_WIFI_STA)` from `<esp_mac.h>` per the IDF docs (which claim the eFuse path is dependency-free), but on this core/version it silently returns `00:00:00:00:00:00` until the WiFi driver is up — so we stopped relying on it. On D1mini the legacy `WiFi.macAddress()` path is kept under `#else`.

## WiFi onboarding (multi-network + portal)

All WiFi-related code lives in **`wifi.ino`** (concatenated with `minimessenger.ino` by the Arduino IDE — they share one translation unit). The state machine, NVS-backed network list (Preferences namespace `"wifi"`), WiFiMulti integration, and WiFiManager captive portal are isolated there. The shared `WifiState` enum and exported function prototypes live in `wifi_state.h`, included by both files. Compile-time defaults come from `compiled_wifi.h` (gitignored, copied from `compiled_wifi.h.template`). The main sketch only calls `setupWifi()` once in `setup()` then `wifiTick(currentMillis)` every `loop()` — boot is non-blocking. The status screen (`showUpdatedInfoScreen`) branches on `wifiGetState()` to either show normal SSID/IP/MQTT/TIME rows or the captive-portal instructions. **See `docs/howto_wifi.md` for the full workflow documentation.**

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

`mm_blekb.{h,cpp}` wraps the **NimBLE-Arduino** stack into `MiniMessengerBLEKeyboardInterface`, which multi-inherits `NimBLEScanCallbacks` and `NimBLEClientCallbacks`. NimBLE folds the security callbacks (`onPassKeyEntry`, `onConfirmPasskey`, `onAuthenticationComplete`) into `NimBLEClientCallbacks`, so there is no third base class as there was under Bluedroid. The expected keyboard MAC is `KEYBOARD_MAC_ADDRESS` in the sketch. Bonding is persisted in NVS under NimBLE-specific keys (so the existing Bluedroid bonds are not reused — re-pair the keyboard the first time after the migration). Pass `true` to `setup()`'s `clearExistingBonds` to wipe and re-pair.

`tryToMaintainConnection()` is called every `loop()` iteration and drives the scan → connect → re-scan state machine. The key difference vs Bluedroid: in NimBLE, `NimBLEScan::start()` is **asynchronous** — it returns immediately and the scan runs in the background. The state machine therefore sets `doScan = false` right after starting a scan, and relies on the `onScanEnd()` callback to re-arm `doScan = true` when the scan window elapses without finding the keyboard. Don't reintroduce a blocking pattern here, or you'll stall MQTT.

Two NimBLE 2.x signature gotchas worth remembering: `onPassKeyEntry(NimBLEConnInfo&)` returns `void` (you call `NimBLEDevice::injectPassKey(connInfo, key)` to inject the value, not via the return); and scan/connect durations are in **milliseconds** (Bluedroid and NimBLE 1.x used seconds — multiply by 1000 if porting old snippets).

HID reports arrive in `decodeHIDReport()`, which maintains `kbIsCapsLockOn` locally (toggled on the keyboard's CapsLock keypress, since we don't read the LED state back from the keyboard) and uses `keymapLower` / `keymapUpper` lookup tables keyed by HID codes from `hid_keys.h`. Currently keystrokes accumulate into `g_currentMsg` but pressing Enter only logs "TODO Send message" — this path is not yet wired to MQTT publishing; serial input *is* wired (via `FLAG_READ_SERIAL_INPUTS`).

## LEDs

Three LEDs (`LED_STATUS`, `LED_FRIEND_1`, `LED_FRIEND_2`) are managed via a tiny non-blocking state machine: `g_ledRequiredState[pin]` holds OFF / ON / BLINK_FAST / BLINK_SLOW, and the `loop()` tail toggles blinking pins based on `LED_BLINK_*_DURATION`. Use `ledSetState(pin, state)` rather than `digitalWrite` directly so blink state stays consistent. The `LED_QTY` (17) array size is sized for raw GPIO numbers, not LED count.

## Logging

Custom variadic templates `hlog` / `hlogn` / `log` / `logn` (in the sketch) prefix output with `g_deviceName`. All logging is gated by `#define WITH_LOGS`; removing it is the production path (see TODO list at top of `.ino`). Don't replace these with bare `Serial.print` — you lose the device-name prefix that's essential when watching multiple devices on one serial console.

## Secrets

WiFi SSID/password are no longer in `minimessenger.ino` — they live in `compiled_wifi.h` (gitignored, copied from `compiled_wifi.h.template`) and/or NVS at runtime. See `docs/howto_wifi.md`. MQTT user/password and the HiveMQ Let's Encrypt root CA are still hardcoded in `minimessenger.ino`; treat the file as sensitive when sharing diffs externally and don't commit alternate credentials without the user's say-so.
