# Code audit — minimessenger

<!-- pac-audit-arduino: machine-readable header — do not hand-edit -->
<!--
last_run: 2026-06-02T18:30:00Z
files_scanned: 17
counters: { BUG: 5, LOGIC: 8, EDGE: 1, MEM: 7, PERF: 8, SEC: 9, OBS: 3, DUP: 5, LANG: 2, COMMENT: 2, HW: 5 }
-->

**Last run:** 2026-06-02 18:30 UTC
**Project root:** `/home/pascal/Dev/workspace_pascal/iot-minimessenger/minimessenger`
**Files scanned:** 17 (`minimessenger.ino`, `wifi.ino`, `mqtt.ino`, `commands.ino`, `contacts.ino`, `bars.ino`, `strings.ino`, `mm_blekb.cpp`, `mm_blekb.h`, `mm_log.h`, `mqtt.h`, `wifi.h`, `contacts.h`, `display.h`, `hid_keys.h`, `symbols.h`, `personal-data.h`)

## Summary

| Severity | Count |
|----------|-------|
| CRITICAL | 0     |
| HIGH     | 1     |
| MEDIUM   | 3     |
| LOW      | 23    |
| INFO     | 0     |

- Active: **27**
- New this run (currently active): **8** (BUG-005, LOGIC-007, MEM-005, MEM-006, MEM-007, PERF-008, DUP-004, DUP-005)
- Carried over: **19**
- Retired since last run (fixed in code; IDs not recycled): **6** (COMMENT-001, COMMENT-002, BUG-004, LOGIC-008, PERF-006, PERF-007)
- Won't fix: **3** (SEC-009, LANG-002 merged with LANG-001, HW-005)
- Suppressed by Won't fix: **0**

The counters in the header are the high-water mark for ID allocation. Retired IDs (issues fixed by code changes) are not listed here but must never be reused for new findings — bump the counter, do not recycle.

## Hardware inventory (this run)

- **MCU:** ESP32 (primary, arduino-esp32 3.3.8) with alternate ESP8266 D1 mini target via `#define PAC_ON_D1MINI`.
- **Display:** ST7789 240×320 TFT, SPI; **backlight hardwired to 3.3 V** (TFT_BL = -1, no PWM control possible).
- **Radios:** Wi-Fi (built-in) + Bluetooth LE via NimBLE-Arduino 2.5.0 (HID-over-GATT keyboard client).
- **LEDs/Input:** 3 status LEDs (GPIO32 / GPIO33 / GPIO25); BLE keyboard for input; serial console for debug.
- **Power source:** USB only — documented in [`README.md`](../README.md). No battery, no sleep strategy by design.
- **Sensors / actuators:** none.

## Active issues

Ordered by severity, then category, then ID.

### HIGH

#### BUG-005 — LED state arrays indexed by raw GPIO pin number; size 17 < highest pin (33) corrupted BSS on every call

- **File:** `minimessenger.ino:378-381` (array sizing), `907-925` (`ledSetState` / `ledCommuteBlinkState`), `161-163` (`LED_STATUS=GPIO32`, `LED_FRIEND_1=GPIO33`, `LED_FRIEND_2=GPIO25`)
- **Category:** BUG
- **First seen:** 2026-06-02

`g_ledRequiredState[LED_QTY]`, `g_ledBlinkStateIsHigh[LED_QTY]`, and `g_ledBlinkLastTimestampMs[LED_QTY]` were sized at `LED_QTY = 17` (inherited from the D1 mini era when GPIO topped out at 16). `ledSetState(pin, state)` indexes those arrays by **raw GPIO pin number** — i.e. 32 / 33 / 25 on the ESP32 build — so every LED state change wrote 8 to 16 bytes (and `g_ledBlinkLastTimestampMs` 60+ bytes) past the end of the arrays, straight into adjacent BSS globals. The corruption was very visible on `/status`: `LED_STATE_BLINK_FAST = 2` written via `g_ledRequiredState[32] = 2` landed inside the (then-existing) `g_deviceName` buffer, and since byte `0x02` is the second smiley glyph of the Adafruit default GLCD 5×7 font, the "Name:" row rendered as `E32_0` followed by a smiley face (truncated there by the `0x00` produced one byte later by `ledSetState(LED_FRIEND_1=33, LED_STATE_OFF=0)` acting as a parasitic null terminator). Bumping `G_USER_PSEUDO_LEN` (10 → 11 → 40) shifted the corruption to a different offset — making the bug appear to depend on identity-buffer sizing and obscuring the real cause for a while.

The original victims (`g_deviceName`, `g_userPseudo`, and friends) have since been merged into a single `g_deviceData` record (with a `name()` member returning a static-buffer-backed formatted string), so the exact "E32_0 + smiley" symptom can't recur in the same form. The latent risk class is unchanged though: the OOB writes still land on *some* BSS global depending on link order — function pointers, MQTT state, the contact table, font tables consulted via PROGMEM-resident wrappers, etc. — and the manifestations would be wildly different (silent malfunction, hard fault, random crash) on the next refactor that reshuffles BSS layout.

