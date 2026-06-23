#ifndef DISPLAY_H
#define DISPLAY_H


#define centerIn(w, total) ((total - w) / 2)


// Pour le calcul des bounds d'un texte construit avec une font donnée
#define BOX_X 0
#define BOX_Y 1
#define BOX_W 2
#define BOX_H 3

// Lines management in display
enum Align { LEFT, CENTER, RIGHT };

// ST7789 is placed FIRST (value 0) on purpose: g_deviceData is in BSS (zero-init), so before identifyDevice() runs g_deviceData.screen reads as ST7789
// — the primary target with a working renderer. If a consumer ever runs before identifyDevice() it will take the ST7789 branch instead of falling into
// the OLEDSHIELD stub path that logs "DISPLAY_TYPE_NOT_CONFIGURED" and returns. Don't reorder these unless you've audited every code path that reads
// g_deviceData.screen for early access.
enum DisplayType {
    ST7789,      // 320x240, lib Adafruit_ST7789 — primary target
    OLEDSHIELD,  // lib Adafruit_SSD1306 — alternate target, stub implementation only
};

// Burn-in protection: drives the local activity / sleep state machine.
enum DisplayPowerState {
    DISPLAY_ON,
    DISPLAY_DIMMED,  // 50% backlight (if TFT_BL is a real PWM pin); otherwise no visible change but timer still progresses to OFF
    DISPLAY_OFF,     // panel sleeps via Adafruit_ST7789::enableDisplay(false)
};

#endif