# Info — Burn-in vs image retention (ST7789 LCD)

**Date:** 2026-06-02
**Context:** Quick reference about the actual risks of leaving the ST7789
panel on continuously, and why the screensaver matters (or doesn't).

## TL;DR

The ST7789 is an **LCD** (liquid crystal + LED backlight), not an OLED.
True burn-in (permanent pixel damage) effectively does not happen on a
well-driven LCD. Image retention (temporary ghost) can occur but disappears
on its own. There is **no hardware risk** to leaving the screen on 24/7 —
the screensaver exists for power consumption and backlight lifetime, not
for damage prevention.

## The two phenomena — easy to confuse

|  | Burn-in (true) | Image retention (rémanence) |
|--|----------------|------------------------------|
| **Visible symptom** | Ghost of the previous image visible when content changes | Ghost of the previous image visible when content changes |
| **Duration** | **Permanent** (until the pixel dies) | **Temporary** — minutes to hours, gone after displaying other content |
| **Physical cause** | Irreversible chemical degradation of the materials (OLED emitters, or LC layer in extreme cases) | Transient memory of the liquid crystal orientation |
| **Tech affected** | Mostly OLED | LCD (and OLED) |
| **Recovery** | None — the pixel is dead | Spontaneous — clears on its own |

Same visual symptom, very different consequences. In French "rémanence"
sometimes covers both meanings in casual usage. In English the distinction
is clean: **burn-in** = permanent, **image retention** = temporary.

## What actually happens on the ST7789 in this project

### Burn-in (true) — risk ≈ 0

The ST7789 controller performs **frame inversion** on every refresh:
the polarity of the voltage driving each pixel alternates from one frame
to the next. This eliminates the DC bias that would, over years, slowly
degrade the liquid crystal material. It's built into the silicon — we
have nothing to do.

On a well-driven LCD, true burn-in essentially does not occur within the
panel's useful lifetime.

### Image retention — possible, transient

Long-static elements (the top status bar with WiFi/MQTT/BT/CapsLock
icons, the bottom input footer) could leave a faint ghost if displayed
unchanged for many hours / days. Behaviour:
- Visible briefly when the content changes (e.g. when entering an info
  overlay or running `/dbg redraw`).
- **Self-erases** within minutes once the pixel content varies.
- No accumulating damage.

### Backlight wear — the actual long-term concern

The white LED backlight, not the LCD layer, is what eventually wears out.
Typical rating is 30 000-50 000 hours to half brightness:
- 24/7 use → ~3-5 years before noticeable dimming.
- With our screensaver (dim at 5 min, off at 6 min) → 10+ years.

This is the real reason the screensaver exists. It's a power and
longevity feature, not a damage-prevention feature.

## Practical guidance for this device

- **Leaving the screen always on is safe** from a hardware-damage
  standpoint. The risk is wasted power (~50-100 mA continuously for the
  backlight) and slow backlight aging.
- **Static UI elements** (status bar, footer) are fine. If you do notice
  a faint ghost after a particularly long static session, displaying
  varied content for a few minutes clears it.
- **Comparison with OLED**: an OLED panel in the same layout (fixed
  status bar + fixed footer) would show a visible permanent ghost after
  a few weeks of 24/7 use. That's why OLED UIs on phones / watches
  shift their fixed elements periodically and aggressively dim in
  standby. On LCD none of this is required.

## Why the screensaver is still useful

Even though it doesn't prevent damage:
- Cuts ~50-100 mA backlight draw when idle — meaningful for battery /
  USB-powered setups.
- Extends backlight half-life from years to decades.
- Avoids any chance of transient retention from very long static
  sessions.
- Quality-of-life: a black screen at night is less distracting than a
  bright always-on display.

## References

- ST7789 datasheet, section "Frame rate / inversion mode" — confirms
  per-frame polarity inversion is enabled by default.
- Adafruit ST7789 product page lists typical backlight life ratings.
- General LCD vs OLED burn-in primer:
  <https://www.rtings.com/tv/learn/permanent-image-retention-burn-in-lcd-oled>
