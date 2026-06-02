// ================================================================================
// bars.ino — Top status bar + bottom keyboard bar: color palette, helpers, drawing functions
// ================================================================================
//
// Owns every pixel painted into the two fixed strips (TFA = top status bar, BFA = bottom keyboard input footer). Arduino IDE concatenates this
// file with the main .ino into a single translation unit (alphabetical order after the sketch-named file, so bars.ino lands right after
// minimessenger.ino). Consequence: all the layout constants in minimessenger.ino above (STATUS_BAR_H, FOOTER_H, FOOTER_Y_FB, FB_WIDTH,
// ICON_*_X, ICON_RADIUS, ICON_Y_CENTER, g_disp, g_statusBarDirty, g_inConversationMode, g_kb, …) are visible here without forward decls.
//
// Why colocate constants + functions in the same .ino rather than splitting into bars.h + bars.ino: pure #defines in a separate .ino file
// would be invisible to any earlier-concatenated file. Keeping the call sites of every color macro inside this same TU section means we can
// rename / retune palette entries with a single grep here. Same pattern as commands.ino (CMD_* constants + dispatcher together).


#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))


// === Color palette ==============================================================

// Light-gray hairline drawn on the bottom row of the status bar to visually separate it from the scroll area. RGB565 of #DDDDDD = 0xDEFB.
#define STATUS_BAR_SEPARATOR_COLOR 0xDEFB

// Background of the whole status-bar strip — also used as the wipe color before each repaint.
#define STATUS_BAR_BG_COLOR RGB565(0x30, 0x15, 0x10) //ST77XX_BLACK

// Per-indicator colors on the top status bar. ICON_BT_COLOR is reused by the footer's "<no keyboard>" placeholder so the BT state stays
// visually coherent across both bars (same hue when the keyboard is absent, regardless of which bar is showing it).
#define ICON_WIFI_COLOR    ST77XX_WHITE
#define ICON_MQTT_COLOR    ST77XX_YELLOW
#define ICON_BT_COLOR      ST77XX_BLUE
#define ICON_CAPS_COLOR    ST77XX_WHITE
#define ICON_CONTACT_COLOR ST77XX_RED

// Background of the whole keyboard bar (input footer) strip — wipe color before each repaint.
#define KB_BAR_BG_COLOR  ST77XX_BLACK

// Hairline separator on the top row of the footer. Mirrors STATUS_BAR_SEPARATOR_COLOR — kept as a separate macro so the two bars can drift
// apart later (e.g. different shade on the footer) without a cascading rename.
#define KB_BAR_SEPARATOR_COLOR STATUS_BAR_SEPARATOR_COLOR

// Color of the currently-being-typed text shown right-aligned in the footer.
#define KB_BAR_TEXT_COLOR ST77XX_WHITE

// Yellow caret bar drawn at the insertion point, growing leftward as the user types.
#define KB_BAR_CURSOR_COLOR ST77XX_YELLOW


// === Last-drawn-state cache =====================================================
// Used by redrawStatusBar() to skip a repaint when no indicator's state changed. Lives here because nothing outside redrawStatusBar reads or
// writes these — they are entirely internal to the bar's repaint debounce. g_statusBarDirty (defined in minimessenger.ino) is the external
// override flag that callers use to FORCE a repaint regardless of cache.
static bool g_lastDrawnBt           = false;
static bool g_lastDrawnWifi         = false;
static bool g_lastDrawnMqtt         = false;
static bool g_lastDrawnCaps         = false;
// Sentinel -1 forces a paint on the very first redraw — any real count from contactGetActiveCount() (0..MAX_CONTACTS) breaks the cache match.
static int  g_lastDrawnContactCount = -1;


// === Status bar (TFA) ===========================================================
// Three indicators on the left, one chip on the right. Filled = ON, outline only = OFF.
static void drawIndicatorAt(int x, bool filled, uint16_t color) {
    if (filled) {
        g_disp->fillCircle(x, ICON_Y_CENTER, ICON_RADIUS, color);
    } else {
        g_disp->drawCircle(x, ICON_Y_CENTER, ICON_RADIUS, color);
    }
}

// CapsLock indicator: glyph 'A' (caps ON) ou 'a' (caps OFF) en blanc, à droite du chip BT. La détection est portée par la variable globale
// kbIsCapsLockOn, basculée dans decodeHIDReport() à la frappe du KEY_CAPSLOCK ; ce dernier marque g_statusBarDirty pour un rafraîchissement immédiat
// sans attendre le tick périodique. Police par défaut 5×7 à setTextSize(2) → glyphe 10×14 px, comparable au diamètre des disques voisins. Le cursor
// est offset de la moitié des dimensions du glyphe pour centrer visuellement sur (cx, ICON_Y_CENTER).
static void drawCapsAt(int cx, bool capsOn, uint16_t color) {
    g_disp->setFont(NULL);
    g_disp->setTextSize(2);
    g_disp->setTextColor(color);
    g_disp->setCursor(cx - 5, ICON_Y_CENTER - 7);
    g_disp->print(capsOn ? 'A' : 'a');
}

