/**
 * @file User_Setup.h
 * @brief TFT_eSPI 配置文件 — 学而思小喵 ESP32 掌机
 * 
 * 适配 justcallmekoko/TFT_eSPI 分支
 * 屏幕: ST7735 128x160 SPI (rotation 1 -> 逻辑 160x128 横屏)
 * 引脚来源: wener.me 实测 + MeowBit v1 逆向
 *   TFT FPC 14Pin: CS=5, DC=4, RST=19, SCK=18, MOSI=23
 *   背光: 无独立 GPIO 控制 (LEDA/LEDK 直连)
 * 
 * 如果屏幕颜色异常(偏红/偏蓝), 尝试切换:
 *   ST7735_BLACKTAB <-> ST7735_GREENTAB <-> ST7735_REDTAB
 */

#ifndef USER_SETUP_H
#define USER_SETUP_H

// ── 驱动选择 ──
#define ST7735_DRIVER      // ST7735 驱动芯片
#define TFT_WIDTH  128     // 物理宽度 (竖屏)
#define TFT_HEIGHT 160     // 物理高度 (竖屏)
// 代码中 setRotation(1) 后逻辑尺寸为 160x128

// ── 颜色顺序 (ST7735 标签颜色决定初始化序列) ──
// 小喵掌机屏幕: 先试 BLACKTAB, 如果颜色不对(偏红/偏蓝)改为 GREENTAB
// 如果屏幕完全不显示, 尝试注释所有 TAB 定义, 只用 INITR_MINI160x80
#define ST7735_BLACKTAB    // 黑色标签 (最常见) — 不行改 GREENTAB

// ── 引脚定义 (wener.me 实测) ──
#define TFT_CS   5
#define TFT_DC   4
#define TFT_RST  19
#define TFT_BL   -1        // 无独立背光 GPIO

// ── SPI 引脚显式定义 (ESP32 VSPI 默认) ──
// 必须显式定义, 否则某些 TFT_eSPI 版本可能选错引脚
#define TFT_MOSI 23
#define TFT_MISO 19
#define TFT_SCLK 18

// ── SPI 频率 ──
// ST7735 最大稳定频率为 27MHz (TFT_eSPI 官方文档)
// 超过 27MHz 可能出现花屏、白屏、杂散像素
#define SPI_FREQUENCY  27000000
#define SPI_READ_FREQUENCY 20000000
#define SPI_TOUCH_FREQUENCY  2500000

// ── 字体 ──
#define LOAD_GLCD        // 字体 1: 标准 Adafruit GLCD 字体
#define LOAD_FONT2       // 字体 2: 小号 (16px)
#define LOAD_FONT4       // 字体 4: 中号 (26px)
#define LOAD_FONT6       // 字体 6: 大号 (48px)
#define LOAD_FONT7       // 字体 7: 7段数码管 (48px)
#define LOAD_FONT8       // 字体 8: 大号 (75px)
#define LOAD_GFXFF       // FreeFonts

// ── 平滑字体 ──
#define SMOOTH_FONT

#endif // USER_SETUP_H