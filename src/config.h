/**
 * config.h — XiaoMiaoOS Marauder-style firmware
 * Hardware: ESP32, ST7735 128x160, 6-btn, SD SPI, Buzzer GPIO14
 * Style: Bruce/Marauder hacker terminal, all English
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ═══════════════ VERSION ═══════════════
#define FW_NAME    "XiaoMiaoOS"
#define FW_VERSION "2.3.0"
#define FW_AUTHOR  "XiaoMiao"

// ═══════════════ SCREEN ═══════════════
#define TFT_CS   5
#define TFT_DC   4
#define TFT_RST  19
#define TFT_BL   -1

#define SCR_W  160
#define SCR_H  128
#define TFT_ROT 3

// Status bar
#define SB_H     14
#define SB_Y     0
#define CONTENT_Y (SB_H)
#define CONTENT_H (SCR_H - SB_H)
#define ITEM_H 10

// ═══════════════ SD CARD ═══════════════
#define SD_CS   22
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK  18

#define SD_BG_DIR    "/bg"
#define SD_LOGS_DIR  "/logs"
#define SD_PCAP_DIR  "/pcap"
#define SD_HTML_DIR  "/web"
#define SD_PORTAL_DIR "/portal"

// ═══════════════ BUTTONS ═══════════════
#define BTN_UP    2
#define BTN_DOWN  13
#define BTN_LEFT  27
#define BTN_RIGHT 35
#define BTN_A     34
#define BTN_B     12

#define BTN_DEBOUNCE     30
#define BTN_DEBOUNCE_MS  30
#define BTN_LONG         2000
#define BTN_LONG_PRESS_MS 2000
#define BTN_REPEAT_D     300
#define BTN_REPEAT_DELAY 300
#define BTN_REPEAT_R     80
#define BTN_REPEAT_RATE  80

// ═══════════════ BUZZER ═══════════════
#define BZR_PIN        14
#define BZR_CH         0
#define BUZZER_PIN     14
#define BUZZER_CHANNEL 0
#define BUZZER_FREQ    2000
#define BUZZER_RESOLUTION 8

// ═══════════════ I2C / SENSORS ═══════════════
#define I2C_SDA 21
#define I2C_SCL 15
#define TEMP_PIN 39
#define BAT_PIN  TEMP_PIN   // ADC for battery voltage
#define LIGHT_PIN 36

// ═══════════════ WIFI / BLE ═══════════════
#define WIFI_MAX_AP   40
#define BLE_MAX_DEV   40
#define BLE_SCAN_SEC  15
#define DEAUTH_REASON 7

// ═══════════════ COLORS (Marauder green) ═══════════════
// Must be #defined as compile-time constants for TFT_eSPI
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_GREEN   0x07E0
#define C_DGREEN  0x0320
#define C_CYAN    0x07FF
#define C_RED     0xF800
#define C_YELLOW  0xFFE0
#define C_ORANGE  0xFD20
#define C_GRAY    0x8410
#define C_DGRAY   0x4208
#define C_LGRAY   0xC618
#define C_BLUE    0x001F
#define C_MAGENTA 0xF81F

// Saber green (Marauder)
#define C_SGREEN  0x07E0
#define C_SDIM    0x0200
#define C_SMID    0x0400
#define C_SHEADER 0x01A0
#define C_SBORDER 0x0300

// ═══════════════ PAGE STATES ═══════════════
enum Page : uint8_t {
    PG_LOCK = 0,
    PG_MENU,
    PG_RECON_WIFI,
    PG_RECON_BLE,
    PG_RECON_WARD,
    PG_ATTK_DEAUTH,
    PG_ATTK_BEACON,
    PG_ATTK_PORTAL,
    PG_ATTK_BLE,
    PG_ATTK_BADUSB,
    PG_ATTK_DEFENSE,
    PG_ATTK_TCPFLOOD,
    PG_NET_TELNET,
    PG_NET_SSH,
    PG_NET_TCP,
    PG_NET_HOST,
    PG_NET_TRAFFIC,
    PG_EXPLOIT,
    PG_SYS_FILES,
    PG_SYS_WEBUI,
    PG_SYS_TERM,
    PG_SYS_TIME,
    PG_SYS_BRIGHT,
    PG_SYS_BUZZER,
    PG_SYS_UPDATE,
    PG_SYS_APPCENTER,
    PG_SYS_REBOOT,
    PG_ABOUT,
    PG_WEATHER,
    PG_COUNT
};

// ═══════════════ ATTACK STATE ═══════════════
enum AtkState : uint8_t {
    ATK_OFF = 0,
    ATK_ARMED,
    ATK_RUNNING
};

// ═══════════════ WIFI MODE ═══════════════
enum WifiMode : uint8_t {
    WM_OFF = 0,
    WM_STA,
    WM_AP
};

// ═══════════════ STRUCTS ═══════════════
struct WifiAP {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t ch;
    uint8_t enc; // 0=Open,1=WEP,2=WPA,3=WPA2,4=WPA3
};

struct BLE_Dev {
    char name[32];
    uint8_t mac[6];
    int8_t rssi;
    uint8_t type;
};

struct PcapEntry {
    char name[48];
    uint32_t size;
    uint32_t time;
};

struct MenuItem {
    const char* label;
    Page target;
    bool is_cat; // category header
};

// System config
struct SysCfg {
    bool time24h;
    uint8_t brightness;
    bool buzzer_on;
    int8_t tz_offset; // timezone offset hours
    char ntp_srv[48];
    uint32_t lock_timeout;
};

// ═══════════════ GLOBALS (extern) ═══════════════
extern Page g_page;
extern Page g_prev;
extern SysCfg g_cfg;

extern AtkState g_wifi_atk;
extern AtkState g_ble_atk;
extern AtkState g_wifi_def;
extern WifiMode g_wifi_mode;
extern bool g_wifi_conn;
extern bool g_ble_on;

extern uint32_t g_packets_sent;
extern uint32_t g_beacons_sent;
extern uint32_t g_ble_spam_cnt;
extern volatile uint32_t g_packets_blocked;
extern volatile uint32_t g_traffic_rx;
extern volatile uint32_t g_traffic_tx;

extern bool sd_ok;
extern char weather_buf[32];

// Network stats for latency display
extern uint32_t g_net_latency;  // ping latency in ms
extern uint32_t g_last_wifi_check;

// Weather data
extern char weather_temp[8];    // temperature string
extern char weather_cond[16];   // condition text
extern char weather_loc[16];    // location
extern uint32_t weather_last;

#endif