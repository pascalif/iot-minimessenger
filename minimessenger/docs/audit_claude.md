# Code audit — minimessenger

<!-- pac-audit-arduino: machine-readable header — do not hand-edit -->
<!--
last_run: 2026-05-03T15:24:56Z
files_scanned: 6
counters: { BUG: 3, LOGIC: 3, EDGE: 0, MEM: 3, PERF: 2, SEC: 7, OBS: 3, DUP: 1, LANG: 1 }
fixed: [ BUG-001, BUG-002, BUG-003, LOGIC-001, PERF-001, OBS-001, OBS-003, PERF-002 ]
-->

**Last run:** 2026-05-03 15:24 UTC
**Project root:** `/home/pascal/Dev/workspace_pascal/arduino/pascal_projects/minimessenger`
**Files scanned:** 6 (`minimessenger.ino`, `mm_blekb.cpp`, `mm_blekb.h`, `display.h`, `symbols.h`, `hid_keys.h`)

## Summary

| Severity | Count |
|----------|-------|
| CRITICAL | 0     |
| HIGH     | 0     |
| MEDIUM   | 4     |
| LOW      | 11    |
| INFO     | 0     |

- Active: **15**
- Fixed since last run: **8** (BUG-001, BUG-002, BUG-003, LOGIC-001, PERF-001, OBS-001, OBS-003, PERF-002)
- Carried over: **15**
- Suppressed by Won't fix: **0**

IDs of fixed issues are retired and will not be reused — the counters above
are the high-water mark for ID allocation, not the active count.

## Active issues

Ordered by severity, then category, then ID.

### MEDIUM

#### MEM-001 — `String` use in display redraw causes heap fragmentation

- **File:** `minimessenger.ino:955-981`, `display.h` (TextLine)
- **Category:** MEM
- **First seen:** 2026-05-03

`TextLine` stores both the timestamp and the message as Arduino `String`. Each `addConversationBlock()` triggers a full `redrawAllConversations()` and the ring-shift assigns `lines[i-1] = lines[i]` (deep copy of two `String`s per shift), then `g_disp->print(line.ts/msg)` walks the array. Every conversation entry produces multiple small allocations of varying sizes — exactly the pattern that fragments the ESP heap over hours/days of uptime. Symptoms appear as eventual `String` allocation failures (silent truncated text) or MQTT/BLE allocation failures attributed to "random" instability.

**Recommendation:** store messages as fixed-size `char[]` inside `TextLine`, or replace the array shift with a true ring buffer (`head`/`tail` indices, no copy). At minimum, call `String::reserve(MAX_MSG_LEN)` on the strings stored in `TextLine` to stabilise capacity.

```cpp
1043: while (g_nextTextTopY + ... >= g_disp->height() || lineCount >= MAX_LINES) {
1044:   for (int i = 1; i < lineCount; i++) {
1045:     lines[i - 1] = lines[i];   // deep String copy each shift
```

#### SEC-001 — `strcpy()` into `g_userPseudo` without bounds

- **File:** `minimessenger.ino:650,660,667,680,691`
- **Category:** SEC
- **First seen:** 2026-05-03

Five call-sites use `strcpy(g_userPseudo, "...")`. The literals are short and safe today, but if `g_userPseudo` ever becomes user-configurable (serial command, MQTT-pushed pseudo, web UI) those lines turn into a textbook overflow. CWE-120-class smells should not survive in firmware that talks to a public broker.

**Recommendation:** replace with `snprintf(g_userPseudo, sizeof(g_userPseudo), "%s", "...")`. Cheap, fixes the latent risk in one pass.

```cpp
650:     strcpy(g_userPseudo, "Papa");
660:     strcpy(g_userPseudo, "Maïa");
691:     strcpy(g_userPseudo, "JohnDoe");
```

#### SEC-002 — `atoi()` on unvalidated MQTT payload

- **File:** `minimessenger.ino:1430,1433`
- **Category:** SEC
- **First seen:** 2026-05-03

`onMqttIncomingMessage()` calls `atoi(message.c_str())` to extract a remote device ID from `admin/live` and `admin/dead`. `atoi()` returns `0` on non-numeric input and has no error signal. A peer (or a misbehaving broker) publishing a malformed payload silently becomes "device 0" and toggles the friend-presence LEDs against the wrong target. Combined with the lack of message authentication (SEC-003), it is also a trivial spoof primitive.

