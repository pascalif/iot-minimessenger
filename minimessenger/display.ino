// ================================================================================
// display.ino — Splash, info screen, conversation drawing, /help info-line printers
// ================================================================================
//
// Owns the pixels of every "long-form" UI element: the boot splash, the full-screen info / status overlay, every block of the conversation
// scroll area, and the two-column /help listings. Arduino IDE concatenates this file with minimessenger.ino into a single translation unit
// (alphabetical order after the sketch-named file), so all the layout constants in minimessenger.ino (FB_WIDTH, FB_HEIGHT, SCROLL_AREA_*,
// CONVO_*, INFO_LINE_RIGHT_COL_X, BOX_*, …), all the globals (g_disp, g_deviceData, g_drawY, g_scrollY, g_lineHead, g_lineCount, lines[],
// g_inConversationMode, g_lastShownTsEpoch, g_lastMsgSenderId, g_mqttClient, g_kb, …), and the HW-scroll primitives (hwScrollSetupArea(),
// hwScrollTo(), hwScrollReset()) are visible here without forward decls.
//
// What stays in minimessenger.ino on purpose:
//   - setupDisplay() / setDisplayPowerState() / updateDisplayPowerState() — boot-time hardware init and idle-timer dim/off state machine,
//     orchestration code that doesn't draw glyphs.
//   - the HW-scroll primitives (hwScrollSetupArea / hwScrollTo / hwScrollReset) — low-level ST7789 register pokes used from many call sites
//     including setupDisplay and clearConversationHistory; live next to them in minimessenger.ino.
//   - clearConversationHistory() / goAndResetConversationScreen() / returnToConversationsScreen() / refreshInfoScreenIfShown() — screen-state
//     transitions, not glyph drawing.
//
// Bar drawing (top status bar + bottom input footer) is in bars.ino, not here. Portal-mode instructions (drawPortalInstructions, called from
// showUpdatedInfoScreen below) live in wifi.ino since they're WiFi-state-specific.


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
        pDisp->setFont(NULL);
        pDisp->setTextSize(2);
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


// Replay every entry of the ring buffer through the same HW-scroll draw
// algorithm as addConversationBlock. After this:
//   - status bar and footer are untouched (protected by VSCRDEF — we only
//     fillRect the scroll area, never fillScreen)
//   - the latest block ends up at the bottom of the scroll area with older
//     blocks stacked above (or scrolled off the top if cumulated height
//     exceeds SCROLL_AREA_H) — visually identical to live message arrival
//   - g_drawY / g_scrollY are left in the "next slot" position so a new
//     addConversationBlock right after lands naturally below the last block
//
// Triggered by the `cmd redraw` command (see processPayloadAsCommand).
// We draw directly from the stored TextLine fields — no re-bound, no
// re-utf8-conversion, no re-alignment compute, no ring rewrite. Everything
// needed was captured at original send time.
void redrawAllConversations() {
    if (g_deviceData.screen != DisplayType::ST7789) {
        ESP_LOGW(TAG_MM, "redrawAllConversations: DISPLAY_TYPE_NOT_CONFIGURED");
        return;
    }
    ESP_LOGI(TAG_MM, "Full redraw — replaying %d block(s) from ring", g_lineCount);

    // Reset HW scroll state and wipe ONLY the scroll area.
    g_drawY   = 0;
    g_scrollY = 0;
    hwScrollTo(0);
    g_disp->fillRect(0, SCROLL_AREA_Y_FB, FB_WIDTH, SCROLL_AREA_H, ST77XX_BLACK);

    for (int k = 0; k < g_lineCount; k++) {
        const TextLine& line = lines[(g_lineHead + k) % MAX_LINES];
        uint16_t        H    = line.tsHeightWithBottomMargin + line.msgHeightWithBottomMargin;

        // Wrap avoidance, mirroring addConversationBlock: if the block would
        // cross the scroll-area top boundary, skip the remaining tail and
        // restart at scroll-offset 0, bumping the scroll register to swallow
        // the skipped pixels.
        if (g_drawY + H > SCROLL_AREA_H) {
            uint16_t skipped = SCROLL_AREA_H - g_drawY;
            g_disp->fillRect(0, g_drawY + SCROLL_AREA_Y_FB, FB_WIDTH, skipped, ST77XX_BLACK);
            g_drawY   = 0;
            g_scrollY = (g_scrollY + skipped) % SCROLL_AREA_H;
        }

        g_disp->fillRect(0, g_drawY + SCROLL_AREA_Y_FB, FB_WIDTH, H, ST77XX_BLACK);

        uint16_t fbY = g_drawY + SCROLL_AREA_Y_FB;
        if (line.ts[0] != '\0') {
            g_disp->setFont(NULL);
            g_disp->setTextSize(line.tsFontSize);
            g_disp->setTextColor(line.tsColor);
            g_disp->setCursor(line.tsX - line.tsBounds[BOX_X], fbY - line.tsBounds[BOX_Y]);
            g_disp->print(line.ts);
            fbY += line.tsHeightWithBottomMargin;
        }
        g_disp->setFont(&CONVO_MSG_FONT);
        g_disp->setTextSize(line.msgFontSize);
        g_disp->setTextColor(line.msgColor);
        g_disp->setCursor(line.msgX - line.msgBounds[BOX_X], fbY - line.msgBounds[BOX_Y]);
        g_disp->print(line.msg);

        g_drawY   = (g_drawY + H) % SCROLL_AREA_H;
        g_scrollY = (g_scrollY + H) % SCROLL_AREA_H;
        hwScrollTo(g_scrollY);
    }
}

