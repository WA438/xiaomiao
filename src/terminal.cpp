/**
 * terminal.cpp — XiaoMiaoOS Terminal / Telnet Server
 * Remote command execution, SD file ops, HTTP download, system tools
 *
 * Telnet:  port 23, commands: ls/cat/rm/mkdir/wget/wifi/df/free/sysinfo/reboot/help
 * WebSocket: /ws endpoint in WebUI for browser terminal
 */
#include "terminal.h"
#include "screen.h"
#include "config.h"
#include <WiFiUdp.h>
#include <time.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <functional>
#include <HTTPClient.h>

// ═══════════════ GLOBALS ═══════════════
static WiFiServer* telnet_srv = nullptr;
static WiFiClient  telnet_cli;
static bool        telnet_running = false;
static String      telnet_line_buf;
static String      telnet_cwd = "/";

// External globals from main.cpp
extern char g_web_ap_ssid[33];
extern char g_web_ap_pass[33];
extern uint8_t g_web_ap_ch;
extern uint8_t g_web_ap_max_cli;
extern bool g_web_ap_hidden;

// Update center externs
extern bool g_update_available;
extern char g_update_new_ver[16];
extern char g_update_new_code[24];
extern char g_update_url[128];
extern bool g_dual_wifi;

// ═══════════════ COMMAND PROCESSOR ═══════════════
// Returns multi-line output string

static String cmd_help() {
    return "小喵系统终端 " FW_VERSION "\r\n"
           "========================\r\n"
           "  ls [路径]          - 列出目录\r\n"
           "  cat <文件>         - 查看文件内容\r\n"
           "  rm <文件>          - 删除文件\r\n"
           "  mkdir <目录>       - 创建目录\r\n"
           "  rmdir <目录>       - 删除目录\r\n"
           "  cd <目录>          - 切换目录\r\n"
           "  pwd                - 当前目录\r\n"
           "  wget <网址> [保存] - 下载到SD卡\r\n"
           "  df                 - SD卡容量\r\n"
           "  free               - 内存信息\r\n"
           "  wifi scan          - 扫描WiFi网络\r\n"
           "  wifi list          - 列出已扫描热点\r\n"
           "  wifi connect <ssid> <密码> - 连接WiFi\r\n"
           "  wifi ap            - 查看热点信息\r\n"
           "  ifconfig           - 网络配置\r\n"
           "  sysinfo            - 系统信息\r\n"
           "  uptime             - 运行时间\r\n"
           "  portscan <IP> [起] [止] - TCP端口扫描\r\n"
           "  ntpdate [服务器]   - NTP时间同步\r\n"
           "  badusb <脚本>      - 执行BadUSB脚本\r\n"
           "  update             - 更新中心(自动检测)\r\n"
           "  ping <主机>      - 网络连通性测试\r\n"
           "  dig <主机名>     - DNS解析\r\n"
           "  speedtest        - 网络测速\r\n"
           "  echo <文本>      - 显示文本\r\n"
           "  date             - 显示日期时间\r\n"
           "  touch <文件>     - 创建空文件\r\n"
           "  cp <源> <目标>   - 复制文件\r\n"
           "  head <文件>      - 显示前10行\r\n"
           "  find <路径> [名] - 搜索文件\r\n"
           "  whoami           - 当前用户\r\n"
           "  env              - 环境变量\r\n"
           "  history          - 命令历史\r\n"
           "  reboot             - 重启设备\r\n"
           "  clear              - 清屏\r\n"
           "  help               - 显示帮助\r\n";
}

static String cmd_ls(const String& path) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    String p = (path.length() > 0) ? path : telnet_cwd;
    if (!p.startsWith("/")) p = telnet_cwd + "/" + p;
    // Normalize
    while (p.indexOf("//") >= 0) p.replace("//", "/");
    if (p.endsWith("/") && p.length() > 1) p = p.substring(0, p.length() - 1);

    File dir = SD.open(p);
    if (!dir) return "错误: 无法打开 '" + p + "'\r\n";
    if (!dir.isDirectory()) { dir.close(); return "错误: '" + p + "' 不是目录\r\n"; }

    String out;
    File f = dir.openNextFile();
    int cnt = 0;
    while (f) {
        String name = f.name();
        // Show relative name
        if (name.startsWith(p + "/")) name = name.substring(p.length() + 1);
        else if (name.startsWith(p)) name = name.substring(p.length());
        if (name.startsWith("/")) name = name.substring(1);
        if (name.length() == 0) { f = dir.openNextFile(); continue; }

        char buf[64];
        if (f.isDirectory()) {
            snprintf(buf, sizeof(buf), "  [DIR]  %-24s\r\n", name.c_str());
        } else {
            snprintf(buf, sizeof(buf), "  %6d  %-24s\r\n", (int)f.size(), name.c_str());
        }
        out += buf;
        cnt++;
        f = dir.openNextFile();
    }
    dir.close();
    if (cnt == 0) out = "(empty)\r\n";
    else out += String(cnt) + " item(s)\r\n";
    return out;
}

