# Howto — Reading factory-burned data from the ESP32 eFuse

**Date:** 2026-05-31
**Context:** during early `setup()` we need the device MAC to dispatch to the
right `identifyDevice()` branch. `WiFi.macAddress()` and
`esp_read_mac(ESP_MAC_WIFI_STA)` both returned all-zeros because the WiFi
driver wasn't initialised yet. The reliable path is to bypass the runtime
network layer entirely and read the MAC straight from the silicon eFuse.
While we're at it, the eFuse exposes a lot of other useful info — chip ID,
revision, ADC calibration, flash properties, plus a 256-bit user-writable
block. This doc lists what's accessible and how.

## What the eFuse actually is

The ESP32 has a 1024-bit one-time-programmable (OTP) memory block called
the **eFuse**, burned at the factory with information that is part of the
silicon's identity: the base MAC address, the chip revision, the package
variant, ADC calibration coefficients, secure boot / flash encryption key
digests, and some user-reserved fields. Once a bit is burned to 1 it can
never be reset to 0 — hence "one-time programmable".

The eFuse is **always available**, on every boot, in every code path. It
does not need a driver to be initialised, a clock to be configured, or a
peripheral to be powered on. That's why it's the right place to look when
you need device identity in early setup — well before WiFi, BT, Ethernet,
or even SPI are running.

## MAC addresses

### The bug it solves

Four MAC-read APIs are available. **Only one reads the silicon eFuse
directly.** Empirical results on arduino-esp32 3.3.8 (IDF 5.x) when called
in `setup()` before any driver init:

| API | Header | Works in early setup? |
|-----|--------|----------------------|
| `WiFi.macAddress()` / `WiFi.macAddress(uint8_t*)` | `<WiFi.h>` | ❌ returns `00:00:00:00:00:00` |
| `esp_read_mac(buf, ESP_MAC_WIFI_STA)` | `<esp_mac.h>` | ❌ same, depends on WiFi init |
| `esp_efuse_mac_get_default(buf)` | `<esp_mac.h>` | ❌ **TRAP** — returns `ESP_OK` + zeros, see below |
| **`esp_read_mac(buf, ESP_MAC_EFUSE_FACTORY)`** | `<esp_mac.h>` | ✅ **direct eFuse register read** |

#### The `esp_efuse_mac_get_default` trap

The name suggests "read the factory MAC from eFuse" — and that's how the
doc reads. But on IDF 5.x, the implementation has been refactored into a
shim: internally it calls `esp_read_mac(mac, ESP_MAC_BASE)`. And
`ESP_MAC_BASE` does NOT read silicon — it returns a **static cache in RAM**
that is populated by `esp_base_mac_addr_set()` somewhere during IDF startup.
If you call before that startup hook has fired, you get `ESP_OK` + a zeroed
buffer, with no error indication.

This burned us for a long time. The fix is to use `ESP_MAC_EFUSE_FACTORY`
explicitly, documented as "MAC_FACTORY eFuse which was burned by Espressif
in production" — it skips the cache and queries the eFuse hardware.

### Recommended pattern (3-strategy waterfall)

```cpp
#include <esp_mac.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>

uint8_t mac[6] = {0};
auto isAllZero = [](const uint8_t* m) { return (m[0]|m[1]|m[2]|m[3]|m[4]|m[5]) == 0; };

// 1. Direct silicon eFuse read — should always work.
esp_err_t err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);

// 2. Low-level field read — same hardware, different API path.
if (err != ESP_OK || isAllZero(mac)) {
  err = esp_efuse_read_field_blob(ESP_EFUSE_MAC, mac, 48);
}

// 3. Last resort: bring WiFi up and read its derived MAC.
if (err != ESP_OK || isAllZero(mac)) {
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.macAddress(mac);
}
```

Strategy 1 alone suffices in 99 % of cases; the other two are belt-and-
suspenders so an unknown future regression doesn't silently break device
identity again.

### Base MAC vs the derived ones

The eFuse stores **one base MAC** (6 bytes). The other MACs (WiFi STA,
WiFi SoftAP, BT, Ethernet) are derived from it by the silicon at runtime:

| MAC type | Constant | Derivation from base |
|----------|----------|----------------------|
| WiFi STA | `ESP_MAC_WIFI_STA` | base + 0 (= base) |
| WiFi SoftAP | `ESP_MAC_WIFI_SOFTAP` | base + 1 (last byte) |
| Bluetooth | `ESP_MAC_BT` | base + 2 |
| Ethernet | `ESP_MAC_ETH` | base + 3 |