// Person silhouette used for the contact chip on the right of the status bar: small head + rounded shoulders/torso, sized to match visually the
// radius-6 disc indicators on the left. Plein si filled=true (contact présent), simple contour sinon. Tête = cercle r=3 centré à (cx, 8) ; corps =
// RoundRect 11×7 démarrant à (cx-5, 12) avec coins r=3 pour évoquer les épaules. Un pixel d'air entre la tête (y=5..11) et le corps (y=12..18) sert
// de cou et garde la silhouette lisible en mode contour.
static void drawPersonAt(int cx, bool filled, uint16_t color) {
    const int headCy = ICON_Y_CENTER - 4;  // 8
    const int headR  = 3;
    const int bodyX  = cx - 5;
    const int bodyY  = ICON_Y_CENTER;  // 12
    const int bodyW  = 11;
    const int bodyH  = 7;
    const int bodyR  = 3;

    if (filled) {
        g_disp->fillCircle(cx, headCy, headR, color);
        g_disp->fillRoundRect(bodyX, bodyY, bodyW, bodyH, bodyR, color);
    } else {
        g_disp->drawCircle(cx, headCy, headR, color);
        g_disp->drawRoundRect(bodyX, bodyY, bodyW, bodyH, bodyR, color);
    }
}

// Read current connection states and repaint the bar if any of them changed since the last draw (or if forced by g_statusBarDirty). No-op when
// a fullscreen mode (splash, info) is showing — would otherwise paint icons over their content.
void redrawStatusBar() {
        if (g_displayType != DisplayType::ST7789) {
            return;
        }

        if (!g_inConversationMode) {
        return;
    }

    bool bt           = g_kb.isFullyConnected();
    bool wifi         = (WiFi.status() == WL_CONNECTED);
    bool mqtt         = g_mqttClient.connected();
    bool caps         = kbIsCapsLockOn;
    int  contactCount = contactGetActiveCount();  // 0 = aucun contact en ligne, 1 = un seul, 2+ = au moins deux (capped à l'affichage 2-icônes).

    if (!g_statusBarDirty && bt == g_lastDrawnBt && wifi == g_lastDrawnWifi && mqtt == g_lastDrawnMqtt && caps == g_lastDrawnCaps &&
        contactCount == g_lastDrawnContactCount) {
        return;
    }

    g_disp->fillRect(0, 0, FB_WIDTH, STATUS_BAR_H, STATUS_BAR_BG_COLOR);

    // Left cluster (network reachability): WiFi puis MQTT côte à côte.
    drawIndicatorAt(ICON_WIFI_X, wifi, ICON_WIFI_COLOR);
    drawIndicatorAt(ICON_MQTT_X, mqtt, ICON_MQTT_COLOR);

    // Right cluster (keyboard input state): BT puis indicateur CapsLock 'A'/'a', séparés des chips réseau par le spacer de 50 px défini dans ICON_BT_X.
    drawIndicatorAt(ICON_BT_X, bt, ICON_BT_COLOR);
    drawCapsAt(ICON_CAPS_X, caps, ICON_CAPS_COLOR);
    // Contact silhouettes: 0 contact → 1 icône en contour, 1 contact → 1 icône pleine, 2+ contacts → 2 icônes pleines côte à côte (centrées sur
    // l'ancre historique ICON_CONTACT_X). Le compte vient de contactGetActiveCount() (contacts.ino), alimenté par les liveness MQTT admin/live + admin/dead.
    if (contactCount <= 0) {
        drawPersonAt(ICON_CONTACT_X, false, ICON_CONTACT_COLOR);
    } else if (contactCount == 1) {
        drawPersonAt(ICON_CONTACT_X, true, ICON_CONTACT_COLOR);
    } else {
        drawPersonAt(ICON_CONTACT_X_LEFT, true, ICON_CONTACT_COLOR);
        drawPersonAt(ICON_CONTACT_X_RIGHT, true, ICON_CONTACT_COLOR);
    }

    // Separator hairline, one pixel above the bottom of the bar — the very bottom row stays black to give the icons breathing room.
    g_disp->drawFastHLine(0, STATUS_BAR_H - 2, FB_WIDTH, STATUS_BAR_SEPARATOR_COLOR);

    g_lastDrawnBt           = bt;
    g_lastDrawnWifi         = wifi;
    g_lastDrawnMqtt         = mqtt;
    g_lastDrawnCaps         = caps;
    g_lastDrawnContactCount = contactCount;
    g_statusBarDirty        = false;
}


