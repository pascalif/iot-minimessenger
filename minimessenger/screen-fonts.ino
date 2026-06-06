

void fontsTestSizeComputation() {
    // ==== Font default
    // lineAdvance : 8
    // Bounds for text [jjjjj]: x1=0, y1=0, w=30, h=8
    // Bounds for text [Abefg]: x1=0, y1=0, w=30, h=8
    // Bounds for text [     ]: x1=0, y1=0, w=30, h=8
    // Bounds for text [_____]: x1=0, y1=0, w=30, h=8
    // ==== Font FreeSans09pt8b
    // yAdvance : 22
    // lineAdvance : 22
    // Bounds for text [aaaaa]: x1=1, y1=-9, w=49, h=10
    // Bounds for text [ttttt]: x1=1, y1=-11, w=24, h=12
    // Bounds for text [jjjjj]: x1=0, y1=-12, w=20, h=17
    // Bounds for text [Abefg]: x1=0, y1=-12, w=46, h=17
    // Bounds for text [     ]: x1=0, y1=0, w=20, h=0
    // Bounds for text [_____]: x1=0, y1=3, w=50, h=1

    uint8_t        textSize    = 1;
    String         texts[]     = { "aaaaa", "AAAAA", "ttttt", "qqqqy", "Attqy", "     ", "_____" };
    String         fontNames[] = { "default", "FreeSans09pt8b" };
    const GFXfont* fonts[]     = { NULL, CONVO_MSG_FONT_REF };

    int16_t  x1, y1;
    uint16_t w, h;

    for (int f = 0; f < 2; f++) {
        ESP_LOGD(TAG_MM, "==== Font %s", fontNames[f].c_str());
        const GFXfont* font = fonts[f];

        g_disp->setFont(font);
        g_disp->setTextSize(textSize);

        uint8_t yAdvance = 8;
        if (font != NULL) {
            yAdvance = pgm_read_byte(&font->yAdvance);
            ESP_LOGD(TAG_MM, "yAdvance: %u", yAdvance);
        }
        uint8_t lineAdvance = yAdvance * textSize;
        ESP_LOGD(TAG_MM, "lineAdvance: %u", lineAdvance);

        for (auto& text : texts) {
            g_disp->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
            ESP_LOGD(TAG_MM, "Bounds for [%s]: x1=%d y1=%d w=%u h=%u", text.c_str(), x1, y1, w, h);
        }
    }
}

#define GRADIENT_1 RGB565(255, 255, 255)  // White
#define GRADIENT_2 RGB565(255, 240, 180)  // Blanc chaud
#define GRADIENT_3 RGB565(255, 220, 100)  // Jaune clair
#define GRADIENT_4 RGB565(255, 200, 0)    // Yellow
#define GRADIENT_5 RGB565(255, 160, 0)    // Jaune-orange
#define GRADIENT_6 RGB565(255, 120, 0)    // Orange
#define GRADIENT_7 RGB565(255, 60, 0)     // Orange-rouge
#define GRADIENT_8 RGB565(255, 0, 0)      // Red

void fontsTestRenderMiscFonts() {
    if (g_deviceData.screen != DisplayType::ST7789) {
        return;
    }

    hwScrollReset();

    Adafruit_ST7789* pDisp = (Adafruit_ST7789*)g_disp;
    pDisp->fillScreen(ST77XX_BLACK);
    pDisp->setTextColor(ST77XX_WHITE);

    char accents[] = " Tg";
    //    char accents[] = { ' ', 0xC3, 0xA9, 0xC3, 0xAF, 0xC3, 0xA7, 0x00 };  // é=0xC3A9, ï=0xC3AF, ç=0xC3A7
    //     ESP_LOGI(TAG_MM, "************ length: %d (%s)", accents, sizeof(accents));
    //    utf8ToLatin1(accents);
    //    ESP_LOGI(TAG_MM, "************ length: %d (%s)", accents, sizeof(accents));

    int y = 0;

    auto show = [&](const GFXfont* font, int size, const String& label) {
        static int16_t tsBox[4] = { 0, 0, 0, 0 };  // x1, y1, w, h

        uint16_t color = GRADIENT_1;
        if (font == FREESANS_ACCENTS_08_1PX) {
            color = GRADIENT_2;
        } else if (font == FREESANS_ACCENTS_09_2PX) {
            color = GRADIENT_3;
        } else if (font == FREESANS_ACCENTS_10_2PX) {
            color = GRADIENT_4;
        } else if (font == FREESANS_ACCENTS_12_3PX) {
            color = GRADIENT_5;
        } else if (font == FREESANS_ACCENTS_13_3PX) {
            color = GRADIENT_6;
        } else if (font == FREESANS_ACCENTS_15_3PX) {
            color = GRADIENT_7;
        }

        pDisp->setFont(font);
        pDisp->setTextColor(color);
        pDisp->setTextSize(size);

        pDisp->getTextBounds("Tj", 0, 0, &tsBox[BOX_X], &tsBox[BOX_Y], (uint16_t*)&tsBox[BOX_W], (uint16_t*)&tsBox[BOX_H]);
        pDisp->setCursor(0, y - tsBox[BOX_Y]);

        pDisp->print(label);

        y += tsBox[BOX_H] + 5;
    };

    // nullptr: 7px high
    show(FONT_DEFAULT_07_0PX, 1, "Null 1y");
    show(FREESANS_ACCENTS_08_1PX, 1, "Free08-1 1y");
    show(FREESANS_ACCENTS_09_2PX, 1, "Free09-2 1y");
    show(FREESANS_ACCENTS_10_2PX, 1, "Free10-2 1y");
    show(FREESANS_ACCENTS_12_3PX, 1, "Free12-3 1y");
    show(FREESANS_ACCENTS_13_3PX, 1, "Free13-3 1y");
    show(FONT_DEFAULT_07_0PX, 2, "Null 2y");
    show(FREESANS_ACCENTS_15_3PX, 1, "Free15-3 1y");
    show(FREESANS_ACCENTS_08_1PX, 2, "Free08-1 2y");
    show(FREESANS_ACCENTS_09_2PX, 2, "Free09-2 2y");
    show(FONT_DEFAULT_07_0PX, 3, "Null 3y");
    show(FREESANS_ACCENTS_13_3PX, 2, "F13-2 2y");
    show(FREESANS_ACCENTS_15_3PX, 2, "F10 2y");

    delay(60'000);
}