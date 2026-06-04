# Howto — Connecting a BLE keyboard to the ESP32

End-to-end reference for the BLE keyboard subsystem of minimessenger: how a HID-over-GATT keyboard advertises itself, how this project discovers and pairs with it, what the security and bonding choices mean, how HID reports become characters on screen, and what to do when it breaks. Source files involved: `mm_blekb.h`, `mm_blekb.cpp`, and the `decodeHIDReport()` block in `minimessenger.ino`.

This document assumes you already know what BLE is in broad strokes (a 2.4 GHz radio, packets, the central/peripheral roles). Everything else is explained here in terms of what the code actually does.

---

## 1. BLE in 30 seconds (only what you need)

A BLE link has two asymmetric roles:

- **Peripheral** — broadcasts an *advertising* packet on three channels (37/38/39) every advertising interval. Optionally responds to a `SCAN_REQ` with a `SCAN_RSP`. Goes to sleep between bursts. In our setup, this is the keyboard.
- **Central** — listens (scans). When it sees an interesting advertisement, it can send a `CONNECT_REQ` to upgrade from broadcast to a point-to-point link. In our setup, this is the ESP32.

After the link is up, the peripheral exposes data as a **GATT** tree:

```
Service (16-bit or 128-bit UUID)
└─ Characteristic (16-bit or 128-bit UUID)
   ├─ Value
   └─ Descriptors (e.g. CCCD = "subscribe to notifications" flag)
```

The central discovers services, then characteristics, then writes to the CCCD descriptor (UUID `0x2902`) to enable notifications, and finally receives notifications carrying the actual data.

**Active vs passive scan**: a passive scan only collects the primary advertising packet. An active scan also sends `SCAN_REQ` after each adv it likes and collects the `SCAN_RSP`. Many keyboards split their data — name in the adv, service UUIDs in the scan response. **We use active scan** (see `mm_blekb.cpp::setup()`), which is why our service-UUID filter sees the full picture.

---

## 2. HID over GATT — what makes a keyboard a keyboard at the BLE level

The Bluetooth SIG defines a profile called **HID over GATT** (HOGP), which is the protocol every modern BLE keyboard, mouse, gamepad, and stylus implements. Three things in particular are worth knowing:

### 2.1 The standard service UUIDs

| Identifier | Hex | Role | Constant in this project |
|---|---|---|---|
| HID Service | `0x1812` | Top-level service every HID peripheral exposes | `BT_SERVICE_HID_1812` (mm_blekb.h:16) |
| HID Report  | `0x2A4D` | The actual data characteristic — one instance per "Report ID" | `BT_CHAR_HID_REPORT_2A4D` (mm_blekb.h:17) |
| Report Map  | `0x2A4B` | Descriptor of the report layout (not parsed by this project) | — |
| HID Information | `0x2A4A` | Vendor metadata | — |
| Boot Keyboard Input | `0x2A22` | Legacy 8-byte boot report (alternative to Report) | — |

A peripheral advertising service `0x1812` is, by definition, a HID device. That is enough for our scan filter — we don't bother with the more specific characteristics until after connection.

### 2.2 Appearance (optional, but a strong signal when present)

The **Appearance** field is a 16-bit number broadcast in the adv data, indicating what the device IS. HID values live in the `0x03Cx` range:

| Value | Meaning | Constant |
|---|---|---|
| `0x03C0` | Generic HID | — |
| `0x03C1` | Keyboard | `BT_APPEARANCE_KEYBOARD_03C1` |
| `0x03C2` | Mouse | `BT_APPEARANCE_MOUSE_03C2` |
| `0x03C3` | Joystick | — |
| `0x03C4` | Gamepad | — |
| `0x03C5` | Digitizer Tablet | — |

**Beware**: Appearance is OPTIONAL. Many keyboards do NOT advertise it. So:
- Treating "Appearance == Keyboard" as a hard requirement → would reject legitimate keyboards.
- Treating "Appearance == Mouse" as a hard rejection → safe, because that's a positive signal that it's NOT a keyboard.

This project uses Appearance only as a **negative filter** (skip mice) — see `mm_blekb.cpp::onResult()`.

### 2.3 HID Report layout — Boot Keyboard

This is the 8-byte format the kernel-level "Boot Keyboard" protocol uses. Most BLE keyboards send their input either in this exact format or in a vendor-specific Report ID that has the same layout:

