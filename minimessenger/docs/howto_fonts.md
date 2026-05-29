# Howto — Accented characters (é, ç, à, ô, …) on the TFT

## Problem

Accented characters appear correctly in the serial console but **do not appear
at all** on the ST7789 display: not even a space, not even a placeholder
square. They are silently dropped, leaving no gap in the rendered text.

### Why serial works

The Arduino IDE saves source files in **UTF-8**. Strings received from MQTT
arrive over the wire as raw bytes (also UTF-8 in practice). `Serial.print()`
forwards bytes verbatim over the UART; a UTF-8-capable terminal recognises the
multi-byte sequences and renders them correctly.

| Character | UTF-8 bytes | Latin-1 (ISO-8859-1) | CP437 |
|-----------|-------------|----------------------|-------|
| `é`       | `0xC3 0xA9` | `0xE9`               | `0x82` |
| `è`       | `0xC3 0xA8` | `0xE8`               | `0x8A` |
| `ê`       | `0xC3 0xAA` | `0xEA`               | `0x88` |
| `à`       | `0xC3 0xA0` | `0xE0`               | `0x85` |
| `ç`       | `0xC3 0xA7` | `0xE7`               | `0x87` |
| `ô`       | `0xC3 0xB4` | `0xF4`               | `0x93` |
| `î`       | `0xC3 0xAE` | `0xEE`               | `0x8C` |

### Why the TFT doesn't

`Adafruit_GFX::write(uint8_t c)` routes each byte through the active GFX font's
glyph table. The relevant block in `Adafruit_GFX.cpp` (around line 1519):

```cpp
uint8_t first = pgm_read_byte(&gfxFont->first);
if ((c >= first) && (c <= (uint8_t)pgm_read_byte(&gfxFont->last))) {
  // draw glyph, advance cursor
}
// else: NOTHING — no glyph, no cursor advance.
```

`FreeSans9pt7b.h:200-201` declares its glyph range as:

```cpp
const GFXfont FreeSans9pt7b PROGMEM = { /* glyphs */, /* bitmap */, 0x20, 0x7E, 22 };
//                                                                   first  last
```

So any byte ≥ `0x80` is silently dropped. The UTF-8 sequence `0xC3 0xA9` ("é")
is therefore two consecutive ignored bytes — explaining why the character
vanishes without trace.

## Solutions overview

| Option | Effort | Visual fidelity | Notes |
|--------|--------|-----------------|-------|
| **A** — UTF-8 → ASCII transliteration (`é → e`) | ~15 min | Lossy (no accents) | Keep current font, add a transliteration table |
| **B** — UTF-8 → Latin-1 + extended GFX font | ~30 min one-time + small code change | Perfect | Recommended — standard ESP32 + French solution |
| **C** — Built-in 5×7 font + CP437 mapping | ~10 min | Tiny pixelated text | Quick proof of concept only |

The rest of this document focuses on **Option B**, the recommended path.

---

## Option B — Extended GFX font + UTF-8 → Latin-1 (recommended)

The plan:

1. Build the Adafruit `fontconvert` utility from the GFX library.
2. Generate a `FreeSans9pt7b` variant whose glyph range covers Latin-1
   (`0x20`..`0xFF`) instead of ASCII-only (`0x20`..`0x7E`).
3. Drop the generated `.h` into the sketch and change the `#include`.
4. Add a UTF-8 → Latin-1 conversion function and call it on every string that
   reaches the display.

### Step 1 — Install build dependencies

```bash
sudo apt update
sudo apt install -y build-essential libfreetype6-dev
```

### Step 2 — Build the fontconvert utility

`fontconvert` is shipped inside the Adafruit_GFX library:

```bash
cd ~/Dev/workspace_pascal/arduino/libraries/Adafruit_GFX_Library/fontconvert
make
```

The output is an executable named `fontconvert` in the same directory. The
binary takes a TTF and emits a `GFXfont` C header to stdout:

```
Usage: fontconvert fontfile size [first] [last]
```

When `first` / `last` are omitted, the defaults are `0x20`..`0x7E` (ASCII).
Passing both extends the glyph range.