**Recommendation:** use `strtol()` with `errno`/`endptr` checking, reject non-numeric / out-of-range values, and ignore the message when validation fails.

```cpp
1429: if (strcmp(topic, g_mqttOutgoingTopicLive) == 0) {
1430:   int remoteDeviceId = atoi(message.c_str());
1431:   onLiveness(remoteDeviceId, true);
1432: } else if (strcmp(topic, g_mqttOutgoingTopicWill) == 0) {
1433:   int remoteDeviceId = atoi(message.c_str());
```

#### SEC-003 — MQTT messages have no authentication or integrity check

- **File:** `minimessenger.ino:821-837` (mqttPushFormattedMessage)
- **Category:** SEC
- **First seen:** 2026-05-03

The protocol embeds `deviceId` in the payload trailer, but anything that can publish to the topic (any peer, the broker itself if compromised, anyone who learns the credentials) can forge that field and impersonate another device. There is no nonce / sequence number / signature — replays are also accepted. For a closed personal appliance this is a known limitation; it is still worth recording so the threat model stays explicit.

**Recommendation:** add an HMAC-SHA256 trailer keyed off a per-device secret (still hardcoded, still LOW threat surface), include `msgId` in the MAC input to defeat replay, and reject messages whose MAC does not verify. Alternatively, configure HiveMQ ACLs so each `deviceId` can only publish from its own MQTT user, and verify the topic source on receive.

```cpp
821: void mqttPushFormattedMessage(const char* topic, const char* payload) {
822:   snprintf(g_mqttOutgoingMsg, MSG_BUFFER_SIZE,
823:            "%s ### ts:%s deviceId:%d msgId:%d",
824:            payload, getCurrentDateTime(), g_deviceIdMe, g_mqttOutputMsgId);
```

### LOW

#### LOGIC-002 — `redrawAllConversations` iterates all 40 slots instead of `lineCount`

- **File:** `minimessenger.ino:955`
- **Category:** LOGIC
- **First seen:** 2026-05-03

The range-for `for (auto& line : lines)` walks all `MAX_LINES` (40) entries every redraw, then guards the body with `if (!line.ts.isEmpty())`. A commented-out `for (int i = 0; i < lineCount; i++)` is right above. Functionally equivalent thanks to default-constructed empty `String`s, but wasteful and obscures intent.

**Recommendation:** restore the index-based loop.

```cpp
953: //for (int i = 0; i < lineCount; i++) {
955: for (auto& line : lines) {
956:   if (!line.ts.isEmpty()) {
```

#### LOGIC-003 — Ring-shift logic in `addConversationBlock` relies on shallow-copy semantics

- **File:** `minimessenger.ino:1044-1047`
- **Category:** LOGIC
- **First seen:** 2026-05-03

The shift loop `for (int i = 1; i < lineCount; i++) lines[i-1] = lines[i];` works because `TextLine` has no custom destructor or move constructor — assignment is a member-wise copy of `String`s. If anyone adds RAII-managed resources to `TextLine` (file handles, owning pointers, etc.) the shift becomes a leak/double-free. Worth replacing with a real ring buffer regardless (also resolves part of MEM-001).

**Recommendation:** switch to head/tail indices, no shifting.

```cpp
1044: for (int i = 1; i < lineCount; i++) {
1045:   lines[i - 1] = lines[i];
1046: }
```

#### MEM-002 — `new Adafruit_ST7789` in `setupDisplay` never paired with `delete`

- **File:** `minimessenger.ino:899,908`
- **Category:** MEM
- **First seen:** 2026-05-03

`setupDisplay()` is called exactly once today, so this is not an actual leak — but the global `g_disp` is a raw owning pointer, which is a fragile pattern. If anyone re-runs `setupDisplay()` (e.g. for the not-yet-implemented OLEDSHIELD branch, or after a soft reconfigure) the prior allocation is silently abandoned.

**Recommendation:** either (a) make `g_disp` a static instance instead of heap-allocated, or (b) `delete g_disp` at the top of `setupDisplay()` before reassigning.

```cpp
899: Adafruit_ST7789* pDisp = new Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
908: g_disp = pDisp;
```