```
pData[0]    = modifier bitmask
              bit 0 = LCtrl, 1 = LShift, 2 = LAlt, 3 = LGUI,
              bit 4 = RCtrl, 5 = RShift, 6 = RAlt, 7 = RGUI
pData[1]    = reserved (always 0)
pData[2..7] = up to 6 currently-held HID key codes (6-key rollover)
```

The full HID Usage table for the keyboard usage page is in `hid_keys.h`. The two big things to internalise about this format:

1. **State, not events** — A new report is sent every time the held-key set changes. Slots `[2..7]` reflect what's pressed *right now*, not what just happened.
2. **6-key rollover** — Up to 6 simultaneous keys are reported. Slots are unordered; the same key can migrate from `pData[2]` to `pData[3]` from one report to the next.

The press-edge detection in `decodeHIDReport()` (`minimessenger.ino`) diffs the current slot set against the previous one to emit one event per *new* press — see section 7 below.

---

## 3. NimBLE-Arduino vs Bluedroid (why we switched)

The ESP32 Arduino core ships with two BLE stacks. Up until early 2026 this project used Bluedroid; it now uses **NimBLE-Arduino 2.5.0** (h2zero). The switch was forced by RAM budget, not by choice.

### 3.1 Heap budget

| Stack | Free heap after BLE+WiFi init | Largest contiguous block |
|---|---|---|
| Bluedroid | ~70 KB total | ~24 KB |
| NimBLE    | ~95 KB total | ~50 KB |

mbedtls needs ~38 KB contiguous heap for the HiveMQ TLS handshake (default 16 KB IN + 16 KB OUT + SSL ctx). With Bluedroid, the TLS handshake fails with `rc=-2` (out-of-memory). With NimBLE, it succeeds. This is why the project description ships the line:

> Do **not** also `#include <BLEDevice.h>` — calling Bluedroid's `BLEDevice::init()` alongside NimBLE wastes the savings.

### 3.2 API differences worth knowing (2.x signatures)

If you're porting Bluedroid snippets or older NimBLE 1.x snippets, watch for these:

| Concern | Bluedroid / NimBLE 1.x | NimBLE 2.x (current) |
|---|---|---|
| Pass-key callback | `uint32_t onPassKeyRequest()` | `void onPassKeyEntry(NimBLEConnInfo&)` — inject value with `NimBLEDevice::injectPassKey(ci, key)` |
| Security callbacks | Separate base class | Folded into `NimBLEClientCallbacks` |
| Scan duration | seconds | **milliseconds** (multiply by 1000 if porting) |
| Connect duration | seconds | milliseconds |
| Scan start | mostly blocking | **asynchronous** — returns immediately, results arrive in `onScanEnd` |
| Bond storage | NVS, Bluedroid keys | NVS, NimBLE-specific keys (existing Bluedroid bonds are NOT reused) |
| `getCharacteristics()` | sometimes blocking | iterates the already-discovered list |

The asynchronous-scan part is the one that bites hardest in practice. See section 5.3.

---

## 4. Security model used by this project

Configured once in `MiniMessengerBLEKeyboardInterface::setup()` (`mm_blekb.cpp`):

```cpp
NimBLEDevice::setSecurityAuth(true, false, false);   // bonding=YES, MITM=NO, SC=NO
NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
```

What that buys us:

| Setting | Value | Meaning |
|---|---|---|
| **Bonding** | true | The long-term key (LTK) is persisted in NVS after pairing. Subsequent reconnections skip pairing and just resume the encrypted session. |
| **MITM (Man-In-The-Middle protection)** | false | We don't verify out-of-band that we're talking to the *intended* peripheral — we'd need a display/keypad on the ESP for that. |
| **Secure Connections** | false | We don't require LE Secure Connections (P-256 ECDH). Legacy pairing is allowed for old/cheap keyboards that don't support SC. |
| **IO capability** | NO_INPUT_OUTPUT | We have neither input nor display from the BLE peer's perspective — so the only pairing method compatible with both peers is **Just Works**: no PIN exchange, no confirmation prompt. |

Consequence: an attacker that advertises a HID service with the right service UUID at the right moment can be paired with instead of the legitimate keyboard. For this project (an at-home messenger) the risk is acceptable. For anything security-sensitive, you'd need MITM=true + IO capability with at least a display (Passkey Entry pairing).

### 4.1 Where bonds live

NimBLE stores bonds in NVS under its own namespace. They survive reboots. They are forgotten when:

