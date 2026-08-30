# Wiring — ESP32 ↔ ST7789 TFT ↔ LEDs

Single source of truth for the physical wiring of minimessenger on an **ESP32 DevKit V1 (30-pin)** board. If your board has a different pinout (38-pin DOIT, NodeMCU-32S, WROOM custom layout, etc.), the GPIO numbers below are still correct — only the physical pin order on the headers changes.

The view is from above, USB connector at the bottom — the way the board normally sits when soldered. The TFT module sits to the right of the ESP32 with its pin header on its **left** short edge (the 240-pixel side), and its active area extends to the right.

---

## 1. The complete wiring

Sur la nomenclature Dxx : sur votre carte ESP32, les sérigraphies D2, D5, D23, etc. correspondent 1:1 aux GPIO2, GPIO5, GPIO23. C'est la convention standard sur les cartes ESP32 de cette famille.


```
                                     ┌─────────────────────────┐
                               EN ──►│ 1                SDA 30 ├── GPIO23 ─────────────────────────────────┐
                           GPIO36   ─┤ 2 (input only)       29 │     GPIO22 (free)                         │      ┌──────┬───────────────────────────────────┐
                           GPIO39   ─┤ 3 (input only)       28 │     GPIO1  TX0 ── USB-Ser                 │      │      │                                   │
                           GPIO34   ─┤ 4 (input only)       27 │     GPIO3  RX0 ── USB-Ser         ┌───────┼───►  │ST(CS)│                                   │
                           GPIO35   ─┤ 5 (input only)       26 │     GPIO21 (free)                 │ ┌─────┼───►  │  DC  │                                   │
    ┌─ [220Ω] ─  Green |◄─   GPIO32 ─┤ 6                    25 │     GPIO19 (free)                 │ │     │      │ RST  │                                   │
    +─ [220Ω] ─    Red |◄─   GPIO33 ─┤ 7                SCL 24 ├── GPIO18 ─────────────────────────┼─┼───┐ └───►  │ SDA  │      ST7789 panel — 320 × 240     │
    +─ [220Ω] ─ Yellow |◄─   GPIO25 ─┤ 8 (DAC1)             23 ├── GPIO5  ─────────────────────────┘ │   └─────►  │ SCL  │                                   │
    +───────────  Btn  |◄─   GPIO26 ─┤ 9 (DAC2)             22 ├── GPIO17 ───────────────────────────┘ ┌───────►  │ VCC  │                                   │
    │                      GPIO27   ─┤ 10                   21 │     GPIO16 (free)                     │    ┌──►  │ GND  │                                   │
    │                      GPIO14   ─┤ 11                   20 │     GPIO4  (free)                     │    │     │      │                                   │
    │                      GPIO12   ─┤ 12 (strap)   (strap) 19 ├── GPIO2 (free)                        │    │     └──────┴───────────────────────────────────┘
    │                      GPIO13   ─┤ 13           (strap) 18 │     GPIO15                            │    │
    │                      5V/VIN   ─┤ 15                   16 ├── 3V3 ────────────────────────────────┘    │
    └────────────────────────── GND ─┤ 14                   17 ├── GND ─────────────────────────────────────┘
                                     └─────────┐ USB ┌─────────┘
                                               └─────┘
Rappel MQTT:
    msg/broadcast       : <txt> # deviceId:<id>
                          <txt> # did <id>            testing: shortest way
    msg/unicast/<id>    : idem
    admin/liveness/<id> : BOOT|LIVE|RECO <ts>
                          DEAD
                          LIVE 222                    testing: for forcing a "current" ping

Commands:
    /cmd or 'cmd
```

**Legend**

- EN = Enable. C'est le pin de reset de l'ESP32 — appuyer sur ce bouton tire EN à LOW, ce qui redémarre le microcontrôleur. C'est l'équivalent du bouton RESET sur un Arduino classique.
- `(strap)` strapping pin — the bootloader samples it at reset. Don't drive it unconditionally LOW/HIGH at startup; reusing these for outputs requires extra care (pull-ups, late init).  Pins de strapping à éviter pour des signaux actifs au boot :
 GPIO2, GPIO12, GPIO15 — toutes peuvent perturber le boot selon leur état. Pour s'en servir : ajouter une résistance pull-down (10kOhm) sur GPIOx et la forcer à LOW au boot.
- `(input only)` GPIO34–39 cannot be driven, only read (no PWM, no `digitalWrite`).
- `DAC1`, `DAC2` only on these two GPIOs (25, 26). Les deux seules pins capables de produire une vraie tension analogique en sortie, de 0V à 3.3V avec une résolution de 8 bits (256 niveaux).
  C'est utile pour : Générer un signal audio / Produire une tension de référence / Contrôler un circuit analogique. À ne pas confondre avec le PWM (qui simule une tension analogique par découpage) — le DAC produit une vraie tension continue sans filtrage nécessaire.
- The ST7789 module has **no separate BL pin** — the backlight is wired internally to VCC and is therefore permanently on. `TFT_BL = -1` in the code, and `GPIO4` stays free on the ESP32. The 8-pin module variant (with a BL pin) is not what this build uses.
- The `ST(CS)` label on the module connector is what the silkscreen prints (`ST`); in firmware terms it is the standard ST7789 **CS (chip select)** signal, wired to `GPIO5` (`TFT_CS = GPIO_NUM_5`). **Empirically required on this module** — see "CS-to-GND test" callout below. Not to be confused with `RST` — RST is the panel reset signal (active-low), and BL would be the backlight enable (not present on this module).

---

## 2. ST7789 TFT — pin mapping

