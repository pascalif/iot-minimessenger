# Wiring — ESP32 ↔ ST7789 TFT ↔ LEDs

Single source of truth for the physical wiring of minimessenger on an **ESP32 DevKit V1 (30-pin)** board. If your board has a different pinout (38-pin DOIT, NodeMCU-32S, WROOM custom layout, etc.), the GPIO numbers below are still correct — only the physical pin order on the headers changes.

The view is from above, USB connector at the bottom — the way the board normally sits when soldered. The TFT module sits to the right of the ESP32 with its pin header on its **left** short edge (the 240-pixel side), and its active area extends to the right.

---

## 1. The complete wiring

```
                          ┌─────────────────────────┐                                        ┌──────────────────────────────────────────────────────────┐
                    EN ──►│ 1                    30 ├── GPIO23 ────────────────────── SDA ──►│                                                          │
                  GPIO36 ─┤ 2 (input only)       29 │     GPIO22 (free)                      │                                                          │
                  GPIO39 ─┤ 3 (input only)       28 │     GPIO1  TX0 ── USB-Ser              │                                                          │
                  GPIO34 ─┤ 4 (input only)       27 │     GPIO3  RX0 ── USB-Ser              │                                                          │
                  GPIO35 ─┤ 5 (input only)       26 │     GPIO21 (free)                      │                                                          │
   LED_STATUS ◄── GPIO32 ─┤ 6                    25 │     GND                                │                 ST7789 panel — 320 × 240                 │
 LED_FRIEND_1 ◄── GPIO33 ─┤ 7                    24 │     GPIO19 (free)                      │                                                          │
 LED_FRIEND_2 ◄── GPIO25 ─┤ 8 (DAC1)             23 ├── GPIO18 ────────────────────── SCL ──►│                                                          │
                  GPIO26 ─┤ 9 (DAC2)             22 ├── GPIO5  ────────────────────── CS  ──►│                                                          │
                  GPIO27 ─┤ 10                   21 │     GPIO17 (free)                      │                                                          │
                  GPIO14 ─┤ 11                   20 │     GPIO16 (free)                      │                                                          │
                  GPIO12 ─┤ 12 *strap            19 ├── GPIO4  ────────────────────── BL  ──►│                                                          │
                  GPIO13 ─┤ 13                   18 │     GPIO0  *strap                      │                                                          │
                     GND ─┤ 14                   17 ├── GPIO2  ────────────────────── DC  ──►│                                                          │
                  5V/VIN ─┤ 15                   16 │     GPIO15 *strap                      │                                                          │
                          └────────────┬────────────┘                                        └──────────────────────────────────────────────────────────┘
                                    ┌──┴──┐
                                    │ USB │ ◄── plug end (5 V → AMS1117 LDO → 3V3 rail, console serial)
                                    └─────┘

                                                                                             pin header on this short (240-px) left edge ▲
                                                                                             the 320-px long edge extends to the right
```

**Legend**

- `──►` an active wire entering a peripheral (ESP32 GPIO → TFT pin, or external → ESP32).
- `◄──` a wire leaving the ESP32 toward an output peripheral (LEDs on the left header).
- `(free)` no wire connected today — available for future expansion.
- `*strap` strapping pin — the bootloader samples it at reset. Don't drive it unconditionally LOW/HIGH at startup; reusing these for outputs requires extra care (pull-ups, late init).
- `(input only)` GPIO34–39 cannot be driven, only read (no PWM, no `digitalWrite`).
- `DAC1`, `DAC2` only on these two GPIOs (25, 26).
- The TFT is drawn as a **landscape rectangle**: the pin header (the short 240-px edge) is on the left, facing the ESP32; the active area extends 320 px to the right. The physical module is portrait (pins on a short edge); rotate it 90° clockwise on the bench and the orientation matches the diagram. Only the wires entering the rectangle on its left edge are connected — everything inside is just empty panel area.
- TFT power (VCC, GND) is **not** drawn on the wire bus to keep the diagram clean: VCC ties to the 3V3 rail and GND to common ground. See §2 for the full pin mapping.

---

## 2. ST7789 TFT — pin mapping

| TFT pin | Function | Wired to ESP32 | Code constant |
|---|---|---|---|
| GND | Ground | `GND` | — |
| VCC | Logic + LDO supply (3.3 V on most boards; some accept 5 V) | `3V3` | — |
| SCL | SPI clock | `GPIO18` (VSPI default SCK) | implicit — `SPI` lib default |
| SDA | SPI MOSI | `GPIO23` (VSPI default MOSI) | implicit — `SPI` lib default |
| RES | Hardware reset | **not connected** | `#define TFT_RST -1` |
| DC  | Data/Command select | `GPIO2`  | `#define TFT_DC  2` |
| CS  | Chip select | `GPIO5`  | `#define TFT_CS  5` |
| BL  | Backlight enable / PWM | **tied to 3.3 V** today | `#define TFT_BL -1` (see §4) |

Reset note — most ST7789 panels recover correctly without a dedicated reset line because they integrate a power-on reset circuit. If garbage appears on the first frame after a brown-out, wire `RES` to `EN` (or a free GPIO) and update `TFT_RST` accordingly.

---

## 3. LEDs

Three indicator LEDs, each driven directly from a GPIO through a 220 Ω series resistor to ground (anode → GPIO, cathode via R → GND).

| LED constant | GPIO | Role |
|---|---|---|
| `LED_STATUS`   | `GPIO32` | Boot / WiFi / MQTT state (blink fast = connecting, blink slow = degraded, on = OK) |
| `LED_FRIEND_1` | `GPIO33` | Solid when device `g_deviceIdFriend1` is online (driven by the `admin/live` MQTT topic) |
| `LED_FRIEND_2` | `GPIO25` | Solid when device `g_deviceIdFriend2` is online |