On ESP32 classic, `esp_read_mac(buf, ESP_MAC_EFUSE_FACTORY)` returns the
**same value** as `esp_read_mac(ESP_MAC_WIFI_STA)` would return once WiFi
is up — so use the eFuse-factory variant in early code and treat it as
your STA MAC.

If you call `esp_read_mac(buf, ESP_MAC_BT)` later, you get the BT MAC
(base+2). That part is reliable once at least the BT stack is initialised.

## Chip identification

```cpp
#include <esp_chip_info.h>

esp_chip_info_t info;
esp_chip_info(&info);

// info.model    — esp_chip_model_t: CHIP_ESP32, CHIP_ESP32S2, CHIP_ESP32S3,
//                 CHIP_ESP32C3, CHIP_ESP32H2, CHIP_ESP32C6, CHIP_ESP32P4
// info.revision — silicon revision (0, 1, 3 …)
// info.cores    — 1 or 2
// info.features — bitmask of enabled capabilities:
//                   CHIP_FEATURE_WIFI_BGN
//                   CHIP_FEATURE_BT
//                   CHIP_FEATURE_BLE
//                   CHIP_FEATURE_EMB_FLASH   (flash on the chip die)
//                   CHIP_FEATURE_EMB_PSRAM   (PSRAM on the chip die)
//                   CHIP_FEATURE_IEEE802154
```