- The user runs `cmd bonds` (sent over serial or MQTT) — calls `NimBLEDevice::deleteAllBonds()`.
- The firmware calls `setup(clearExistingBonds=true, ...)` at boot — equivalent, just earlier.
- The NVS partition is erased (`esptool.py erase_flash`).

Bonds are keyed by the peer's resolvable identity (IRK if the peer uses RPAs, otherwise the public/static random address). A new physical keyboard = new identity = bonds don't match.

---

## 5. State machine — connect, disconnect, reconnect

The lifecycle of `MiniMessengerBLEKeyboardInterface` is driven from two places:
- `tryToMaintainConnection()` is called every iteration of the main `loop()`. It's the only place that initiates scans and connections.
- The async NimBLE callbacks (`onResult`, `onScanEnd`, `onConnect`, `onDisconnect`, `onAuthenticationComplete`) flip flags that `tryToMaintainConnection` reads on its next pass.

### 5.1 The state graph

```
                                  +------------------------+
                                  |   m_connectionDone=    |
                                  |        false           |
                                  |   doScan = true        |  ← boot
                                  +-----------+------------+
                                              |
                                tryToMaintainConnection()
                                              |
                                              v
                                  +-----------+------------+
                                  |  scan() started (async)|
                                  |  doScan = false        |
                                  +-----------+------------+
                                              |
                                              | (NimBLE scans in background)
                                              v
                +--------------- HID device   yes  ----+
                |   match in onResult() ?              |  no
                |                                      v
                v                                +-----+----------+
       +--------+--------+                       | onScanEnd()    |
       | doConnect=true  |                       | re-arms        |
       | scan stopped    |                       | doScan = true  |
       +--------+--------+                       +-------+--------+
                |                                        |
                v                                        v
       +--------+----------+                    (next loop iter)
       | connectToServer() |
       | bond / encrypt    |
       | subscribe HID rep |
       | m_connectionDone  |
       |       = true      |
       +--------+----------+
                |
                |  HID notifications stream in
                |  → bleNotifyCallback → decodeHIDReport
                v
       +--------+----------+
       | NOMINAL operation |
       +--------+----------+
                |
                |  signal lost / power lost / bond purged
                v
       +--------+----------+
       | onDisconnect()    |
       | m_connectionDone  |
       |       = false     |
       | doScan = true     |
       +--------+----------+
                |
                +-------------- back to top --------------+
```

### 5.2 Flags driving the state machine

| Flag | Owned by | Set to true by | Set to false by | Read by |
|---|---|---|---|---|
| `doScan` | instance | `setup()`, `onScanEnd()`, `onDisconnect()`, failed `connectToServer()` | `setup()` after starting the scan, `tryToMaintainConnection()` after re-starting it, `onResult()` after a match | `tryToMaintainConnection()` |
| `doConnect` | instance | `onResult()` after a match | `tryToMaintainConnection()` after attempting connect | `tryToMaintainConnection()` |
| `m_connectionDone` | instance | `connectToServer()` on success | `onDisconnect()` | `isFullyConnected()`, upper layer, scan loop |

### 5.3 The async-scan gotcha

`NimBLEScan::start(durationMs, blocking=false)` returns **immediately**. The scan continues in NimBLE's background task and:
- Each match fires `onResult()`.
- The scan window expiring fires `onScanEnd()`.

If you re-enter `tryToMaintainConnection()` while a scan is in flight and naively call `start()` again, you'll either get "scan already running" warnings or worse, restart the scan over and over without ever giving it time to see anything.

That's why `tryToMaintainConnection()` sets `doScan = false` right after calling `start()`, and only `onScanEnd()` (or a disconnect) re-arms it.

**Don't reintroduce a blocking pattern here** (e.g., `while (!found) { scan(); delay(...); }`). NimBLE 2.x scan is non-blocking by design, and blocking the main loop will stall MQTT — leading to disconnect/reconnect storms on HiveMQ.

---

## 6. Scan filters used in this project

`onResult()` in `mm_blekb.cpp` applies, in order:

1. **Positive filter on service UUID `BT_SERVICE_HID_1812`** — must be advertising HID-over-GATT. Anything else (phones, headsets, watches) is silently ignored. Cheap (no `SCAN_REQ` overhead beyond what was needed anyway).
2. **Negative filter on Appearance `BT_APPEARANCE_MOUSE_03C2`** — if the device explicitly publishes itself as a mouse, skip without attempting connect. Acts only on a positive signal; absence of Appearance is treated as "unknown, give it a try".

