# Idea — Native WiFi onboarding (screen + keyboard, no captive portal)

**Date:** 2026-06-01
**Status:** Exploratory — no code yet. This doc captures the analysis so we
can revisit later without re-thinking from scratch.

## The idea

Replace the WiFiManager captive portal with a fully on-device onboarding
flow:
1. Boot tries known networks from NVS (unchanged).
2. On failure, instead of opening an AP + captive portal, the device:
   - Scans the air with `WiFi.scanNetworks()`.
   - Shows a scrollable list of visible SSIDs on the TFT.
   - Lets the user pick one with the BLE keyboard (UP / DOWN / ENTER).
   - Prompts for the password on the same screen, masked with `*`.
   - Attempts `WiFi.begin(ssid, pwd)`, shows progress / success / failure.
   - Saves successful credentials to NVS (existing `wifiSaveToNvs()`).
3. Cancel via ESC, retry via R, hidden-SSID entry via a dedicated list item.

Phone is no longer required. The device is fully self-contained: keyboard
+ screen handle the whole flow.

## Why it's probably worth doing

| Reason | Detail |
|--------|--------|
| **UX coherence** | The whole product is designed around the BLE keyboard + TFT. Forcing the user to grab a phone for onboarding breaks the self-contained promise. |
| **Fewer deps** | WiFiManager pulls in WebServer + DNSServer + HTTP handlers + asset blob. ~50 KB of flash. Useful headroom if we migrate to "Minimal SPIFFS" for OTA support. |
| **No more WM bugs** | Captive-portal quirks on Android, the double-`stopConfigPortal()` crash, mysterious "NUM CLIENTS: 0" RX starvation — all gone with the lib. |
| **Simpler radio mgmt** | Native flow runs in pure STA (scan + begin) — no AP_STA mode flip, no shared radio between our AP beacons and BLE scan. |
| **Charset is ours** | We already mastered the UTF-8 → Latin-1 pipeline for keystrokes. No need to second-guess what an HTTP form POST from an unknown browser sent. |

## Feasibility — bricks we already have

| Brick | Status | Effort |
|-------|--------|--------|
| `WiFi.scanNetworks(false, true)` + `WiFi.SSID(i)` / `RSSI(i)` / `encryptionType(i)` | API standard | trivial |
| Keystroke pipeline (`decodeHIDReport` → `g_currentMsg`) | Already wired for chat input | facile, reuse |
| Display rendering (TextLine / addConversationBlock / redrawAllConversations) | Designed for chat, not for a sortable selectable list | moderate refactor — need a "menu mode" alongside chat mode |
| Password masking (`*` instead of cleartext) | Just a render variant | facile |
| `WiFi.begin()` + `WiFi.status()` polling with timeout | Standard arduino-esp32 | trivial |
| NVS save (`wifiSaveToNvs`) | Done, tested | gratuit |
| LED feedback (status, scanning, connecting, OK, KO) | LED state machine already exists | gratuit |

**Total honest estimate**: 0.5 to 1 day of focused work for v1.

## The risk that decides if we do it

**Scenario "no keyboard"**: BLE keyboard not bonded, broken, out of
battery, MAC changed. Today this is recoverable: the user grabs a phone,
connects to `minimsg-cfg`, configures via the portal. With the proposed
native flow alone, the device is unusable in that scenario until someone
reflashes it (or has another paired keyboard handy).

This is the gating concern. Three mitigation strategies, sorted by safety
vs. simplification:

### Mitigation A — Keep WiFiManager as last-resort fallback (RECOMMENDED)

Default flow becomes:
1. Boot → try known NVS networks for `WIFI_TRYING_KNOWN_TIMEOUT_MS`.
2. Failure → native WiFi picker (new flow).
3. If keyboard not connected within ~60 s of entering the picker → auto-fallback to WiFiManager captive portal.
4. `/wifi portal` command always available for manual debug.

Cost: keep the WiFiManager dependency and (some of) its bugs, just not on the happy path.

Upside: never bricked. The phone path stays as a safety net.

### Mitigation B — Compile-time creds as safety net

Lean on `personal-data.h` more aggressively. If NVS empty AND keyboard
absent AND no other path → fall back to the compiled defaults. Implies a
"factory" Wi-Fi network always available somewhere (e.g. home / office
LAN).