**Immediate fix already applied (this run):** `LED_QTY` bumped from 17 to 40 to cover the full ESP32 GPIO range (0..39), with an inline comment documenting the rationale so a future port to the D1 mini does not try to "save bytes" by lowering it. Bumping the size makes the arrays large enough that the indices used today fit cleanly; it does not change the indexing scheme.

**Proper recommendation (still open):** stop indexing by raw pin number. Define an enum of logical slots and a small pin lookup table so the state arrays only need three entries and the slot/pin distinction is type-enforced:
```cpp
enum LedSlot { LED_SLOT_STATUS, LED_SLOT_FRIEND_1, LED_SLOT_FRIEND_2, LED_SLOT_QTY };
static const uint8_t g_ledPin[LED_SLOT_QTY] = { GPIO_NUM_32, GPIO_NUM_33, GPIO_NUM_25 };
byte g_ledRequiredState[LED_SLOT_QTY];  // 3 bytes instead of 40
// ledSetState(LedSlot slot, int state) → g_ledRequiredState[slot] = state; digitalWrite(g_ledPin[slot], …);
```
Removes the OOB class of bug permanently and the fragility around "what's the highest pin we use today?".

```cpp
161: #define LED_STATUS   GPIO_NUM_32   // pin 32 → would be OOB in a [17]-sized array
162: #define LED_FRIEND_1 GPIO_NUM_33
163: #define LED_FRIEND_2 GPIO_NUM_25
...
378: #define LED_QTY                  40   // post-fix; was 17, which corrupted BSS on every ledSetState call
379: byte          g_ledRequiredState[LED_QTY];
909:     g_ledRequiredState[pin] = requiredState;   // indexed by raw pin number → relies on LED_QTY ≥ max_pin + 1
```

### MEDIUM

#### LOGIC-004 — `getTextBounds()` called with `int16_t[]` slots reinterpret-cast to `uint16_t*`

- **File:** `minimessenger.ino:1376,1387`
- **Category:** LOGIC
- **First seen:** 2026-06-02

`addConversationBlock()` declares `static int16_t tsBox[4]` and `int16_t msgBox[4]` and then calls `g_disp->getTextBounds(... &tsBox[BOX_X], &tsBox[BOX_Y], (uint16_t*)&tsBox[BOX_W], (uint16_t*)&tsBox[BOX_H])` — reinterpret-casting two `int16_t` slots to `uint16_t*`. The Adafruit_GFX API stores width/height through the latter, and the rest of the function reads `tsBox[BOX_W]` / `tsBox[BOX_H]` back as `int16_t`. As long as the panel is ≤ 32767 px wide/tall the bit pattern round-trips, but the cast is undefined behaviour under strict-aliasing and silently breaks if a font ever returns a w/h ≥ 0x8000 (negative when re-read as `int16_t`).

**Recommendation:** declare dedicated `uint16_t tsW, tsH, msgW, msgH;` and pass their addresses to `getTextBounds`, then copy the values into `tsBox[BOX_W] = (int16_t)tsW;` if the rest of the code wants signed slots. Drops the casts and the aliasing concern in one pass.

```cpp
1376: g_disp->getTextBounds(ts, 0, 0, &tsBox[BOX_X], &tsBox[BOX_Y], (uint16_t*)&tsBox[BOX_W], (uint16_t*)&tsBox[BOX_H]);
1387: g_disp->getTextBounds(msgBuf, 0, 0, &msgBox[BOX_X], &msgBox[BOX_Y], (uint16_t*)&msgBox[BOX_W], (uint16_t*)&msgBox[BOX_H]);
```

#### SEC-003 — MQTT messages have no authentication or integrity check

- **File:** `mqtt.ino:174` (`mqttPushFormattedMessage`)
- **Category:** SEC
- **First seen:** 2026-05-03

The trailer `### ts:<...> deviceId:<n> msgId:<n>` is plain text. Any client with the shared broker credentials (every device) can publish a forged `deviceId` field and impersonate another peer. There is no nonce / sequence / signature — replays are also accepted. Recorded for traceability of the threat model on a closed appliance; not currently exploited but the surface stays open as long as the credentials are shared.

**Recommendation:** HMAC-SHA256 trailer keyed off a per-device secret, with `msgId` mixed into the MAC input for replay defence; reject messages whose MAC does not verify. Alternative: HiveMQ ACLs that bind each `deviceId` to its own MQTT user and a server-side topic check.

```cpp
174: snprintf(g_mqttOutgoingMsg, MSG_BUFFER_SIZE, "%s ### ts:%s deviceId:%d msgId:%d", payload, getCurrentDateTime(), g_deviceData.deviceId, g_mqttOutputMsgId);
```

#### HW-001 — Backlight hardwired to 3.3 V; static UI shown for hours risks ST7789 ghosting

- **File:** `minimessenger.ino:196` (`#define TFT_BL -1`), `1025-1070` (`setupDisplay` / `setDisplayPowerState`)
- **Category:** HW
- **First seen:** 2026-06-02

