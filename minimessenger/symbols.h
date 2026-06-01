#ifndef SYMBOLS_H
#define SYMBOLS_H


// Lines management in display
enum Align { LEFT, CENTER, RIGHT };

enum DisplayType {
    OLEDSHIELD,  // lib Adafruit_SSD1306
    ST7789,      // 320x240, lib Adafruit_ST7789
};

// Burn-in protection: drives the local activity / sleep state machine.
enum DisplayPowerState {
    DISPLAY_ON,
    DISPLAY_DIMMED,  // 50% backlight (if TFT_BL is a real PWM pin); otherwise no visible change but timer still progresses to OFF
    DISPLAY_OFF,     // panel sleeps via Adafruit_ST7789::enableDisplay(false)
};

// Origin of a payload landing in processMessage(): drives which side of the
// conversation it renders on and whether it gets republished to peers.
enum class MessageSource {
    REMOTE,  // arrived via MQTT — render LEFT, do NOT republish
    LOCAL,   // typed locally (serial today, BLE keyboard later) — render RIGHT, publish
};


#endif