---
name: feedback-display-charset
description: "For minimessenger and similar Arduino projects displaying text via Adafruit_GFX, restrict on-screen string literals to the font's actual glyph range (here Latin-1 ≤ 0xFF). Em dash, ellipsis, smart quotes, etc. fall back to '?' or vanish silently. Comments / serial logs are unaffected — only strings reaching printInfoLine() / addConversationBlock() / display-side print() calls."
metadata:
  node_type: memory
  type: feedback
  originSessionId: b780dbf5-b8b1-4b64-acf6-10743f84fba1
---

Rule: **never put non-Latin-1 characters in strings that reach the TFT** (printInfoLine, addConversationBlock, drawInfoRow, any `pDisp->print(...)` of a literal).

**Why:** the bundled GFX fonts in the minimessenger sketch are `FreeSans*pt8b_latin1` — generated with `fontconvert ... 32 255`, so the glyph range tops out at codepoint 0xFF. The `utf8ToLatin1()` helper that runs before every display draw collapses 2-byte UTF-8 sequences (`0xC2..` / `0xC3..`) to single Latin-1 bytes, but emits `?` for any codepoint above 0xFF — which is exactly what em dash (U+2014), ellipsis (U+2026), curly quotes (U+2018/2019/201C/201D), arrows, etc. all are.

**Concrete characters to avoid in display strings:**
- `—` (em dash, U+2014)  → use `-` or ` - `
- `–` (en dash, U+2013)  → use `-`
- `…` (ellipsis, U+2026) → use `...`
- `’` `‘` (curly single quotes) → use `'`
- `“` `”` (curly double quotes) → use `"`
- `→` `←` `↑` `↓` (arrows) → use `->` `<-` etc.
- `•` (bullet, U+2022) → use `*` or `-`
- `©` (U+00A9), `®` (U+00AE), `°` (U+00B0) — these ARE in Latin-1, OK to use.

**Caveat:** `œ` `Œ` (U+0152/0153) are NOT in Latin-1 (they're in ISO-8859-15) so they also fail. Already documented in `docs/howto_fonts.md` "Limitations of Latin-1" section.

**How to apply:** when writing a new printInfoLine / addConversationBlock literal or editing an existing one, check it visually for any "fancy" punctuation. Plain ASCII is always safe; Latin-1 accented letters (é, è, ç, à, ô, …) are safe. Anything else needs replacement.

**Not applicable to:**
- Comments in source code (compiler doesn't see them, Serial monitor renders UTF-8 fine)
- `ESP_LOGI/W/E/...` arguments (serial output, UTF-8 capable)
- Markdown docs in `docs/`
- `README.md`, `CLAUDE.md`, etc.

These can keep using em dash for readability — the constraint is only on what reaches the embedded font.

**Related:** [[feedback-comment-wrapping]], `docs/howto_fonts.md`.