// Drop a single line into the conversation scroll area, with optional two-column layout for command listings.
//
// Two overloads share the same impl below; the single-arg one is a thin forwarder that passes an empty `right`. Behavior:
//   - `right` empty  → just `left`, left-aligned at x=2. Used for banners ("Commands:"), status messages ("Forgot: X"), errors.
//   - `right` filled → `left` at x=2, `right` at x=INFO_LINE_RIGHT_COL_X. Used for /help listings (cmd name + description on the same row).
//
// Overload disambiguation: the 1-string version takes a uint16_t color as 2nd arg; the 2-string takes a String. A literal "foo" as 2nd arg
// resolves to String (direct match), beating the uint16_t version which would require a const-char* → uint16_t conversion that doesn't exist.
// A numeric color as 2nd arg resolves to uint16_t (direct match) over String (would need a user-defined conversion). No ambiguity in practice.
//
// HW-scroll primitives (g_drawY / g_scrollY / hwScrollTo) make successive calls accumulate visually like any other conversation content,
// and hwScrollReset() drops the lot during a screen switch.
//
// Defaults: pink CMD color + CONVO_CMD_FONT; both overridable.

void printInfoLine(const String& left, const String& right, uint16_t color, const GFXfont* font) {
    if (g_deviceData.screen != DisplayType::ST7789) {
        return;
    }

    // UTF-8 → Latin-1: bundled FreeSans*_latin1 fonts cap at 0xFF. Mirrors what addConversationBlock does to its message before drawing.
    char leftBuf[CONVO_MSG_MAX_LEN];
    strncpy(leftBuf, left.c_str(), sizeof(leftBuf) - 1);
    leftBuf[sizeof(leftBuf) - 1] = '\0';
    utf8ToLatin1(leftBuf);

    char rightBuf[CONVO_MSG_MAX_LEN];
    strncpy(rightBuf, right.c_str(), sizeof(rightBuf) - 1);
    rightBuf[sizeof(rightBuf) - 1] = '\0';
    utf8ToLatin1(rightBuf);
    const bool hasRight = (rightBuf[0] != '\0');

    g_disp->setFont(font);
    g_disp->setTextSize(CONVO_MSG_FONT_SIZE);
    g_disp->setTextColor(color);

    // Measure left for the row height. getTextBounds returns the bbox of the EXACT string (not the font's max metrics) — lines without
    // descenders take ~3-4 px less, saving vertical space across cumulative /help listings on the 320 px tall screen. With GFX fonts the
    // returned y is negative (bbox top above the baseline-anchored cursor) and we compensate at setCursor time below.
    int16_t  bx, by;
    uint16_t bw, bh;
    g_disp->getTextBounds(leftBuf, 0, 0, &bx, &by, &bw, &bh);

    // Defensive: if the right column has glyphs reaching further down/up than the left, take the max bbox height so the row reserves enough
    // vertical room. Same font / size means by is typically identical for both strings, so we keep `by` from the left for the cursor offset.
    if (hasRight) {
        int16_t  rbx, rby;
        uint16_t rbw, rbh;
        g_disp->getTextBounds(rightBuf, 0, 0, &rbx, &rby, &rbw, &rbh);

        // Important, la hauteur de ligne est la plus grande hauteur des 2 bouts de texte
        if (rbh > bh) {
            bh = rbh;
        }
    }

    const uint16_t lineH = bh + CONVO_HELP_MARGIN_BOTTOM;

    // Wrap avoidance, mirrors addConversationBlock: if the line would overflow the scroll area bottom, paint the leftover tail black and
    // restart at offset 0, bumping the scroll register to swallow the skipped pixels.
    if (g_drawY + lineH > SCROLL_AREA_H) {
        uint16_t skipped = SCROLL_AREA_H - g_drawY;
        g_disp->fillRect(0, g_drawY + SCROLL_AREA_Y_FB, FB_WIDTH, skipped, ST77XX_BLACK);
        g_drawY   = 0;
        g_scrollY = (g_scrollY + skipped) % SCROLL_AREA_H;
    }

    // Black background + draw left at x=2. setCursor compensation: top-of-box Y minus the (negative) by gives the baseline anchor.
    g_disp->fillRect(0, g_drawY + SCROLL_AREA_Y_FB, FB_WIDTH, lineH, ST77XX_BLACK);
    g_disp->setCursor(2 - bx, g_drawY + SCROLL_AREA_Y_FB - by);
    g_disp->print(leftBuf);

    // Draw right at the fixed column, same baseline as left.
    if (hasRight) {
        g_disp->setCursor(INFO_LINE_RIGHT_COL_X, g_drawY + SCROLL_AREA_Y_FB - by);
        g_disp->print(rightBuf);
    }

    // Bump the HW scroll — this is what makes successive calls accumulate visually.
    g_drawY   = (g_drawY + lineH) % SCROLL_AREA_H;
    g_scrollY = (g_scrollY + lineH) % SCROLL_AREA_H;
    hwScrollTo(g_scrollY);
}

