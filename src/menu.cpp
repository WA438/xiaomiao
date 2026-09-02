#include "menu.h"
#include "screen.h"
#include "buttons.h"
#include "buzzer.h"
#include "cnfont.h"

#define ITEM_H 16
#define MAX_VIS 6

static const MenuItem main_items[] = {
    {"= 侦察 =",         PG_MENU,       true},
    {"  WiFi扫描",       PG_RECON_WIFI, false},
    {"  蓝牙扫描",        PG_RECON_BLE,  false},
    {"  战驾扫描",        PG_RECON_WARD, false},
    {"= 攻击 =",          PG_MENU,       true},
    {"  断连攻击",          PG_ATTK_DEAUTH, false},
    {"  信标洪泛",     PG_ATTK_BEACON, false},
    {"  邪恶门户",     PG_ATTK_PORTAL, false},
    {"  蓝牙洪泛",        PG_ATTK_BLE,   false},
    {"  蓝牙坏键盘",      PG_ATTK_BADUSB, false},
    {"  防御护盾",         PG_ATTK_DEFENSE, false},
    {"  TCP洪泛",       PG_ATTK_TCPFLOOD, false},
    {"= 网络 =",       PG_MENU,       true},
    {"  主机扫描",       PG_NET_HOST,   false},
    {"  TCP探测",       PG_NET_TCP,    false},
    {"  流量监控",     PG_NET_TRAFFIC, false},
    {"  天气预报",      PG_WEATHER,    false},
    {"= 日志 =",          PG_MENU,       true},
    {"  凭证文件",    PG_EXPLOIT,    false},
    {"= 系统 =",        PG_MENU,       true},
    {"  SD文件",        PG_SYS_FILES,  false},
    {"  网页管理",           PG_SYS_WEBUI,  false},
    {"  终端",        PG_SYS_TERM,   false},
    {"  时间设置",      PG_SYS_TIME,   false},
    {"  亮度",      PG_SYS_BRIGHT, false},
    {"  蜂鸣",          PG_SYS_BUZZER, false},
    {"  系统更新",    PG_SYS_UPDATE, false},
    {"  应用中心",      PG_SYS_APPCENTER, false},
    {"  重启",          PG_SYS_REBOOT, false},
};

static int item_count = sizeof(main_items) / sizeof(main_items[0]);
static int sel_idx = 1;
static int scroll = 0;
static bool need_redraw = true;

// Category color map
static uint16_t cat_color(const char* label) {
    if (strstr(label, "侦察"))   return C_CYAN;
    if (strstr(label, "攻击"))  return C_RED;
    if (strstr(label, "网络")) return C_YELLOW;
    if (strstr(label, "日志"))    return C_MAGENTA;
    if (strstr(label, "系统"))  return C_WHITE;
    return C_WHITE;
}

void menu_init() {
    sel_idx = 1;
    scroll = 0;
    need_redraw = true;
}

void menu_draw() {
    if (!need_redraw) return;
    
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    
    // ═══ Clean title ═══
    cnfont_print_centered(CONTENT_Y, "小喵系统", C_WHITE, C_BLACK);
    tft.drawFastHLine(0, CONTENT_Y + 16, SCR_W, C_DGRAY);
    
    int start = scroll;
    int end = start + MAX_VIS;
    if (end > item_count) end = item_count;
    
    for (int i = start; i < end; i++) {
        int y = CONTENT_Y + 18 + (i - start) * ITEM_H;
        bool sel = (i == sel_idx);
        bool cat = main_items[i].is_cat;
        uint16_t cc = cat_color(main_items[i].label);
        
        if (cat) {
            // Category header: colored accent bar + text
            tft.fillRect(0, y, 3, ITEM_H, cc);
            cnfont_print(7, y, main_items[i].label, cc, C_BLACK);
        } else if (sel) {
            // Selected item: subtle highlight, colored arrow
            tft.fillRect(0, y, SCR_W, ITEM_H, C_DGRAY);
            tft.setTextFont(1);
            tft.setTextColor(cc, C_DGRAY);
            tft.setCursor(8, y + 4);
            tft.print(">");
            cnfont_print(14, y, main_items[i].label + 2, C_WHITE, C_DGRAY);
        } else {
            cnfont_print(14, y, main_items[i].label + 2, C_GRAY, C_BLACK);
        }
    }
    
    // ═══ Scrollbar ═══
    if (item_count > MAX_VIS) {
        int sb_y = CONTENT_Y + 18;
        int sb_h = MAX_VIS * ITEM_H;
        int thumb_h = (sb_h * MAX_VIS) / item_count;
        if (thumb_h < 4) thumb_h = 4;
        int thumb_y = sb_y + (scroll * (sb_h - thumb_h)) / (item_count - MAX_VIS);
        tft.drawRect(SCR_W - 4, sb_y, 3, sb_h, C_DGRAY);
        tft.fillRect(SCR_W - 3, thumb_y, 2, thumb_h, C_CYAN);
    }
    
    // ═══ Bottom bar ═══
    scr_draw_bottom("A:确认", "B:返回");
    
    need_redraw = false;
}

