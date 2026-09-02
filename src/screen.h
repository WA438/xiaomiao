#ifndef SCREEN_H
#define SCREEN_H
#include "config.h"
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

void scr_init();
void scr_clear_content();
void scr_clear_all();
void scr_center(const char* text, int16_t y, uint8_t font, uint16_t fg, uint16_t bg);
void scr_draw_status_bar();
void scr_draw_status_icons();
void scr_draw_title(const char* title);
void scr_draw_bottom(const char* left, const char* right);
void scr_clip_text(char* dst, const char* src, int max_chars);
void scr_hline(int16_t y, uint16_t color);
uint16_t scr_rgb(uint8_t r, uint8_t g, uint8_t b);
void scr_draw_hbar(int16_t x, int16_t y, int16_t w, int16_t h, float pct, uint16_t color, uint16_t bg);
void scr_lock_wallpaper();
void scr_draw_shield(int16_t cx, int16_t cy);
void tft_restore();
void tft_soft_restore();
#endif