void printInfoLine(const String& msg, uint16_t color, const GFXfont* font) {
    printInfoLine(msg, String(), color, font);
}

void printInfoLineNumber(const String& left, uint32_t right, uint16_t color, const GFXfont* font) {
    printInfoLine(left, String(right), color, font);
}

// TODO pourquou pas const String& msg
void printVersatileConversationInfo(String msg) {
    addConversationBlock(String(), msg, CONVO_INFO_COLOR, CENTER);
}

void printVersatileConversationError(String msg) {
    addConversationBlock(String(), msg, CONVO_ERROR_COLOR, CENTER);
}


void addConversationBlock(String ts, String msg, uint16_t msgColor, Align align, byte senderDeviceId) {
    char msgBuf[CONVO_MSG_MAX_LEN];
    strncpy(msgBuf, msg.c_str(), sizeof(msgBuf) - 1);
    msgBuf[sizeof(msgBuf) - 1] = '\0';
    utf8ToLatin1(msgBuf);
    // Then use msgBuf everywhere instead of msg.c_str() for the on-screen draw.

    // Timestamp clustering + author prefix. Two-pronged:
    //   - Clustering: hide ts if the previous message was from the SAME author and arrived less than CONVO_TS_HIDE_THRESHOLD_S ago. A sender change
    //     bypasses the window — the ts is always re-shown so the eye can track who said what.
    //   - Author prefix: prepend the sender's pseudo before the ts (e.g. "Pac 14:23:11") when the message is from a peer. For our own messages
    //     (senderDeviceId == g_deviceData.deviceId) the RIGHT alignment is already the visual cue, no prefix. For system lines (Ready / Lost
    //     server / [ERROR]) ts is "" and we skip the whole block.
    if (!ts.isEmpty()) {
        const time_t now              = time(nullptr);
        const bool   sameSenderAsLast = (senderDeviceId == g_lastMsgSenderId);
        const bool   insideTimeWindow     = (now - g_lastShownTsEpoch < CONVO_TS_HIDE_THRESHOLD_S);

        if (sameSenderAsLast && insideTimeWindow) {
            ts = "";  // suppressed; the empty-ts path below skips the ts row entirely
        } else if (senderDeviceId != DEVICE_ID_UNSET && senderDeviceId != g_deviceData.deviceId) {
            // Peer or anonymous "unk" — prefix the pseudo. findById() does an O(N) walk over COMPILED_DEVICE_DATA_ENTRIES; the table is small
            // (a handful of rows) so the lookup is essentially free, and doing it here keeps the routing layer agnostic to identity.
            const char* pseudoSrc = "unk";  // default = received without trailer (web console / mosquitto_pub)
            if (senderDeviceId != 0) {
                const DeviceDataEntry* entry = DeviceDataEntry::findById(senderDeviceId);
                pseudoSrc                    = (entry != nullptr) ? entry->pseudo : "???";
            }
            // Pseudos in personal-data.h may carry accents (Aimée, François, …). The font used to draw `ts` only knows Latin-1 codepoints, so we
            // convert via utf8ToLatin1 — same pre-treatment we do further below for `msg`. Buffer sized for short pseudos (convention: < 16 chars).
            char pseudoBuf[24];
            strncpy(pseudoBuf, pseudoSrc, sizeof(pseudoBuf) - 1);
            pseudoBuf[sizeof(pseudoBuf) - 1] = '\0';
            utf8ToLatin1(pseudoBuf);
            ts = String(pseudoBuf) + " - " + ts;
        }

        // Always update the trackers when we entered this block, even if ts was suppressed: the NEXT message compares against this most recent
        // arrival, not the most recent DISPLAYED ts. Otherwise a long stream of same-sender messages would never reset and a sender change after
        // many silent clusters wouldn't bypass cleanly.
        g_lastShownTsEpoch = now;
        g_lastMsgSenderId  = senderDeviceId;
    }

    // Dimension TS
    uint16_t tsBlockHWithMargin = 0;

    static int16_t tsBox[4] = { 0, 0, 0, 0 };  // x1, y1, w, h

    /*
1. Pourquoi getTextBounds retourne des valeurs de y négatives avec certaines polices comme FreeSans9pt8b ?
Dans les bibliothèques graphiques comme Adafruit_GFX, le système de coordonnées pour le texte est basé sur le point de base (baseline) du texte. Voici ce qui se passe :

Origine du texte :
Le point (0, 0) pour le texte est généralement placé sur la ligne de base (baseline) du texte, c'est-à-dire la ligne sur laquelle reposent les lettres (sans les descendantes comme "j", "p", "g", etc.).

Valeurs négatives de y :
Pour les polices avec des ascendantes (parties des lettres qui montent au-dessus de la ligne de base, comme "h", "b", "d"), la coordonnée y du rectangle englobant (getTextBounds) peut être négative. Cela signifie que la partie supérieure du texte s'étend au-dessus de la ligne de base.

Exemple : Si la police a une hauteur de 9 pixels et que la ligne de base est à y=0, le sommet des ascendantes pourrait être à y=-2 (selon la police).

Polices comme FreeSans9pt8b :
Ces polices sont souvent conçues avec des ascendantes et descendantes importantes, ce qui explique pourquoi getTextBounds peut retourner des valeurs de y négatives.


Dans la méthode getTextBounds de la bibliothèque Adafruit_GFX, les paramètres retournés sont généralement les suivants :

x : Coordonnée horizontale du coin supérieur gauche du rectangle englobant.
y : Coordonnée verticale du coin supérieur gauche du rectangle englobant (peut être négative si le texte dépasse au-dessus de la ligne de base).
w : Largeur du rectangle englobant.
h : Hauteur totale du rectangle englobant, c'est-à-dire la distance entre le point le plus haut (ascendantes) et le point le plus bas (descendantes) du texte.
Pas besoin d'ajouter l'opposé de y : h est déjà calculé comme la distance entre le point le plus haut (même s'il est au-dessus de la ligne de base, donc y négatif) et le point le plus bas.
*/

    if (!ts.isEmpty()) {
        g_disp->setFont(NULL);
        g_disp->setTextSize(CONVO_TS_FONT_SIZE);
        g_disp->getTextBounds(ts, 0, 0, &tsBox[BOX_X], &tsBox[BOX_Y], (uint16_t*)&tsBox[BOX_W], (uint16_t*)&tsBox[BOX_H]);

        tsBlockHWithMargin = tsBox[BOX_H] + CONVO_TS_MARGIN_BOTTOM;
    }

    // Dimension msg
    uint16_t       msgBlockHWithMargin = 0;
    static int16_t msgBox[4]           = { 0, 0, 0, 0 };

    g_disp->setFont(&CONVO_MSG_FONT);
    g_disp->setTextSize(CONVO_MSG_FONT_SIZE);
    g_disp->getTextBounds(msgBuf, 0, 0, &msgBox[BOX_X], &msgBox[BOX_Y], (uint16_t*)&msgBox[BOX_W], (uint16_t*)&msgBox[BOX_H]);
    msgBlockHWithMargin = msgBox[BOX_H] + CONVO_MSG_MARGIN_BOTTOM;


    uint16_t H = tsBlockHWithMargin + msgBlockHWithMargin;

    // Compute X alignment in screen-space (g_disp->width() reflects rotation 0
    // → 240 px, so this works directly in framebuffer coords too).
    uint16_t tsX = 0, msgX = 0;
    if (align == RIGHT) {
        tsX  = g_disp->width() - tsBox[BOX_W];
        msgX = g_disp->width() - msgBox[BOX_W];
    } else if (align == CENTER) {
        tsX  = (g_disp->width() - tsBox[BOX_W]) / 2;
        msgX = (g_disp->width() - msgBox[BOX_W]) / 2;
    }

    // === Maintain the logical ring buffer (state only — not used for drawing) ===
    while (g_lineCount >= MAX_LINES) {
        g_lineHead = (g_lineHead + 1) % MAX_LINES;
        g_lineCount--;
    }
    int writeIdx    = (g_lineHead + g_lineCount) % MAX_LINES;
    lines[writeIdx] = TextLine(ts,
                               CONV0_TS_COLOR,
                               NULL,
                               CONVO_TS_FONT_SIZE,
                               tsBlockHWithMargin,
                               tsX,
                               tsBox,
                               msgBuf,
                               msgColor,
                               &CONVO_MSG_FONT,
                               CONVO_MSG_FONT_SIZE,
                               msgBlockHWithMargin,
                               msgX,
                               msgBox);
    g_lineCount++;

    // === HW scroll draw path: write the block directly into the framebuffer
    // ring, then bump VSCSAD by H. Old content scrolls off the top as the
    // controller re-maps the visible window — no full redraw. ===

    // Wrap avoidance: if the block would cross the scroll-area top boundary,
    // skip the remaining tail and restart at scroll-offset 0. The skipped tail
    // is wiped (otherwise stale content from a previous wrap would scroll back
    // into view), and the same number of pixels is added to the scroll so the
    // user perceives a single (slightly bigger than H) smooth scroll.
    // NOTE: `g_drawY` and `g_scrollY` are offsets INSIDE the scroll area
    // ([0, SCROLL_AREA_H)). All fillRect / setCursor positions are absolute
    // framebuffer Y, so we add SCROLL_AREA_Y_FB (= STATUS_BAR_H) at draw time.
    if (g_drawY + H > SCROLL_AREA_H) {
        uint16_t skipped = SCROLL_AREA_H - g_drawY;
        g_disp->fillRect(0, g_drawY + SCROLL_AREA_Y_FB, FB_WIDTH, skipped, ST77XX_BLACK);
        g_drawY   = 0;
        g_scrollY = (g_scrollY + skipped) % SCROLL_AREA_H;
    }

    // Wipe the strip we're about to overdraw (stale content from the previous
    // wrap of the framebuffer ring).
    g_disp->fillRect(0, g_drawY + SCROLL_AREA_Y_FB, FB_WIDTH, H, ST77XX_BLACK);

    // Draw TS then MSG at framebuffer Y = g_drawY + SCROLL_AREA_Y_FB.
    // getTextBounds returns negative tsBox/msgBox y for ascender-using fonts,
    // so we offset the cursor by `- bounds[BOX_Y]` (cf. the big French comment
    // above).
    uint16_t fbY = g_drawY + SCROLL_AREA_Y_FB;
    if (!ts.isEmpty()) {
        g_disp->setFont(NULL);
        g_disp->setTextSize(CONVO_TS_FONT_SIZE);
        g_disp->setTextColor(CONV0_TS_COLOR);
        g_disp->setCursor(tsX - tsBox[BOX_X], fbY - tsBox[BOX_Y]);
        g_disp->print(ts);
        fbY += tsBlockHWithMargin;
    }
    g_disp->setFont(&CONVO_MSG_FONT);
    g_disp->setTextSize(CONVO_MSG_FONT_SIZE);
    g_disp->setTextColor(msgColor);
    g_disp->setCursor(msgX - msgBox[BOX_X], fbY - msgBox[BOX_Y]);
    g_disp->print(msgBuf);

    // Bump scroll-area cursor and the user-visible scroll register.
    g_drawY   = (g_drawY + H) % SCROLL_AREA_H;
    g_scrollY = (g_scrollY + H) % SCROLL_AREA_H;
    hwScrollTo(g_scrollY);
}