Listed in the **physical order printed on the module connector** (this is a 7-pin module — there is no separate BL pin; backlight is internally tied to VCC and is always on).

| Order on connector (top → bottom on the 240-px short edge) | Silkscreen label | Function | Wired to ESP32 | Code constant |
|---|---|---|---|---|
| 1 (top)    | **ST(CS)** | Chip select (standard ST7789 "CS") | `GPIO5` (MUST be a real GPIO, not tied to GND — see "CS-to-GND test" below) | `#define TFT_CS  GPIO_NUM_5` |
| 2          | **DC**  | Data/Command select | `GPIO2` | `#define TFT_DC  GPIO_NUM_2` |
| 3          | **RST** | Hardware reset (active low) | **not connected** | `#define TFT_RST GPIO_NUM_NC` |
| 4          | **SDA** | SPI MOSI (data in) | `GPIO23` (VSPI default MOSI) | implicit — `SPI` lib default |
| 5          | **SCL** | SPI clock | `GPIO18` (VSPI default SCK) | implicit — `SPI` lib default |
| 6          | **VCC** | Logic + LDO supply (3.3 V on most boards; some accept 5 V) | `3V3` (right header pin 16) | — |
| 7 (bottom) | **GND** | Ground | GND rail | — |

Notes —

- The `ST(CS)` silkscreen marking (just `ST` on the actual board) is the standard ST7789 **chip-select** signal (most other modules print it as `CS`). Treat them as synonymous in the firmware.
- **CS-to-GND test (don't do it on this module).** The Sitronix ST7789 spec says the controller works in single-device mode with CS permanently low, so in theory one could remove the D5↔CS wire, tie CS to GND on the breadboard, and set `TFT_CS = GPIO_NUM_NC` to free GPIO5. **This was tried on the actual hardware and failed**: the backlight stays lit but the panel never displays anything. Likely cause — this AliExpress clone of the ST7789 controller uses the rising edge of CS to reset its internal parser between commands; without that edge the SPI byte stream is mis-interpreted from the first command onwards and no valid frame ever lands on the panel. **Keep CS wired to GPIO5** and let Adafruit_SPITFT toggle it per transaction. If you ever want to free GPIO5 anyway, the workaround is to keep CS on GPIO5 (the wire stays) but drive it LOW in software via a free GPIO — but that's more complexity for the same end result and isn't worth it.
- Backlight is **not exposed** on this 7-pin variant: the panel's BL pin is bonded to VCC inside the module, so the display is always lit. `TFT_BL = -1` and there is no software dim/off control. To get backlight power control, you would need an 8-pin module (with a separate BL pin) wired to a free PWM-capable GPIO.
- Most ST7789 panels recover correctly without a dedicated reset line because they integrate a power-on reset circuit — that's why `RST` is left floating here. If garbage appears on the first frame after a brown-out, wire `RST` to `EN` (or a free GPIO) and update `TFT_RST` accordingly.

---

## 3. LEDs

Three indicator LEDs, each driven directly from a GPIO through a 220 Ω series resistor to ground (anode → GPIO, cathode via R → GND).

| LED constant   | GPIO     | Color  | Role                                                                                    |
|----------------|----------|--------|-----------------------------------------------------------------------------------------|
| `LED_POWER_ON` | `GPIO25` | Green  | Solid when device `g_deviceIdFriend2` is online                                         |
| `LED_STATUS`   | `GPIO33` | Red    | Solid when device `g_deviceIdFriend1` is online (driven by the `admin/live` MQTT topic) |
| `LED_FRIEND`   | `GPIO32` | Yellow | Boot / WiFi / MQTT state (blink fast = connecting, blink slow = degraded, on = OK)      |

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

Note on this board (HW-394 / DOIT-V1 variant — see photo): unlike the canonical 30-pin pinout, **GPIO0 is not broken out on the right header**, GND lives at pin 17 (not pin 25 as on most variants), and the right header ends with a dedicated **3V3 pin at position 16** (closest to USB). The GPIO numbers below are still correct for the chip itself; only the *physical position* on the header changed.

| GPIO | Status | Used by |
|---|---|---|
| 0     | strapping | bootloader (USB upload) — only via the BOOT button on this board, not broken out on the header |
| 1     | UART      | USB-Serial TX (console) |
| 2     | TFT       | `TFT_DC` |
| 3     | UART      | USB-Serial RX (console) |
| 4     | **free**  | candidate for `TFT_BL` |
| 5     | TFT       | `TFT_CS` (VSPI default CS) — MUST stay on a real GPIO on this clone module (CS-to-GND was tested and broken, see §2 notes) |
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

Power pins on this board: **5V/VIN** on the left header (bottom, pin 15 near USB) takes the raw USB voltage; **3V3** on the right header (bottom, pin 16 near USB) is the AMS1117-3.3 LDO output — that's the pin to use for TFT VCC and any other 3.3 V peripheral.

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

1. **TFT first** — GND, VCC, SDA→G23, SCL→G18, DC→G2, **CS→G5** (must be a real GPIO on this clone module — CS-to-GND was tested and the panel stays dark, see §2 notes), BL→3V3, RES floating.
2. **Power up and flash** — the splash screen should appear; if not, check SCL/SDA aren't swapped.
3. **LEDs next** — 220 Ω series resistor, anode to GPIO, cathode to GND. Watch the polarity.
4. **Smoke test** — boot, watch the status LED blink during WiFi/MQTT handshake.
5. **Optional: TFT_BL** — cut the BL→VCC tie on the panel, re-route to GPIO4, set `#define TFT_BL 4`.

If the panel stays dark on boot, in order of likelihood: BL not powered → CS/DC swap → SCL/SDA swap → bad solder joint on VCC.