Useful for: gating code to a specific variant ("if PSRAM is on-package, use
it for the conversation buffer; else stay in regular heap"), or reporting
to the cloud / log which exact silicon is running.

## Package version

```cpp
#include <esp_efuse.h>

uint32_t pkg = esp_efuse_get_pkg_ver();
// 0 = D0WDQ6
// 1 = D0WD
// 2 = D2WD
// 4 = U4WDH
// 5 = PICO-V3
// 6 = PICO-V3-02
// 7 = PICO-D4
```

Mostly cosmetic information for inventory / logging.

## ADC calibration

The ADC on ESP32 is not super linear out of the box. The factory burns
correction coefficients (Vref, optionally a two-point characterisation)
into eFuse for each individual chip. The `esp_adc_cal` API reads them and
returns a function that converts raw ADC counts to millivolts with much
better accuracy:

```cpp
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

adc_cali_handle_t cal_handle;
adc_cali_curve_fitting_config_t cal_cfg = {
  .unit_id = ADC_UNIT_1,
  .atten   = ADC_ATTEN_DB_12,
  .bitwidth = ADC_BITWIDTH_DEFAULT,
};
adc_cali_create_scheme_curve_fitting(&cal_cfg, &cal_handle);

int millivolts;
adc_cali_raw_to_voltage(cal_handle, raw_adc_value, &millivolts);
```

Worth it if you use `analogRead()` for anything that requires accuracy
(battery monitoring, sensor reading). For our LED / button GPIOs, no.

## Flash properties

These come through ESP.* helpers (which themselves read from the SPI flash
chip header, not strictly the eFuse, but it's the same "factory-fixed"
category):

```cpp
ESP.getFlashChipSize();    // bytes
ESP.getFlashChipMode();    // QIO=0, QOUT=1, DIO=2, DOUT=3, FAST_READ=4, SLOW_READ=5
ESP.getFlashChipSpeed();   // Hz (40 MHz, 80 MHz …)
```

Plus the secure/encryption flags, read straight from eFuse bits:

```cpp
#include <esp_efuse.h>

bool flashEnc = esp_flash_encryption_enabled();   // true if flash encryption was turned on (irreversible)
bool secureBoot = esp_secure_boot_enabled();      // true if secure boot is active

// Number of remaining flash-encryption rotations (typically 7 fresh, decrements per re-key).
// On a stock module it's typically 7.
```

## User-writable area: eFuse block 3 (BLK3)

ESP32 reserves a 256-bit block (BLK3) that's **writable once by the
application** for product-specific data — typical uses:

- Product serial number
- Per-device factory calibration values
- Hardware-bound encryption key (combined with secure boot)
- Provisioning token

```cpp
#include <esp_efuse.h>

uint8_t serial[32];
esp_efuse_read_block(EFUSE_BLK3, serial, 0, 256 /* bits */);

// One-time write (use with care — irreversible):
// esp_efuse_write_block(EFUSE_BLK3, my_data, 0, 256);
```

**Warning:** burning eFuse bits is **physically irreversible**. Test on a
scrap device first. The bits go from 0 → 1 and stay there. There is no
undo, no factory reset, no JTAG override.

## Reset reason

Not eFuse but related identity / diagnostic info — what caused the last
boot:

```cpp
#include <esp_system.h>

esp_reset_reason_t reason = esp_reset_reason();
// ESP_RST_POWERON   — cold power-on
// ESP_RST_EXT       — external reset (RST button)
// ESP_RST_SW        — software esp_restart()
// ESP_RST_PANIC     — exception / panic handler (means CRASH on previous boot)
// ESP_RST_INT_WDT   — interrupt watchdog
// ESP_RST_TASK_WDT  — task watchdog
// ESP_RST_WDT       — other watchdogs
// ESP_RST_DEEPSLEEP — woken from deep sleep
// ESP_RST_BROWNOUT  — brown-out detected (low voltage)
// ESP_RST_SDIO      — reset from SDIO
```

Worth logging at every boot — when a device starts mysteriously
disappearing from MQTT, the reset reason tells you immediately if it's
crashing (PANIC), running out of voltage (BROWNOUT), or just intentionally
restarting (SW).

## Full diagnostic snippet

Drop this in `setup()` or wire it to a `cmd chip` command for on-demand
introspection over serial / MQTT:

```cpp
#include <esp_chip_info.h>
#include <esp_mac.h>
#include <esp_system.h>

void dumpChipInfo() {
  esp_chip_info_t info;
  esp_chip_info(&info);

  ESP_LOGI(TAG_MM, "Chip: model=%d revision=%d cores=%d features=0x%lx",
           (int)info.model, info.revision, info.cores, (unsigned long)info.features);
  ESP_LOGI(TAG_MM, "WiFi:%s BT:%s BLE:%s EmbFlash:%s EmbPSRAM:%s",
           (info.features & CHIP_FEATURE_WIFI_BGN) ? "y" : "-",
           (info.features & CHIP_FEATURE_BT)       ? "y" : "-",
           (info.features & CHIP_FEATURE_BLE)      ? "y" : "-",
           (info.features & CHIP_FEATURE_EMB_FLASH)? "y" : "-",
           (info.features & CHIP_FEATURE_EMB_PSRAM)? "y" : "-");

  ESP_LOGI(TAG_MM, "CPU: %u MHz", ESP.getCpuFreqMHz());
  ESP_LOGI(TAG_MM, "Flash: %u bytes, mode=%d, speed=%u Hz",
           ESP.getFlashChipSize(), ESP.getFlashChipMode(), ESP.getFlashChipSpeed());
  ESP_LOGI(TAG_MM, "Heap: total=%u free=%u largest=%u",
           ESP.getHeapSize(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);  // see "The trap" section — do NOT use esp_efuse_mac_get_default here
  ESP_LOGI(TAG_MM, "MAC base/STA: %02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  esp_read_mac(mac, ESP_MAC_BT);
  ESP_LOGI(TAG_MM, "MAC BT:       %02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  ESP_LOGI(TAG_MM, "Sketch: size=%u free=%u", ESP.getSketchSize(), ESP.getFreeSketchSpace());
  ESP_LOGI(TAG_MM, "IDF: %s", esp_get_idf_version());
  ESP_LOGI(TAG_MM, "Last reset reason: %d", (int)esp_reset_reason());
}
```

Example output on our E32_004:

```
I _MM_ Chip: model=1 revision=3 cores=2 features=0x32
I _MM_ WiFi:y BT:y BLE:y EmbFlash:- EmbPSRAM:-
I _MM_ CPU: 240 MHz
I _MM_ Flash: 4194304 bytes, mode=2, speed=40000000 Hz
I _MM_ Heap: total=327680 free=234412 largest=110580
I _MM_ MAC base/STA: 84:1F:E8:32:B5:B8
I _MM_ MAC BT:       84:1F:E8:32:B5:BA
I _MM_ Sketch: size=1156544 free=786432
I _MM_ IDF: v5.3.1-1078-g1bb0e93da9
I _MM_ Last reset reason: 1
```

## References

- ESP-IDF docs on eFuse: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/efuse.html>
- ESP-IDF docs on MAC API: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/misc_system_api.html#mac-address>
- ESP32 Technical Reference Manual, chapter on eFuse (block layout)
- `esp_chip_info.h`, `esp_mac.h`, `esp_efuse.h`, `esp_system.h` in the
  installed core: `~/.arduino15/packages/esp32/tools/esp32-libs/3.3.8/include/`
