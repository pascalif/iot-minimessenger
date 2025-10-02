#ifndef SYMBOLS_H
#define SYMBOLS_H


// Lines management in display
enum Align { LEFT,
             CENTER,
             RIGHT };

enum DisplayType {
    OLEDSHIELD, // lib Adafruit_SSD1306
    ST7789, // 320x240, lib Adafruit_ST7789
};


#endif