The wiring note at line 4 confirms this physical limitation ("pas de pin pour TFT_BL pour dimmer le backlight"). The code wraps every `analogWrite(TFT_BL, …)` in `#if TFT_BL >= 0`, so the dim/off states currently only blank the controller — the panel stays lit at full brightness. The conversation block (with the same status-bar icons, recipient row, MQTT/WiFi badges) sits unchanged on screen for the full idle window, then for as long as no peer publishes. On a device that lives 24/7 on a desk this is exactly the long-static-content scenario that produces ST7789 ghosting after weeks.

**Recommendation:** two options, ideally both. (a) Shorten the idle-to-blank-controller delay (the `setDisplayPowerState` schedule) so the panel actually goes dark within tens of minutes of idle. (b) Add a low-cost pixel-shift screensaver: every few seconds while idle, redraw the conversation block 1–2 px right/down (modulo a small offset). With the panel always at 100 %, a future hardware revision that adds a transistor-driven backlight pin is the only way to recover dim/off proper; document this in `docs/info_burnin.md`.

```cpp
196: #define TFT_BL -1   // wiring: no PWM pin available on the alixp board
1025: #if TFT_BL >= 0
1026:     pinMode(TFT_BL, OUTPUT);
1027:     analogWrite(TFT_BL, 255);  // full brightness at boot
1028: #endif
```

### LOW

#### EDGE-001 — MQTT Will-message stack buffer `[4]` is exact-fit for `"999\0"`; one digit away from truncation

- **File:** `mqtt.ino` inside `mqttReconnectAttempt()` (the inlined `char willMsg[4]; snprintf(willMsg, sizeof(willMsg), "%d", g_deviceData.deviceId);` immediately before `g_mqttClient.connect(…, willMsg, MQTT_SESSION_VOLATILE)`)
- **Category:** EDGE
- **First seen:** 2026-06-02

The Will payload is built on the stack right before `connect()`: `char willMsg[4]; snprintf(willMsg, sizeof(willMsg), "%d", g_deviceData.deviceId);`. Today `deviceId` is a `byte` (max 255) or a fallback `random(100, 1000)` (max 999) — three digits plus the null terminator fits exactly. The `sizeof()` argument is correct, so any future widening of the device-ID range silently truncates instead of overflowing. Blast radius is contained to a 4-byte stack slot in one function — no BSS collateral, no heap, no helper static. Was previously hosted by `getDeviceIdAsChars()` (a helper with the same internal buffer); the helper was removed and inlined for zero-fragmentation-on-reconnect.

**Recommendation:** bump `willMsg[4]` to `willMsg[5]` (`"9999\0"` worst case) the day deviceId could exceed 999. Until then, the present sizing is well-bounded. If the same string is needed at a second site, hoist a helper that takes a caller-provided buffer (so the call site keeps controlling the storage class — stack vs. static) rather than reintroducing a hidden-static one.

```cpp
// mqtt.ino, inside mqttReconnectAttempt()
char willMsg[4];
snprintf(willMsg, sizeof(willMsg), "%d", g_deviceData.deviceId);
g_mqttClient.connect(g_deviceData.name(), …, willMsg, MQTT_SESSION_VOLATILE);
```

#### LOGIC-005 — `printValueWrapped` walks `value[i]` with bounds tied to a derived `upperBound`

- **File:** `minimessenger.ino:1647-1656`
- **Category:** LOGIC
- **First seen:** 2026-06-02

The reverse-walk uses `upperBound = (maxChars - 1 < len - 1) ? (maxChars - 1) : (len - 1);` then `for (int i = upperBound; i > 0; i--) value[i];`. Functionally correct today because `len > 0` is gated above, but the bounds invariant lives implicitly in the ternary. A refactor that changes either branch (e.g. extracting `len` into `value.length()` repeated calls, or swapping `String` for `const char*`) would break the safety quietly.

**Recommendation:** keep the guard explicit at the loop: `for (int i = upperBound; i > 0 && i < (int)value.length(); i--)`. The extra check is one cmp per iteration and documents the invariant.

```cpp
1647: int len        = value.length();
1654: int upperBound = (maxChars - 1 < len - 1) ? (maxChars - 1) : (len - 1);
1656: for (int i = upperBound; i > 0; i--) {
1657:     char c = value[i];
```

#### LOGIC-006 — `nvsSsidCount` bound check is implicit in the producer; consumers don't guard

- **File:** `wifi.ino:430-444`
- **Category:** LOGIC
- **First seen:** 2026-06-02

`wifiLoadNVSAndCompiledIntoMulti()` writes into `nvsSsids[nvsSsidCount++]` only when `nvsSsidCount < MAX_WIFI_NETWORKS`. Downstream the de-duplication loop reads `for (int k = 0; k < nvsSsidCount; k++) nvsSsids[k]` — safe as long as the producer's guard holds. The invariant `nvsSsidCount ≤ MAX_WIFI_NETWORKS` lives in one branch, in one function. A future refactor that drops the producer guard (or that reuses the counter for a different array) walks straight off the end without any compiler help.

**Recommendation:** add a `static_assert(MAX_WIFI_NETWORKS > 0, "...");` and an `assert(nvsSsidCount <= MAX_WIFI_NETWORKS);` at the top of the de-dup loop, or replace `nvsSsidCount` with `std::min((size_t)nvsSsidCount, MAX_WIFI_NETWORKS)` at every consumer site.

