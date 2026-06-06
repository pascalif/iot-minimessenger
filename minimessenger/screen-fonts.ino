

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
    String         texts[]     = { "aaaaa", "AAAAA", "ttttt", "qqqqq", "Attqq", "     ", "_____" };
    String         fontNames[] = { "default", "FreeSans09pt8b" };
    const GFXfont* fonts[]     = { NULL, &CONVO_MSG_FONT_REF };

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


void fontsTestRenderMiscFonts() {
    if (g_deviceData.screen != DisplayType::ST7789) {
        return;
    }

    int inBetweenLines = 5;
    int inBetweenFonts = 25;  // inBetweenLines is also added

    Adafruit_ST7789* pDisp = (Adafruit_ST7789*)g_disp;
    pDisp->fillScreen(ST77XX_BLACK);
    pDisp->setTextColor(ST77XX_WHITE);

    char accents[] = " Tg";
    //    char accents[] = { ' ', 0xC3, 0xA9, 0xC3, 0xAF, 0xC3, 0xA7, 0x00 };  // é=0xC3A9, ï=0xC3AF, ç=0xC3A7
    //     ESP_LOGI(TAG_MM, "************ length: %d (%s)", accents, sizeof(accents));
    //    utf8ToLatin1(accents);
    //    ESP_LOGI(TAG_MM, "************ length: %d (%s)", accents, sizeof(accents));

    int y = 0;

    auto show = [&](const GFXfont* font, uint16_t color, int size, const String& label) {
        static int16_t tsBox[4] = { 0, 0, 0, 0 };  // x1, y1, w, h
        g_disp->setTextColor(color);
        pDisp->setTextSize(size);
        g_disp->getTextBounds("Tj", 0, 0, &tsBox[BOX_X], &tsBox[BOX_Y], (uint16_t*)&tsBox[BOX_W], (uint16_t*)&tsBox[BOX_H]);

        pDisp->setCursor(0, y);
        pDisp->print(label);
        y += tsBox[BOX_H] + inBetweenLines;
    };

    show(nullptr, ST77XX_WHITE, 1, "Nullj 1");
    show(&FreeSans09pt8b, ST77XX_YELLOW, 1, "Free09pt8b 1");
    show(&FreeSans10pt8b, ST77XX_ORANGE, 1, "Free10pt8b 1");
    show(nullptr, ST77XX_WHITE, 2, "Nullj 2");
    show(&FreeSans09pt8b, ST77XX_YELLOW, 2, "Free09pt8b 2");
    show(&FreeSans10pt8b, ST77XX_ORANGE, 2, "Free10pt8b 2");
    show(nullptr, ST77XX_WHITE, 3, "Nullj 3");
    show(&FreeSans09pt8b, ST77XX_YELLOW, 3, "Free09pt8b 3");
    show(&FreeSans10pt8b, ST77XX_ORANGE, 3, "Free10pt8b 3");
    show(nullptr, ST77XX_WHITE, 4, "Nullj 4");

    //    pDisp->setFont(NULL);
    //    for (int size = 1; size <= 3; size++) {
    //        pDisp->setTextSize(size);
    //        pDisp->setCursor(0, y);
    //        pDisp->print(String("Nullg s") + size + accents);
    //        y += 8 * size + inBetweenLines;
    //    }
    //
    //    y += 20;
    //    pDisp->setFont(&FreeSans09pt8b);
    //    for (int size = 1; size <= 2; size++) {
    //        pDisp->setTextSize(size);
    //        pDisp->setCursor(0, y);
    //        pDisp->print(String("FS9 s") + size + accents);
    //        y += 18 * size + inBetweenLines;
    //    }
    //
    //    y += inBetweenFonts;
    //    pDisp->setFont(&FreeSans10pt8b);
    //    for (int size = 1; size <= 2; size++) {
    //        pDisp->setTextSize(size);
    //        pDisp->setCursor(0, y);
    //        pDisp->print(String("FS10 s") + size + accents);
    //        y += 24 * size + inBetweenLines;
    //    }

    delay(120'000);
}