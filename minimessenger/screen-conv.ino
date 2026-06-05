// ================================================================================
// screen-conv.ino — Conversation scroll area + two-column /help printers
// ================================================================================
//
// Owns every pixel drawn into the conversation scroll area between the top status bar and the bottom input footer: incoming/outgoing message blocks
// (with their timestamp / pseudo prefix / alignment + clustering), the "Ready" / "Lost server" system banners, and the cmd-listing rows printed by
// /help. Also owns the HW-scroll bookkeeping that lets new messages scroll in without a full repaint.



// ================================================================================
// Librairies
// ================================================================================
// Cf howto_fond pour la bascule vers une font buildée pour les accents
//#include <Fonts/FreeSans9pt8b.h>  // Police SANS accents 9x7 au lieu de 7x5 de la font par defaut
#include "fonts/FreeSans10pt8b_latin1.h"  // Police AVEC accents (et 9x7 au lieu de 7x5)
#include "fonts/FreeSans9pt8b_latin1.h"   // Police AVEC accents (et 9x7 au lieu de 7x5)
// A "null" police == Glcdfont, une police bitmap 5x7 pixels fixe, définie dans glcdfont.c., et non accessible à travers une variable

// To switch font size or range (7b/8b), change the #include above + #define below


// ================================================================================
// Constants
// ================================================================================

// Remote pseudo & Timestamp lines
// -------------------------------
#define CONVO_TS_FONT_REF      nullptr
#define CONVO_TS_FONT_SIZE     1
#define CONVO_TS_COLOR         ST77XX_CYAN
#define CONVO_TS_MARGIN_BOTTOM 3  // avec font par defaut: 3

// When two messages from the SAME author land within this many seconds, suppress the second one's timestamp to declutter the conversation view.
// "Same author" is determined by senderDeviceId (see g_lastMsgSenderId in minimessenger.ino): a sender change bypasses the clustering window and
// always shows the timestamp, so the conversation visually breathes between speakers even if their messages arrive back-to-back. The "last visible
// timestamp" tracking continues until either (a) a message arrives outside the window OR (b) a different sender shows up, at which point the
// timestamp is shown again.
#define CONVO_TS_HIDE_THRESHOLD_S 10


// Effective user messages lines
// ------------------------------
#define CONVO_CMD_FONT_REF FreeSans9pt8b
#define CONVO_CMD_COLOR    0xFB56  // Hot pink (RGB565 ≈ #FF69B4).
#define CONVO_INFO_COLOR   ST77XX_GREEN
#define CONVO_ERROR_COLOR  ST77XX_RED


// Command lines
// --------------
#define CONVO_CMD_RIGHT_COL_X    90
#define CONVO_MSG_FONT_REF       FreeSans10pt8b
#define CONVO_MSG_FONT_SIZE      1  // 2 est vraiment trop énorme avec la font FreeSans9pt8b
#define CONVO_MSG_MYSELF_COLOR   ST77XX_WHITE
#define CONVO_MSG_OTHERS_COLOR   ST77XX_YELLOW
#define CONVO_MSG_MARGIN_BOTTOM  7  //
#define CONVO_HELP_MARGIN_BOTTOM 4  //


// ================================================================================
// Code
// ================================================================================

void setupFontTests() {
    // ==== Font default
    // lineAdvance : 8
    // Bounds for text [jjjjj]: x1=0, y1=0, w=30, h=8
    // Bounds for text [Abefg]: x1=0, y1=0, w=30, h=8
    // Bounds for text [     ]: x1=0, y1=0, w=30, h=8
    // Bounds for text [_____]: x1=0, y1=0, w=30, h=8
    // ==== Font FreeSans9pt8b
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
    String         fontNames[] = { "default", "FreeSans9pt8b" };
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


// Replay every entry of the ring buffer through the same HW-scroll draw
// algorithm as addConversationBlockImpl. After this:
//   - status bar and footer are untouched (protected by VSCRDEF — we only
//     fillRect the scroll area, never fillScreen)
//   - the latest block ends up at the bottom of the scroll area with older
//     blocks stacked above (or scrolled off the top if cumulated height
//     exceeds SCROLL_AREA_H) — visually identical to live message arrival
//   - g_drawY / g_scrollY are left in the "next slot" position so a new
//     addConversationBlockImpl right after lands naturally below the last block
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

        // Wrap avoidance, mirroring addConversationBlockImpl: if the block would
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
        g_disp->setFont(&CONVO_MSG_FONT_REF);
        g_disp->setTextSize(line.msgFontSize);
        g_disp->setTextColor(line.msgColor);
        g_disp->setCursor(line.msgX - line.msgBounds[BOX_X], fbY - line.msgBounds[BOX_Y]);
        g_disp->print(line.msg);

        g_drawY   = (g_drawY + H) % SCROLL_AREA_H;
        g_scrollY = (g_scrollY + H) % SCROLL_AREA_H;
        hwScrollTo(g_scrollY);
    }
}

// Internal one-line painter for the conversation scroll area. Not exposed in minimessenger.ino on purpose — outside callers go through the
// printCmdInfo / printCmdError / printInfoLineNumber wrappers below.
//   - `right` filled → `left` at x=2, `right` at x=CONVO_CMD_RIGHT_COL_X. Used for /help listings (cmd name + description on the same row).
//
// HW-scroll primitives (g_drawY / g_scrollY / hwScrollTo) make successive calls accumulate visually like any other conversation content, and
// hwScrollReset() drops the lot during a screen switch.