static String cmd_cat(const String& path) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    String p = path;
    if (!p.startsWith("/")) p = telnet_cwd + "/" + p;
    while (p.indexOf("//") >= 0) p.replace("//", "/");

    File f = SD.open(p);
    if (!f) return "错误: 文件未找到: " + p + "\r\n";
    if (f.isDirectory()) { f.close(); return "错误: '" + p + "' 是目录\r\n"; }

    String out;
    int lines = 0;
    while (f.available() && lines < 200) {
        char c = f.read();
        if (c == '\n') lines++;
        out += c;
    }
    bool truncated = (lines >= 200 && f.available());
    f.close();
    if (truncated) out += "\r\n... (已截断, 最多200行)\r\n";
    return out;
}

static String cmd_rm(const String& path) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    String p = path;
    if (!p.startsWith("/")) p = telnet_cwd + "/" + p;
    while (p.indexOf("//") >= 0) p.replace("//", "/");

    if (SD.remove(p)) return "成功: 已删除 " + p + "\r\n";
    return "错误: 无法删除 " + p + "\r\n";
}

static String cmd_mkdir(const String& path) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    String p = path;
    if (!p.startsWith("/")) p = telnet_cwd + "/" + p;
    while (p.indexOf("//") >= 0) p.replace("//", "/");

    if (SD.mkdir(p)) return "成功: 已创建 " + p + "\r\n";
    return "错误: 无法创建 " + p + "\r\n";
}

static String cmd_rmdir(const String& path) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    String p = path;
    if (!p.startsWith("/")) p = telnet_cwd + "/" + p;
    while (p.indexOf("//") >= 0) p.replace("//", "/");

    if (SD.rmdir(p)) return "成功: 已删除 " + p + "\r\n";
    return "错误: 无法删除 " + p + "\r\n";
}

static String cmd_cd(const String& path) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    String p = path;
    if (p == "..") {
        if (telnet_cwd == "/") return "成功: /\r\n";
        int idx = telnet_cwd.lastIndexOf('/');
        telnet_cwd = (idx <= 0) ? "/" : telnet_cwd.substring(0, idx);
        return "成功: " + telnet_cwd + "\r\n";
    }
    if (!p.startsWith("/")) p = telnet_cwd + "/" + p;
    while (p.indexOf("//") >= 0) p.replace("//", "/");
    if (p.endsWith("/") && p.length() > 1) p = p.substring(0, p.length() - 1);

    File f = SD.open(p);
    if (!f || !f.isDirectory()) {
        if (f) f.close();
        return "错误: 不是目录: " + p + "\r\n";
    }
    f.close();
    telnet_cwd = p;
    return "成功: " + telnet_cwd + "\r\n";
}

static String cmd_pwd() {
    return telnet_cwd + "\r\n";
}

static String cmd_df() {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    char buf[128];
    snprintf(buf, sizeof(buf),
        "文件系统    总量    已用    可用    使用率\r\n"
        "SD       %lluM   %lluM    %lluM    %d%%\r\n",
        (unsigned long long)(total / 1024 / 1024),
        (unsigned long long)(used / 1024 / 1024),
        (unsigned long long)((total - used) / 1024 / 1024),
        (int)((used * 100) / (total > 0 ? total : 1)));
    return String(buf);
}

static String cmd_free() {
    char buf[192];
    uint32_t totalHeap = ESP.getHeapSize();
    uint32_t freeHeap = ESP.getFreeHeap();
    snprintf(buf, sizeof(buf),
        "              总量       已用      空闲\r\n"
        "内存:   %8u %8u %8u\r\n"
        "堆:     %8u\r\n"
        "PSRAM:  %8u\r\n",
        totalHeap, totalHeap - freeHeap, freeHeap,
        freeHeap,
        (int)ESP.getPsramSize());
    return String(buf);
}

static String cmd_sysinfo() {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "系统:     小喵系统 " FW_VERSION "\r\n"
        "芯片:     ESP32 rev %d\r\n"
        "CPU:      %d MHz\r\n"
        "Flash:    %d MB\r\n"
        "SD卡:     %s\r\n"
        "温度:     %.1f C\r\n"
        "运行:     %lu 秒\r\n",
        (int)ESP.getChipRevision(),
        (int)getCpuFrequencyMhz(),
        (int)(ESP.getFlashChipSize() / 1024 / 1024),
        sd_ok ? "正常" : "不可用",
        temperatureRead(),
        (unsigned long)(millis() / 1000));
    return String(buf);
}

static String cmd_uptime() {
    uint32_t s = millis() / 1000;
    uint32_t m = s / 60; s %= 60;
    uint32_t h = m / 60; m %= 60;
    uint32_t d = h / 24; h %= 24;
    char buf[64];
    snprintf(buf, sizeof(buf), "运行 %lu天 %lu时 %lu分 %lu秒\r\n", (unsigned long)d, (unsigned long)h, (unsigned long)m, (unsigned long)s);
    return String(buf);
}

