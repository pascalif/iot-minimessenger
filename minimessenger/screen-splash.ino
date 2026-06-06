// ================================================================================
// screen-splash.ino — Boot splash screen (logo + title), shown once at startup
// ================================================================================
//
// Auto-generated splash bitmap (see ../docs/howto_logo.md). Must be a .h, not a .ino: Arduino IDE concatene les .ino apres minimessenger.ino, donc un
// screen-splash-img.ino arrive trop tard pour que showSplashScreen() voie les declarations de splash_bmp / splash_bmp_w / splash_bmp_h.
#include "screen-splash-img.h"

#define SPLASH_FONT_REF  FONT_DEFAULT_07_0PX
#define SPLASH_FONT_SIZE 2


// Owns the pixels painted at boot before the runtime UI takes over: a centered title text and the auto-generated Goku bitmap from
// screen-splash-img.h. Arduino IDE concatenates this file with minimessenger.ino into a single TU, so the layout constants (FB_WIDTH, FB_HEIGHT),
// the global g_disp / g_deviceData / g_inConversationMode, the splash_bmp* symbols (from screen-splash-img.h, included near the top of
// minimessenger.ino), and the HW-scroll primitives are all visible without forward decls.

void showSplashScreen() {
    // Show image buffer on the display hardware.
    // Since the buffer is intialized with an Adafruit splashscreen internally, this will display the splashscreen.

    if (g_deviceData.screen == DisplayType::ST7789) {
        Adafruit_ST7789* pDisp = (Adafruit_ST7789*)g_disp;

        g_inConversationMode = false;  // fullscreen mode, suppress status bar repaint
        hwScrollReset();               // ensure we draw at framebuffer Y = screen Y = 0

        // Vertical layout: trois gaps égaux entourent le texte et l'image. Le texte (default 5×7 font à setTextSize(2)) est traité comme 24 px de
        // hauteur — c'est la hauteur visuelle "encombrement" décidée côté UI, pas la hauteur de glyphe stricte (16 px) — et l'image est de
        // splash_bmp_h (128 px). gap = (FB_HEIGHT - kTitleH - splash_bmp_h) / 3 = (320 - 24 - 128) / 3 = 56 px → texte à Y=56, image à Y=136.
        // Avec setFont(NULL) le setCursor(y) cible le TOP du texte (pas la baseline comme avec les fontes GFX), et drawRGBBitmap place le coin
        // haut-gauche → on peut écrire les Y directement sans compensation.
        constexpr int16_t kTitleH = 24;
        const int16_t     gap     = (FB_HEIGHT - kTitleH - (int16_t)splash_bmp_h) / 3;

        // Title centered horizontally. Default 5×7 font at setTextSize(2) → 12 px advance per glyph. Text width = strlen × 12 ; centered X.
        pDisp->setFont(SPLASH_FONT_REF);
        pDisp->setTextSize(SPLASH_FONT_SIZE);
        pDisp->setTextColor(ST77XX_WHITE);
        const char* title  = "MiniMessenger !";
        int16_t     titleW = (int16_t)strlen(title) * 12;
        pDisp->setCursor((FB_WIDTH - titleW) / 2, gap);
        pDisp->print(title);

        // Logo Goku, centré horizontalement, positionné verticalement après texte + gap.
        pDisp->drawRGBBitmap((FB_WIDTH - splash_bmp_w) / 2, gap + kTitleH + gap, splash_bmp, splash_bmp_w, splash_bmp_h);

        delay(2'000);
    } else {
        ESP_LOGW(TAG_MM, "Display: no splash screen (display not configured)");
    }
}