void printLineLowLevelImpl(const String& left, const String& right, uint16_t color, const GFXfont* font = &CONVO_CMD_FONT_REF) {
    if (g_deviceData.screen != DisplayType::ST7789) {
        return;
    }

    // UTF-8 → Latin-1: bundled FreeSans*_latin1 fonts cap at 0xFF. Mirrors what addConversationBlockImpl does to its message before drawing.
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

    // Wrap avoidance, mirrors addConversationBlockImpl: if the line would overflow the scroll area bottom, paint the leftover tail black and
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
        g_disp->setCursor(CONVO_CMD_RIGHT_COL_X, g_drawY + SCROLL_AREA_Y_FB - by);
        g_disp->print(rightBuf);
    }

    // Bump the HW scroll — this is what makes successive calls accumulate visually.
    g_drawY   = (g_drawY + lineH) % SCROLL_AREA_H;
    g_scrollY = (g_scrollY + lineH) % SCROLL_AREA_H;
    hwScrollTo(g_scrollY);
}


// `ts` stays by value because it's mutated locally (cleared on cluster-suppress, prefixed with the sender's pseudo); `msg` is read-only — just
// copied into msgBuf via strncpy — so const-ref to avoid the per-call heap alloc the by-value copy would trigger on ESP32's String.
void addConversationBlockImpl(String ts, const String& msg, uint16_t msgColor, Align align, byte senderDeviceId = DEVICE_ID_UNSET) {
    // Function-local static buffers.
    static time_t gs_lastShownTsEpoch = 0;
    static byte   g_lastMsgSenderId   = DEVICE_ID_UNSET;


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
        const bool   insideTimeWindow = (now - gs_lastShownTsEpoch < CONVO_TS_HIDE_THRESHOLD_S);

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
        gs_lastShownTsEpoch = now;
        g_lastMsgSenderId   = senderDeviceId;
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
        g_disp->setFont(CONVO_TS_FONT_REF);
        g_disp->setTextSize(CONVO_TS_FONT_SIZE);
        g_disp->getTextBounds(ts, 0, 0, &tsBox[BOX_X], &tsBox[BOX_Y], (uint16_t*)&tsBox[BOX_W], (uint16_t*)&tsBox[BOX_H]);

        tsBlockHWithMargin = tsBox[BOX_H] + CONVO_TS_MARGIN_BOTTOM;
    }

    // Dimension msg
    uint16_t       msgBlockHWithMargin = 0;
    static int16_t msgBox[4]           = { 0, 0, 0, 0 };

    g_disp->setFont(&CONVO_MSG_FONT_REF);
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
                               CONVO_TS_COLOR,
                               NULL,
                               CONVO_TS_FONT_SIZE,
                               tsBlockHWithMargin,
                               tsX,
                               tsBox,
                               msgBuf,
                               msgColor,
                               &CONVO_MSG_FONT_REF,
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
        g_disp->setTextColor(CONVO_TS_COLOR);
        g_disp->setCursor(tsX - tsBox[BOX_X], fbY - tsBox[BOX_Y]);
        g_disp->print(ts);
        fbY += tsBlockHWithMargin;
    }
    g_disp->setFont(&CONVO_MSG_FONT_REF);
    g_disp->setTextSize(CONVO_MSG_FONT_SIZE);
    g_disp->setTextColor(msgColor);
    g_disp->setCursor(msgX - msgBox[BOX_X], fbY - msgBox[BOX_Y]);
    g_disp->print(msgBuf);

    // Bump scroll-area cursor and the user-visible scroll register.
    g_drawY   = (g_drawY + H) % SCROLL_AREA_H;
    g_scrollY = (g_scrollY + H) % SCROLL_AREA_H;
    hwScrollTo(g_scrollY);
}


void printCmdInfo(const String& message) {
    printLineLowLevelImpl(message, String(), CONVO_CMD_COLOR);
}
void printCmdInfo(const String& left, const String& right) {
    printLineLowLevelImpl(left, right, CONVO_CMD_COLOR);
}

void printCmdError(const String& message) {
    printLineLowLevelImpl(message, String(), CONVO_ERROR_COLOR);
}


void printGeneralInfo(const String& message) {
    addConversationBlockImpl(String(), message, CONVO_INFO_COLOR, CENTER);
}

void printGeneralError(const String& message) {
    addConversationBlockImpl(String(), message, CONVO_ERROR_COLOR, CENTER);
}


void addConversationOtherBlock(String ts, const String& message, byte senderDeviceId) {
    addConversationBlockImpl(ts, message, CONVO_MSG_OTHERS_COLOR, LEFT, senderDeviceId);
}

void addConversationMeOKBlock(String ts, const String& message) {
    addConversationBlockImpl(ts, message, CONVO_MSG_MYSELF_COLOR, RIGHT, g_deviceData.deviceId);
}

void addConversationMeErrorBlock(String ts, const String& message) {
    addConversationBlockImpl(ts, "[ERROR] " + message, CONVO_ERROR_COLOR, RIGHT, g_deviceData.deviceId);
}
