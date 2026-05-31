# Howto — Split a growing `.ino` into multiple files

**Date:** 2026-05-31
**Context:** `minimessenger.ino` is past 2000 lines and mixes display logic,
MQTT, WiFi, BLE keyboard, commands, screensaver, etc. This doc explains how
the Arduino IDE handles multi-file sketches and recommends a split for
this project.

## TL;DR

You don't need `.h` / `.cpp` / `extern` to split an Arduino sketch. Just add
more **`.ino` files in the same folder as the main sketch**. The Arduino IDE
concatenates them all into a single compilation unit at build time and
auto-generates function prototypes. Globals are shared without ceremony.

## How the Arduino IDE actually builds a sketch

1. The IDE scans the sketch folder for `.ino` files.
2. It concatenates them all into one virtual source file, in this order:
   - the main sketch (the one matching the folder name, e.g.
     `minimessenger.ino` in folder `minimessenger/`)
   - all other `.ino` files **alphabetically**. (No need for a numeric prefix
     convention like `10-foo.ino` / `20-bar.ino` to enforce an order: the
     auto-prototype pass at step 4 below resolves all forward references
     within the concatenated source, so the relative order of the secondary
     `.ino` files does not affect compilation or behavior.)
3. It prepends `#include <Arduino.h>` to the result.
4. It scans the concatenated source for function definitions and auto-emits
   forward prototypes at the top (this is the Arduino "magic" that lets you
   use a function before its definition without declaring it).
5. It hands the resulting `.cpp` to the compiler.

In parallel, the IDE also compiles every `.cpp` and `.c` file in the sketch
folder as **separate translation units**. Those don't get the auto-prototype
treatment and need their own `#include` directives.

Headers (`.h`, `.hpp`) are not compiled directly; they're pulled in via
`#include` from wherever they're referenced.

Files inside subfolders (e.g. `docs/`, `data/`) are ignored by the build.

## Recommended approach: more `.ino` files

Drop additional `.ino` files in the sketch folder. The IDE will pick them up
automatically. Each file:

- needs **no `#include`** to reference other files in the same sketch — they
  are concatenated, so it's all one translation unit.
- can define functions, globals, constants, and use functions defined in
  other `.ino` files. The auto-prototyper handles forward references.
- shows up as its own tab in the IDE, which is the actual ergonomic win.

### Caveats

1. **No subfolders.** A `.ino` inside a subfolder is ignored by the build.
2. **Auto-prototypes occasionally trip.** They cover 99 % of normal
   functions, but variadic templates, default arguments with complex types,
   and functions defined inside macros sometimes need a manual prototype at
   the top of the main `.ino`.
3. **`static` does not isolate.** In standard C/C++, `static` at file scope
   means "file-local". After Arduino's concat, every `.ino` is part of the
   same file, so a `static` function in `display.ino` is reachable from
   `mqtt.ino`. Not a bug, just a sharper-than-expected scope.
4. **Globals are shared, double-definitions error.** A `int g_foo = 0;` in
   one `.ino` is visible from all others — no `extern` needed. But defining
   the same global twice (in two `.ino` files) breaks linking with a
   "multiple definition" error.
5. **Type definitions go in `.h` files when shared.** Functions
   auto-prototype, but `typedef`, `enum class`, `struct`, `class`
   definitions don't. Put those in `symbols.h` (already in use here) and
   `#include` from any `.ino` that uses them.
6. **Build order matters for shared `.cpp` / `.h`.** The `mm_blekb.cpp`,
   `mm_log.h`, etc. are independent translation units, compiled separately.
   They keep needing real `#include` and `extern` declarations. Only the
   `.ino` files benefit from the concat magic.

## Concrete split proposal for minimessenger

Roughly grouped by subject. Each row is a candidate `.ino` file alongside
`minimessenger.ino`:

