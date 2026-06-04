# Howto — Hardware vertical scrolling on TFT displays

**Date:** 2026-05-30
**Context:** evaluating whether to replace the full-redraw conversation
buffer (`redrawAllConversations()` in `minimessenger.ino`) with the
controller's built-in hardware scroll, for performance and burn-in margin.

## Library / core versions at time of writing

| Component | Version | Path |
|-----------|---------|------|
| ESP32 Arduino core | **3.3.8** | `~/.arduino15/packages/esp32/hardware/esp32/3.3.8/` |
| Adafruit_GFX_Library | **1.12.6** | `~/Dev/workspace_pascal/arduino/libraries/Adafruit_GFX_Library/` |
| Adafruit ST7735 and ST7789 Library | **1.11.0** | `~/Dev/workspace_pascal/arduino/libraries/Adafruit_ST7735_and_ST7789_Library/` |
| NimBLE-Arduino | 2.5.0 | (unrelated to display, recorded for completeness) |

Re-run the section "Verifying support on your install" at the bottom of this
file if these versions change — the public API surface for raw command sending
is stable since GFX 1.x but worth double-checking.

## What hardware vertical scrolling actually is

Most modern color TFT controllers (ST7789, ST7735, ILI9341, ILI9488, HX8357,
GC9A01…) implement the **MIPI DCS** (Display Command Set) scroll commands:

| Command | Hex | Purpose |
|---------|-----|---------|
| `VSCRDEF` (Vertical Scroll Definition) | `0x33` | Partition the framebuffer into Top Fixed / Scroll Area / Bottom Fixed (in lines) |
| `VSCSAD` (Vertical Scroll Start Address) | `0x37` | Set the "virtual top" of the scroll area — bumping this by N lines visually scrolls everything up by N |

The controller maintains its own framebuffer in RAM. Writing to `VSCSAD`
**doesn't move any pixels** — it changes which framebuffer line is mapped to
the top of the visible scroll area. Effect: a scroll of 60 fps with zero CPU
load. New content is drawn at the (now bottom-of-screen) line that the scroll
just vacated.

## Compatibility matrix

Practical compatibility across the controllers most often paired with ESP32:

| Controller | Typical use | HW vertical scroll | Notes |
|------------|-------------|--------------------|-------|
| **ST7789** | 1.3"–2.4" TFT 240×240 / 240×320 (minimessenger) | ✅ | VSCRDEF + VSCSAD, 320-line framebuffer |
| ST7735    | 1.44" / 1.8" TFT | ✅ | Same commands, smaller framebuffer |
| ILI9341   | 2.2"–2.8" TFT 240×320 (most ubiquitous) | ✅ | Same commands |
| ILI9488   | 3.5" 320×480 | ✅ | Same commands |
| HX8357    | 3.5" 320×480 | ✅ | Same commands |
| GC9A01    | 1.28" round 240×240 (smartwatch DIY) | ✅ | Same commands |
| SSD1351   | 1.5" OLED color | ✅ | Native scroll command, slightly different opcodes |
| SSD1306   | 0.96" OLED mono 128×64 | ⚠️ | Continuous auto-scroll only (no direct `seek` to an address) — awkward for chat use |
| SH1106    | OLED mono (SSD1306 clone) | ⚠️ | Even more limited than SSD1306 |
| Waveshare e-paper / GxEPD | E-ink | ❌ | Impossible by construction: e-ink refresh is full-frame, not line-by-line |
| Nextion / "smart" TFT | Integrated GPU | n/a | Their own scripting stack; DCS not exposed |

**Verdict:** anything modern with a colour TFT controller is fine. OLED mono
is partial. E-paper is a hard no.

## The four gotchas that actually bite

### 1. Rotation breaks the intuition (the big one for landscape projects)