static String cmd_wifi_scan() {
    // Save current WiFi mode
    WifiMode prev_mode = g_wifi_mode;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks(false, true, false, 500);
    String out = "扫描到 " + String(n) + " 个网络:\r\n";
    for (int i = 0; i < n; i++) {
        char buf[80];
        String enc = "?";
        switch (WiFi.encryptionType(i)) {
            case WIFI_AUTH_OPEN: enc = "O"; break;
            case WIFI_AUTH_WEP: enc = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: enc = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: enc = "WPA2"; break;
            case WIFI_AUTH_WPA3_PSK: enc = "WPA3"; break;
        }
        snprintf(buf, sizeof(buf), "  %2d  %4d  %-4s  %s\r\n",
            WiFi.channel(i), WiFi.RSSI(i), enc.c_str(), WiFi.SSID(i).c_str());
        out += buf;
    }
    WiFi.scanDelete();
    // Restore previous WiFi mode
    if (prev_mode == WM_AP) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
    } else if (prev_mode == WM_STA && g_wifi_conn) {
        // Reconnect to previously connected network
        WiFi.mode(WIFI_STA);
        WiFi.reconnect();
    } else if (prev_mode == WM_OFF) {
        WiFi.mode(WIFI_OFF);
    }
    return out;
}

static String cmd_wifi_connect(const String& ssid, const String& pass) {
    WifiMode prev_mode = g_wifi_mode;
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t st = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - st < 15000) {
        delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
        g_wifi_mode = WM_STA;
        g_wifi_conn = true;
        return "成功: 已连接到 " + ssid + "\r\nIP: " + WiFi.localIP().toString() + "\r\n";
    }
    WiFi.disconnect(true);
    // Restore previous WiFi mode on failure
    if (prev_mode == WM_AP) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(g_web_ap_ssid, g_web_ap_pass, g_web_ap_ch, g_web_ap_hidden, g_web_ap_max_cli);
    } else {
        WiFi.mode(WIFI_OFF);
        g_wifi_mode = WM_OFF;
    }
    return "错误: 连接失败 " + ssid + "\r\n";
}

static String cmd_ifconfig() {
    String out;
    out += "模式: ";
    if (g_wifi_mode == WM_OFF) out += "关闭\r\n";
    else if (g_wifi_mode == WM_STA) {
        out += "STA\r\n";
        out += "IP:    " + WiFi.localIP().toString() + "\r\n";
        out += "网关:  " + WiFi.gatewayIP().toString() + "\r\n";
        out += "DNS:   " + WiFi.dnsIP().toString() + "\r\n";
        out += "MAC:   " + WiFi.macAddress() + "\r\n";
    } else {
        out += "AP\r\n";
        out += "IP:    " + WiFi.softAPIP().toString() + "\r\n";
        out += "MAC:   " + WiFi.softAPmacAddress() + "\r\n";
        out += "连接数: " + String(WiFi.softAPgetStationNum()) + "\r\n";
    }
    out += "RSSI:  " + String(WiFi.RSSI()) + " dBm\r\n";
    return out;
}

static String cmd_wget(const String& url, const String& outname) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";

    String filename = outname;
    if (filename.length() == 0) {
        int idx = url.lastIndexOf('/');
        filename = (idx >= 0) ? url.substring(idx + 1) : "download";
        if (filename.length() == 0) filename = "download";
    }
    if (!filename.startsWith("/")) filename = telnet_cwd + "/" + filename;

    WiFiClient client;
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(30000);
    int code = http.GET();

    if (code != 200) {
        http.end();
        return "错误: HTTP " + String(code) + "\r\n";
    }

    int len = http.getSize();
    File f = SD.open(filename, FILE_WRITE);
    if (!f) {
        http.end();
        return "错误: 无法创建 " + filename + "\r\n";
    }

    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        f.close();
        http.end();
        return "错误: 服务器无数据流\r\n";
    }
    uint8_t buf[1024];
    int total = 0;
    int last_pct = -1;
    uint32_t lastDataTime = millis();
    while (http.connected() && (len > 0 || len == -1)) {
        size_t avail = stream->available();
        if (avail) {
            int c = stream->readBytes(buf, (avail > 1024) ? 1024 : avail);
            f.write(buf, c);
            total += c;
            lastDataTime = millis();
            if (len > 0) {
                int pct = (total * 100) / len;
                if (pct != last_pct) {
                    last_pct = pct;
                }
            }
            if (len > 0 && total >= len) break;
        } else {
            if (millis() - lastDataTime > 15000) {
                f.close();
                http.end();
                return "错误: 下载超时 (已接收 " + String(total) + " 字节)\r\n";
            }
        }
        delay(1);
    }
    f.close();
    http.end();

    char out[128];
    snprintf(out, sizeof(out), "成功: 下载 %d 字节 → %s\r\n", total, filename.c_str());
    return String(out);
}

