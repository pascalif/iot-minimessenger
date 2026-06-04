# minimessenger

A self-contained ESP32 (or ESP8266 D1 mini) "messenger" appliance: BLE keyboard
for input, ST7789 240×320 TFT for display, MQTT-over-TLS to HiveMQ Cloud as the
transport between paired devices. Think of it as a hardware mini-WhatsApp for a
small group of people on the same Wi-Fi network — broadcast to everyone, or
unicast to a specific peer.

For the full project structure, build/flash steps, and per-module conventions,
see [`CLAUDE.md`](./CLAUDE.md).

## Power

The device is **USB-powered**. There is no battery on board, no Li-ion / Li-Po
cell, no voltage divider on a VBAT GPIO, and no sleep-mode strategy in the
firmware loop. Battery-related concerns (low-battery shutdown, light/deep sleep
between MQTT keepalives, regulator dropout, BOD wiring) are therefore
deliberately out of scope and not addressed in the firmware. If a future
hardware revision adds a battery, those topics need to be revisited.

## Code audit

A tracked audit report lives at [`docs/audit_claude.md`](docs/audit_claude.md).
Issues have stable IDs (`SEC-001`, `HW-002`, …) so the report survives across
runs; the `Won't fix` section captures known intentional trade-offs.
