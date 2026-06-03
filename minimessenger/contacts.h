#pragma once

#include <Arduino.h>
#include "symbols.h"  // DisplayType


// === Per-device identity record ===
//
// Owns the C++ side of the COMPILED_DEVICE_DATA_ENTRIES table that lives in personal-data.h (data only, gitignored). The struct definition is in
// this header rather than directly inside contacts.ino because minimessenger.ino — concatenated first by the Arduino IDE — needs the type in scope
// to declare `DeviceDataEntry g_deviceData` and to read `g_deviceData.deviceId / pseudo / screen / namePrefix` from many sites. The find* methods
// are declared here and defined out-of-class inline in contacts.ino so the body changes stay close to the rest of the contacts logic.
//
// Linkage / build flow:
//   - extern declarations of the table and its count below give external linkage to the names; personal-data.h provides the actual definitions
//     (with `const`), and since `extern` precedes the `const` definition in the single Arduino-concatenated TU, the linker treats them as one
//     externally-linked entity (C++ [basic.link]: a const variable previously declared extern keeps external linkage).
//   - personal-data.h itself is included from wifi.ino (last in concatenation), at which point this header has been pulled in via minimessenger.ino's
//     include — so `DeviceDataEntry` is a complete type when the table is materialised.
struct DeviceDataEntry {
    const char* mac;
    byte        deviceId;
    const char* pseudo;
    const char* namePrefix;
    DisplayType screen;

    // Formatted "namePrefix_NNN" string (e.g. "E32_004"). Built on demand into a function-local static buffer that is SHARED across every
    // DeviceDataEntry instance — calling entry1.name() then entry2.name() invalidates the first pointer. Today the only consumer is the local
    // device's g_deviceData.name() (boot log, MQTT client_id, WiFi hostname, info-screen "ID:" row); peers read entry->pseudo directly, never
    // name(), so the shared-buffer caveat is harmless.
    const char* name() const {
        static char buf[12];  // <namePrefix=3..5>_<id=3> + '\0' — large enough for "PROTO_999\0"
        snprintf(buf, sizeof(buf), "%s_%03d", namePrefix, deviceId);
        return buf;
    }

    // Lookup helpers — O(N) over COMPILED_DEVICE_DATA_ENTRIES_COUNT, which is bounded to a handful of rows. Both return nullptr if no match.
    // Bodies live in contacts.ino so the iteration logic sits next to the rest of the contact-table code.
    static const DeviceDataEntry* findByMac(const char* mac);
    static const DeviceDataEntry* findById(byte deviceId);
};


// Materialised by personal-data.h, included from wifi.ino.
extern const DeviceDataEntry COMPILED_DEVICE_DATA_ENTRIES[];
extern const size_t          COMPILED_DEVICE_DATA_ENTRIES_COUNT;