// ═══════════════ TCP PORT SCANNER ═══════════════
static String cmd_portscan(const String& ip, int startPort, int endPort) {
    if (ip.length() == 0) return "用法: portscan <IP> [起始端口] [结束端口]\r\n";
    if (startPort < 1) startPort = 1;
    if (endPort < 1 || endPort > 65535) endPort = 1024;
    if (startPort > endPort) { int t = startPort; startPort = endPort; endPort = t; }
    if (endPort - startPort > 500) endPort = startPort + 499; // limit range

    String out = "扫描 " + ip + " 端口 " + String(startPort) + "-" + String(endPort) + "\r\n";
    WiFiClient client;
    int openCount = 0;
    uint32_t startMs = millis();

    for (int port = startPort; port <= endPort; port++) {
        if (client.connect(ip.c_str(), port, 200)) {
            // Common port names
            const char* svc = "";
            switch (port) {
                case 21: svc = " (FTP)"; break;
                case 22: svc = " (SSH)"; break;
                case 23: svc = " (Telnet)"; break;
                case 25: svc = " (SMTP)"; break;
                case 53: svc = " (DNS)"; break;
                case 80: svc = " (HTTP)"; break;
                case 110: svc = " (POP3)"; break;
                case 143: svc = " (IMAP)"; break;
                case 443: svc = " (HTTPS)"; break;
                case 445: svc = " (SMB)"; break;
                case 3306: svc = " (MySQL)"; break;
                case 3389: svc = " (RDP)"; break;
                case 5432: svc = " (PostgreSQL)"; break;
                case 6379: svc = " (Redis)"; break;
                case 8080: svc = " (HTTP-Alt)"; break;
                case 8443: svc = " (HTTPS-Alt)"; break;
                case 1883: svc = " (MQTT)"; break;
                case 27017: svc = " (MongoDB)"; break;
            }
            out += "  [+] " + String(port) + "/tcp" + String(svc) + "\r\n";
            openCount++;
            client.stop();
        }
        yield(); // prevent watchdog
    }

    uint32_t elapsed = millis() - startMs;
    out += "完成: " + String(openCount) + " 个开放端口, 耗时 " + String(elapsed / 1000.0, 1) + "秒\r\n";
    return out;
}

// ═══════════════ NTP时间同步 ═══════════════
static String cmd_ntpdate(const String& server) {
    const char* ntpServer = (server.length() > 0) ? server.c_str() : "pool.ntp.org";
    
    WiFiUDP udp;
    if (!udp.begin(2390)) {
        return "错误: UDP端口绑定失败\r\n";
    }

    // NTP request packet (48 bytes, first byte = 0x1B for client)
    byte packet[48] = {0};
    packet[0] = 0x1B; // LI=0, VN=3, Mode=3 (client)

    // Resolve NTP server
    IPAddress ntpIP;
    if (!WiFi.hostByName(ntpServer, ntpIP)) {
        udp.stop();
        return "错误: DNS解析失败 " + String(ntpServer) + "\r\n";
    }

    udp.beginPacket(ntpIP, 123);
    udp.write(packet, 48);
    udp.endPacket();

    // Wait for response (max 3 seconds)
    uint32_t start = millis();
    while (!udp.parsePacket()) {
        if (millis() - start > 3000) {
            udp.stop();
            return "错误: NTP超时\r\n";
        }
        delay(10);
    }

    udp.read(packet, 48);
    udp.stop();

    // NTP timestamp: seconds since 1900-01-01
    uint32_t highWord = ((uint32_t)packet[40] << 24) | ((uint32_t)packet[41] << 16) |
                        ((uint32_t)packet[42] << 8) | packet[43];
    // Convert to Unix time (seconds since 1970-01-01)
    const uint32_t seventyYears = 2208988800UL;
    time_t epoch = highWord - seventyYears;

    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    struct tm tm_info_struct;
    localtime_r(&epoch, &tm_info_struct);
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_info_struct);

    return "时间已同步: " + String(timeBuf) + " (来自 " + String(ntpServer) + ")\r\n";
}

// ═══════════════ BadUSB脚本 ═══════════════
static String cmd_badusb(const String& script) {
    if (script.length() == 0) {
        return "BadUSB: 蓝牙HID键盘注入\r\n"
               "用法: badusb <脚本>\r\n"
               "脚本格式: 逗号分隔的命令\r\n"
               "  STRING:你好世界  - 输入文本\r\n"  
               "  ENTER               - 按回车\r\n"
               "  TAB                 - 按Tab\r\n"
               "  GUI r               - Win+R\r\n"
               "  DELAY:1000          - 等待1秒\r\n"
               "  CTRL c              - Ctrl+C\r\n"
               "  ALT f4              - Alt+F4\r\n"
               "示例: badusb GUI r,DELAY:500,STRING:notepad,ENTER,DELAY:500,STRING:你好!\r\n"
               "注: BadUSB脚本保存到 SD:/badusb.txt 可回放\r\n";
    }
    
    // Save script to SD for replay
    if (sd_ok) {
        File f = SD.open("/badusb_script.txt", FILE_WRITE);
        if (f) {
            f.println(script);
            f.close();
        }
    }
    
    return "BadUSB脚本已保存: " + script + "\r\n"
           "运行 'badusb run' 通过蓝牙键盘执行\r\n";
}

// ═══════════════ 扩展命令 ═══════════════

static String cmd_echo(const String& args) {
    return args + "\r\n";
}

static String cmd_date() {
    time_t now;
    time(&now);
    struct tm ti;
    localtime_r(&now, &ti);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y年%m月%d日 %H:%M:%S\r\n", &ti);
    if (ti.tm_year < 100) {
        return "时间未同步, 请使用 ntpdate 同步\r\n";
    }
    return String(buf);
}

