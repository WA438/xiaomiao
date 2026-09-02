/**
 * cnfont.h — Chinese font rendering for XiaoMiaoOS
 * 16x16 bitmap font with mixed ASCII/CN support
 * Uses PROGMEM bitmap data generated from Noto Sans CJK
 */
#ifndef CNFONT_H
#define CNFONT_H

#include <Arduino.h>
#include "config.h"  // for TFT colors and tft

// ── Initialize Chinese font system ──
void cnfont_init();

// ── Draw a single 16x16 Chinese character at (x, y) ──
// Returns width: 16 for Chinese, 0 if not found
int cnfont_draw_char(int x, int y, uint16_t ch, uint16_t fg, uint16_t bg);

// ── Print a UTF-8 string with mixed CN/ASCII at (x, y) ──
// Chinese chars: 16x16, ASCII chars: 6x8 (font 1)
// Returns total width drawn
int cnfont_print(int x, int y, const char* str, uint16_t fg, uint16_t bg);

// ── Print centered text ──
void cnfont_print_centered(int y, const char* str, uint16_t fg, uint16_t bg);

// ── Check if a character is in the font ──
bool cnfont_has_char(uint16_t codepoint);

// ── Get text width (CN=16px, ASCII=6px per char) ──
int cnfont_text_width(const char* str);

#endif // CNFONT_H
