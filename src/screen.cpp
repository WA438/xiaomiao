/**
 * screen.cpp — XiaoMiaoOS v2.2 display functions
 * ST7735 128x160, TFT_eSPI
 * Clean flat design, refined pixel-art icons
 * Status bar: time left, WiFi/sword/shield/BT/battery right
 */
#include "screen.h"
#include "cnfont.h"

TFT_eSPI tft = TFT_eSPI();

static uint32_t sb_last = 0;
static char     sb_time_buf[8] = "--:--";
static char     sb_weather_buf[12] = "";

void scr_init() {
    Serial.println("[SCR] init start");
    pinMode(TFT_CS, OUTPUT);
    pinMode(TFT_DC, OUTPUT);
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, HIGH);
    delay(10);
    tft.init();
    tft.setRotation(TFT_ROT);
    tft.fillScreen(C_BLACK);
    tft.setSwapBytes(true);
    pinMode(TFT_RST, INPUT_PULLUP);
    Serial.println("[SCR] init done");
}

void tft_restore() {
    // Full restore: needed AFTER SD card SPI operations.
    // SD MISO (GPIO19 = TFT_RST) drives RST low, causing a hardware reset
    // of the ST7735 controller. Full re-init via tft.init() is required.
    // This causes a brief white flash — unavoidable with this hardware design.
    Serial.println("[SCR] tft_restore start");
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, HIGH);
    delay(10);
    tft.init();
    tft.setRotation(TFT_ROT);
    tft.setSwapBytes(true);
    tft.fillScreen(C_BLACK);
    pinMode(TFT_RST, INPUT_PULLUP);
    Serial.println("[SCR] tft_restore done");
}

void tft_soft_restore() {
    // Soft restore: used after WiFi/BLE operations.
    // WiFi/BLE do NOT use SPI MISO, so the TFT is NOT reset.
    // Just ensure RST is HIGH and clear the screen — no white flash!
    // Only SD card operations (which share GPIO19=MISO) need full tft_restore().
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, HIGH);
    delay(2);
    tft.setRotation(TFT_ROT);
    tft.setSwapBytes(true);
    tft.fillScreen(C_BLACK);
    pinMode(TFT_RST, INPUT_PULLUP);
}

void scr_clear_content() {
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
}

void scr_clear_all() {
    tft.fillScreen(C_BLACK);
}

void scr_center(const char* text, int16_t y, uint8_t font, uint16_t fg, uint16_t bg) {
    tft.setTextFont(font);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(fg, bg);
    tft.drawString(text, SCR_W / 2, y);
    tft.setTextDatum(TL_DATUM);
}

void scr_draw_title(const char* title) {
    tft.fillRect(0, CONTENT_Y, SCR_W, 16, C_BLACK);
    cnfont_print(4, CONTENT_Y, title, C_WHITE, C_BLACK);
    tft.drawFastHLine(0, CONTENT_Y + 15, SCR_W, C_DGRAY);
}

void scr_draw_bottom(const char* left, const char* right) {
    tft.fillRect(0, SCR_H - 16, SCR_W, 16, C_BLACK);
    tft.drawFastHLine(0, SCR_H - 16, SCR_W, C_DGRAY);
    if (left) cnfont_print(3, SCR_H - 14, left, C_WHITE, C_BLACK);
    if (right) {
        int w = cnfont_text_width(right);
        cnfont_print(SCR_W - w - 3, SCR_H - 14, right, C_CYAN, C_BLACK);
    }
}

void scr_clip_text(char* dst, const char* src, int max_chars) {
    if (!dst || !src || max_chars < 4) { if (dst) dst[0] = '\0'; return; }
    int len = strlen(src);
    if (len <= max_chars) { strcpy(dst, src); return; }
    strncpy(dst, src, max_chars - 3);
    dst[max_chars - 3] = '.'; dst[max_chars - 3 + 1] = '.';
    dst[max_chars - 3 + 2] = '.';
    dst[max_chars] = '\0';
}

void scr_hline(int16_t y, uint16_t color) {
    tft.drawFastHLine(0, y, SCR_W, color);
}

uint16_t scr_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return tft.color565(r, g, b);
}

// ═══════════════ STATUS BAR ICONS v3 (refined, recognizable pixel art) ═══════════════