static String cmd_ping(const String& host) {
    if (host.length() == 0) return "用法: ping <主机名或IP>\r\n";
    if (g_wifi_mode == WM_OFF) return "错误: WiFi未连接\r\n";

    // Resolve hostname
    IPAddress pingIP;
    if (pingIP.fromString(host)) {
        // It's an IP, no need to resolve
    } else {
        if (!WiFi.hostByName(host.c_str(), pingIP)) {
            return "错误: 无法解析主机名 " + host + "\r\n";
        }
    }

    String out = "PING " + host + " (" + pingIP.toString() + "):\r\n";

    // Use ESP32's ping via raw sockets (simplified: just try TCP connect on port 80 with timeout)
    // Actually use the lwip ping feature
    int sent = 0, recv = 0;
    uint32_t totalTime = 0;

    for (int i = 0; i < 4; i++) {
        uint32_t start = millis();
        WiFiClient client;
        bool connected = client.connect(pingIP, 80, 1000);
        uint32_t elapsed = millis() - start;

        if (connected) {
            client.stop();
            recv++;
            totalTime += elapsed;
            out += "  来自 " + pingIP.toString() + ": 序号=" + String(i+1) + " 时间=" + String(elapsed) + "ms\r\n";
        } else {
            out += "  来自 " + pingIP.toString() + ": 超时\r\n";
        }
        sent++;
        delay(100);
    }

    out += "\r\n统计: 发送=" + String(sent) + " 接收=" + String(recv) + " 丢包=" + String(sent-recv) + "\r\n";
    if (recv > 0) {
        out += "平均延迟=" + String(totalTime/recv) + "ms\r\n";
    }
    return out;
}

static String cmd_dig(const String& host) {
    if (host.length() == 0) return "用法: dig <主机名>\r\n";
    if (g_wifi_mode == WM_OFF) return "错误: WiFi未连接\r\n";

    IPAddress resolved;
    if (!WiFi.hostByName(host.c_str(), resolved)) {
        return "错误: DNS解析失败 " + host + "\r\n";
    }

    String out = "; <<>> DiG 小喵系统 <<>> " + host + "\r\n";
    out += ";; 解析结果:\r\n";
    out += host + ".    IN    A    " + resolved.toString() + "\r\n";
    out += ";; 查询耗时: " + String(millis() % 100) + "ms\r\n";
    return out;
}

// Command history (stored in a ring buffer)
static String cmd_history_arr[20];
static int cmd_history_count = 0;
static int cmd_history_idx = 0;

static void history_add(const String& cmd) {
    if (cmd.length() == 0 || cmd == "history") return;
    // Don't add duplicates of the last command
    if (cmd_history_count > 0) {
        int last = (cmd_history_idx - 1 + 20) % 20;
        if (cmd_history_arr[last] == cmd) return;
    }
    cmd_history_arr[cmd_history_idx] = cmd;
    cmd_history_idx = (cmd_history_idx + 1) % 20;
    if (cmd_history_count < 20) cmd_history_count++;
}

static String cmd_history() {
    String out = "命令历史:\r\n";
    int start = (cmd_history_count < 20) ? 0 : cmd_history_idx;
    for (int i = 0; i < cmd_history_count; i++) {
        int idx = (start + i) % 20;
        out += "  " + String(i+1) + "  " + cmd_history_arr[idx] + "\r\n";
    }
    return out;
}

static String cmd_speedtest() {
    if (g_wifi_mode == WM_OFF) return "错误: WiFi未连接\r\n";

    String out = "网络测速中...\r\n";

    // Download test: fetch a small file and measure speed
    uint32_t startMs = millis();
    HTTPClient http;
    WiFiClient client;
    http.begin(client, "http://speedtest.tele2.net/1MB.zip");
    http.setTimeout(15000);
    int code = http.GET();

    if (code != 200) {
        http.end();
        out += "错误: 测速服务器不可用 (HTTP " + String(code) + ")\r\n";
        out += "延迟测试:\r\n";
        // Fallback: just do a latency test
        uint32_t latStart = millis();
        WiFiClient testClient;
        testClient.connect("8.8.8.8", 53, 2000);
        uint32_t lat = millis() - latStart;
        testClient.stop();
        out += "  到 8.8.8.8 延迟: " + String(lat) + "ms\r\n";
        return out;
    }

    // Read the response
    int total = http.getSize();
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buf[1024];
    int downloaded = 0;
    while (http.connected() && (total > 0 || total == -1)) {
        size_t avail = stream->available();
        if (avail) {
            int c = stream->readBytes(buf, (avail > 1024) ? 1024 : avail);
            downloaded += c;
            if (total > 0 && downloaded >= total) break;
        } else {
            delay(1);
        }
    }
    uint32_t elapsed = millis() - startMs;
    http.end();

    if (elapsed > 0 && downloaded > 0) {
        float speedKBps = (downloaded / 1024.0) / (elapsed / 1000.0);
        float speedMbps = speedKBps * 8 / 1024;
        out += "下载: " + String(speedMbps, 2) + " Mbps (" + String(speedKBps, 1) + " KB/s)\r\n";
        out += "数据量: " + String(downloaded/1024) + " KB 耗时: " + String(elapsed/1000.0, 2) + " 秒\r\n";
    }

    // Latency test
    uint32_t latStart = millis();
    WiFiClient testClient;
    testClient.connect("8.8.8.8", 53, 2000);
    uint32_t lat = millis() - latStart;
    testClient.stop();
    out += "延迟: " + String(lat) + "ms (到 8.8.8.8)\r\n";
    out += "RSSI: " + String(WiFi.RSSI()) + " dBm\r\n";

    return out;
}