| File | Contents |
|------|----------|
| `minimessenger.ino` | `setup()`, `loop()`, includes, top-level constants, global state declarations. Stays small. |
| `commands.ino` | `routeMessage()`, `processPayloadAsCommand()`, `CMD_*` constants, the funnel doc-comment. |
| `display.ino` | `setupDisplay()`, `cleanScreen()`, `hwScrollSetupArea()`, `hwScrollTo()`, `hwScrollReset()`, `addConversationBlock()`, `redrawAllConversations()`, `redrawStatusBar()`, `redrawInputFooter()`, `drawIndicatorAt()`, `printValueWrapped()`, `drawInfoRow()`, `showUpdatedInfoScreen()`, `showSplashScreen()`. |
| `mqtt.ino` | `mqttReconnect()`, `mqttPushFormattedMessage()`, `mqttSendAlive()`, `onMqttIncomingMessage()`, `onMQTTReconnected()`, `onReceivedContactLiveness()`, `onOutgoingMessage()`, `onIncomingTextMessage()`, `setRecipient()`. |
| `keyboard.ino` | `decodeHIDReport()`, `keymapLower` / `keymapUpper` arrays, `onBluetoothKeyboardConnectionCallback()`, `onBluetoothKeyboardNotifyCallback()`, `setupKeyboard()`. |
| `wifi_time.ino` | `setupWifi()`, `setupNTP()`, `getCurrentDateTime()`, `getCurrentTime()`, `getTimezoneLabel()`. |
| `identity.ino` | `identifyDevice()` (and its 8 MAC-matching branches). |
| `screensaver.ino` | `setDisplayPowerState()`, `noteUserActivity()`, `updateDisplayPowerState()`. |
| `leds.ino` | `setupLeds()`, `ledSetState()`, `ledCommuteBlinkState()`. |
| `utf8.ino` | `utf8ToLatin1()` (currently sitting inside the main `.ino`). |
| `serial_input.ino` | Serial input reading loop + buffer reset (optional — small enough to leave in `minimessenger.ino`). |

After the split, `minimessenger.ino` should be down to maybe 200–300 lines:
includes, all global state declarations, `setup()`, `loop()`.

### Where do globals go?

Two valid options:

- **Keep all globals in `minimessenger.ino`** — single place to find any
  variable. Pro: easy. Con: long preamble.
- **Move globals next to the code that owns them** — e.g. `g_displayType`,
  `g_disp`, `g_scrollY`, `g_drawY` live in `display.ino`. Pro: locality.
  Con: a search across files is needed to find a global.

For this project, option 1 is probably cleaner because we have around 40
globals and many cross-module dependencies. Pick whichever you prefer, just
be consistent.

### Why not just use `.cpp` / `.h` pairs?

You can, and you'd do it if you wanted real translation-unit isolation. But
that means:
- Writing forward declarations in `.h` files
- Writing `extern` declarations for shared globals
- Maintaining include lists

For an Arduino sketch with one binary and no test harness, the auto-concat
flow gives you almost all the navigation benefit of separate files without
any of the discipline cost.

`mm_blekb.{h,cpp}` and `mm_log.h` are the exception: they're already
isolated because they're stable, self-contained components with a small
public API. That's the right call. Other features that are still moving
fast (display logic, MQTT message routing, …) don't need the rigor.

## The hack approach: `#include "foo.cpp"`

Technically possible, but the IDE also compiles `foo.cpp` separately, so
you get "multiple definition" errors at link time. The workaround is to
rename the included file to a non-`.cpp` extension (e.g. `.inc`, `.impl`)
that the IDE won't auto-compile:

```cpp
// In minimessenger.ino:
#include "messages.inc"
```

Drawbacks:
- IDE loses C++ syntax highlighting on `.inc` files
- Third-party tools and reviewers won't recognize the convention
- Future-you will be confused

Use only as a stopgap, e.g. to conditionally include implementation under
a `#if` flag.

## Verification after splitting

1. **Compile.** The IDE should produce the same binary size (± a few
   bytes) as before the split.
2. **Spot-check that no function got dropped or duplicated:**
   ```bash
   grep -rE '^(void|bool|int|String|char\*|static)\s+\w+\s*\(' \
     ~/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/*.ino \
     | wc -l
   ```
   Run before and after — the count should be identical.
3. **`grep` for any stale forward reference:**
   ```bash
   grep -n 'TODO\|FIXME\|XXX' ~/Dev/workspace_pascal/arduino/pascal_projects/minimessenger/*.ino
   ```
4. **Flash and smoke-test** the boot flow, BLE keystroke, MQTT publish.
   Behavior must be byte-identical because the source content is
   byte-identical after concat — only the file layout changes.

## Order in which to do the split

Do it in passes, not all at once:

1. Start with the **most isolated** subject (e.g. `screensaver.ino`,
   `leds.ino`, `utf8.ino`). Move those out first; compile; smoke-test.
2. Then **single-domain subjects** with few cross-deps (`identity.ino`,
   `wifi_time.ino`).
3. Then **larger subjects** (`display.ino`, `mqtt.ino`, `keyboard.ino`).
4. Last, the **funnel** (`commands.ino`) once everything else is settled.

Doing it in passes lets you catch any auto-prototype edge case early on a
small module rather than diagnosing it in a 500-line move.

## References

- Arduino IDE build system: <https://docs.arduino.cc/learn/programming/build-process/>
- arduino-cli docs on multi-file sketches: <https://arduino.github.io/arduino-cli/latest/sketch-build-process/>
- The auto-prototype generator lives in the `arduino-builder` source: see
  the `ctags`-based prototype extractor.