### Step 3 — Get the FreeSans.ttf source

The Adafruit_GFX repo does not ship the TTF outline files (license / size
issue). Grab GNU FreeFont:

```bash
mkdir -p ~/Downloads/freefont
cd ~/Downloads/freefont
wget -q https://ftp.gnu.org/gnu/freefont/freefont-ttf-20120503.zip
unzip -o freefont-ttf-20120503.zip
# The TTF we need is now at ~/Downloads/freefont/freefont-20120503/FreeSans.ttf
```

### Step 4 — Generate the extended font header

Generate FreeSans 9pt with the Latin-1 glyph range `[0x20, 0xFF]` = `[32, 255]`:

```bash
cd ~/Dev/workspace_pascal/arduino/libraries/Adafruit_GFX_Library/fontconvert
./fontconvert ~/Downloads/freefont/freefont-20120503/FreeSans.ttf 9 32 255 \
  > ~/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/FreeSans9pt8b_latin1.h
```

This produces a header roughly 4–5 KB in size (versus ~1.8 KB for the
ASCII-only version). Flash budget is unaffected on a 1.9 MB partition.

Sanity check the generated file:

```bash
grep -E "^const GFXfont " ~/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/FreeSans9pt8b_latin1.h
# Expected line ends with:  ..., 0x20, 0xFF, <yAdvance> };
```

If `first=0x20` and `last=0xFF` show up, the extended range is in.

### Step 5 — Switch the sketch to the new font

Two things to update in `minimessenger.ino`:

**5a. Change the `#include`** to point at the generated header:

```bash
sed -i \
  's|#include <Fonts/FreeSans9pt7b.h>|#include "FreeSans9pt8b_latin1.h"|' \
  ~/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/minimessenger.ino
```

**5b. Rename the C symbol** from `FreeSans9pt7b` to `FreeSans9pt8b`.

This is mandatory: `fontconvert` builds the symbol name from the size and a
suffix that depends on the highest glyph code (`fontconvert.c:112`):

```c
sprintf(ptr, "%dpt%db", size, (last > 127) ? 8 : 7);
```

With `last=255` we get the `8b` suffix, so the generated header declares
`const GFXfont FreeSans9pt8b PROGMEM = {...}`. The old `&FreeSans9pt7b`
references in the sketch will no longer compile.

Rename all symbol references with a word-boundary-aware `sed`. The `\b`
anchors prevent the filename `FreeSans9pt7b_latin1.h` (which has `_` after the
`7b`, not a word boundary) from being touched:

```bash
sed -i 's/\bFreeSans9pt7b\b/FreeSans9pt8b/g' \
  ~/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/minimessenger.ino
```

Verify:

```bash
grep -n "FreeSans9pt" ~/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/minimessenger.ino
# Expected: line 97 still has `#include "FreeSans9pt8b_latin1.h"` (filename intact),
# all `&FreeSans9ptXb` references now point at `&FreeSans9pt8b`.
```

Side effect: the (commented-out) reference to the **original** stock font at
the top of the file (`//#include <Fonts/FreeSans9pt7b.h>`) is also renamed by
the sed. This is harmless — the line is commented out and the rename is
self-documenting (it makes it obvious the project no longer uses the stock
ASCII-only font). Hand-revert that one comment if you want to keep the
historical reference to the Adafruit stock filename.

**5c. Add an alias `#define`** to decouple the rest of the sketch from the
specific font symbol. This is what lets you swap font size (9 → 10 → 12) or
range (7b ↔ 8b) later without re-greping the whole sketch.

Right after the `#include` of the generated font, add:

```cpp
// Alias to decouple the sketch from the specific generated font. To switch
// font size or range (7b/8b), change ONLY the #include above + this #define.
#define CONVO_MSG_FONT FreeSans10pt8b
```

Then rename the 5 symbol references in the sketch (the ones used as
`&FreeSans...Xpt8b`) so they point at the alias instead of the raw symbol:

```bash
sed -i 's/&FreeSans[0-9]\+pt8b\b/\&CONVO_MSG_FONT/g' \
  ~/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/minimessenger.ino
```

