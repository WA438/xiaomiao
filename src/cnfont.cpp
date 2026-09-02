/**
 * cnfont.cpp — Chinese font rendering for XiaoMiaoOS
 * Renders 16x16 Chinese characters from PROGMEM bitmap data
 * Mixed CN/ASCII text support for ST7735 + TFT_eSPI
 */
#include "cnfont.h"
#include "cnfont_data.h"
#include "screen.h"  // for tft (extern TFT_eSPI)

// ── Binary search for character in sorted codepoint array ──
static int cnfont_find(uint16_t codepoint) {
    int lo = 0, hi = CN_FONT_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t cp = pgm_read_word(&cn_codepoints[mid]);
        if (cp == codepoint) return mid;
        if (cp < codepoint) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;  // not found
}

void cnfont_init() {
    // Nothing to init — data is in PROGMEM
    Serial.printf("[CNFONT] %d chars loaded, %d bytes\n", CN_FONT_COUNT, CN_FONT_COUNT * 32);
}

bool cnfont_has_char(uint16_t codepoint) {
    return cnfont_find(codepoint) >= 0;
}

int cnfont_draw_char(int x, int y, uint16_t codepoint, uint16_t fg, uint16_t bg) {
    int idx = cnfont_find(codepoint);
    if (idx < 0) return 0;

    // Read bitmap from PROGMEM and draw pixel by pixel
    // 16x16 = 16 rows, 2 bytes per row (16 bits)
    for (int row = 0; row < 16; row++) {
        uint8_t hi = pgm_read_byte(&cn_bitmaps[idx][row * 2]);
        uint8_t lo = pgm_read_byte(&cn_bitmaps[idx][row * 2 + 1]);

        for (int col = 0; col < 8; col++) {
            uint16_t color = (hi & (0x80 >> col)) ? fg : bg;
            tft.drawPixel(x + col, y + row, color);
        }
        for (int col = 0; col < 8; col++) {
            uint16_t color = (lo & (0x80 >> col)) ? fg : bg;
            tft.drawPixel(x + 8 + col, y + row, color);
        }
    }
    return 16;
}

int cnfont_print(int x, int y, const char* str, uint16_t fg, uint16_t bg) {
    if (!str) return 0;
    int cx = x;
    int i = 0;
    while (str[i]) {
        uint8_t c = (uint8_t)str[i];
        
        // Check for UTF-8 multi-byte sequence (Chinese)
        if (c >= 0xE0 && c <= 0xEF) {
            // 3-byte UTF-8 (Chinese characters are in this range)
            if (str[i+1] && str[i+2]) {
                uint16_t codepoint = ((c & 0x0F) << 12) | 
                                     ((uint8_t)(str[i+1] & 0x3F) << 6) | 
                                     ((uint8_t)(str[i+2] & 0x3F));
                
                if (cnfont_has_char(codepoint)) {
                    cnfont_draw_char(cx, y, codepoint, fg, bg);
                    cx += 16;
                    i += 3;
                    continue;
                } else {
                    // Character not in font — draw as space
                    tft.fillRect(cx, y, 16, 16, bg);
                    cx += 16;
                    i += 3;
                    continue;
                }
            }
        }
        
        // ASCII character — use TFT_eSPI's built-in font
        // Set position and draw
        tft.drawChar(cx, y, c, fg, bg, 1);  // font size 1
        cx += 6;  // ASCII char width at size 1
        i++;
    }
    return cx - x;
}

void cnfont_print_centered(int y, const char* str, uint16_t fg, uint16_t bg) {
    if (!str) return;
    int w = cnfont_text_width(str);
    int x = (SCR_W - w) / 2;
    if (x < 0) x = 0;
    cnfont_print(x, y, str, fg, bg);
}

int cnfont_text_width(const char* str) {
    if (!str) return 0;
    int w = 0;
    int i = 0;
    while (str[i]) {
        uint8_t c = (uint8_t)str[i];
        if (c >= 0xE0 && c <= 0xEF) {
            // Chinese char = 16px
            w += 16;
            i += 3;
        } else {
            // ASCII = 6px
            w += 6;
            i++;
        }
    }
    return w;
}