#### MEM-003 — `pServerAddress = new BLEAddress(...)` leaks on rescan

- **File:** `mm_blekb.cpp:92`
- **Category:** MEM
- **First seen:** 2026-05-03

When the scanner finds the keyboard, the previous `BLEAddress` is overwritten without `delete`. Each unpair/rescan cycle leaks a small object. Practically negligible for the typical 1-pair-per-boot lifetime, but the fix is one line.

**Recommendation:**

```cpp
if (pServerAddress) delete pServerAddress;
pServerAddress = new BLEAddress(advertisedDevice.getAddress());
```

#### SEC-004 — Hardcoded WiFi and MQTT credentials in source

- **File:** `minimessenger.ino:122-131`
- **Category:** SEC
- **First seen:** 2026-05-03

Acknowledged in `CLAUDE.md`: "Treat the file as sensitive when sharing diffs externally." Down-graded to LOW per the audit rubric. Re-flagged here only so the trade-off stays visible in the report.

**Recommendation:** none required; if the device count ever grows, migrate to `WiFiManager` + NVS-stored MQTT creds.

```cpp
122: const char* ssid = "SatelliteThree";
123: const char* password = "xxxxxxx";
130: const char* mqtt_user = "xxxxx";
131: const char* mqtt_password = "xxxxxxx";
```

#### SEC-005 — Hardcoded HiveMQ root CA (Let's Encrypt ISRG Root X1)

- **File:** `minimessenger.ino:144-175`
- **Category:** SEC
- **First seen:** 2026-05-03

ISRG Root X1 expires **2035-06-04**, so no immediate rotation is needed; documenting it makes the next-flash-by-then constraint explicit. If HiveMQ ever switches its chain (e.g. ISRG Root X2, Sectigo, etc.) before then, devices will need a firmware update.

**Recommendation:** add an inline comment with the expiry date next to the literal, and note the rotation procedure in `docs/`.

```cpp
144: const char* root_ca = "-----BEGIN CERTIFICATE-----\n..."
```

#### SEC-006 — MQTT keepalive payload leaks WiFi SSID and device IP

- **File:** `minimessenger.ino:807-815` (mqttSendAlive)
- **Category:** SEC
- **First seen:** 2026-05-03

The retained `admin/live` payload includes `ssid:<SSID> ip:<IP>`. Anyone with broker read access (which today is anyone with the shared credentials) can map device IDs to networks. Not a high-impact leak for a personal appliance, but a free improvement.

**Recommendation:** drop `ssid` and `ip` from the alive payload; keep `deviceId`, `mac`, `recoId`. Add them back behind a debug flag if needed.

```cpp
809: snprintf(payload, MSG_BUFFER_SIZE,
810:          "%d %s mac:%s ssid:%s ip:%s recoId:%d",
811:          g_deviceIdMe, ...);
```

#### SEC-007 — `random(100, 1000)` after `analogRead`-seeded RNG for fallback device ID

- **File:** `minimessenger.ino:692`, seeding context elsewhere
- **Category:** SEC
- **First seen:** 2026-05-03

Used only for an unknown-MAC fallback device ID, which is a UI label, not a security token. Recording so this never silently graduates into an authentication path.

**Recommendation:** keep, but add a `// not for security` comment to discourage reuse. If you ever need an actual nonce on ESP32, call `esp_random()`.

```cpp
691: strcpy(g_userPseudo, "JohnDoe");
692: g_deviceIdMe = random(100, 1000);
```

#### OBS-002 — PubSubClient 2.8 is functional but unmaintained

- **File:** `minimessenger.ino:76-77`
- **Category:** OBS
- **First seen:** 2026-05-03

Pinned at 2.8 in CLAUDE.md; widely-known limitations (16-bit packet length, no QoS 2, fragile reconnect). Fine for this project's volume.

**Recommendation:** track for migration to `ArduinoMqttClient` (or `MQTT` by Joel Gähwiler) only if you hit a concrete limit. Not urgent.

```cpp
76: // Install from library manager: "PubSubClient" (2.8)
77: #include <PubSubClient.h>
```

#### DUP-001 — `if (g_displayType == DisplayType::ST7789) { ... } else { hlogn("DISPLAY_TYPE_NOT_CONFIGURED"); }` repeated in 6+ functions