// Print `value` at (x, y) wrapping to a second line at (x, y + lineH) if it doesn't fit in `availableWidth`. Both lines start at the same x.
// Returns the total vertical pixels consumed (lineH for a single line, 2*lineH if wrapped).
//
// Uses the default 5×7 font at setTextSize(2) → 12 px advance per glyph. The split prefers the rightmost natural separator
// (':', '.', '-', '_', '/', ' ') that keeps the first half within the line, so the cut feels meaningful (e.g. a MAC splits cleanly on a colon).
// Falls back to a hard cut at maxChars when no separator is present. A leading space on the second half is trimmed so the two lines align on x.
static int printValueWrapped(Adafruit_ST7789* pDisp, const String& value, int x, int y, int availableWidth, int lineH) {
    const int charW = 12;
    int       len   = value.length();
    pDisp->setCursor(x, y);
    if (len * charW <= availableWidth) {
        pDisp->print(value);
        return lineH;
    }
    int maxChars   = availableWidth / charW;
    int upperBound = (maxChars - 1 < len - 1) ? (maxChars - 1) : (len - 1);
    int breakAt    = -1;
    for (int i = upperBound; i > 0; i--) {
        char c = value[i];
        if (c == ':' || c == '.' || c == '-' || c == '_' || c == '/' || c == ' ') {
            breakAt = i + 1;  // first half keeps the separator for visual continuity
            break;
        }
    }
    if (breakAt < 1) {
        breakAt = maxChars;  // hard cut fallback
    }
    String first  = value.substring(0, breakAt);
    String second = value.substring(breakAt);
    while (second.length() > 0 && second[0] == ' ') {
        second.remove(0, 1);
    }
    pDisp->print(first);
    pDisp->setCursor(x, y + lineH);
    pDisp->print(second);
    return 2 * lineH;
}

