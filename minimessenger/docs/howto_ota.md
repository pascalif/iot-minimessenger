# Howto — OTA (Over-The-Air firmware updates) on this project

**Date:** 2026-06-01
**Context:** The WiFiManager captive portal at `192.168.4.1` shows an
"Update" menu that accepts a `.bin` upload. With our current partition
scheme it doesn't actually work — this doc explains why, what we'd need to
change to make it work, and the partition concepts that underpin all of it.

## TL;DR

- The "Update" button in the WiFiManager portal will fail silently (or with
  a generic "not enough space" error) because the project uses the
  **"Huge App"** partition scheme, which has **one** app slot — no second
  slot for OTA to write into.
- To enable OTA we'd switch to **"Minimal SPIFFS"** (Arduino IDE → Tools →
  Partition Scheme). That gives two ~1.9 MB app slots which is enough for
  the current firmware.
- The partition switch itself requires a one-time USB reflash (rewrites the
  whole flash layout). After that, all subsequent updates can go through
  the portal.
- SPIFFS is a small read/write filesystem on the flash for user files. We
  don't actually use it; we'd lose ~1.1 MB of unused SPIFFS in the switch,
  no impact.

## How ESP32 flash is laid out

The ESP32 has external SPI flash (typically 4 MB or 8 MB on common
modules; ours is 4 MB). The bootloader reads a **partition table** burned
at offset `0x8000` that describes how the rest of the flash is sliced.
Conceptually:

```
0x0000  bootloader            (28 KB)
0x8000  partition table       (4 KB)        ← describes everything below
0x9000  NVS                   (20 KB)       ← key/value store (WiFi creds, ...)
0xe000  OTA-data              (8 KB)        ← "which app slot to boot" pointer
0x10000 app0                  (varies)      ← THE running application
0x????? app1                  (varies)      ← (optional) second slot for OTA
0x????? SPIFFS / LittleFS     (varies)      ← (optional) user filesystem
```

The actual byte offsets and sizes depend on the **partition scheme** chosen
in the Arduino IDE: each scheme is just a different CSV file in the core
that specifies the slices.

## Partition schemes shipped with arduino-esp32 3.x

Arduino IDE → Tools → Partition Scheme lists ~15 variants. The ones that
matter for a 4 MB module:

| Scheme | app0 | app1 | SPIFFS | OTA? | Notes |
|--------|------|------|--------|------|-------|
| Default 4MB with spiffs | 1.2 MB | 1.2 MB | 1.5 MB | ✅ | The IDE's default. Too small for our firmware. |
| Minimal SPIFFS | 1.9 MB | 1.9 MB | 190 KB | ✅ | **The OTA-friendly choice for big firmware.** |
| No OTA (1MB SPIFFS) | 2.0 MB | — | 1 MB | ❌ | Single slot, more app room, no OTA. |
| No OTA (2MB SPIFFS) | 1.9 MB | — | 2 MB | ❌ | Same but bigger filesystem. |
| **Huge App (3MB + 1MB SPIFFS)** | **3 MB** | — | 1 MB | ❌ | **Current choice.** Single slot, room for very big firmware, NO OTA. |
| 8M with spiffs (8MB) | 3 MB | 3 MB | 1.5 MB | ✅ | Requires an 8 MB module. |

The key axis is **single-slot vs dual-slot**:
- **Single-slot** schemes give you more room for the app but cannot OTA — there's nowhere to write the new firmware while the current one runs.
- **Dual-slot** schemes split the available app space in half. They support OTA but each slot is smaller.

We picked Huge App at the start because the firmware (BLE stack + WiFi + WiFiClientSecure + mbedtls + display + MQTT + GFX fonts + …) approaches 2 MB. We were not sure it would fit the 1.9 MB slot of Minimal SPIFFS at the time. **It probably does now** — check after a compile.

### Checking your current firmware size

After Arduino IDE compiles, the bottom-pane status shows:

```
Le croquis utilise 1 678 432 octets (53 %) de l'espace de stockage de programmes.
Le maximum est 3 145 728 octets.
```

