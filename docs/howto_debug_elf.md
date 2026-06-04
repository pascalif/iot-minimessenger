# Howto — Decoding ESP32 crash backtraces with `addr2line`

**Date:** 2026-06-01
**Context:** When the ESP32 panics, the serial console dumps a register dump
followed by a backtrace — a list of hexadecimal addresses with no symbols.
This doc explains how to translate those addresses back into
`function() @ file.cpp:line` using the toolchain that ships with
arduino-esp32, with the actual paths on this workstation.

## What a backtrace looks like

Typical output after a `Guru Meditation Error`:

```
Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled.

Core  1 register dump:
PC      : 0x40103c3d  PS      : 0x00060830  A0      : 0x801043f8  A1      : 0x3ffd2db0
A2      : 0x3ffc4334  ... (more registers)
EXCCAUSE: 0x0000001c
EXCVADDR: 0x00000000

Backtrace: 0x40103c3a:0x3ffd2db0 0x401043f5:0x3ffd2df0 0x400d6907:0x3ffd2e10 \
           0x400d6a35:0x3ffd2e30 0x400d805e:0x3ffd2e60 0x40115080:0x3ffd2ec0 \
           0x40095af5:0x3ffd2ee0
```

## Reading the registers

A few fields are interpretable without any tooling:

| Field | Meaning |
|-------|---------|
| `EXCCAUSE` | Why the panic happened (see table below) |
| `EXCVADDR` | The memory address being accessed when the fault occurred — `0x0` means NULL deref |
| `PC` | Program Counter — the instruction that crashed |
| `A1` | Stack pointer at the moment of the crash |

Common `EXCCAUSE` values:

| Hex | Symbolic | Meaning |
|-----|----------|---------|
| `0x0` | IllegalInstruction | CPU tried to execute a non-instruction (jump into data, corrupt return addr) |
| `0x1c` | LoadProhibited | Load from an invalid memory region (often NULL deref, freed pointer) |
| `0x1d` | StoreProhibited | Write to an invalid memory region |
| `0x6` | IntegerDivideByZero | Self-explanatory |
| `0x9` | LoadStoreAlignment | Unaligned 4-byte access (rare on Arduino code) |

So `EXCCAUSE=0x1c + EXCVADDR=0x0` is a textbook NULL-pointer load.

## The backtrace format

Each `PC:SP` pair represents one stack frame:
- **PC** (Program Counter) = the address in flash where execution was inside that function.
- **SP** (Stack Pointer) = where the stack was for that frame.

The leftmost pair is the **most recent** call (the function that actually
crashed). Each subsequent pair is one level further up the call chain —
the caller, the caller's caller, etc. — usually ending at `loopTask`.

The addresses fall in two ranges:
- `0x4008_0000 – 0x4040_0000` → IROM (code in external SPI flash, mapped read-only).
- `0x4000_0000 – 0x4007_FFFF` → IRAM (instructions cached in internal RAM — ISRs, hot path).

Both ranges are translatable by `addr2line` as long as you have the ELF
file from the exact build that was running.

## Translating addresses to source — `addr2line`

The tool ships with the ESP32 GCC toolchain. On this workstation:

```
/home/pascal/.arduino15/packages/esp32/tools/esp-x32/2601/bin/xtensa-esp32-elf-addr2line
```

Version (as of 2026-06-01): `GNU addr2line (crosstool-NG esp-14.2.0_20260121) 2.43.1`.

### Useful flags

| Flag | Effect |
|------|--------|
| `-e <ELF>` | Path to the ELF file with debug symbols (mandatory). |
| `-p` | "Pretty" output: one line per address. |
| `-f` | Print the function name as well as file:line. |
| `-i` | Recurse into inlined functions (otherwise frames inlined by the compiler are silently merged into their parent). |
| `-a` | Echo the input address as a prefix on each line. |
| `-C` | Demangle C++ symbols (`_Z14wifiStopPortalv` → `wifiStopPortal()`). |

The combination you almost always want: `-pfiaC`.

## Where the ELF lives

Arduino IDE writes a build into a per-sketch cache directory whose name is
a hash of the absolute sketch path. **Your local ELF for the
minimessenger project is here:**

```
/home/pascal/.cache/arduino/sketches/2BF659361DD81A4488A6FD59BF8AB8C3/minimessenger.ino.elf
```

The hash `2BF659361DD81A4488A6FD59BF8AB8C3` is specific to this workstation
and this sketch path. If the sketch is ever moved to a different directory
the hash changes and a new cache folder appears.

To locate it from scratch:

```bash
find /tmp /home/pascal -name "minimessenger.ino.elf" 2>/dev/null
```