- **File:** `minimessenger.ino:897-911,915-933,1087-1098,1101-1109,1117-1124,1130-1216`
- **Category:** DUP
- **First seen:** 2026-05-03

The same skeleton — branch on `g_displayType`, log the not-configured message in the OLEDSHIELD arm — appears in `setupDisplay`, `showSplashScreen`, `cleanScreen`, `redrawAllConversations`, `addConversationBlock`, and `showUpdatedInfoScreen`. Adding a real OLEDSHIELD implementation later means touching every site.

**Recommendation:** introduce a thin abstraction — a `Display` interface with `ST7789Display` and `OLEDDisplay` implementations, or even just a `displaySupported()` early-return helper. For the OLEDSHIELD log, a single helper `logDisplayNotConfigured(const char* fn)` removes most of the repetition immediately.

```cpp
897: void setupDisplay() {
898:   if (g_displayType == DisplayType::ST7789) { ... }
910:   else { hlogn("setupDisplay: DISPLAY_TYPE_NOT_CONFIGURED"); }
912: }
1130: void showUpdatedInfoScreen(bool withMQTTInfo) {
1131:   if (g_displayType == DisplayType::ST7789) { ... }
1215:   else { hlogn("showUpdatedInfoScreen: DISPLAY_TYPE_NOT_CONFIGURED"); }
```

#### LANG-001 — `"Ready !"` uses French spacing in an English string

- **File:** `minimessenger.ino:1350`
- **Category:** LANG
- **First seen:** 2026-05-03

Displayed on the TFT after MQTT connect succeeds. The space before `!` is the French typographic rule applied to an otherwise English word. The rest of the user-visible UI is consistent on this point — only this string mixes conventions.

**Recommendation:** `"Ready!"` (no space). If bilingual intent exists, swap to `"Prêt !"`.

```cpp
1350: addConversationBlock("", "Ready !", CONVO_INFO_COLOR, CENTER);
```

## Fixed

These issues were flagged by a prior audit run and have since been addressed
by code changes. IDs are retired and never reused. The original body of each
entry is kept here verbatim for traceability — line numbers refer to the code
at the time the issue was flagged, not to the current tree.

#### BUG-001 — _(no body retained; fixed before the 2026-05-03 audit run)_

#### BUG-002 — _(no body retained; fixed before the 2026-05-03 audit run)_

#### BUG-003 — `sprintf()` for MAC formatting in `mm_blekb`

- **File:** `mm_blekb.cpp:30`
- **Category:** BUG
- **First seen:** 2026-05-03

The buffer is exact-fit (`char[18]` for `"XX:XX:XX:XX:XX:XX"`), so it does not actually overflow today. But `sprintf` should not exist in modern firmware — every audit / static analyzer flags it, and the cost of switching is one character.

**Recommendation:** `snprintf(bda_str, sizeof(bda_str), ...)`.

```cpp
29: char bda_str[18];
30: sprintf(bda_str, "%02X:%02X:%02X:%02X:%02X:%02X", ...);
```

#### LOGIC-001 — Comma instead of semicolon ends `g_mqttWasConnected = true`

- **File:** `minimessenger.ino:790`
- **Category:** LOGIC
- **First seen:** 2026-05-03

`g_mqttWasConnected = true,` followed on the next line by `g_mqttConnectionId++;` parses as a comma-expression statement: the assignment IS executed (the comma evaluates the LHS for side effects), so the code happens to behave correctly. But this is a typo — one accidental edit on the next line and the assignment vanishes. Static analyzers will flag this every time.

**Recommendation:** change the trailing `,` to `;`.

```cpp
790:     g_mqttWasConnected = true,
791:     g_mqttConnectionId++;
```

#### PERF-001 — Unconditional `delay(500)` at end of `loop()` starves BLE / keystroke handling

- **File:** `minimessenger.ino:1419`
- **Category:** PERF
- **First seen:** 2026-05-03

Every loop iteration ends with a 500 ms blocking delay. `mm_blekb.tryToMaintainConnection()`, the LED state machine tick, and any incoming-keystroke processing all run at most twice per second. CLAUDE.md explicitly notes that `loop()` must not block longer than "a couple of seconds" or BLE reconnection stalls — 500 ms is the same order of magnitude and clearly already hurts perceived responsiveness when typing.