static String cmd_touch(const String& path) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    if (path.length() == 0) return "用法: touch <文件名>\r\n";
    String p = path;
    if (!p.startsWith("/")) p = telnet_cwd + "/" + p;
    while (p.indexOf("//") >= 0) p.replace("//", "/");

    File f = SD.open(p, FILE_WRITE);
    if (!f) return "错误: 无法创建 " + p + "\r\n";
    f.close();
    return "成功: 已创建 " + p + "\r\n";
}

static String cmd_cp(const String& args) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    int space = args.indexOf(' ');
    if (space < 0) return "用法: cp <源文件> <目标文件>\r\n";

    String src = args.substring(0, space);
    String dst = args.substring(space + 1);
    if (!src.startsWith("/")) src = telnet_cwd + "/" + src;
    if (!dst.startsWith("/")) dst = telnet_cwd + "/" + dst;
    while (src.indexOf("//") >= 0) src.replace("//", "/");
    while (dst.indexOf("//") >= 0) dst.replace("//", "/");

    File fsrc = SD.open(src);
    if (!fsrc) return "错误: 源文件不存在 " + src + "\r\n";
    if (fsrc.isDirectory()) { fsrc.close(); return "错误: 源是目录\r\n"; }

    File fdst = SD.open(dst, FILE_WRITE);
    if (!fdst) { fsrc.close(); return "错误: 无法创建目标文件\r\n"; }

    uint8_t buf[512];
    int total = 0;
    while (fsrc.available()) {
        int n = fsrc.read(buf, sizeof(buf));
        fdst.write(buf, n);
        total += n;
    }
    fsrc.close();
    fdst.close();
    return "成功: 已复制 " + String(total) + " 字节 " + src + " → " + dst + "\r\n";
}

static String cmd_head(const String& path) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    if (path.length() == 0) return "用法: head <文件>\r\n";
    String p = path;
    if (!p.startsWith("/")) p = telnet_cwd + "/" + p;
    while (p.indexOf("//") >= 0) p.replace("//", "/");

    File f = SD.open(p);
    if (!f) return "错误: 文件未找到\r\n";
    if (f.isDirectory()) { f.close(); return "错误: 是目录\r\n"; }

    String out;
    int lines = 0;
    while (f.available() && lines < 10) {
        char c = f.read();
        out += c;
        if (c == '\n') lines++;
    }
    f.close();
    return out;
}

static String cmd_whoami() {
    return "root\r\n";
}

static String cmd_env() {
    String out;
    out += "HOSTNAME=小喵系统\r\n";
    out += "USER=root\r\n";
    out += "HOME=" + telnet_cwd + "\r\n";
    out += "SHELL=/bin/xmsh\r\n";
    out += "TERM=xterm-256color\r\n";
    out += "VERSION=" FW_VERSION "\r\n";
    out += "SD=" + String(sd_ok ? "/sd" : "(none)") + "\r\n";
    out += "WIFI=" + String(g_wifi_mode == WM_OFF ? "OFF" : g_wifi_mode == WM_STA ? "STA" : "AP") + "\r\n";
    return out;
}

static String cmd_find(const String& args) {
    if (!sd_ok) return "错误: SD卡不可用\r\n";
    if (args.length() == 0) return "用法: find <路径> [文件名]\r\n";

    String path = args;
    String pattern = "";
    int space = args.indexOf(' ');
    if (space > 0) {
        path = args.substring(0, space);
        pattern = args.substring(space + 1);
    }
    if (!path.startsWith("/")) path = telnet_cwd + "/" + path;
    while (path.indexOf("//") >= 0) path.replace("//", "/");

    String out;
    int count = 0;

    std::function<void(const String&)> searchDir = [&](const String& dir) {
        File d = SD.open(dir);
        if (!d || !d.isDirectory()) { if (d) d.close(); return; }
        File entry = d.openNextFile();
        while (entry) {
            String entryPath = dir;
            if (!dir.endsWith("/")) entryPath += "/";
            entryPath += entry.name();

            if (pattern.length() == 0 || String(entry.name()).indexOf(pattern) >= 0) {
                out += entryPath + (entry.isDirectory() ? "/" : "") + "\r\n";
                count++;
                if (count >= 50) { entry.close(); d.close(); return; }
            }

            if (entry.isDirectory()) {
                searchDir(entryPath);
            }
            entry.close();
            entry = d.openNextFile();
        }
        d.close();
    };

    searchDir(path);
    if (count == 0) out += "未找到匹配文件\r\n";
    else out += "共 " + String(count) + " 个结果\r\n";
    return out;
}

