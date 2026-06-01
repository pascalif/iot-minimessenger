// ================================================================================
// strings.ino — Generic char* / String utilities
// ================================================================================
//
// This file owns string-level helpers that don't depend on display, MQTT, BLE or WiFi state. Concatenated by the Arduino IDE into the main TU
// after minimessenger.ino (alphabetical order, after commands.ino, before wifi.ino), so it has access to everything declared in the sketch
// without forward decls — and conversely, its functions are reachable from earlier .ino files only through the explicit forward declarations
// in the sketch's forward-decl block.

#include <Arduino.h>


// Convert in-place from UTF-8 to Latin-1 (ISO-8859-1). Two-byte UTF-8 sequences `0xC2 0xXX` (control / Latin-1 supplement) and `0xC3 0xXX`
// (most accented letters) collapse to a single Latin-1 byte. Codepoints outside U+0000..U+00FF are replaced with '?'. Returns the new length.
size_t utf8ToLatin1(char* s) {
    uint8_t* in  = (uint8_t*)s;
    uint8_t* out = (uint8_t*)s;
    while (*in) {
        uint8_t b = *in++;
        if (b < 0x80) {
            *out++ = b;  // ASCII passthrough
        } else if ((b & 0xE0) == 0xC0 && *in) {
            uint8_t  b2 = *in++;
            uint32_t cp = ((b & 0x1F) << 6) | (b2 & 0x3F);
            *out++      = (cp <= 0xFF) ? (uint8_t)cp : '?';
        } else if ((b & 0xF0) == 0xE0 && in[0] && in[1]) {
            in += 2;  // BMP > U+00FF
            *out++ = '?';
        } else if ((b & 0xF8) == 0xF0 && in[0] && in[1] && in[2]) {
            in += 3;  // outside BMP
            *out++ = '?';
        } else {
            *out++ = '?';  // malformed
        }
    }
    *out = '\0';
    return out - (uint8_t*)s;
}