No name filter is applied. Any keyboard model from any vendor — Logitech K380, MX Keys, Apple Magic Keyboard, generic ChiCony — will be picked up. This is intentional: the previous name-based filter required recompiling to swap keyboards.

If two keyboards both advertise simultaneously, the **first one observed** is picked. NimBLE doesn't expose RSSI ordering in the callback, so this is non-deterministic. To pin to a specific physical keyboard, you'd need to extend the filter with a stored MAC (planned, not yet implemented — see section 11).

---

## 7. From HID report to ASCII character

`bleNotifyCallback()` (`mm_blekb.cpp`) is the NimBLE entry point. It just forwards `pData`/`length` to the upper-layer callback installed at setup time, which is `onBluetoothKeyboardNotifyCallback` → `decodeHIDReport` (`minimessenger.ino`).

`decodeHIDReport` does three things:

1. **Wake the screen** — `noteActivity()`. If the screen was off, the report is consumed *only* to wake (the user pressed a key to light the screen, not to type).
2. **Edge-detect press events** — keeps the previous report's 6-slot key set in a static buffer. For each key in the new report that wasn't in the previous one, emit *one* press event. This is what makes auto-repeat invisible (you'd otherwise get duplicates while the user holds a key) and lets fast two-hand typing work (a single report can carry [A, B] when both go down in the same notification window).
3. **Translate to ASCII** — using `keymapLower` / `keymapUpper` indexed by HID code, applying the current Shift/CapsLock state. Special keys (Backspace, Enter, ESC, Tab, arrows) are intercepted before the lookup.

The HID Usage codes vs ASCII mapping is hardcoded in `keymapLower[128]` / `keymapUpper[128]` in `minimessenger.ino`. It's a US-layout-ish mapping. For non-US keyboards (AZERTY, etc.) you'd extend or swap those tables.

Multi-byte UTF-8 characters from the keyboard side are NOT generated by this project — the keyboard sends a HID Usage code, which `decodeHIDReport` maps to a single ASCII byte. Accented characters from MQTT arrive as UTF-8 and are rendered by the display path (see `docs/howto_fonts.md`), but composing them on the BLE keyboard itself would require an extended key composer.

---

## 8. Local commands relevant to BLE

Sent either via the serial monitor or via MQTT publish to `msg/unicast/<deviceId>` / `msg/broadcast`. Defined in `minimessenger.ino`:

| Command | Effect |
|---|---|
| `cmd bonds` | `NimBLEDevice::deleteAllBonds()` — wipes all BLE bonds from NVS. Use to swap to a new physical keyboard. Does NOT disconnect the currently-active session (planned improvement). |
| `cmd wifi` | `WiFi.disconnect()` — useful when debugging reconnection logic. |
| `cmd mqtt` | `g_mqttClient.disconnect()` — same, on the MQTT side. |

The funnel that picks these up is `processMessage(message, source)` in `minimessenger.ino`. See its comment block for the wake + interpret + dispatch flow.

---

## 9. Bringing up a brand-new keyboard

Three scenarios, with concrete actions:

### 9.1 First-ever pairing (NVS empty of bonds)

1. Power the keyboard. Press its pairing button (most keyboards: hold Fn+P or a dedicated button until the LED blinks fast).
2. The ESP, with no keyboard currently connected, has its scan running continuously.
3. `onResult()` matches on HID UUID → connects → Just Works pairing → bond stored in NVS → `onAuthenticationComplete()` fires with `isEncrypted() == true`.
4. Done. Subsequent reboots will silently reconnect.

### 9.2 Replacing a keyboard (same name or different, doesn't matter)

The previous bond is still in NVS but tied to the old keyboard's identity. The new keyboard will fail pairing (LTK mismatch).

1. From the serial monitor (or via another device publishing MQTT), send `cmd bonds`.
2. The ESP wipes NVS bonds.
3. Put the new keyboard in pairing mode.
4. The ESP's scan picks it up and bonds fresh.

**Caveat** — `cmd bonds` doesn't currently force-disconnect the active client. If the old keyboard is still connected (still in range, still paired in NimBLE's RAM session), you'd want to also power it off / take it out of range before the new one pairs. A future improvement will make `cmd bonds` also call `client->disconnect()` and restart the scan.

### 9.3 Lost bond / weird state ("nothing connects anymore")

When in doubt:

1. `cmd bonds` over serial.
2. Reboot the ESP (`Ctrl+T R` in Arduino IDE serial monitor, or the EN button).
3. Put the keyboard in pairing mode.

If that still fails, see troubleshooting below.

---

## 10. Troubleshooting