// WiFi icon: 3-tier signal bars, 12x9, clear and recognizable
static void sb_draw_wifi(int16_t x, int16_t y, WifiMode mode) {
    uint16_t c;
    if (mode == WM_OFF) c = C_DGRAY;
    else if (mode == WM_AP) c = C_CYAN;
    else c = C_WHITE;
    uint8_t strength = (mode == WM_OFF) ? 0 : 3;

    // Base bar (always visible outline)
    tft.drawRect(x, y + 6, 3, 3, (mode == WM_OFF) ? C_DGRAY : c);
    // Mid bar
    if (strength >= 2) {
        tft.drawRect(x + 4, y + 4, 3, 5, c);
        tft.fillRect(x + 5, y + 5, 1, 3, c);
    } else {
        tft.drawRect(x + 4, y + 4, 3, 5, C_DGRAY);
    }
    // Top bar (tallest)
    if (strength >= 3) {
        tft.drawRect(x + 8, y + 2, 3, 7, c);
        tft.fillRect(x + 9, y + 3, 1, 5, c);
    } else {
        tft.drawRect(x + 8, y + 2, 3, 7, C_DGRAY);
    }
    // Fill base bar if connected
    if (strength >= 1) {
        tft.fillRect(x + 1, y + 7, 1, 1, c);
    }
}

// Sword icon: 8x9, clean crossed sword
static void sb_draw_sword(int16_t x, int16_t y, AtkState st) {
    uint16_t c;
    if (st == ATK_RUNNING) c = C_RED;
    else if (st == ATK_ARMED) c = C_YELLOW;
    else c = C_DGRAY;
    // Blade (diagonal)
    tft.drawLine(x + 1, y, x + 6, y + 5, c);
    tft.drawLine(x + 2, y, x + 7, y + 5, c);
    // Tip
    tft.drawPixel(x, y, c);
    // Crossguard
    tft.drawFastHLine(x + 4, y + 5, 4, c);
    tft.drawPixel(x + 3, y + 5, c);
    // Handle
    tft.drawFastVLine(x + 7, y + 6, 2, c);
    tft.drawPixel(x + 6, y + 6, c);
    tft.drawPixel(x + 6, y + 7, c);
    // Pommel
    tft.drawPixel(x + 7, y + 8, c);
}

// Shield icon: 8x9, bold outline with checkmark
static void sb_draw_shield(int16_t x, int16_t y, AtkState st) {
    uint16_t c, fill_c;
    if (st == ATK_RUNNING) { c = C_CYAN; fill_c = 0x0208; }
    else if (st == ATK_ARMED) { c = C_YELLOW; fill_c = 0x8400; }
    else { c = C_WHITE; fill_c = 0x2104; }

    // Shield outline (bold)
    tft.drawFastHLine(x + 2, y, 4, c);
    tft.drawFastVLine(x, y + 2, 4, c);
    tft.drawFastVLine(x + 7, y + 2, 4, c);
    tft.drawFastHLine(x + 1, y + 1, 1, c);
    tft.drawFastHLine(x + 6, y + 1, 1, c);
    tft.drawFastHLine(x + 1, y + 6, 1, c);
    tft.drawFastHLine(x + 6, y + 6, 1, c);
    tft.drawLine(x + 2, y + 7, x + 3, y + 8, c);
    tft.drawLine(x + 4, y + 8, x + 5, y + 7, c);
    // Inner fill
    tft.fillRect(x + 2, y + 2, 4, 3, fill_c);
    // Active checkmark
    if (st == ATK_RUNNING) {
        tft.drawPixel(x + 2, y + 4, C_WHITE);
        tft.drawPixel(x + 3, y + 5, C_WHITE);
        tft.drawPixel(x + 4, y + 4, C_WHITE);
        tft.drawPixel(x + 5, y + 3, C_WHITE);
    }
}