```cpp
442: if (nvsSsidCount < MAX_WIFI_NETWORKS) {
443:     nvsSsids[nvsSsidCount++] = ssid;
444: }
```

#### LOGIC-007 — `showFontTest()` function and its `texts[]` / `fontNames[]` arrays are dead code

- **File:** `minimessenger.ino:1795-1796` (and the enclosing function)
- **Category:** LOGIC
- **First seen:** 2026-06-02

Grep for `showFontTest` across `*.ino *.h *.cpp` returns zero callers. The function is a self-contained font-metrics probe (calls `getTextBounds` for various strings and logs the results) and was clearly used during font selection — but no command, no `setup()`, no `loop()` path invokes it now. The function body declares `String texts[]` and `String fontNames[]` arrays which themselves are referenced only inside the dead function. Keeping unreachable code rotting in the main `.ino` increases the compile time and confuses anyone navigating the file looking for current behaviour.

**Recommendation:** delete `showFontTest()` entirely (and the trailing comment block of measured bounds that lives just above it, lines ~1770-1793). If you anticipate needing it again, recover it from git history. If you want a `/dbg fonts` command to print metrics on demand, wire it through the existing `processDbgSubcommand` dispatcher rather than leaving it floating.

```cpp
1795: String         texts[]     = { "aaaaa", "AAAAA", "ttttt", "qqqqq", "Attqq", "     ", "_____" };
1796: String         fontNames[] = { "default", "FreeSans9pt8b" };
```

#### MEM-002 — `new Adafruit_ST7789` in `setupDisplay()` is never paired with `delete`

- **File:** `minimessenger.ino:1016`
- **Category:** MEM
- **First seen:** 2026-05-03