`3 145 728` = 3 MB → confirms Huge App. If your sketch is **under 1 900 000 bytes** (~1.9 MB), you can switch to Minimal SPIFFS without code changes. If it's between 1.9 MB and 3 MB, you'd need to trim the firmware before OTA is possible (remove unused libs, font glyph ranges, etc.).

## What SPIFFS is

**SPIFFS** = "SPI Flash File System". It turns part of the flash into a
filesystem with read/write file API similar to fopen / fread / fwrite. Used
when you need to store user-generated data on the device itself: HTML
files served by an embedded web server, JSON config, logs, recorded audio,
etc.

Sketch-side API (in arduino-esp32):
```cpp
#include <SPIFFS.h>
SPIFFS.begin();
File f = SPIFFS.open("/config.json", "r");
String content = f.readString();
f.close();
```

It's deprecated in modern arduino-esp32 in favor of **LittleFS** (same
idea, more robust against power loss, better wear levelling). Both occupy
the same kind of flash partition — what matters here is the *size* of the
partition, not which FS is used.

### Do we use SPIFFS in this project?

No. We have no `SPIFFS.h` / `LittleFS.h` include, no file I/O. The whole
`/spiffs` partition (1 MB in Huge App, 190 KB in Minimal SPIFFS) is dead
weight. Shrinking it to 190 KB to enable OTA is free.

If we ever wanted to use it (e.g. to cache the conversation history across
reboots, or store per-user preferences), 190 KB is plenty for tens of
thousands of small text messages.

## How OTA actually works

Dual-slot OTA leans on three flash regions cooperating:

1. **`app0` (= subtype `ota_0`)**: contains the firmware currently running.
2. **`app1` (= subtype `ota_1`)**: empty, or contains a previous firmware
   version. This is where new uploads go.
3. **`otadata`**: 8 KB pointer that says "boot from app0" or "boot from
   app1". The bootloader reads it on every power-on.

OTA update flow:
1. Current firmware runs from `app0`.
2. App calls `Update.begin(size)`, gets a handle to write into `app1`.
3. App receives the new firmware over WiFi (HTTP POST, MQTT chunks, etc.)
   and streams it byte-for-byte into `app1` via `Update.write()`.
4. App calls `Update.end()` which finalises the image and validates its
   SHA-256.
5. App calls `Update.setBootNext()` (implicit in `Update.end(true)`) which
   updates `otadata` to point at `app1`.
6. App reboots.
7. Bootloader reads updated `otadata`, sees "boot app1", launches the new
   firmware from `app1`. `app0` is now the "old" slot.
8. If the new firmware fails to call `esp_ota_mark_app_valid_cancel_rollback()`
   within a configurable time, the bootloader auto-reverts to `app0`. This
   is the safety net against a bad firmware.

WiFiManager's `/update` endpoint implements steps 2-5 in a single HTTP
POST handler. The user just clicks "Choose file", picks a `.bin`, clicks
upload, and waits.

### What `.bin` to upload

In Arduino IDE: Sketch → Export Compiled Binary. Produces:

```
<sketch>.ino.bin                  ← the app binary (this is what OTA wants)
<sketch>.ino.partitions.bin       ← partition table (do NOT upload this)
<sketch>.ino.bootloader.bin       ← bootloader (do NOT upload this)
<sketch>.ino.merged.bin           ← the FULL flash image, USB-only
```

For OTA upload through the portal, use `<sketch>.ino.bin` only. The
partition table and bootloader stay as they were — the new firmware lives
inside the existing layout.

## Why our current setup fails OTA