// Bluetooth icon: 8x9, clean runic B
static void sb_draw_bt(int16_t x, int16_t y, bool on) {
    uint16_t c = on ? C_WHITE : C_DGRAY;
    // Vertical spine
    tft.drawFastVLine(x + 3, y, 9, c);
    // Upper fork
    tft.drawLine(x + 3, y + 1, x + 6, y + 3, c);
    tft.drawLine(x + 3, y + 1, x + 0, y + 3, c);
    // Lower fork
    tft.drawLine(x + 3, y + 5, x + 6, y + 7, c);
    tft.drawLine(x + 3, y + 5, x + 0, y + 7, c);
    // Crosspoints
    tft.drawPixel(x + 4, y + 4, c);
    tft.drawPixel(x + 2, y + 4, c);
}

// Battery icon: 14x9, clear with fill level and bolt
static void sb_draw_battery(int16_t x, int16_t y, int pct, bool charging) {
    uint16_t body_c = C_WHITE;
    // Body outline (rounded corners look)
    tft.drawRect(x, y + 1, 12, 7, body_c);
    // Terminal nub
    tft.fillRect(x + 12, y + 3, 2, 3, body_c);
    // Fill
    int fill_w = (pct * 10) / 100;
    if (fill_w > 0) {
        uint16_t fc = (pct > 20) ? C_GREEN : C_RED;
        tft.fillRect(x + 1, y + 2, fill_w, 5, fc);
    }
    // Segment dividers (only on empty area)
    if (fill_w < 4) tft.drawFastVLine(x + 4, y + 2, 5, C_DGRAY);
    if (fill_w < 8) tft.drawFastVLine(x + 8, y + 2, 5, C_DGRAY);
    // Lightning bolt for charging
    if (charging) {
        tft.drawLine(x + 6, y + 2, x + 4, y + 5, C_YELLOW);
        tft.drawLine(x + 4, y + 5, x + 7, y + 5, C_YELLOW);
        tft.drawLine(x + 7, y + 5, x + 5, y + 7, C_YELLOW);
    }
    // Low battery warning flash
    if (pct <= 15 && !charging) {
        if ((millis() / 500) % 2 == 0) {
            tft.fillRect(x + 1, y + 2, 2, 5, C_RED);
        }
    }
}

// ═══════════════ STATUS BAR v2 (refined icons) ═══════════════
void scr_draw_status_bar() {
    bool time_changed = false;
    char tbuf[8];
    time_t now;
    time(&now);
    struct tm ti_struct;
    localtime_r(&now, &ti_struct);
    struct tm* ti = &ti_struct;
    if (g_cfg.time24h) {
        snprintf(tbuf, 8, "%02d:%02d", ti->tm_hour, ti->tm_min);
    } else {
        int h = ti->tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(tbuf, 8, "%02d:%02d", h, ti->tm_min);
    }
    if (strcmp(tbuf, sb_time_buf) != 0) {
        strcpy(sb_time_buf, tbuf);
        time_changed = true;
    }

    // Extract temperature from weather_buf (only temp for display)
    bool weather_changed = false;
    char wbuf[12];
    wbuf[0] = '\0';
    if (weather_buf[0] != '\0' && strcmp(weather_buf, "N/A") != 0) {
        int i = 0;
        while (i < 10 && weather_buf[i] && weather_buf[i] != ' ') {
            wbuf[i] = weather_buf[i];
            i++;
        }
        wbuf[i] = '\0';
    }
    if (strcmp(wbuf, sb_weather_buf) != 0) {
        strcpy(sb_weather_buf, wbuf);
        weather_changed = true;
    }

    uint32_t ms = millis();
    bool icons_changed = (ms - sb_last >= 500);

    if (icons_changed || time_changed || weather_changed) {
        sb_last = ms;
        tft.fillRect(0, SB_Y, SCR_W, SB_H, C_BLACK);

        // Time
        tft.setTextFont(1);
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.setCursor(3, SB_Y + 3);
        tft.print(tbuf);

        // Latency indicator (compact, shown when connected)
        if (g_wifi_conn && g_net_latency > 0) {
            uint16_t lat_c = (g_net_latency < 100) ? C_GREEN : (g_net_latency < 300 ? C_YELLOW : C_RED);
            tft.setTextColor(lat_c, C_BLACK);
            tft.setCursor(36, SB_Y + 3);
            tft.printf("%ums", (unsigned)g_net_latency);
        }

        // Weather temp only (right-aligned before icons, max 6 chars)
        if (wbuf[0] != '\0') {
            tft.setTextColor(C_CYAN, C_BLACK);
            int wlen = strlen(wbuf);
            int wx = 94 - (wlen * 6);  // right-align to x=94
            if (wx < 62) wx = 62;       // don't overlap latency
            tft.setCursor(wx, SB_Y + 3);
            tft.print(wbuf);
        }

        // Icons right side (repositioned for wider icons)
        // Battery (rightmost, x=145)
        int bat_raw = analogRead(BAT_PIN);
        float bat_v = (bat_raw / 4095.0f) * 3.3f * 2.0f;
        int bat_pct = (int)((bat_v - 3.3f) * 111.1f);
        if (bat_pct < 0) bat_pct = 0;
        if (bat_pct > 100) bat_pct = 100;
        // Simple charging detection: if ADC reading is unstable (charging)
        bool charging = (g_wifi_mode == WM_STA && bat_v > 4.0f);
        sb_draw_battery(144, SB_Y + 2, bat_pct, charging);
        // Bluetooth (x=134)
        sb_draw_bt(134, SB_Y + 2, g_ble_on);
        // Shield (defense, x=124)
        sb_draw_shield(124, SB_Y + 2, g_wifi_def);
        // Sword (attack, x=114)
        sb_draw_sword(114, SB_Y + 2, g_wifi_atk);
        // WiFi (x=99)
        sb_draw_wifi(99, SB_Y + 2, g_wifi_mode);

        // Bottom separator (thin, subtle)
        tft.drawFastHLine(0, SB_Y + SB_H - 1, SCR_W, C_DGRAY);
    }
}