`/tmp` is checked because older Arduino IDE versions (and some CLI
build flows) put intermediate artifacts there.

**⚠️ Critical**: the ELF must correspond byte-for-byte to the firmware
that was running when the crash happened. If you recompile between the
crash and the `addr2line` run, the cached ELF gets overwritten and
addresses no longer line up with the right source lines. Recovery workflow:

1. Crash happens on the device.
2. **Immediately** copy the current ELF somewhere safe:
   ```bash
   cp /home/pascal/.cache/arduino/sketches/2BF659361DD81A4488A6FD59BF8AB8C3/minimessenger.ino.elf \
      ~/crash-elf-$(date +%Y%m%d_%H%M%S).elf
   ```
3. Then go ahead and edit/recompile.
4. Run `addr2line` against the saved copy.

## Full example

Putting it all together for the backtrace at the top of this doc:

```bash
/home/pascal/.arduino15/packages/esp32/tools/esp-x32/2601/bin/xtensa-esp32-elf-addr2line \
    -pfiaC \
    -e /home/pascal/.cache/arduino/sketches/2BF659361DD81A4488A6FD59BF8AB8C3/minimessenger.ino.elf \
    0x40103c3a 0x401043f5 0x400d6907 0x400d6a35 0x400d805e 0x40115080 0x40095af5
```

Expected output (illustrative — actual symbols depend on your build):

```
0x40103c3a: WebServer::~WebServer() at libraries/WebServer/src/WebServer.cpp:120
0x401043f5: WiFiManager::shutdownConfigPortal() at libraries/WiFiManager/WiFiManager.cpp:2845
0x400d6907: WiFiManager::stopConfigPortal() at libraries/WiFiManager/WiFiManager.cpp:2802
0x400d6a35: wifiStopPortal() at /home/pascal/Dev/workspace_pascal/iot-minimessenger/minimessenger/wifi.ino:285
0x400d805e: wifiTick(unsigned long) at /home/pascal/Dev/workspace_pascal/iot-minimessenger/minimessenger/wifi.ino:172
0x40115080: loop() at /home/pascal/Dev/workspace_pascal/iot-minimessenger/minimessenger/minimessenger.ino:2200
0x40095af5: loopTask(void*) at cores/esp32/main.cpp:73
```

Read it top-down to get the call stack from crash site up to the task
entry point.

## Useful shell alias

Add to `~/.bashrc` for quick decoding without re-typing the paths:

```bash
alias esp32-decode='/home/pascal/.arduino15/packages/esp32/tools/esp-x32/2601/bin/xtensa-esp32-elf-addr2line -pfiaC -e /home/pascal/.cache/arduino/sketches/2BF659361DD81A4488A6FD59BF8AB8C3/minimessenger.ino.elf'
```

Then:

```bash
esp32-decode 0x40103c3a 0x401043f5 0x400d6907
```

## Alternative — the IDE plugin

The library **EspExceptionDecoder** (installable from Arduino IDE → Library
Manager) provides a GUI window where you paste the raw backtrace and it
runs `addr2line` automatically against the most recent build's ELF. More
convenient for one-off decodings; the CLI is preferable for scripted /
repeatable analysis (e.g. when you keep crash dumps in a notes file).

## When addresses fail to resolve

`addr2line` sometimes returns `??:0` for an address. Common causes:

1. **Wrong ELF** — sketch was recompiled since the crash. Re-flash and
   reproduce, OR find an older ELF you saved aside.
2. **IRAM ISR address** — the function is in IRAM (e.g. `IRAM_ATTR` ISR
   handlers). Should still resolve with the correct ELF; if not, the
   function might be in a library compiled without `-g` (rare with
   arduino-esp32).
3. **Address is in the bootloader** — `0x4007_xxxx` range. Bootloader
   has its own ELF in `~/.arduino15/packages/esp32/hardware/esp32/<ver>/tools/sdk/esp32/bin/`
   but you almost never need to decode bootloader-level crashes from app code.
4. **JIT / dynamic code** — not applicable on ESP32 (everything is
   ahead-of-time compiled), but worth knowing as a general possibility.

If only some of the addresses resolve, fix the ELF; if NONE resolve, the
build cache was wiped — recompile to regenerate it and reproduce the
crash if you need the symbols.

## See also

- `docs/howto_efuse.md` — for `/dbg-chip` and other runtime introspection.
- ESP-IDF docs on panic handling:
  <https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/fatal-errors.html>
- Xtensa ISA EXCCAUSE reference (table 7-44 in the manual):
  <https://0x04.net/~mwk/doc/xtensa.pdf>
