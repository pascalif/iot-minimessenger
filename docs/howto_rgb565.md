# How-to — RGB565 colors

Quick reference for converting between the standard 24-bit RGB notation (`0xRRGGBB`, what every CSS / Photoshop / Figma picker outputs) and the 16-bit RGB565 format used by the ST7789 panel — and by every `uint16_t color` argument in `bars.ino`, `display.h`, `addConversationBlock`, `printInfoLine`, etc.

## Why RGB565 and not RGB888

The ST7789 controller takes 16-bit pixels over SPI. Sending 24 bits per pixel would be 50% more SPI traffic for ~zero perceptual gain — the eye can barely distinguish ~64 levels of gray at this screen size and viewing distance, and 5/6/5 quantization is far below that threshold. The 6 bits for green (vs 5 for red and blue) reflect that the human retina has roughly twice as many green-sensitive cones as red- or blue-sensitive ones.

## Bit layout

A 16-bit RGB565 value packs three channels into a single `uint16_t`:

```
bit 15 14 13 12 11 | 10  9  8  7  6  5 | 4  3  2  1  0
    R  R  R  R  R  |  G  G  G  G  G  G | B  B  B  B  B
    └── 5 bits ──┘   └─── 6 bits ───┘   └── 5 bits ──┘
```

Maximum per channel: R = 31, G = 63, B = 31.

## Conversion `0xRRGGBB` → RGB565

Drop the low bits of each channel and shift into place:

```cpp
R5 = R8 >> 3     // 8 bits → 5 bits
G6 = G8 >> 2     // 8 bits → 6 bits
B5 = B8 >> 3     // 8 bits → 5 bits
rgb565 = (R5 << 11) | (G6 << 5) | B5
```

As a single C macro:

```cpp
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
```

Usage: `RGB565(0xDD, 0xDD, 0xDD)` → `0xDEFB`.

## Worked example — `0xDEFB`

The hairline separator color used by `STATUS_BAR_SEPARATOR_COLOR` in `bars.ino`.

**Decomposition** of `0xDEFB`:

```
   D    E    F    B
 1101 1110 1111 1011
 └─┬─┘└──┬──┘└─┬─┘
   R     G     B
```

| Channel | Bits     | Decimal |
| ------- | -------- | ------- |
| R       | `11011`  | 27 / 31 |
| G       | `110111` | 55 / 63 |
| B       | `11011`  | 27 / 31 |

**Back to 8-bit per channel** (multiplying by ~8 for R/B, ~4 for G):

- R8 ≈ 27 × 8 = 216 = `0xD8`
- G8 ≈ 55 × 4 = 220 = `0xDC`
- B8 ≈ 27 × 8 = 216 = `0xD8`

So `0xDEFB` ≈ `#D8DCD8` — a light gray with a faint green tint.

**Verifying the comment in `bars.ino`** (which claims `#DDDDDD → 0xDEFB`):

- 0xDD = 221
- R5 = 221 >> 3 = 27
- G6 = 221 >> 2 = 55
- B5 = 221 >> 3 = 27
- packed = `(27 << 11) | (55 << 5) | 27` = `0xD800 | 0x06E0 | 0x001B` = **`0xDEFB`** ✓

The conversion is lossy: `#DDDDDD` and `#D8DCD8` both quantize to the same `0xDEFB`. The quantum is ~8 RGB values per channel for R/B, ~4 for G — invisible on the panel for non-gradient content.

## Reverse — RGB565 → `0xRRGGBB`

Two ways, depending on accuracy needs:

**Quick** (lose the bottom 3/2/3 bits — fine for hex sanity-checking):

```cpp
R8 = (rgb565 >> 11) << 3        // top 5 bits → shifted to top of byte
G8 = ((rgb565 >> 5) & 0x3F) << 2
B8 = (rgb565 & 0x1F) << 3
```

**Better** (replicate the high bits into the low bits so white stays white — matches what most hardware actually displays):

```cpp
R5 = (rgb565 >> 11) & 0x1F
G6 = (rgb565 >> 5)  & 0x3F
B5 = rgb565         & 0x1F
R8 = (R5 << 3) | (R5 >> 2)
G8 = (G6 << 2) | (G6 >> 4)
B8 = (B5 << 3) | (B5 >> 2)
```

Without replication, RGB565 white (`0xFFFF`) decodes to `#F8FCF8` — slightly green-tinted gray, not pure white.

## Memorable pure-channel values

Useful to recognize at a glance:

| Color   | RGB565   | Why                              |
| ------- | -------- | -------------------------------- |
| WHITE   | `0xFFFF` | all bits set                     |
| BLACK   | `0x0000` | all bits clear                   |
| RED     | `0xF800` | R = 31, G = B = 0                |
| GREEN   | `0x07E0` | G = 63, R = B = 0                |
| BLUE    | `0x001F` | B = 31, R = G = 0                |
| YELLOW  | `0xFFE0` | R + G (no B)                     |
| CYAN    | `0x07FF` | G + B (no R)                     |
| MAGENTA | `0xF81F` | R + B (no G)                     |
| ORANGE  | `0xFC00` | R = 31, G = 32 (half), B = 0     |

The `ST77XX_*` macros in `Adafruit_ST77xx.h` are exactly these values.

## Calculators

- Web: <https://rgbcolorpicker.com/565> — interactive, supports both directions.
- Python one-liner:
  ```bash
  python3 -c "r,g,b=0xDD,0xDD,0xDD; print(hex((r>>3<<11)|(g>>2<<5)|(b>>3)))"
  # → 0xdefb
  ```
- Bash:
  ```bash
  printf '0x%04X\n' $(( ((0xDD & 0xF8) << 8) | ((0xDD & 0xFC) << 3) | (0xDD >> 3) ))
  # → 0xDEFB
  ```

## In this project

Centralized in `bars.ino` (per-bar palette) and a handful of `CONVO_*_COLOR` macros in `minimessenger.ino`. All call sites pass `uint16_t` color arguments — there is no RGB888 layer. To retune a palette entry, compute the new RGB565 once (using one of the calculators above) and drop it into the `#define`.