The `LED_QTY = 17` array in the code is sized for **raw GPIO numbers**, not LED count — bear that in mind if a fourth LED ever lands on a GPIO > 16.

---

## 4. TFT backlight shutdown — planned but not yet wired

The screen-power state machine in `updateDisplayPowerState()` has three states. With `TFT_BL == -1` (today), only `DISPLAY_OFF` has a visible effect because the backlight is hard-wired to the 3V3 rail; dimming is invisible and the panel "off" state still leaks ~25–40 mA of LED current.

| State | Action when `TFT_BL == -1` (today) | Action when `TFT_BL == 4` (target) |
|---|---|---|
| `DISPLAY_ON`      | nothing (always on)             | PWM at 100 % |
| `DISPLAY_DIMMED`  | **invisible** (still 100 %)     | PWM at 50 % |
| `DISPLAY_OFF`     | `enableDisplay(false)` — panel sleep, backlight stays lit | PWM at 0 % **and** `enableDisplay(false)` — fully dark |

To enable it: cut the trace (or unsolder the resistor) that ties BL to VCC on the TFT module, re-route BL to `GPIO4`, and change the constant to `#define TFT_BL 4`.

Why GPIO4 is the recommended target:

- **Not a boot strap** (unlike GPIO0/2/12/15) — safe to drive any value at reset.
- **Not input-only** (unlike GPIO34/35/36/39) — supports `analogWrite()` / `ledcWrite()` for PWM dimming.
- **Not on the SPI bus** used by the TFT (GPIO5/18/19/23 are reserved).
- **Not on UART2** (GPIO16/17 are kept reserved for a possible future serial peripheral).
- **Physical proximity** — pin 19 on the right header, two rows above GPIO2/DC and GPIO5/CS, so the harness routing stays short.

Alternative candidates if GPIO4 is needed for something else later: `GPIO13`, `GPIO14`, `GPIO27`. Avoid `GPIO12` (boot strap) and `GPIO21/22` (commonly reserved for I2C if you ever add an I2C sensor).

---

## 5. Pin reservation summary

| GPIO | Status | Used by |
|---|---|---|
| 0     | strapping | bootloader (USB upload) |
| 1     | UART      | USB-Serial TX (console) |
| 2     | TFT       | `TFT_DC` |
| 3     | UART      | USB-Serial RX (console) |
| 4     | **free**  | candidate for `TFT_BL` |
| 5     | TFT       | `TFT_CS` (VSPI default CS) |
| 12    | strapping | boot voltage select — keep unused |
| 13    | free      | (HSPI MOSI, unused since we use VSPI) |
| 14    | free      | (HSPI CLK, unused) |
| 15    | strapping | bootloader logging select — keep unused |
| 16    | free      | (UART2 RX, unused) |
| 17    | free      | (UART2 TX, unused) |
| 18    | TFT       | `SCL` / SPI clock (VSPI default) |
| 19    | free      | (VSPI MISO, unused — display is write-only) |
| 21    | free      | (I2C SDA default, unused — no I2C peripherals) |
| 22    | free      | (I2C SCL default, unused) |
| 23    | TFT       | `SDA` / SPI MOSI (VSPI default) |
| 25    | LED       | `LED_FRIEND_2` |
| 26    | free      | (DAC2) |
| 27    | free      | |
| 32    | LED       | `LED_STATUS` |
| 33    | LED       | `LED_FRIEND_1` |
| 34    | free      | input-only, ADC1_6 — could host a battery-voltage divider |
| 35    | free      | input-only, ADC1_7 |
| 36    | free      | input-only (`VP`), ADC1_0 |
| 39    | free      | input-only (`VN`), ADC1_3 |

In-radio peripherals (no pin cost):

- **WiFi** — uses the on-chip 2.4 GHz radio, antenna on the module's PCB / U.FL connector.
- **BLE** — same radio, time-multiplexed with WiFi (NimBLE coexistence handled by the IDF).

---

## 6. Power budget

USB-C / micro-USB supplies 5 V → the board's on-board **AMS1117-3.3** LDO drops it to 3.3 V → ESP32 VDD3P3, TFT VCC (and, today, TFT BL). GND is common.

| Consumer | Typical | Peak |
|---|---|---|
| ESP32 (WiFi+BLE active) | 80–120 mA | ~320 mA on TX bursts |
| TFT panel (logic only) | ~15 mA | — |
| TFT backlight (3 LEDs in series, BL → VCC) | 25–40 mA | — |
| 3 indicator LEDs (worst case, all on) | 30 mA | — |
| **Total** | **~180 mA** | **~410 mA** |

The AMS1117-3.3 is rated for ~1 A but is thermally limited; expect ~600 mA usable in practice. Comfortably within budget for the current load. If a buzzer or motor is added later, give it its own 3.3 V supply.

---

## 7. Quick checklist when wiring a fresh board

1. **TFT first** — GND, VCC, SDA→G23, SCL→G18, DC→G2, CS→G5, BL→3V3, RES floating.
2. **Power up and flash** — the splash screen should appear; if not, check SCL/SDA aren't swapped.
3. **LEDs next** — 220 Ω series resistor, anode to GPIO, cathode to GND. Watch the polarity.
4. **Smoke test** — boot, watch the status LED blink during WiFi/MQTT handshake.
5. **Optional: TFT_BL** — cut the BL→VCC tie on the panel, re-route to GPIO4, set `#define TFT_BL 4`.

If the panel stays dark on boot, in order of likelihood: BL not powered → CS/DC swap → SCL/SDA swap → bad solder joint on VCC.