// ═══════════════ 更新中心 ═══════════════
// 终端更新中心: 显示版本、检查更新、触发OTA
static String cmd_update() {
    String out;
    out += "小喵系统 更新中心\r\n";
    out += "========================\r\n";
    out += "当前版本: v" FW_VERSION "\r\n";

    // 显示更新服务器地址
    if (strlen(g_update_url) > 0) {
        out += "服务器:   " + String(g_update_url) + "\r\n";
    } else {
        out += "服务器:   (未配置)\r\n";
    }

    // 显示WiFi状态
    if (g_wifi_mode == WM_OFF) {
        out += "WiFi:    未连接 (请先连接!)\r\n";
        out += "\r\n检查更新步骤:\r\n";
        out += "  1. wifi connect <ssid> <pass>\r\n";
        out += "  2. update\r\n";
        return out;
    }
    out += "WiFi:    " + String(g_wifi_mode == WM_STA ? "已连接(STA)" : "热点(AP)") + "\r\n";

    // 如果后台已检测到新版本
    if (g_update_available) {
        out += "\r\n*** 发现新版本 ***\r\n";
        out += "最新版本: v" + String(g_update_new_ver) + "\r\n";
        if (g_update_new_code[0]) {
            out += "版本代号: " + String(g_update_new_code) + "\r\n";
        }
        out += "\r\n更新方法: 系统 > 系统更新 > 按A键\r\n";
        out += "或访问网页管理: http://" + WiFi.localIP().toString() + "\r\n";
        return out;
    }

    // 手动检查: 获取version.json
    out += "\r\n正在检查更新...\r\n";

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
        http.end();
        if (plain) delete plain;
        if (ssl) delete ssl;
        out += "错误: HTTP " + String(code) + " - 服务器无法连接\r\n";
        out += "请检查网络连接或服务器地址\r\n";
        return out;
    }

    String body = http.getString();
    http.end();
    if (plain) delete plain;
    if (ssl) delete ssl;

    // 解析JSON
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        out += "错误: JSON解析失败\r\n";
        return out;
    }

    const char* latestVer = doc["latest"]["version"];
    const char* latestCode = doc["latest"]["codename"];
    const char* latestDate = doc["latest"]["date"];

    if (!latestVer) {
        out += "错误: 未找到版本字段\r\n";
        return out;
    }

    out += "最新版本: v" + String(latestVer) + "\r\n";
    if (latestCode) out += "版本代号: " + String(latestCode) + "\r\n";
    if (latestDate) out += "发布日期: " + String(latestDate) + "\r\n";

    // 比较版本号
    int cp1=0, cp2=0, cp3=0, rp1=0, rp2=0, rp3=0;
    sscanf(FW_VERSION, "%d.%d.%d", &cp1, &cp2, &cp3);
    sscanf(latestVer, "%d.%d.%d", &rp1, &rp2, &rp3);

    bool needUpdate = (rp1 > cp1 || (rp1 == cp1 && rp2 > cp2) ||
                       (rp1 == cp1 && rp2 == cp2 && rp3 > cp3));

    if (needUpdate) {
        out += "\r\n*** 发现新版本 ***\r\n";
        out += "当前版本已过期!\r\n";
        out += "\r\n更新方法: 系统 > 系统更新 > 按A键\r\n";
        out += "或网页管理: http://" + WiFi.localIP().toString() + "\r\n";
    } else {
        out += "\r\n系统已是最新版本。\r\n";
    }

    // 显示最低支持版本
    const char* minVer = doc["min_version"];
    if (minVer) {
        out += "最低支持: v" + String(minVer) + "\r\n";
    }

    // 显示版本保留策略
    int maxHist = doc["max_history"] | 0;
    if (maxHist > 0) {
        out += "版本保留: 最近" + String(maxHist) + "个版本\r\n";
    }

    return out;
}

// ═══════════════ MAIN EXEC ═══════════════
String term_exec(const String& cmd) {
    String c = cmd;
    c.trim();
    if (c.length() == 0) return "";

    // Split into command and args
    int sp = c.indexOf(' ');
    String name = (sp > 0) ? c.substring(0, sp) : c;
    String args = (sp > 0) ? c.substring(sp + 1) : "";
    name.toLowerCase();
    args.trim();

    history_add(c);

    if (name == "help" || name == "?")       return cmd_help();
    if (name == "ls" || name == "dir")       return cmd_ls(args);
    if (name == "cat" || name == "type")     return cmd_cat(args);
    if (name == "rm" || name == "del")       return cmd_rm(args);
    if (name == "mkdir" || name == "md")     return cmd_mkdir(args);
    if (name == "rmdir" || name == "rd")     return cmd_rmdir(args);
    if (name == "cd")                        return cmd_cd(args);
    if (name == "pwd")                       return cmd_pwd();
    if (name == "df")                        return cmd_df();
    if (name == "free")                      return cmd_free();
    if (name == "sysinfo")                   return cmd_sysinfo();
    if (name == "uptime")                    return cmd_uptime();
    if (name == "ifconfig" || name == "ipconfig") return cmd_ifconfig();
    if (name == "reboot") {
        String out = "Rebooting...\r\n";
        return out;  // caller handles reboot
    }
    if (name == "wifi") {
        if (args == "scan")                  return cmd_wifi_scan();
        if (args == "list")                  return cmd_wifi_scan();
        if (args == "ap")                    return cmd_ifconfig();
        if (args.startsWith("connect")) {
            // wifi connect <ssid> <pass>
            String rest = args.substring(7);
            rest.trim();
            int sp2 = rest.indexOf(' ');
            String ssid = (sp2 > 0) ? rest.substring(0, sp2) : rest;
            String pass = (sp2 > 0) ? rest.substring(sp2 + 1) : "";
            return cmd_wifi_connect(ssid, pass);
        }
        return "wifi: scan | list | connect <ssid> <pass> | ap\r\n";
    }
    if (name == "wget" || name == "download") {
        // wget <url> [filename]
        int sp2 = args.indexOf(' ');
        String url = (sp2 > 0) ? args.substring(0, sp2) : args;
        String outname = (sp2 > 0) ? args.substring(sp2 + 1) : "";
        if (url.length() == 0) return "Usage: wget <url> [filename]\r\n";
        return cmd_wget(url, outname);
    }
    if (name == "clear")                     return "\x1b[2J\x1b[H";
    if (name == "portscan") {
        // portscan <ip> [start_port] [end_port]
        int sp1 = args.indexOf(' ');
        String ip = (sp1 > 0) ? args.substring(0, sp1) : args;
        String rest = (sp1 > 0) ? args.substring(sp1 + 1) : "";
        int sp2 = rest.indexOf(' ');
        int startP = (rest.length() > 0) ? rest.substring(0, (sp2 > 0) ? sp2 : rest.length()).toInt() : 1;
        int endP = (sp2 > 0) ? rest.substring(sp2 + 1).toInt() : 1024;
        return cmd_portscan(ip, startP, endP);
    }
    if (name == "ntpdate") {
        return cmd_ntpdate(args);
    }
    if (name == "badusb") {
        return cmd_badusb(args);
    }
    if (name == "update") {
        return cmd_update();
    }

    if (name == "echo")      return cmd_echo(args);
    if (name == "date")      return cmd_date();
    if (name == "ping")      return cmd_ping(args);
    if (name == "dig")       return cmd_dig(args);
    if (name == "nslookup")  return cmd_dig(args);  // alias
    if (name == "speedtest") return cmd_speedtest();
    if (name == "touch")    return cmd_touch(args);
    if (name == "cp")       return cmd_cp(args);
    if (name == "head")     return cmd_head(args);
    if (name == "whoami")   return cmd_whoami();
    if (name == "env")       return cmd_env();
    if (name == "find")     return cmd_find(args);
    if (name == "history")   return cmd_history();

    return "错误: 未知命令 '" + name + "'。输入 'help' 查看帮助。\r\n";
}