The regex `FreeSans[0-9]\+pt8b` matches `FreeSans9pt8b`, `FreeSans10pt8b`,
`FreeSans12pt8b`, etc. — whichever size you've generated.

After this, switching to a different font becomes a 2-line change:
```cpp
#include "FreeSans12pt8b_latin1.h"   // re-generated at size 12
#define CONVO_MSG_FONT FreeSans12pt8b
```
No code further down the file needs to be touched.

Note: pure `#define` is preferred over a C++ reference or `const GFXfont*` for
this kind of alias because `&CONVO_MSG_FONT` keeps the natural take-address
syntax used by `setFont()` / `getTextBounds()` / etc., and the substitution
happens at preprocessing time — zero runtime overhead.

### Step 6 — Convert UTF-8 → Latin-1 before drawing

The strings coming from `Serial.read()`, MQTT payloads, and source-code
literals are all UTF-8. The new font expects single-byte Latin-1 codepoints.
Add this small converter near the other text helpers in `minimessenger.ino`:

```cpp
// Convert in-place from UTF-8 to Latin-1 (ISO-8859-1). Two-byte UTF-8
// sequences `0xC2 0xXX` (control / Latin-1 supplement) and `0xC3 0xXX`
// (most accented letters) collapse to a single Latin-1 byte. Codepoints
// outside U+0000..U+00FF are replaced with '?'. Returns the new length.
size_t utf8ToLatin1(char* s) {
  uint8_t* in = (uint8_t*)s;
  uint8_t* out = (uint8_t*)s;
  while (*in) {
    uint8_t b = *in++;
    if (b < 0x80) {
      *out++ = b;                                  // ASCII passthrough
    } else if ((b & 0xE0) == 0xC0 && *in) {
      uint8_t b2 = *in++;
      uint32_t cp = ((b & 0x1F) << 6) | (b2 & 0x3F);
      *out++ = (cp <= 0xFF) ? (uint8_t)cp : '?';
    } else if ((b & 0xF0) == 0xE0 && in[0] && in[1]) {
      in += 2;                                     // BMP > U+00FF
      *out++ = '?';
    } else if ((b & 0xF8) == 0xF0 && in[0] && in[1] && in[2]) {
      in += 3;                                     // outside BMP
      *out++ = '?';
    } else {
      *out++ = '?';                                // malformed
    }
  }
  *out = '\0';
  return out - (uint8_t*)s;
}
```

Call it on every string that ends up on the display. Two natural integration
points:

```cpp
// In addConversationBlock(String ts, String msg, ...)
//   Adapt the message right after the noteActivity() call:
char msgBuf[CONVO_MSG_MAX_LEN];
strncpy(msgBuf, msg.c_str(), sizeof(msgBuf) - 1);
msgBuf[sizeof(msgBuf) - 1] = '\0';
utf8ToLatin1(msgBuf);
// Then use msgBuf everywhere instead of msg.c_str() for the on-screen draw.
```

For incoming MQTT, the easiest place is to convert the payload buffer once in
`onMqttIncomingMessage()` before it reaches `addConversationBlock()`. For
serial input, the conversion can happen right before pushing to
`mqttPushFormattedMessage()` or simply skipped — depending on whether you want
the *broker* to see UTF-8 (preferred) or Latin-1 (don't, MQTT clients on phones
will be confused).

Rule of thumb:
- **Network payloads (MQTT in / out)**: keep UTF-8.
- **Display buffers (`TextLine.msg`)**: store Latin-1.
- Conversion happens at the boundary, just before the bytes hit the GFX
  rendering path.

### Step 7 — Rebuild, flash, test

In the Arduino IDE, recompile and upload. Type `café` on the serial monitor —
it should now render on the TFT exactly as on the host terminal.

---

## Option A — Transliteration (fallback if you can't run fontconvert)

Drop a small UTF-8 → ASCII fold table that maps every accented letter to its
unaccented equivalent. Affected characters become approximate but at least
visible: `Frère → Frere`, `Çà → Ca`. No font generation involved, no flash
overhead.