`setupDisplay()` is called exactly once today, so this is not an actual leak — but the global `g_disp` is a raw owning pointer. If anyone re-runs `setupDisplay()` (e.g. for the OLEDSHIELD branch that's still a stub, or after a soft reconfigure) the prior allocation is silently abandoned.

**Recommendation:** either (a) make `g_disp` point at a static instance, or (b) `delete g_disp` at the top of `setupDisplay()` before reassigning.

```cpp
1016: Adafruit_ST7789* pDisp = new Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
1027: g_disp = pDisp;
```

#### MEM-005 — `wifiNvsSsidKey()` / `wifiNvsPwdKey()` allocate fresh `String` temporaries on every call

- **File:** `wifi.ino:403-408` (definitions), called at lines `434-435, 508-511, 521-524, 535-536, 559-562, 591`
- **Category:** MEM
- **First seen:** 2026-06-02

The helpers return `String("ssid_") + (int)i` and `String("pwd_") + (int)i`. They're called from six different sites that walk the slot index 0..MAX_WIFI_NETWORKS, so each `wifiLoadNVSAndCompiledIntoMulti` / `wifiSave` / `wifiForget` / `wifiPrintSavedNetworks` pass triggers ~10-20 `String` allocations just for the keys. These functions are cold-path (boot / portal save / `/wifi forget`), not hot-path, so the heap churn is bounded and contained — but the pattern undermines the rest of the file's snprintf-discipline and is visible in any future refactor that decides to call these helpers more often.

**Recommendation:** swap to a small stack buffer:
```cpp
static inline void wifiNvsKey(char out[12], const char* prefix, uint8_t i) {
    snprintf(out, 12, "%s_%u", prefix, (unsigned)i);
}
```
At each call site replace the temporary with `char key[12]; wifiNvsKey(key, "ssid", i); g_wifiPrefs.getString(key, "");`. Zero allocations, zero copies.

```cpp
403: static inline String wifiNvsSsidKey(uint8_t i) {
404:     return String("ssid_") + (int)i;
405: }
406: static inline String wifiNvsPwdKey(uint8_t i) {
407:     return String("pwd_") + (int)i;
408: }
```

#### MEM-006 — `announceContactTransition()` builds the banner via `String` `+=`

- **File:** `contacts.ino:72-77`
- **Category:** MEM
- **First seen:** 2026-06-02

The banner string is built as `String label = (entry ? String(entry->pseudo) : (String("device #") + deviceId)); label += isLive ? " connected" : " disconnected";` — three or four heap reallocations per transition. Transition events are rare (online/offline of a peer, plus the applicative timeout), so the total allocation rate is low, but the pattern stands out in a file that otherwise stays heap-quiet. The `String("device #") + deviceId` line also forces a conversion from byte to String through the integer-concatenation operator, allocating again.

**Recommendation:** stack buffer + `snprintf`:
```cpp
char label[40];
snprintf(label, sizeof(label), "%s %s",
         entry ? entry->pseudo : ("device #" /* + deviceId via %d */),
         isLive ? "connected" : "disconnected");
addConversationBlock("", label, ...);
```
or, even simpler, two `snprintf` branches keyed on `entry != nullptr` so the `%d` only fires in the unknown-peer path.

```cpp
72: static void announceContactTransition(byte deviceId, bool isLive) {
73:     const DeviceDataEntry* entry = DeviceDataEntry::findById(deviceId);
74:     String                 label = (entry != nullptr) ? String(entry->pseudo) : (String("device #") + deviceId);
75:     label += isLive ? " connected" : " disconnected";
76:     addConversationBlock("", label, isLive ? CONVO_INFO_COLOR : CONVO_ERROR_COLOR, CENTER);
77: }
```

#### MEM-007 — `printInfoLine()` allocates two 128-byte stack buffers per call (256 B)

- **File:** `minimessenger.ino:1254-1262` (`printInfoLine`), `minimessenger.ino:1323` (`addConversationBlock`)
- **Category:** MEM
- **First seen:** 2026-06-02

`CONVO_MSG_MAX_LEN` is 128. `printInfoLine` declares two such arrays back-to-back (`leftBuf` + `rightBuf` = 256 B) before calling `utf8ToLatin1`; `addConversationBlock` declares one more (`msgBuf` = 128 B). On ESP32 with `loopTaskStackSize = 8192` this is fine in isolation, but a /help dump that loops `printInfoLine` ten times for one help group, each frame nesting `getTextBounds` + `print` + GFX font scratch, eats stack in a path the user can trigger from BLE keyboard input. Not currently a crash, but the checklist's "static buffers > 256 B in render path" trips here.

**Recommendation:** share one 128-byte scratch declared once at function scope (the right value is rarely a separate string — it's the description column; could be borrowed from `g_mqttOutgoingMsg[MSG_BUFFER_SIZE]` already 500 B at module scope), or shrink `CONVO_MSG_MAX_LEN` to 80 (the displayable width with FreeSans 10 pt + the cap on the input footer is well below 100 chars).

```cpp
1254: char leftBuf[CONVO_MSG_MAX_LEN];
1259: char rightBuf[CONVO_MSG_MAX_LEN];
1323: char msgBuf[CONVO_MSG_MAX_LEN];
```

#### PERF-003 — Two `delay(1000)` at the top of `setup()` plus `delay(2'000)` in the splash

- **File:** `minimessenger.ino:1826-1827` (setup), plus the splash delay inside `showSplashScreen()`
- **Category:** PERF
- **First seen:** 2026-06-02

Total ~4 seconds blocked before NTP / WiFi / MQTT setup. Not on the hot path (setup runs once), but: one `delay(1000)` after `Serial.begin` is the standard UART-settle workaround and is fine; the *second* `delay(1000)` immediately after has no obvious reason and probably survives from a paste. The splash `delay(2'000)` is intentional but is the dominant boot-latency contributor on every cold start.

**Recommendation:** drop the duplicate `delay(1000)` at line 1827. Consider letting the splash linger via `millis()` gating while `setupKeyboard()` / `setupWifi()` run in the meantime — those don't depend on the splash being visible and would overlap the 2 s window for free.

```cpp
1825: Serial.begin(115200);
1826: delay(1000);
1827: delay(1000);
```

#### PERF-004 — `utf8ToLatin1()` called unconditionally on every conversation block

- **File:** `minimessenger.ino:1253-1262, 1322-1326` (`printInfoLine`, `addConversationBlock`)
- **Category:** PERF
- **First seen:** 2026-06-02

Every message rendered to the conversation buffer is copied into a stack `char[]` and run through `utf8ToLatin1()`. The function scans byte-by-byte and is correct, but the overwhelming majority of strings passing through — timestamps, device IDs, status banners, ASCII chat — contain no byte > 0x7F. The scan is wasted work on those.

**Recommendation:** add an early-out: scan once for any byte ≥ 0x80, and skip the conversion if none is found. Single pass, no allocation, ~free for ASCII. Or skip the conversion entirely for known-ASCII call-sites (the boot banners, timestamps).

```cpp
1254: char leftBuf[CONVO_MSG_MAX_LEN];
1255: strncpy(leftBuf, left.c_str(), sizeof(leftBuf) - 1);
1256: leftBuf[sizeof(leftBuf) - 1] = '\0';
1257: utf8ToLatin1(leftBuf);  // every call, even pure ASCII
```

#### PERF-005 — `redrawInputFooter()` recomputes the viewport window on every keystroke

- **File:** `bars.ino:165-225`
- **Category:** PERF
- **First seen:** 2026-06-02

The keystroke path goes `currentMsgInsertCharAtCursor → redrawInputFooter → recompute (viewStart, viewEnd)`. The viewport math is small (a few comparisons and a modulo) but it runs unchanged when the message stays inside the visible window. Negligible CPU on ESP32, but combined with allocation cost it adds a few hundred microseconds of arithmetic to each keypress.

**Recommendation:** cache `viewStart` / `viewEnd` in two globals; recompute only when `g_msgCursorIdx` or `g_currentMsgFromKeyboard.length()` crosses the cached window. Or accept as-is — the wall-clock cost is tiny.

```cpp
196: void redrawInputFooter() {
...
208:    const int   kCharWidth  = 12;
209:    int         viewEnd     = cur;
210:    const int   defaultEnd  = (len > kMaxChars) ? kMaxChars : len;
211:    if (viewEnd < defaultEnd) viewEnd = defaultEnd;
```

#### PERF-008 — Command dispatch builds `String` temporaries on every input

- **File:** `commands.ino:137, 140, 169` (and similar in `processWifiSubcommand`)
- **Category:** PERF
- **First seen:** 2026-06-02

`processPayloadAsCommand()` and `processWifiSubcommand()` use `message.startsWith(String(GROUP_WIFI) + " ")` — each evaluation builds a fresh `String` (the concat with `" "`), then `startsWith` walks it. Command dispatch fires on every incoming chat message and every Serial line; the rate is human-typing-low but the cost is heap allocations during a path that should be free of them.

**Recommendation:** swap to C-string comparisons:
```cpp
const char* msg = message.c_str();
size_t      n   = strlen(GROUP_WIFI);
if (strncmp(msg, GROUP_WIFI, n) == 0 && (msg[n] == '\0' || msg[n] == ' ')) { ... }
```
No allocations, branch-equivalent.

```cpp
137: if (message == GROUP_WIFI || message.startsWith(String(GROUP_WIFI) + " ")) {
140: if (message == GROUP_DBG  || message.startsWith(String(GROUP_DBG) + " ")) {
169: if (message.startsWith(String(CMD_WIFI_FORGET) + " ")) {
```

#### SEC-004 — Hardcoded WiFi and MQTT credentials in source

- **File:** `minimessenger.ino:119-122` (MQTT), `personal-data.h.template` (WiFi defaults)
- **Category:** SEC
- **First seen:** 2026-05-03

Acknowledged in `CLAUDE.md` ("treat all three files as sensitive"). The compile-time MQTT broker URL/user/password and the per-device WiFi defaults still ship in the binary. Down-rated to LOW because the trade-off is intentional for a closed personal appliance, but kept on the active list so the trade-off stays visible.

**Recommendation:** none required as long as the deployment stays closed. If the device count ever grows or the binary risks public distribution, migrate MQTT creds to NVS (mirror the existing WiFi NVS path) and rotate.

```cpp
119: const char* mqtt_server   = "xxxxxx.s1.eu.hivemq.cloud";
120: const int   mqtt_port     = 8883;
121: const char* mqtt_user     = "xxxxx";
122: const char* mqtt_password = "xxxxxxx";
```

#### SEC-005 — Hardcoded HiveMQ root CA (Let's Encrypt ISRG Root X1, expires 2035-06-04)

- **File:** `mqtt.ino:270-300`
- **Category:** SEC
- **First seen:** 2026-05-03

`g_hiveMQRootCA` is the inline PEM moved into `mqtt.ino` along with the rest of the MQTT plumbing. ISRG Root X1 expires 2035-06-04, so no rotation is urgent. If HiveMQ switches its chain (ISRG Root X2, Sectigo, …) before then, every device needs a firmware update.

**Recommendation:** add a comment with the expiry next to the literal and document the rotation procedure in `docs/`. Optionally, expose a `/mqtt-ca` slash command that loads a new CA from NVS for emergency in-field rotation.

```cpp
270: const char* g_hiveMQRootCA = "-----BEGIN CERTIFICATE-----\n..."
```

#### SEC-006 — `admin/live` keepalive payload leaks the WiFi SSID and the device IP

- **File:** `mqtt.ino:155-167` (`mqttSendAlive`)
- **Category:** SEC
- **First seen:** 2026-05-03

The retained payload published every 120 s embeds `ssid:<SSID> ip:<IP>`. Anyone with broker read access — which today is anyone holding the shared MQTT credentials — can map device IDs to networks. Not a high-impact leak for a personal appliance, but a free improvement.

**Recommendation:** drop `ssid` and `ip` from the alive payload; keep `deviceId`, `mac`, `recoId`. Add them back behind a `/dbg` flag if needed.

```cpp
155: void mqttSendAlive(int liveType) {
156:     char payload[MSG_BUFFER_SIZE];
157:     snprintf(payload, MSG_BUFFER_SIZE,
158:              "%d %s mac:%s ssid:%s ip:%s recoId:%d",
159:              g_deviceData.deviceId, ...);
```

#### SEC-007 — `random(100, 1000)` after `analogRead`-seeded RNG for the unknown-MAC fallback ID

- **File:** `minimessenger.ino:866` (call in `identifyDevice`'s else branch), `~1840` (`randomSeed(analogRead(A0))` in `setup()`)
- **Category:** SEC
- **First seen:** 2026-05-03

Used only for an unknown-MAC fallback device ID — a UI label, not a security token. The call now writes into `g_deviceData.deviceId` (the synthesized-entry path of `identifyDevice`), but the underlying RNG path is unchanged from the legacy version. Recording so this never silently graduates into an auth path. ESP32 has a hardware RNG (`esp_random()`) that is strictly better for free, even if not needed here.

**Recommendation:** swap to `esp_random() % 900 + 100` and add a `// not for security` comment. Removes the analog-pin seed entirely.

```cpp
866:  g_deviceData.deviceId = (byte)random(100, 1000);
1840: randomSeed(analogRead(A0));
```

#### SEC-008 — WiFi captive-portal AP password hardcoded to `"11110000"`

- **File:** `wifi.ino:56`
- **Category:** SEC
- **First seen:** 2026-06-02

`#define WIFI_PORTAL_AP_PASS "11110000"` — used when the device falls back to the WiFiManager AP. Any user within RF range can join that AP and reach the portal page (currently a SSID picker, but a future change to expose MQTT creds or OTA would inherit this auth posture). The current threat is low because the portal is only up during onboarding, and the password is also printed on-screen for the legitimate user to type — that on-screen display is the constraint that prevents per-device randomisation.

**Recommendation:** keep as-is, but: (a) auto-close the portal after N minutes if no client joins, (b) tighten the portal page to read-only operations until the user explicitly authenticates with a per-boot one-time code. Mark the trade-off in `docs/howto_wifi.md` since SSID and PASS are also redrawn on the info screen.

```cpp
56: #define WIFI_PORTAL_AP_PASS "11110000"
```

#### OBS-002 — PubSubClient 2.8 is functional but unmaintained

- **File:** `mqtt.h:27`
- **Category:** OBS
- **First seen:** 2026-05-03

Pinned at 2.8; the known limitations (16-bit packet length, no QoS 2, fragile reconnect under packet loss) are well-known. Fine for this project's traffic volume.

**Recommendation:** no migration unless a concrete limit is hit. If you do, `ArduinoMqttClient` is the easiest target (matches the Arduino-libraries house style and keeps tracking the MQTT spec).

```cpp
27: #include <PubSubClient.h>
```

#### DUP-001 — `if (g_deviceData.screen == DisplayType::ST7789) { ... } else { ESP_LOGW("DISPLAY_TYPE_NOT_CONFIGURED") }` repeated in 11 functions

- **File:** `minimessenger.ino:969, 985, 1002, 1015, 1052, 1113, 1161, 1180, 1249, 1622, 1694` and `bars.ino:107, 165`
- **Category:** DUP
- **First seen:** 2026-05-03

Same skeleton as before, spread across `minimessenger.ino` (display + scroll + info functions) and `bars.ino` (status-bar / footer functions): branch on `g_deviceData.screen`, log "DISPLAY_TYPE_NOT_CONFIGURED" in the else arm (or early-return). Adding a real OLEDSHIELD implementation later means touching every site. After the unified-identity refactor the variable name is now `g_deviceData.screen` (was `g_displayType`); the duplication itself is unchanged.

**Recommendation:** an `inline bool ensureST7789(const char* fn)` helper that does `if (g_deviceData.screen != DisplayType::ST7789) { ESP_LOGW(TAG_MM, "%s: DISPLAY_TYPE_NOT_CONFIGURED", fn); return false; } return true;` removes the boilerplate at every call site. Even cheaper: a macro `#define REQUIRE_ST7789() do { if (g_deviceData.screen != DisplayType::ST7789) { ESP_LOGW(TAG_MM, "%s: DISPLAY_TYPE_NOT_CONFIGURED", __func__); return; } } while(0)` and a `_RET(x)` variant for non-void returns.

```cpp
1015: if (g_deviceData.screen == DisplayType::ST7789) { ... }
1180: if (g_deviceData.screen != DisplayType::ST7789) {
1181:     ESP_LOGW(TAG_MM, "redrawAllConversations: DISPLAY_TYPE_NOT_CONFIGURED");
1182:     return;
1183: }
```

#### DUP-003 — Four identical label/value row blocks in `drawPortalInstructions()`

- **File:** `wifi.ino:613-655`
- **Category:** DUP
- **First seen:** 2026-06-02

The function draws four rows (AP / PASS / URL / THEN) with the exact same 7-line pattern: setCursor, setTextColor(RED), print(label), setCursor, setTextColor(WHITE), print(value), advance Y. The font, the two colors, the two columns, and the line-height are constants of the function. Adding a fifth row or changing the column layout means touching four identical sites.

**Recommendation:** local helper `auto drawRow = [&](const char* label, const char* value) { pDisp->setCursor(colHeaders, nextY); pDisp->setTextColor(ST77XX_RED); pDisp->print(label); pDisp->setCursor(colValues, nextY); pDisp->setTextColor(ST77XX_WHITE); pDisp->print(value); nextY += lineHeight; };` then four calls.

```cpp
617: pDisp->setCursor(colHeaders, nextY);
618: pDisp->setTextColor(ST77XX_RED);
619: pDisp->print("AP:");
620: pDisp->setCursor(colValues, nextY);
621: pDisp->setTextColor(ST77XX_WHITE);
622: pDisp->print(WIFI_PORTAL_AP_SSID);
623: nextY += lineHeight;
```

#### DUP-004 — Three time-snapshot functions share the same `time()` + `localtime_r()` boilerplate

- **File:** `minimessenger.ino:503-538`
- **Category:** DUP
- **First seen:** 2026-06-02

`getCurrentDateTime()` (503-519), `getCurrentTime()` (521-529), and `getTimezoneLabel()` (533-539) each open with `time_t epochTime = time(nullptr); struct tm timeInfo; localtime_r(&epochTime, &timeInfo);`. Only the formatting / lookup that follows differs. Three lines × three callers is a textbook helper. Trivial duplication, but each of the three functions is short enough that the boilerplate is the majority of the body — refactor pays for itself in readability.

**Recommendation:** a private helper:
```cpp
static inline void getCurrentTm(struct tm* out) {
    time_t e = time(nullptr);
    localtime_r(&e, out);
}
```
Each caller then becomes a clean two-liner (one call + one snprintf / lookup).

```cpp
503: char* getCurrentDateTime() {
504:     time_t    epochTime = time(nullptr);
505:     struct tm timeInfo;
506:     localtime_r(&epochTime, &timeInfo);
...
521: char* getCurrentTime() {
522:     time_t    epochTime = time(nullptr);
523:     struct tm timeInfo;
524:     localtime_r(&epochTime, &timeInfo);
...
533: const char* getTimezoneLabel() {
534:     time_t    epochTime = time(nullptr);
535:     struct tm timeInfo;
536:     localtime_r(&epochTime, &timeInfo);
```

#### DUP-005 — `setupLeds()` writes each LED pin individually inside a blink loop

- **File:** `minimessenger.ino:937-946`
- **Category:** DUP
- **First seen:** 2026-06-02

The boot blink-test loop writes `LED_STATUS`, `LED_FRIEND_1`, `LED_FRIEND_2` to HIGH then LOW with the same three-line block repeated twice inside the same `for`. Adding a fourth LED means touching six `digitalWrite` lines. Minor in absolute terms but mechanical — the three LEDs already form a logical group elsewhere in the file (declarations are adjacent, `ledSetState` calls are grouped at the bottom of `setupLeds`).

**Recommendation:** a small array + loop:
```cpp
constexpr int kBootBlinkLeds[] = { LED_STATUS, LED_FRIEND_1, LED_FRIEND_2 };
for (int i = 0; i < 4; i++) {
    for (int led : kBootBlinkLeds) digitalWrite(led, HIGH);
    delay(150);
    for (int led : kBootBlinkLeds) digitalWrite(led, LOW);
    delay(150);
}
```
Future LEDs just go in the array.

```cpp
937: for (int i = 0; i < 4; i++) {
938:     digitalWrite(LED_STATUS, HIGH);
939:     digitalWrite(LED_FRIEND_1, HIGH);
940:     digitalWrite(LED_FRIEND_2, HIGH);
941:     delay(150);
942:     digitalWrite(LED_STATUS, LOW);
943:     digitalWrite(LED_FRIEND_1, LOW);
944:     digitalWrite(LED_FRIEND_2, LOW);
945:     delay(150);
946: }
```

#### HW-003 — `admin/live` and `admin/dead` subscriptions use QoS 0

- **File:** `mqtt.ino:124-125`
- **Category:** HW
- **First seen:** 2026-06-02

Inbound liveness uses QoS 0 (fire-and-forget) while `msg/broadcast` and `msg/unicast/*` are QoS 1. If a liveness packet drops, the peer keeps showing the contact as online until `contactsTick` ages the slot out at ~125 s — i.e. up to two minutes of stale "friend present" LED. The outbound publishes (`mqttSendAlive`) also need to be checked; if the brokered will is QoS 0 the LWT is broker-dropped on close.

**Recommendation:** bump both subscriptions to `MQTT_QOS_1`. PubSubClient handles the at-least-once mechanic for incoming. Verify the publish side uses QoS 1 too (the `g_mqttClient.publish` overload — second arg `retained`, no QoS arg, defaults to 0 in PubSubClient).

```cpp
124: g_mqttClient.subscribe(g_mqttOutgoingTopicLive, MQTT_QOS_0);
125: g_mqttClient.subscribe(g_mqttOutgoingTopicWill, MQTT_QOS_0);
```

## Won't fix

These issues have been reviewed by the user and are intentionally not addressed.
The audit run does **not** modify this section automatically.

#### SEC-009 — `ESP_LOGI` prints stored WiFi passwords in cleartext at INFO level

- **File:** `wifi.ino:481`
- **Category:** SEC
- **Dismissed:** 2026-06-02
- **Reason:** intentional development-time aid. The comment immediately above the log line (lines 478-479) documents it as such: "Cleartext password on purpose — this is a dev-time aid to confirm the right credentials are loaded after a /wifi forget / portal save / template-bump." The serial console is not exposed in normal use; if the device ever ships to non-trusted hands, gate this behind a stricter log level then.

#### LANG-002 — French typographic spacing in English user-visible strings (also covers LANG-001)

- **File:** `minimessenger.ino:1120` (`"MiniMessenger !"` splash title), `minimessenger.ino:1482` (`"Ready !"` MQTT-connected banner)
- **Category:** LANG
- **Dismissed:** 2026-06-02
- **Reason:** intentional. Both strings use the French space-before-`!` convention on purpose. The original LANG-001 finding (`"Ready !"`) is folded in here since it has the same rationale — they ride or die together, no point listing them separately. If the device's UI is ever localised to a fully-English flavour, revisit both at the same time.

#### HW-005 — No battery monitoring, no sleep strategy

- **File:** project-wide
- **Category:** HW
- **Dismissed:** 2026-06-02
- **Reason:** the device is USB-powered (documented in [`README.md`](../README.md)). There is no battery, no Li-ion / Li-Po cell, no voltage divider on a VBAT GPIO. Sleep modes, low-battery shutdown, regulator dropout, BOD wiring are therefore deliberately out of scope. Re-open this finding if a future hardware revision adds a battery.
