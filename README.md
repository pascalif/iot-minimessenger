# minimessenger

![MiniMessenger demo picture](https://lobrico.wordpress.com/wp-content/uploads/2026/06/mm06-ecran-splash-edited.jpg?w=300 "MiniMessenger case")

A self-contained ESP32 (board **ESP32 Dev Module**) "messenger" appliance using:
* Bluetooth Low Energy (BLE) keyboard for input
* ST7789 240×320 TFT for display
* MQTT-over-TLS to HiveMQ Cloud as the transport between devices.

Think of it as a **hardware version of a mini-WhatsApp** for a
small group of people — broadcast to everyone, or
unicast to a specific peer.

![Convo](https://lobrico.wordpress.com/wp-content/uploads/2026/06/mm09-ecran-conv.jpg?w=300)

More on the project : https://lobrico.wordpress.com/2026/06/29/mini-messenger/

## Documentation

* For a detailed **step-by-step setup** (configuration, compilation, flashing guide), see [`INSTALL.md`](./INSTALL.md).
* Parse `docs/` folder for various
  * **how-to** guides,
  * **ideas** for later features
  * misc **information** on why some features are implemented as is
* A tracked **audit report** lives at [`docs/audit_claude.md`](docs/audit_claude.md).
Issues have stable IDs (`SEC-001`, `HW-002`, …) so the report survives across
runs; the `Won't fix` section captures known intentional trade-offs.
* For an LLM agent, the full project structure, codebase details, and per-module conventions are in [`CLAUDE.md`](./CLAUDE.md).

## Power source

The device is **USB-powered**. There is no battery on board, no Li-ion / Li-Po
cell, no voltage divider on a VBAT GPIO, and no sleep-mode strategy in the
firmware loop.

Battery-related concerns (low-battery shutdown, light/deep sleep
between MQTT keepalives, regulator dropout, BOD wiring) are therefore
deliberately out of scope and not addressed in the firmware. If a future
hardware revision adds a battery, those topics need to be revisited.

## 3D printing

An exemple of a 3D printing case made of several elements to assemble (no glue nor screws needed) can be found under `3d/` folder.

![3D plans](https://lobrico.wordpress.com/wp-content/uploads/2026/06/mm03-fusion-bodies.png?w=500)