```cpp
// Caller-side replacement (sketch).
// Returns new length.
size_t utf8ToAsciiFold(char* s) {
  uint8_t* in = (uint8_t*)s;
  uint8_t* out = (uint8_t*)s;
  while (*in) {
    uint8_t b = *in++;
    if (b < 0x80) {
      *out++ = b;
    } else if ((b & 0xE0) == 0xC0 && *in) {
      uint8_t b2 = *in++;
      uint32_t cp = ((b & 0x1F) << 6) | (b2 & 0x3F);
      char fold = '?';
      switch (cp) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: fold = 'A'; break;
        case 0xC7: fold = 'C'; break;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: fold = 'E'; break;
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: fold = 'I'; break;
        case 0xD1: fold = 'N'; break;
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: fold = 'O'; break;
        case 0xD9: case 0xDA: case 0xDB: case 0xDC: fold = 'U'; break;
        case 0xDD: fold = 'Y'; break;
        case 0xDF: fold = 's'; break;            // ß
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: fold = 'a'; break;
        case 0xE7: fold = 'c'; break;
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: fold = 'e'; break;
        case 0xEC: case 0xED: case 0xEE: case 0xEF: fold = 'i'; break;
        case 0xF1: fold = 'n'; break;
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: fold = 'o'; break;
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: fold = 'u'; break;
        case 0xFD: case 0xFF: fold = 'y'; break;
        default: fold = '?'; break;
      }
      *out++ = (uint8_t)fold;
    } else if ((b & 0xF0) == 0xE0 && in[0] && in[1]) {
      in += 2; *out++ = '?';
    } else if ((b & 0xF8) == 0xF0 && in[0] && in[1] && in[2]) {
      in += 3; *out++ = '?';
    } else {
      *out++ = '?';
    }
  }
  *out = '\0';
  return out - (uint8_t*)s;
}
```

Use exactly like `utf8ToLatin1()` above. Stays compatible with the stock
`FreeSans9pt7b` font.

---

## Option C — Built-in 5×7 font + CP437 (proof of concept only)

```cpp
g_disp->setFont(NULL);    // disable GFX font, use built-in 5x7
g_disp->cp437(true);      // enable CP437 mapping for bytes >= 0x80
g_disp->print("\x82tait");  // 0x82 = é in CP437
```

You still need to convert UTF-8 → CP437 (the codepoints differ from Latin-1 —
see the table at the top of this document). Text becomes tiny and pixelated.
Not recommended for the messenger UI.

---

## Cleanup checklist after switching to Option B

- [ ] `FreeSans9pt7b_latin1.h` committed alongside the sketch, license note
      added (GNU FreeFont is GPLv3 with font exception — usable in firmware).
- [ ] Original `#include <Fonts/FreeSans9pt7b.h>` removed.
- [ ] `utf8ToLatin1()` called on every path that lands in `addConversationBlock`.
- [ ] MQTT outgoing payloads still in UTF-8 (verify by subscribing from a
      desktop client — `mosquitto_sub` with `-v` shows raw bytes).
- [ ] Smoke test: send `"àéèçôîÿ"` over MQTT and Serial; both render on the TFT.

## Limitations of Latin-1

The font generation in step 4 covers `[0x20, 0xFF]` = Latin-1. A few French
characters are **outside** that range and will render as `?`:

| Character | Unicode | Workaround |
|-----------|---------|------------|
| `œ`, `Œ`  | U+0153, U+0152 | Fold to `oe`, `OE` in `utf8ToLatin1()` |
| `€`       | U+20AC         | Fold to `EUR`, or skip if unused |
| `…`       | U+2026         | Fold to `...` |
| `«`, `»`  | U+00AB, U+00BB | Already in Latin-1 (0xAB, 0xBB), render fine |

If you need `œ` to render properly, the cleanest path is to add a special case
in `utf8ToLatin1()` that maps U+0152 / U+0153 to the closest Latin-1 glyph
slot (e.g., reuse a rarely-used Latin-1 codepoint, or fold to `oe`).
Generating a wider font range with `fontconvert` is not recommended — the GFX
glyph table is contiguous from `first` to `last`, so extending to U+0153 would
include ~340 glyphs, most of them empty padding.
