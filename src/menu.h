#ifndef MENU_H
#define MENU_H
#include "config.h"

void menu_init();
void menu_draw();
Page menu_update();
void menu_set_page(Page p);
Page menu_current();
void menu_draw_list(const char** items, int count, int sel, const char* title);
void menu_back();
#endif