Cost: assumes a stable compiled-in network. Doesn't work for someone
who flashes the project for the first time on a new network.

### Mitigation C — Go all-in, accept "no keyboard = reflash"

For a personal project this is defensible. For something distributable to
others, it's not.

## Implementation details to anticipate

- **Hidden networks**: `WiFi.scanNetworks()` ignores them. Need a "Saisir SSID manuellement" item appended to the visible list.
- **RSSI display**: sort by `RSSI`. Display as `▂▄▆█` bars (verify Latin-1 compatibility of those U+25xx characters first — they may NOT be in the Latin-1 supplement; fallback to numeric `25%/50%/75%/100%`).
- **Encryption type**: prefix `🔒`-ish indicator for protected nets. If `OPEN` → skip the password screen entirely.
- **Re-scan**: bind to key `R`. Optionally auto-refresh every ~10 s if the user lingers — the list can go stale (mobile hotspots in particular).
- **Password masking**: render `*` per char, allow toggle to "show password" (useful at a physical keyboard where typos are more common than on phones).
- **Special characters in passwords**: `@ # é ç &` etc. must already be in `keymapLower` / `keymapUpper` (hid_keys.h). The chat pipeline already produces them — verify with a complex password before counting on it.
- **Cancel mid-input**: ESC returns to the list. No timeout (we wait indefinitely, same UX as the portal blocking).
- **Connection wait**: spinner on screen — "Connexion à SatelliteThree…" — `WiFi.begin()` then poll `WiFi.status()` every 200 ms with a 15-20 s timeout. On failure: "Mot de passe incorrect ?" + return to list.
- **WPA Enterprise / 802.1X**: NOT supported by this flow. WPA2-PSK only. Same limitation as WiFiManager today.
- **AP_STA leftover**: when entering the picker from a previous PORTAL state, make sure the radio is back to pure STA (we already have the `WiFi.disconnect(true,true) + delay(100) + WiFi.mode(WIFI_STA)` recipe in `wifiStopPortal`).

## Display-mode question (the biggest code change)

Today the screen is a chat — top status bar, conversation buffer of
TextLine entries, footer for typing. The WiFi picker needs a different
visual paradigm:
- A vertical list, one SSID per row, with a highlight on the selected one.
- A header with "Sélection du réseau WiFi" / "Saisir le mot de passe".
- A footer with shortcuts ("↑↓ Naviguer · ⏎ Choisir · ESC Annuler").

Two ways to model this:

1. **Reuse TextLine**: each menu line is a TextLine with manual cursor row management. Cheap but hacky — the chat buffer wasn't designed for in-place edits / highlights.
2. **New "screen mode" abstraction**: introduce a `DisplayMode` enum (`CHAT`, `MENU`, `INPUT`). Each mode has its own render function. The status bar stays shared. Cleaner long-term; more code upfront.

Option 2 is the right call if we expect more on-device UIs in the future
(e.g. device settings menu, bond management, OTA picker). Option 1 is
faster if this is a one-off.

## Recommended path forward (if we decide to do it later)

1. Spike option 1 (reuse TextLine) on a branch to prove the core flow:
   scan → render list → keyboard select → password → connect.
2. If the spike works and we want more menus, refactor to option 2.
3. Keep WiFiManager wired in as a last-resort fallback (Mitigation A) at
   least for the first release.
4. Document the keystroke vocabulary on screen (no hidden shortcuts).
5. Bench-test with a complex password (`Aé9!@#` style) to validate the
   HID keymap covers everything.

## When NOT to do this

- If we end up never shipping this to anyone but ourselves and the captive
  portal works "well enough" after the current round of fixes.
- If we plan to add OTA via MQTT instead of WiFiManager's `/update`
  endpoint — that closes one of the reasons to want WM gone.
- If keyboard reliability remains the weakest link of the project — adding
  more keyboard-dependent UX makes it worse, not better.

## Related docs

- `docs/howto_wifi.md` — current onboarding (WiFiManager + WiFiMulti + NVS).
- `docs/howto_ota.md` — partition / OTA constraints, partly motivates the desire to shed WiFiManager weight.
- `docs/howto_fonts.md` — character set restrictions that the password input must respect.
