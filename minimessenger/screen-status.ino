// ================================================================================
// screen-status.ino — Full-screen info / status overlay (shown by /status and on boot)
// ================================================================================

// Owns the fixed-coordinate info screen layout: a stack of "label: value" rows showing device identity (ID / Owner / MAC), BT keyboard state, WiFi
// state (SSID / IP or portal instructions), MQTT, NTP, free heap, and a help hint. Drawn over the full panel (no status bar / no footer), used at
// boot until MQTT first connects and on demand via /status (which schedules a return to conversation mode after a few seconds).

#define STATUS_FONT_REF  FONT_DEFAULT_07_0PX
#define STATUS_FONT_SIZE 2

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
static int drawStatusRow(Adafruit_ST7789* pDisp, const char* header, const String& value, int colHeaders, int colValues, int nextY, int lineHeight) {
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
             "showUpdatedInfoScreen(): mac=%s ssid=%s ip=%s mqtt=%d ble=%d "
             "wifiState=%d",
             mac.c_str(),
             WiFi.SSID().c_str(),
             WiFi.localIP().toString().c_str(),
             (int)g_mqttClient.connected(),
             (int)g_kb.isFullyConnected(),
             (int)wifiGetState());

    Adafruit_ST7789* pDisp = (Adafruit_ST7789*)g_disp;

    g_inConversationMode = false;  // fullscreen mode, suppress status bar repaint
    hwScrollReset();               // info screen draws at fixed coordinates, scroll must be 0

    pDisp->setFont(STATUS_FONT_REF);
    pDisp->setTextSize(STATUS_FONT_SIZE);

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
    nextY += drawStatusRow(pDisp, "ID:", idAndNameBuf, colHeaders, colValues, nextY, lineHeight);
    nextY += drawStatusRow(pDisp, "Owner:", String(g_deviceData.pseudo), colHeaders, colValues, nextY, lineHeight);
    nextY += drawStatusRow(pDisp, "MAC:", mac, colHeaders, colValues, nextY, lineHeight);

    nextY += separatorHeight;
    nextY += drawStatusRow(pDisp, "BTKB:", g_kb.isFullyConnected() ? "Connected" : "Not found", colHeaders, colValues, nextY, lineHeight);
    nextY += separatorHeight;

    // Branch on the current WiFi state — in WIFI_PORTAL we replace the SSID/IP/MQTT/TIME rows with config instructions, otherwise we keep the
    // standard runtime info layout. The "Connecting…" / "Lost" variants reuse the SSID/IP rows with placeholder values so the row positions
    // stay stable across transitions (less visual jitter when state changes between two info-screen refreshes).
    WifiState st = wifiGetState();
    if (st == WifiState::WIFI_PORTAL) {
        drawPortalInstructions(pDisp, nextY, colHeaders, colValues, lineHeight);
    } else {
        String ssidStr = (st == WifiState::WIFI_CONNECTED) ? WiFi.SSID() : String("(searching)");
        String ipStr;
        if (WiFi.status() == WL_CONNECTED) {
            ipStr = WiFi.localIP().toString();
        } else if (st == WifiState::WIFI_TRYING_KNOWN) {
            ipStr = "Connecting...";
        } else if (st == WifiState::WIFI_LOST) {
            ipStr = "Lost, retrying";
        } else {
            ipStr = "Booting...";
        }
        nextY += drawStatusRow(pDisp, "SSID:", ssidStr, colHeaders, colValues, nextY, lineHeight);
        nextY += drawStatusRow(pDisp, "IP:", ipStr, colHeaders, colValues, nextY, lineHeight);
        nextY += separatorHeight;
        nextY += drawStatusRow(pDisp, "MQTT:", g_mqttClient.connected() ? "OK" : "NOT OK", colHeaders, colValues, nextY, lineHeight);
        nextY += drawStatusRow(pDisp, "NTP:", String(getTimezoneLabel()), colHeaders, colValues, nextY, lineHeight);
    }

    nextY += separatorHeight;

    // Heap row — always shown, regardless of WiFi state. The largest contiguous block is the figure that matters for the TLS handshake (compared
    // against MQTT_TLS_MIN_FREE_HEAP_B in mqttReconnectAttempt()); the free figure is the headline number most users expect to see. Format keeps both
    // values on one row to avoid stealing two info-screen lines.
    char heapStr[24];
    snprintf(heapStr, sizeof(heapStr), "%u/%u", (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getFreeHeap());
    nextY += drawStatusRow(pDisp, "HEAP:", String(heapStr), colHeaders, colValues, nextY, lineHeight);
    nextY += drawStatusRow(pDisp, "HELP:", "/help", colHeaders, colValues, nextY, lineHeight);
}