`huge_app.csv` defines:

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x5000
otadata,  data, ota,     0xe000,   0x2000
app0,     app,  ota_0,   0x10000,  0x300000      ← 3 MB, single app slot
spiffs,   data, spiffs,  0x310000, 0xE0000       ← 0.875 MB
```

There's an `app0` (3 MB) but **no `app1`**. The IDF's OTA API in
`Update.begin()`:

```cpp
const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
if (partition == NULL) {
  return false;  // ← we hit this branch
}
```

`esp_ota_get_next_update_partition()` looks for a partition with subtype
`ota_X` other than the one currently running. With only `ota_0` defined,
it returns NULL. `Update.begin()` returns false; WiFiManager's upload
handler logs an error and returns an HTTP error to the browser, but it's
usually buried in a small dialog the user might miss.

So: even though you see the "Update" button and can pick a file, the
upload will fail before the first byte is written.

## Migration to Minimal SPIFFS

The switch is a one-time operation:

1. **Compile and note the firmware size** (must be < 1.9 MB to fit a
   Minimal SPIFFS slot).
2. **Arduino IDE → Tools → Partition Scheme → "Minimal SPIFFS (1.9MB APP
   with OTA / 190KB SPIFFS)"**.
3. **USB reflash**: this re-writes bootloader + partition table + app
   binary. Cannot be done over the air precisely because we're changing
   the layout the OTA mechanism depends on.
4. **Verify boot**: device should come up normally. Run `/dbg-chip` if
   you've already added that command to confirm flash size / partition
   info.
5. **First OTA test**: change a tiny visible thing in the code (e.g. the
   splash title), recompile, Sketch → Export Compiled Binary, open the
   captive portal at `192.168.4.1/update`, upload the `.ino.bin`, watch
   the device reboot, and confirm the change is visible.

### What you don't lose

- Conversation buffer (in BSS, untouched by partition change)
- WiFi credentials stored in NVS (NVS partition layout doesn't change)
- BLE bonds in NVS (same)
- Compile-time WiFi defaults in `personal-data.h` (in the app image)

### What you do lose

- 875 KB of SPIFFS partition → 190 KB. Irrelevant since we don't use it.
- The "one big app" comfort. If the firmware ever grows past 1.9 MB
  you'll get a build failure and have to either trim or accept losing OTA
  again.

## Verification after migration

Add the chip-info dump (already exists as `/dbg-chip`) and look at the
flash partition info. Or grep at runtime:

```cpp
const esp_partition_t* running = esp_ota_get_running_partition();
const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);
ESP_LOGI(TAG_MM, "Running partition: %s (subtype %d, offset 0x%x, size %u)",
         running->label, running->subtype, running->address, running->size);
if (next) {
    ESP_LOGI(TAG_MM, "Next OTA partition: %s (subtype %d, offset 0x%x, size %u)",
             next->label, next->subtype, next->address, next->size);
} else {
    ESP_LOGE(TAG_MM, "No next OTA partition — OTA will fail");
}
```

Output on Huge App (current):
```
Running partition: app0 (subtype 16, offset 0x10000, size 3145728)
No next OTA partition — OTA will fail
```

Output on Minimal SPIFFS (after migration):
```
Running partition: app0 (subtype 16, offset 0x10000, size 1900544)
Next OTA partition: app1 (subtype 17, offset 0x1e0000, size 1900544)
```

Subtype `16` = `ESP_PARTITION_SUBTYPE_APP_OTA_0`, `17` = `..._OTA_1`.

## Bonus: alternative OTA mechanisms

The WiFiManager portal is one path. Others:

- **`ArduinoOTA`**: standard arduino-esp32 lib, uses mDNS + a Python tool
  (`espota.py`) for over-the-LAN upload. No browser needed. Configure
  hostname + password in `setup()`, then push from `arduino-cli` or the
  IDE's Tools → "Network Port" menu when the device is on the same Wi-Fi.
- **MQTT-based OTA**: subscribe to a topic, receive base64-encoded
  firmware chunks, write them via `Update.write()`. Useful if the device
  is behind NAT and you publish via a public broker (HiveMQ Cloud which
  we already use).
- **HTTPS OTA from a server**: `esp_https_ota` from the IDF. The device
  polls a URL on its own and pulls a new firmware when available.
  Production-grade, requires hosting infrastructure.

All four require the dual-slot partition. The mechanism above the
`Update.begin()` call differs; the partition requirement is the same.

## References

- ESP-IDF Partition Tables: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/partition-tables.html>
- ESP-IDF OTA API: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/ota.html>
- arduino-esp32 partition CSVs:
  `~/.arduino15/packages/esp32/hardware/esp32/3.3.8/tools/partitions/`
- WiFiManager `/update` handler: source around `handleUpdate()` in
  `~/Dev/workspace_pascal/arduino/libraries/WiFiManager/WiFiManager.cpp`
- LittleFS migration guide:
  <https://github.com/lorol/LITTLEFS#why-littlefs>