// === Input feedback footer (BFA) ================================================
// Shows the current `g_currentMsgFromKeyboard` being typed via the BT keyboard, with the yellow cursor bar tracking g_msgCursorIdx (movable
// via arrow keys). Repainted on every keystroke (insert / delete / move / send).
void redrawInputFooter() {
    if (!g_inConversationMode) {
        return;
    }
    if (g_displayType != DisplayType::ST7789) {
        return;
    }

    // Wipe the whole footer strip.
    g_disp->fillRect(0, FOOTER_Y_FB, FB_WIDTH, FOOTER_H, KB_BAR_BG_COLOR);

    // Footer vertical layout (19 px total, rows FOOTER_Y_FB..FOOTER_Y_FB+18):
    //   +0       : 1 px black aération against the scroll area above
    //   +1       : white hairline separator (light gray, matches upper bar)
    //   +2       : 1 px black margin
    //   +3..+18  : 16 px text zone (size-2 default font) + yellow cursor bar
    g_disp->drawFastHLine(0, FOOTER_Y_FB + 1, FB_WIDTH, KB_BAR_SEPARATOR_COLOR);

    // No BT keyboard connected → show a passive "<no keyboard>" banner centered in the footer text area, no cursor. The typed-message buffer
    // is not touched — if the keyboard reconnects, the next redrawInputFooter() repaints what was already typed (e.g. via the serial monitor
    // path). The placeholder reuses ICON_BT_COLOR so the BT-state hue is shared with the top bar's BT chip.
    if (!g_kb.isFullyConnected()) {
        const char* placeholder = "<no keyboard>";
        const int   charW       = 12;
        const int   textW       = (int)strlen(placeholder) * charW;
        g_disp->setFont(NULL);
        g_disp->setTextSize(2);
        g_disp->setTextColor(ICON_BT_COLOR);
        g_disp->setCursor((FB_WIDTH - textW) / 2, FOOTER_Y_FB + 3);
        g_disp->print(placeholder);
        return;
    }

    // Typed text, default 5×7 font at size 2 → 12 px per char, fits ~18 chars. Right-aligned: the rightmost glyph of the viewport is pinned
    // 2 px to the left of the screen edge, and the text grows leftward as the user types.
    const int   kCharWidth  = 12;
    const int   kInsideX    = 6;
    const int   kRightEdgeX = FB_WIDTH - 8;
    const int   kTextAreaW  = kRightEdgeX - kInsideX - 2;
    const int   kMaxChars   = kTextAreaW / kCharWidth;  // 18
    const char* full        = g_currentMsgFromKeyboard.c_str();
    const int   len         = (int)g_currentMsgFromKeyboard.length();
    const int   cur         = (int)g_msgCursorIdx;

    // Viewport [viewStart, viewEnd) — at most kMaxChars chars, always contains the cursor. Anchored to the message end by default; if the
    // cursor is further to the right than that window would allow (which only matters when len <= kMaxChars but cur could equal len), the math
    // degenerates to showing the whole message.
    int       viewEnd    = cur;
    const int defaultEnd = (len > kMaxChars) ? kMaxChars : len;
    if (viewEnd < defaultEnd) {
        viewEnd = defaultEnd;
    }
    if (viewEnd > len) {
        viewEnd = len;
    }
    const int   viewStart  = (viewEnd > kMaxChars) ? (viewEnd - kMaxChars) : 0;
    const int   visibleLen = viewEnd - viewStart;
    const char* shown      = full + viewStart;
    const int   textStartX = kRightEdgeX - 2 - visibleLen * kCharWidth;

    g_disp->setFont(NULL);
    g_disp->setTextSize(2);
    g_disp->setTextColor(KB_BAR_TEXT_COLOR);
    g_disp->setCursor(textStartX, FOOTER_Y_FB + 3);
    g_disp->print(shown);

    // Yellow cursor bar, drawn at the insertion-point gap (1 px wide, slightly shorter than the text height so it never visually touches the
    // white hairline separator just above. When the cursor sits at the end of the visible window it lands in the 2 px right-edge gap; when in
    // the middle it falls in the inter-character spacing of the previous glyph.
    const int cursorOffset = cur - viewStart;  // 0..visibleLen
    const int yellowX      = textStartX + cursorOffset * kCharWidth - 1;
    g_disp->drawFastVLine(yellowX, FOOTER_Y_FB + 5, 14, KB_BAR_CURSOR_COLOR);
}