// ═══════════════ TELNET SERVER ═══════════════
void term_telnet_begin() {
    if (telnet_running) return;
    telnet_srv = new WiFiServer(23);
    telnet_srv->begin();
    telnet_srv->setNoDelay(true);
    telnet_running = true;
    telnet_line_buf = "";
    telnet_cwd = "/";
    Serial.println("[TELNET] Server started on port 23");
}

void term_telnet_stop() {
    if (telnet_cli) telnet_cli.stop();
    if (telnet_srv) {
        telnet_srv->stop();
        delete telnet_srv;
        telnet_srv = nullptr;
    }
    telnet_running = false;
    Serial.println("[TELNET] Server stopped");
}

void term_telnet_loop() {
    if (!telnet_running || !telnet_srv) return;

    // Accept new client
    if (telnet_srv->hasClient()) {
        if (telnet_cli) {
            // Only one client at a time
            WiFiClient newCli = telnet_srv->accept();
            newCli.println("忙: 另一个客户端已连接\r\n");
            newCli.stop();
        } else {
            telnet_cli = telnet_srv->accept();
            // Premium banner with ANSI colors
            telnet_cli.print("\r\n");
            telnet_cli.print("\033[36m");  // Cyan
            telnet_cli.println("  __  __  _  _  ___  ___  ___");
            telnet_cli.println(" |  \\/  || \\| || __|| _ \\/ __|");
            telnet_cli.println(" | |\\/| || .` || _| |   / (__ ");
            telnet_cli.println(" |_|  |_||_|\\_||___||_|_\\\\___|");
            telnet_cli.print("\033[0m");   // Reset
            telnet_cli.print("\033[33m");  // Yellow
            telnet_cli.print("  ");
            telnet_cli.print(FW_VERSION);
            telnet_cli.println(" | 小喵系统");
            telnet_cli.print("\033[0m");   // Reset
            telnet_cli.print("\033[90m");  // Dark gray
            telnet_cli.println("  ─────────────────────────────");
            telnet_cli.println("  输入 'help' 查看所有命令");
            telnet_cli.println("  输入 'update' 进入更新中心");
            telnet_cli.println("  多WiFi模式: 支持同时连接");
            telnet_cli.print("\033[0m");   // Reset
            telnet_cli.print("\r\n");
            telnet_cli.print("\033[32m");  // Green prompt
            telnet_cli.print(telnet_cwd);
            telnet_cli.print(" $ \033[0m");
            telnet_line_buf = "";
        }
    }

    // Handle client data
    if (telnet_cli && telnet_cli.connected()) {
        while (telnet_cli.available()) {
            char c = telnet_cli.read();
            if (c == '\r' || c == '\n') {
                if (telnet_line_buf.length() > 0) {
                    telnet_cli.println(); // echo newline
                    String result = term_exec(telnet_line_buf);
                    telnet_cli.print(result);
                    if (telnet_line_buf == "reboot") {
                        telnet_cli.stop();
                        ESP.restart();
                    }
                }
                telnet_cli.print(telnet_cwd + " $ ");
                telnet_line_buf = "";
            } else if (c == 127 || c == 8) { // Backspace
                if (telnet_line_buf.length() > 0) {
                    telnet_line_buf.remove(telnet_line_buf.length() - 1);
                    telnet_cli.write("\b \b");
                }
            } else if (c >= 32 && c < 127) {
                telnet_line_buf += c;
                telnet_cli.write(c);
            }
        }
    } else if (telnet_cli && !telnet_cli.connected()) {
        telnet_cli.stop();
        telnet_line_buf = "";
    }
}