Page menu_update() {
    if (need_redraw) menu_draw();
    
    ButtonEvent up = buttons_get_event(BTN_ID_UP);
    ButtonEvent dn = buttons_get_event(BTN_ID_DOWN);
    ButtonEvent a = buttons_get_event(BTN_ID_A);
    ButtonEvent b = buttons_get_event(BTN_ID_B);
    
    if (up == BTN_EVENT_PRESS || up == BTN_EVENT_REPEAT) {
        int new_sel = sel_idx;
        do {
            new_sel--;
            if (new_sel < 0) new_sel = 0;
        } while (main_items[new_sel].is_cat && new_sel > 0);
        
        if (new_sel != sel_idx) {
            sel_idx = new_sel;
            if (sel_idx < scroll) scroll = sel_idx;
            need_redraw = true;
        }
    }
    
    if (dn == BTN_EVENT_PRESS || dn == BTN_EVENT_REPEAT) {
        int new_sel = sel_idx;
        do {
            new_sel++;
            if (new_sel >= item_count) new_sel = item_count - 1;
        } while (main_items[new_sel].is_cat && new_sel < item_count - 1);
        
        if (new_sel != sel_idx) {
            sel_idx = new_sel;
            if (sel_idx >= scroll + MAX_VIS) scroll = sel_idx - MAX_VIS + 1;
            need_redraw = true;
        }
    }
    
    if (a == BTN_EVENT_PRESS) {
        if (!main_items[sel_idx].is_cat) {
            return main_items[sel_idx].target;
        }
    }
    
    if (b == BTN_EVENT_PRESS) {
        return PG_MENU;  // B stays in menu, lock screen only via timeout
    }
    
    return PG_MENU;
}

void menu_set_page(Page p) {
    g_prev = g_page;
    g_page = p;
    if (p == PG_MENU) need_redraw = true;
}

void menu_back() {
    g_page = g_prev;
    g_prev = PG_MENU;
    if (g_page == PG_MENU) need_redraw = true;
}

Page menu_current() {
    return g_page;
}

void menu_draw_list(const char** items, int count, int sel, const char* title) {
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title(title);
    
    int max_v = (CONTENT_H - 18) / ITEM_H;
    int start = 0;
    if (sel >= max_v) start = sel - max_v + 1;
    int end = start + max_v;
    if (end > count) end = count;
    
    for (int i = start; i < end; i++) {
        int y = CONTENT_Y + 18 + (i - start) * ITEM_H;
        if (i == sel) {
            tft.fillRect(0, y, SCR_W, ITEM_H, C_DGRAY);
            tft.fillRect(0, y, 3, ITEM_H, C_CYAN);
            tft.setTextFont(1);
            tft.setTextColor(C_WHITE, C_DGRAY);
            tft.setCursor(8, y + 4);
            tft.print(">");
            cnfont_print(14, y, items[i], C_WHITE, C_DGRAY);
        } else {
            cnfont_print(14, y, items[i], C_GRAY, C_BLACK);
        }
    }
    
    scr_draw_bottom("A:确认", "B:返回");
}