The controller's "vertical" is in **native framebuffer coordinates** —
always portrait, always the LCD scan direction. `setRotation(1)` or
`setRotation(3)` rotates the **drawing** in software (the GFX library swaps
coordinates), but the scroll register stays in framebuffer-space.

For minimessenger, `setupDisplay()` calls `pDisp->setRotation(1)` → 320 px
wide × 240 px tall from the user's perspective. In that configuration, asking
the controller to "scroll vertically" actually shifts everything
**horizontally** from the user's POV. Unusable for chat as-is.

Three ways out:

- **Switch the layout to portrait** (`setRotation(0)`): rebuild the line
  positioning (240 wide × 320 tall) and HW scroll is then user-vertical.
  Lots of UI rework but the prize is real.
- **Stay landscape, use software scroll only**: keep the current approach
  (full redraw, soon partial redraw) — HW scroll buys nothing here.
- **Buy a controller whose native orientation is landscape**: rare in the
  hobbyist parts catalog.

### 2. Adafruit_GFX doesn't expose the scroll API

The Adafruit_ST77xx class has no `setupScrollArea()` / `scrollAddress()`
methods. You drop down to the base class `Adafruit_SPITFT::sendCommand()`:

```cpp
// In Adafruit_SPITFT.h:213-215 :
void sendCommand(uint8_t commandByte, uint8_t *dataBytes,
                 uint8_t numDataBytes);
void sendCommand(uint8_t commandByte, const uint8_t *dataBytes = NULL,
                 uint8_t numDataBytes = 0);
```

Concrete usage for ST7789 in portrait, scrolling the entire 320 lines:

```cpp
void setupScrollArea(Adafruit_ST7789* pDisp,
                     uint16_t topFixed, uint16_t scrollHeight, uint16_t bottomFixed) {
  uint8_t args[6] = {
    (uint8_t)(topFixed   >> 8), (uint8_t)(topFixed   & 0xFF),
    (uint8_t)(scrollHeight >> 8), (uint8_t)(scrollHeight & 0xFF),
    (uint8_t)(bottomFixed >> 8), (uint8_t)(bottomFixed & 0xFF),
  };
  pDisp->sendCommand(0x33 /* VSCRDEF */, args, 6);
}

void scrollTo(Adafruit_ST7789* pDisp, uint16_t startLine) {
  uint8_t args[2] = {
    (uint8_t)(startLine >> 8), (uint8_t)(startLine & 0xFF),
  };
  pDisp->sendCommand(0x37 /* VSCSAD */, args, 2);
}
```

The alternative library **TFT_eSPI** (Bodmer) exposes
`setupScrollArea(uint16_t tfa, uint16_t bfa)` and
`scrollAddress(uint16_t vsp)` directly. Faster than Adafruit_GFX on ESP32
too. Worth considering for a port if you commit to HW scroll.

### 3. Scroll wraps inside the framebuffer

The controller's RAM is **fixed-size** (320 lines for ST7789). `VSCSAD` is
modulo that height. So at the boundary, your old content rolls back to the
top and you have to draw over it as new content scrolls in. Typical pattern
for a terminal/chat:

```
1. write new line at virtual_bottom (= (vsp + scroll_area_h - line_h) % fb_h)
2. vsp = (vsp + line_h) % fb_h
3. sendCommand(VSCSAD, vsp)
```

Done. No `for` loop, no `fillScreen`, no `redrawAll`. The controller does the
visible motion; you only ever draw the **new line** plus a small black
clear-strip ahead of it.

### 4. Scroll area is partitioned once with VSCRDEF

`VSCRDEF` declares: `top_fixed_height + scroll_area_height + bottom_fixed_height
= framebuffer_height` (320 for ST7789). You can keep a header or footer
fixed (e.g. status bar at top) and scroll only the middle. But you only
declare this once at setup — you can't dynamically resize the scroll area
mid-conversation without re-sending VSCRDEF and accepting visual artifacts.

## Implementation in minimessenger (2026-05-30)

