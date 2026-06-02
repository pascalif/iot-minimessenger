# Info — When (and how) to wire the TFT RST pin

Reference note on the `RST` pin of the ST7789 module: what it does, why it's left floating today, and the handful of scenarios where actually wiring it pays off. Companion to `wiring.md` (which describes the current default `TFT_RST = -1` setup).

---

## 1. What RST does

`RST` (sometimes silkscreened `RES`) is the **hardware reset** signal of the ST7789 controller. It is active-low: pulling it briefly to GND forces the controller to restart its init sequence — registers reset, framebuffer cleared, display turned off, ready to accept the standard ST7789 boot commands again.

It is functionally independent from:
- **BL** — the backlight enable (controls only the LED, not the controller logic). Not present on this 7-pin module.
- **EN** on the ESP32 — that's the ESP's own reset; doesn't reach the TFT unless explicitly wired.

---

## 2. Why we leave it floating today

`TFT_RST = -1` in `minimessenger.ino`, and the silkscreen `RST` pin on the module is not connected to anything.

This works because **every modern ST7789 module integrates a Power-On Reset (POR) circuit**: a tiny voltage-threshold detector on VCC that pulses the controller's internal reset line as the supply rail rises through ~1.8 V at boot. As long as the panel is powered cleanly from cold, it self-resets correctly without any external help.

For 99 % of the use cases in this project — boot once, run for hours, occasional clean reflash — that's enough.

---

## 3. The five cases where wiring RST is worth the trouble

### 3.1 Brown-out / power glitches

If the ESP32 reboots while the panel keeps its supply (partial voltage dip, micro-cuts on USB, weak battery), the ST7789 can end up in a corrupted state: uninitialised framebuffer, SPI registers out of phase, screen showing bands or glitches on the first frame after the firmware reboots.

Wiring `RST` to the ESP32's `EN` line (or to a GPIO with a `LOW→HIGH` pulse in `setup()`) guarantees the panel restarts **in sync with the firmware**.

### 3.2 Heavy dev sessions with repeated flashes

During a session where you reflash/reset the ESP32 every 30 seconds, sometimes the display "freezes" on a previous state and only un-plugging USB recovers it. With `RST` wired to a GPIO, you can force a clean panel reset on every boot, never touching the cable.

### 3.3 SPI bus shared with another peripheral

If you later add another SPI device (SD card, second display, sensor) on the VSPI bus and they "step on each other" (CS mishandling, stuck transactions), being able to hard-reset the ST7789 independently avoids having to reboot the whole board.

### 3.4 Cheap modules with a weak POR

Some bottom-tier AliExpress modules cut costs on the POR circuit. Symptom: on the very first power-up after a long unplug, the screen shows random pixels until a reset command lands. Rare in practice but it happens — and you typically only notice it after a few days of erratic behaviour.

### 3.5 Deep-sleep wake-up

If you put the ESP32 in deep-sleep and wake it later, the panel may be in any state (especially if BL stayed powered). A reset at wake-up guarantees a clean re-init regardless of what the panel was doing when the ESP went to sleep.

---

## 4. How to wire it, concretely

### Option A — RST tied to ESP32 `EN` (zero-GPIO, simplest)

```
ESP32 EN ─────┬─────── RST (TFT)
              │
              └─── reset button / pull-up R10k
```

The ESP32 board's RESET button now resets the panel too, automatically and in lock-step.

**Code:** no change. `TFT_RST = -1` is still correct since Adafruit_ST7789 doesn't need to drive it — the hardware does the pulse.

**Trade-off:** you lose the ability to reset the panel independently of the ESP32 (every TFT reset = full board reboot). For this project that's fine.

### Option B — RST on a free GPIO (most flexible)

Pick a free GPIO that's not a strapping pin and not on the SPI bus. On the user's HW-394 / DOIT V1 variant, good candidates are `GPIO16` or `GPIO17` (UART2 pins, unused here), or `GPIO27`.

```cpp
#define TFT_RST 16   // or whichever free GPIO
```

Adafruit_ST7789 will pulse the line automatically at `begin()`. You can also call a manual reset later if you want to recover from a glitch without rebooting the ESP32:

```cpp
pinMode(TFT_RST, OUTPUT);
digitalWrite(TFT_RST, LOW);
delayMicroseconds(10);
digitalWrite(TFT_RST, HIGH);
delay(150);     // ST7789 needs ~120 ms to wake up after reset
```

**Trade-off:** consumes one GPIO. Gives you per-panel reset control.

---

## 5. Decision matrix

| Situation | Wire RST? | How |
|---|---|---|
| Stable bench setup, USB always clean | **no** | leave floating, `TFT_RST = -1` |
| Battery-powered, brown-outs likely | **yes** | Option A (RST → EN) |
| Heavy dev iterations, flaky panel state | **yes** | Option A or B |
| Multi-SPI bus, need independent reset | **yes** | Option B (RST → GPIO) |
| Suspect a weak POR on a cheap module | **yes** | Option A is enough |
| Deep-sleep wake-up integration | **yes** | Option B + manual reset on wake |

---

## 6. Quick verification

If you ever see "first frame is garbled but it works fine after a few seconds", that's the classic missing-POR symptom — wire `RST` to `EN` (Option A) and the artifact disappears. No code change required for that path.

If you see "everything works but a hot-reset (button press) leaves the panel stuck", that's the dev-iteration case — Option A also solves it.

Otherwise, the current `TFT_RST = -1` configuration is the right default: cheaper wiring, one less GPIO consumed, and the panel's internal POR handles the cold-boot case correctly.