void scr_draw_status_icons() {
    scr_draw_status_bar();
}

void scr_draw_hbar(int16_t x, int16_t y, int16_t w, int16_t h, float pct, uint16_t color, uint16_t bg) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    tft.fillRect(x, y, w, h, bg);
    int16_t fill_w = (int16_t)(w * pct);
    if (fill_w > 0) tft.fillRect(x, y, fill_w, h, color);
    tft.drawRect(x - 1, y - 1, w + 2, h + 2, C_DGRAY);
}

// ═══════════════ LOCK WALLPAPER v2 (minimal noise) ═══════════════
void scr_lock_wallpaper() {
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    // Very subtle gradient hint (minimal, just 5 faint dots)
    for (int i = 0; i < 5; i++) {
        int x = 20 + random(SCR_W - 40);
        int y = CONTENT_Y + 10 + random(CONTENT_H - 20);
        tft.drawPixel(x, y, 0x2104);  // very dark gray, almost invisible
    }
}

// ═══════════════ SHIELD ICON (24x22, centered, refined) ═══════════════
void scr_draw_shield(int16_t cx, int16_t cy) {
    uint16_t c = C_CYAN;
    uint16_t fill_c = 0x0208;  // dark cyan fill

    // Top triangle (filled)
    for (int y = 0; y < 7; y++) {
        int w = 3 + y * 2;
        tft.drawFastHLine(cx - w, cy - 10 + y, w * 2 + 1, c);
    }
    // Body (filled)
    tft.fillRect(cx - 11, cy - 3, 23, 10, c);
    // Bottom point
    for (int y = 0; y < 5; y++) {
        int w = 11 - y * 2;
        tft.drawFastHLine(cx - w, cy + 7 + y, w * 2 + 1, c);
    }
    // Inner darker fill for depth
    tft.fillRect(cx - 9, cy - 1, 19, 8, fill_c);
    for (int y = 0; y < 5; y++) {
        int w = 9 - y * 2;
        if (w > 0) tft.drawFastHLine(cx - w, cy + 7 + y, w * 2 + 1, fill_c);
    }
    // Lock body (black cutout)
    tft.fillRect(cx - 3, cy + 1, 7, 5, C_BLACK);
    tft.fillRect(cx - 1, cy - 1, 3, 2, C_BLACK);
    // Lock shackle (white, smoother)
    tft.drawLine(cx - 1, cy - 2, cx - 2, cy - 1, C_WHITE);
    tft.drawLine(cx + 1, cy - 2, cx + 2, cy - 1, C_WHITE);
    // Keyhole
    tft.drawPixel(cx, cy + 3, C_YELLOW);
    tft.drawPixel(cx, cy + 4, C_YELLOW);
}