// Draw one info-screen row: red header at colHeaders, white value at colValues (wrapped to a 2nd line via printValueWrapped if too wide).
// Returns the row height consumed (lineHeight or 2 × lineHeight when the value wrapped) so the caller can advance nextY accordingly.
static int drawInfoRow(Adafruit_ST7789* pDisp, const char* header, const String& value, int colHeaders, int colValues, int nextY, int lineHeight) {
    pDisp->setTextSize(2);
    pDisp->setCursor(colHeaders, nextY);
    pDisp->setTextColor(ST77XX_RED);
    pDisp->print(header);
    pDisp->setTextColor(ST77XX_WHITE);
    return printValueWrapped(pDisp, value, colValues, nextY, FB_WIDTH - colValues, lineHeight);
}

void showUpdatedInfoScreen() {
    if (g_deviceData.screen != DisplayType::ST7789) {
        ESP_LOGW(TAG_MM, "showUpdatedInfoScreen: DISPLAY_TYPE_NOT_CONFIGURED");
        return;
    }

    String mac = getRealHardwareMAC();

    // Diagnostic: trace every entry to this function with the exact state we're about to draw. Lets us tell apart 3 scenarios when the screen
    // shows scrambled data after /status: (1) called multiple times in rapid succession (double-render race), (2) called once with wrong data
    // (state-read bug), (3) called once with correct data (purely a rendering / VRAM-residue issue on the ST7789 side).
    ESP_LOGI(TAG_MM,
             "showUpdatedInfoScreen(): mac=%s ssid=%s ip=%s mqtt=%d ble=%d wifiState=%d",
             mac.c_str(),
             WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(),
             (int)g_mqttClient.connected(),
             (int)g_kb.isFullyConnected(),
             (int)wifiGetState());


    Adafruit_ST7789* pDisp = (Adafruit_ST7789*)g_disp;

    g_inConversationMode = false;  // fullscreen mode, suppress status bar repaint
    hwScrollReset();               // info screen draws at fixed coordinates, scroll must be 0

    pDisp->setFont(NULL);  // font par défaut

    int colHeaders      = 2;
    int colValues       = 66;
    int lineHeight      = 22;
    int separatorHeight = 18;

    int nextY = 0;

    // Top rows: device identity — always shown regardless of WiFi state since these don't depend on the network being up.
    // Combined "ID: <id> <name>" row — replaces the previous separate "ID:" / "Name:" rows so the screen reclaims one row (~22 px) for the rest of
    // the status info. snprintf into a stack buffer keeps the formatting explicit and avoids the 3-alloc String-concat chain.
    char idAndNameBuf[20];  // worst case: "999 PROTO_999\0" = 14 bytes — 20 leaves slack for a longer namePrefix in the future.
    snprintf(idAndNameBuf, sizeof(idAndNameBuf), "%d %s", g_deviceData.deviceId, g_deviceData.name());
    nextY += drawInfoRow(pDisp, "ID:", idAndNameBuf, colHeaders, colValues, nextY, lineHeight);
    nextY += drawInfoRow(pDisp, "Owner:", String(g_deviceData.pseudo), colHeaders, colValues, nextY, lineHeight);
    nextY += drawInfoRow(pDisp, "MAC:", mac, colHeaders, colValues, nextY, lineHeight);

    nextY += separatorHeight;
    nextY += drawInfoRow(pDisp, "BTKB:", g_kb.isFullyConnected() ? "Connected" : "Not found", colHeaders, colValues, nextY, lineHeight);
    nextY += separatorHeight;

    // Branch on the current WiFi state — in PORTAL we replace the SSID/IP/MQTT/TIME rows with config instructions, otherwise we keep the
    // standard runtime info layout. The "Connecting…" / "Lost" variants reuse the SSID/IP rows with placeholder values so the row positions
    // stay stable across transitions (less visual jitter when state changes between two info-screen refreshes).
    WifiState st = wifiGetState();
    if (st == WifiState::PORTAL) {
        drawPortalInstructions(pDisp, nextY, colHeaders, colValues, lineHeight);
    } else {
        String ssidStr = (st == WifiState::CONNECTED) ? WiFi.SSID() : String("(searching)");
        String ipStr;
        if (WiFi.status() == WL_CONNECTED) {
            ipStr = WiFi.localIP().toString();
        } else if (st == WifiState::TRYING_KNOWN) {
            ipStr = "Connecting...";
        } else if (st == WifiState::LOST) {
            ipStr = "Lost, retrying";
        } else {
            ipStr = "Booting...";
        }
        nextY += drawInfoRow(pDisp, "SSID:", ssidStr, colHeaders, colValues, nextY, lineHeight);
        nextY += drawInfoRow(pDisp, "IP:", ipStr, colHeaders, colValues, nextY, lineHeight);
        nextY += separatorHeight;
        nextY += drawInfoRow(pDisp, "MQTT:", g_mqttClient.connected() ? "OK" : "NOT OK", colHeaders, colValues, nextY, lineHeight);
        nextY += drawInfoRow(pDisp, "NTP:", String(getTimezoneLabel()), colHeaders, colValues, nextY, lineHeight);
    }

    nextY += separatorHeight;

    // Heap row — always shown, regardless of WiFi state. The largest contiguous block is the figure that matters for the TLS handshake (compared
    // against MQTT_TLS_MIN_FREE_HEAP_B in mqttReconnectAttempt()); the free figure is the headline number most users expect to see. Format keeps both
    // values on one row to avoid stealing two info-screen lines.
    char heapStr[24];
    snprintf(heapStr, sizeof(heapStr), "%u/%u", (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getFreeHeap());
    nextY += drawInfoRow(pDisp, "HEAP:", String(heapStr), colHeaders, colValues, nextY, lineHeight);
    nextY += drawInfoRow(pDisp, "HELP:", "/help", colHeaders, colValues, nextY, lineHeight);
}