We migrated. The `feat/hardware-scrolling` branch contains the working
implementation. Summary of decisions and surprises:

### What we did

1. Switched from `setRotation(1)` (landscape) to **`setRotation(2)`** (portrait
   180°-flipped — see "The rotation gotcha" below for why not 0).
2. Added `hwScrollSetupArea()`, `hwScrollTo()`, `hwScrollReset()` helpers
   that wrap raw `sendCommand(0x33/0x37, ...)` calls.
3. Called `hwScrollSetupArea()` once in `setupDisplay()` with TFA=0, VSA=320,
   BFA=0 (full-screen scroll area).
4. Rewrote the scroll/draw path inside `addConversationBlock()` to write
   directly into the framebuffer ring and bump VSCSAD by H — the call to
   `redrawAllConversations()` is gone from this path.
5. Added `hwScrollReset()` calls at the top of `cleanScreen()`,
   `showSplashScreen()`, and `showUpdatedInfoScreen()` so the "static" screens
   render at known framebuffer coordinates (no leftover scroll offset).

### The rotation gotcha that surprised us

I initially used `setRotation(0)`, which on Adafruit reference ST7789 panels
puts data line 0 at the user's perceived top of the screen. With our
algorithm (draw at the lowest framebuffer Y available, then VSCSAD += H), the
expected visual result was *new line at the user's bottom, old content scrolls
up*. That is in fact what Adafruit's reference module produces.

On the specific ST7789 module wired into this device, however, the panel
behaves as if it's mounted 180° from Adafruit's convention: drawing at GFX
y=0 lands at the user's *bottom*, not their top. With `setRotation(0)`, the
algorithm produced a chat that scrolled in the wrong direction (new lines at
the user's top, scrolling down).

Switching to **`setRotation(2)`** (which sets MADCTL bits `MX=1, MY=1`) makes
GFX flip both axes; text stays right-side-up because GFX inverts internally,
but the framebuffer's "bottom" now aligns with the user's "bottom" again.
Same algorithm, correct visuals.

Lesson: if you ever swap the LCD module for one of a different brand and
chat scrolls "the wrong way" again, try the other portrait rotation
(`0 ↔ 2`) before debugging the algorithm.

### Heap / perf gain

Anecdotal: the old `redrawAllConversations()` path called `fillScreen()`
then re-rendered N TextLines worth of glyphs every time a new line came in.
With HW scroll, only the new line is drawn (one `fillRect` strip + two text
runs) and VSCSAD is bumped. Perceived as instant — no more visible blanking
on each message. Heap pressure on the redraw path is also gone (no full SPI
flush of the framebuffer).

### Dead code left behind

`redrawAllConversations()` is no longer called by any production path. We
keep it for now in case a future "theme change / font swap / clear and
rebuild from ring buffer" path wants it; it works correctly as long as
`hwScrollReset()` is called first to put VSCSAD back at 0.

## Verifying support on your install

If the library versions above have moved, re-check the API surface:

```bash
# Confirm sendCommand exists on the SPITFT base class:
grep -n "sendCommand" ~/Dev/workspace_pascal/arduino/libraries/Adafruit_GFX_Library/Adafruit_SPITFT.h

# Look for any new high-level scroll helper that may have been added to ST77xx:
grep -nE "Scroll|VSCSAD|VSCRDEF" ~/Dev/workspace_pascal/arduino/libraries/Adafruit_ST7735_and_ST7789_Library/*.h
```

## References

- MIPI Display Command Set (DCS) specification, §6.2 (Display Address Mode commands)
- ST7789 datasheet: commands `VSCRDEF (33h)` p. 200, `VSCSAD (37h)` p. 209
- Adafruit_SPITFT.h:213-215 — `sendCommand()` signatures
- TFT_eSPI library — see `setupScrollArea()` / `scrollAddress()` for a
  higher-level equivalent if migrating away from Adafruit_GFX