**Recommendation:** remove the `delay(500)`. If a small yield is needed, replace with `delay(1)` (which calls `yield()` on ESP) and gate other actions on `millis()` deltas as the rest of the codebase already does.

```cpp
1419:   delay(500);
1420: }
```

#### PERF-002 — Blocking `delay(MQTT_CONNECT_RETRY_INTERVAL)` in MQTT reconnect failure path

- **File:** `minimessenger.ino:1352`
- **Category:** PERF
- **First seen:** 2026-05-03

When `mqttReconnect()` fails, the code blocks the entire loop for `MQTT_CONNECT_RETRY_INTERVAL` (5 s). During that window, BLE reconnection, keystroke handling, NTP, and the LED tick are all frozen. The retry-interval gate at line 1344 (`currentMillis - g_mqttLastReconnectTryTimestampMs > MQTT_CONNECT_RETRY_INTERVAL`) is already designed to be a non-blocking back-off — the `delay()` is redundant **and** harmful.

**Recommendation:** remove the `delay(MQTT_CONNECT_RETRY_INTERVAL)` and ensure `g_mqttLastReconnectTryTimestampMs = currentMillis;` is set in the failure branch so the existing time-gate handles the back-off.

```cpp
1346:   if (mqttReconnect()) {
1347:     showUpdatedInfoScreen(true);
1348:     delay(1000);
1349:     cleanScreen();
1350:     addConversationBlock("", "Ready !", CONVO_INFO_COLOR, CENTER);
1351:   } else {
1352:     delay(MQTT_CONNECT_RETRY_INTERVAL);
1353:   }
```

#### OBS-001 — Bluedroid BLE stack is the legacy stack on ESP32; NimBLE-Arduino is recommended

- **File:** `mm_blekb.h:1-10`, `mm_blekb.cpp` (full file)
- **Category:** OBS
- **First seen:** 2026-05-03

The sketch uses the legacy `BLEDevice` / `BLEScan` / `BLESecurity` API from the in-tree ESP32 BLE Arduino library (Bluedroid). Espressif now recommends NimBLE-Arduino: ~50 % less RAM and flash, more responsive scanning, and an API that is more stable across core versions (the recent `setEncryptionLevel` removal and `BLEAdvertisedDeviceCallbacks → BLEScanCallbacks` rename in core 3.x are exactly the kind of churn NimBLE avoids). Memory pressure from `String` use in the display path (MEM-001) makes any RAM win valuable.

**Recommendation:** install `NimBLE-Arduino`, port `mm_blekb.cpp` to its API (drop-in for most call-sites; security callbacks differ). Treat it as a one-evening migration, then re-baseline binary size.

```cpp
1: #include "BLEDevice.h"
3: #include <BLEDevice.h>
4: #include <BLEUtils.h>
5: #include <BLEClient.h>
6: #include <BLEAddress.h>
7: #include <BLEScan.h>
8: #include <BLESecurity.h>
```

#### OBS-003 — `NTPClient` library duplicates ESP32's built-in `configTime()` SNTP

- **File:** `minimessenger.ino:91-92,340`
- **Category:** OBS
- **First seen:** 2026-05-03

ESP32 core ships full SNTP via `configTime(offset, dst, server)` plus `time()` / `localtime()`. Removing `NTPClient` saves a couple of KB of flash and a `WiFiUDP` instance.

**Recommendation:** on ESP32 only, replace with `configTime(NTP_UTC_OFFSET_S, 0, "europe.pool.ntp.org")` and read `time(nullptr)` for the epoch. Keep `NTPClient` on the ESP8266 branch where the ergonomics are uglier.

```cpp
91: // Install from library manager: "NTPClient" (3.2.1)
92: #include <NTPClient.h>
340: NTPClient g_timeClient(ntpUDP, "europe.pool.ntp.org", NTP_UTC_OFFSET_S, NTP_UPDATE_INTERVAL_MS);
```

## Won't fix

These issues have been reviewed by the user and are intentionally not addressed.
The audit run does **not** modify this section automatically.

_(empty — populate by moving items from "Active issues" into this section, keeping their ID, title, file, category, and adding a `Dismissed:` date and `Reason:` line.)_