| Symptom | Likely cause | What to check / try |
|---|---|---|
| `Found HID device` log appears but `Connection failed` follows | Pairing window expired on the keyboard side, or keyboard is bonded with another host and refusing to pair | Re-put keyboard in pairing mode; or `cmd bonds` + keyboard reset. |
| `HID service 0x1812 not found` after a connect | False positive on the scan filter — the device advertised the UUID but didn't actually expose the service (rare, usually a misconfigured DIY peripheral) | Scan callback will rescan automatically. If it loops on the same device, narrow the filter (add Appearance == Keyboard if your real keyboard publishes it). |
| `No notifiable HID Report (0x2A4D) characteristic found` | Connected to a HID device that has no input reports (some output-only HID devices exist) | Same — rescan. |
| Connects then disconnects immediately, reason code visible in `onDisconnect` log | reason `0x13` = remote user terminated, `0x16` = local host terminated, `0x22` = LMP response timeout, `0x3D` = MIC failure (bad LTK) | `0x3D` specifically means stale bonds — run `cmd bonds`. Other codes usually mean RF/range/timing issues. |
| TLS to HiveMQ fails with `rc=-2` after a recent change | Heap exhausted. Did something pull in Bluedroid alongside NimBLE? | Search the project for `#include <BLEDevice.h>` and remove. Verify NimBLE-Arduino version 2.5.0. |
| Scan never finds anything but the keyboard's LED says it's advertising | Keyboard advertises only in SCAN_RSP and active scan is disabled | Confirm `pBLEScan->setActiveScan(true)` is in effect in `setup()`. |
| Mouse keeps getting picked up | Mouse advertises HID UUID + no Appearance | Add a stricter filter (e.g., require Appearance == Keyboard); accept tighter coverage at the cost of rejecting Appearance-less keyboards. |
| MQTT goes silent for ~30 s after a reconnect storm of the keyboard | Blocking pattern reintroduced somewhere in the BLE path | Profile `loop()` iteration time. NimBLE 2.x scan/connect calls must remain non-blocking. |
| `tryToMaintainConnection()` keeps logging `Rescanning...` every loop | Either `onScanEnd` is firing too eagerly or `doScan` is being re-armed by a bug | Check the `doScan = false` line right after `start()` in `tryToMaintainConnection()` is still there. |

Useful logs to grep for:

```
[BTKB] setup()
[BTKB] Found HID device
[BTKB] Connected
[BTKB] Authentication complete
[BTKB] Client disconnected (reason=...)
[BTKB] Scan ended (reason=..., N devices seen)
```

Verbose BLE debug from NimBLE itself can be enabled by setting the log level for the `NimBLEDevice` tag in `mm_log.h` — see how it's done for `TAG_BTKB`.

---

## 11. Known limitations / future work

- **Active session not disconnected when bonds are cleared** — `cmd bonds` purges NVS but the in-RAM session survives until natural disconnect. Add a `client->disconnect()` + scan restart inside the command handler.
- **No MAC-based pinning** — once a keyboard is bonded, we don't store its MAC explicitly, so we can't bias future scans toward it when multiple HID devices are in range. Mitigation idea: capture the MAC during `onAuthenticationComplete`, persist in `Preferences` (NVS), and add a "match this MAC first" branch in `onResult()` that falls back to the generic HID filter if not found.
- **Single keymap layout** — `keymapLower` / `keymapUpper` in `minimessenger.ino` are roughly US/UK. AZERTY users will get the wrong characters until the tables are localised.
- **No LED feedback on the keyboard** — the keyboard's CapsLock LED stays in its hardware-toggled state; we don't push the output report to control it. `kbIsCapsLockOn` is tracked locally only.
- **Just Works pairing** — no MITM protection. Acceptable for the use case but worth knowing.

---

## 12. References

- Bluetooth SIG Assigned Numbers — <https://www.bluetooth.com/specifications/assigned-numbers/>
  - Service UUIDs are in "16-bit UUIDs.pdf"
  - Appearance values are in "Appearance Values.pdf"
- HID over GATT Profile (HOGP) specification — Bluetooth SIG.
- USB HID Usage Tables — <https://usb.org/document-library/hid-usage-tables-15> (the keyboard usage page maps the HID codes we use in `hid_keys.h`).
- NimBLE-Arduino documentation — <https://h2zero.github.io/esp-nimble-cpp/>
- ESP-IDF NimBLE host stack documentation — for the underlying `BLE_HS_*` constants and reason codes.
