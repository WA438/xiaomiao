# XiaoMiaoOS 项目进度全览

> 本文档供其他 AI 接手开发使用，包含完整项目状态、架构、已修复 bug 清单、已知问题和待办事项。
> 最后更新：2026-08-16 | 当前版本：v2.1.3 | 固件大小：1,393,568 bytes (Flash 70.6%)

---

## 1. 项目概述

XiaoMiaoOS 是一款基于 ESP32 的手持渗透测试设备固件，风格参考 ESP32 Marauder / Bruce。运行在自定义硬件上（ESP32 + ST7735 128x160 TFT + 6 按键 + SD 卡 + 蜂鸣器），提供 WiFi/BLE 侦察、攻击、防御、网络工具、WebUI 管理等功能。

设备通过 ZeroTermux（Android 上的 Termux）作为中继服务器，实现远程命令执行和固件 OTA 更新。

---

## 2. 硬件配置

```
MCU:     ESP32 (ESP32-dev, 4MB Flash)
屏幕:    ST7735 128x160 (SPI, 旋转90°)
按键:    6个 (UP/DOWN/LEFT/RIGHT/A/B)
SD卡:    SPI 接口
蜂鸣器:  GPIO14 (LEDC PWM)
电池:    ADC GPIO39 (2:1 分压)
```

### 引脚定义 (config.h)
| 功能 | GPIO | 说明 |
|------|------|------|
| TFT_CS | 5 | TFT 片选 |
| TFT_DC | 4 | TFT 数据/命令 |
| TFT_RST | 19 | TFT 复位 (与 SD_MISO 共用！) |
| SD_CS | 22 | SD 片选 |
| SD_MOSI | 23 | SPI MOSI |
| SD_MISO | 19 | SPI MISO (与 TFT_RST 共用！) |
| SD_SCK | 18 | SPI 时钟 |
| BTN_UP | 2 | 上 |
| BTN_DOWN | 13 | 下 |
| BTN_LEFT | 27 | 左 |
| BTN_RIGHT | 35 | 右 |
| BTN_A | 34 | A (确认) |
| BTN_B | 12 | B (返回) |
| BUZZER | 14 | 蜂鸣器 |
| BAT_PIN | 39 | 电池电压 ADC |

**重要硬件约束**：TFT_RST (GPIO19) 与 SD_MISO (GPIO19) 共用同一引脚。SD 卡初始化会导致 TFT 复位（白屏闪烁）。所有使用 SD 的功能在初始化后必须调用 `tft_restore()` 恢复显示。

---

## 3. 软件架构

### 3.1 源文件结构
```
src/
├── config.h       (210行)  硬件配置、枚举、结构体、全局变量声明
├── main.cpp       (4556行) 主程序：UI、攻击、防御、WebUI、OTA、启动
├── terminal.cpp   (672行)  Telnet 服务器、终端命令处理
├── screen.cpp     (309行)  屏幕初始化、状态栏、文本裁剪、TFT 恢复
├── menu.cpp       (215行)  菜单导航系统
├── buttons.cpp    (132行)  按键驱动（去抖、长按、重复）
├── buzzer.cpp     (96行)   蜂鸣器驱动（LEDC PWM）
├── buttons.h      (67行)   按键接口
├── buzzer.h       (58行)   蜂鸣器接口
├── menu.h         (11行)   菜单接口
├── screen.h       (22行)   屏幕接口
└── terminal.h     (21行)   终端接口
```

总计约 6,369 行代码。

### 3.2 关键全局变量
```cpp
// WiFi 状态
Page g_page;              // 当前页面
WifiMode g_wifi_mode;     // WM_OFF / WM_STA / WM_AP
bool g_wifi_conn;         // WiFi STA 是否已连接
bool g_ble_on;            // BLE 是否已初始化
AtkState g_wifi_atk;      // WiFi 攻击状态
AtkState g_ble_atk;       // BLE 攻击状态
AtkState g_wifi_def;      // 防御模式状态

// 计数器 (volatile - 在 promiscuous 回调中写入)
volatile uint32_t g_packets_blocked;  // 被阻断的攻击包数
volatile uint32_t g_traffic_rx;       // 接收流量计数
volatile uint32_t g_traffic_tx;       // 发送流量计数
uint32_t g_packets_sent;              // 发送的攻击包数
uint32_t g_beacons_sent;              // 发送的 beacon 数
uint32_t g_ble_spam_cnt;             // BLE spam 计数

// AP 配置 (非 static，供 terminal.cpp 引用)
char g_web_ap_ssid[33] = "XiaoMiao-CFG";
char g_web_ap_pass[33] = "xiaomiao123";
uint8_t g_web_ap_ch = 1;
uint8_t g_web_ap_max_cli = 8;
bool g_web_ap_hidden = false;

// STA 凭据 (家庭 WiFi)
char g_sta_ssid[33] = "ye";      // ← 硬编码，应改为 NVS 持久化
char g_sta_pass[33] = "82813269";

// OTA 更新 URL (通过 NVS 持久化)
char g_update_url[256];

// SD 卡状态
bool sd_ok = false;
```

### 3.3 页面/功能路由 (main.cpp `loop()` switch)
| 页面枚举 | 功能 |
|---------|------|
| PG_LOCK | 锁屏：时钟 + 系统状态 + 流量监控 |
| PG_MENU | 主菜单导航 |
| PG_RECON_WIFI | WiFi 扫描 + 详情查看 |
| PG_RECON_BLE | BLE 扫描 |
| PG_RECON_WARD | WarDrive（GPS 记录，需硬件支持） |
| PG_ATTK_DEAUTH | Deauth 攻击 |
| PG_ATTK_BEACON | Beacon Spam |
| PG_ATTK_PORTAL | Evil Portal (Captive Portal) |
| PG_ATTK_BLE | BLE Spam |
| PG_ATTK_BADUSB | BLE BadUSB HID 键盘注入 |
| PG_ATTK_DEFENSE | 防御模式 (Deauth 检测) |
| PG_NET_HOST | 主机扫描 (ARP) |
| PG_NET_TCP | TCP 探测 |
| PG_NET_TRAFFIC | 流量监控 |
| PG_EXPLOIT | PCAP / 凭证查看 |
| PG_SYS_FILES | SD 文件浏览器 |
| PG_SYS_WEBUI | WebUI（AP 模式 + Web 服务器） |
| PG_SYS_TERM | Telnet 终端 |
| PG_SYS_TIME | 时间 / NTP 设置 |
| PG_SYS_BRIGHT | 亮度调节 |
| PG_SYS_BUZZER | 蜂鸣器开关 |
| PG_SYS_REBOOT | 重启 |
| PG_ABOUT | 设备信息 |

### 3.4 启动流程 (setup())
```
1. Serial 初始化 (115200)
2. buttons_init()
3. buzzer_init()
4. scr_init()           ← TFT 初始化
5. sd_init()            ← SD 卡初始化（会重置 TFT）
6. tft_restore()        ← 恢复 TFT 显示
7. boot_screen()        ← 开机动画（此时显示已稳定）
8. WiFi STA 连接（10s 超时）
   ├── 成功 → NTP 同步 → boot_check_update() → tft_restore()
   └── 失败 → 回退 AP 模式 → tft_restore()
9. menu_init() → PG_LOCK
```

---

## 4. 功能清单

### 4.1 侦察
- **WiFi Scan**：扫描附近 AP，显示 SSID/BSSID/信道/RSSI/加密类型，可选中查看详情
- **BLE Scan**：NimBLE 被动扫描 30 秒，显示设备名/RSSI
- **WarDrive**：GPS 记录（需硬件 GPS 模块）

### 4.2 攻击
- **Deauth**：发送 802.11 Deauth 帧，断开目标 WiFi 连接
- **Beacon Spam**：广播大量伪造 SSID 的 Beacon 帧
- **Evil Portal**：Captive Portal 钓鱼（DNS 劫持 + Web 表单）
- **BLE Spam**：BLE 广播垃圾包
- **BadUSB**：BLE HID 键盘注入，支持 Ducky Script（`SD:/badusb_script.txt`）
  - 指令：STRING:、DELAY:、ENTER、TAB、ESC、GUI、CTRL、ALT

### 4.3 防御
- **Defense Mode**：Promiscuous 模式监听 Deauth/Disassoc 帧，蜂鸣器报警 + 计数

### 4.4 网络工具
- **Host Scan**：ARP 扫描局域网存活主机
- **TCP Probe**：TCP 端口探测
- **Traffic Monitor**：实时流量监控（RX/TX 包计数）
- **Port Scan**：终端 `portscan <ip> [start] [end]`，最多 100 端口

### 4.5 系统功能
- **SD 文件浏览器**：浏览/查看/删除文件
- **WebUI**：AP 模式 + Web 服务器（端口 80），Neo 暗黑主题设计
- **Telnet 终端**：端口 23，远程命令执行
- **NTP 时间同步**：终端 `ntpdate`
- **亮度调节**、**蜂鸣器开关**、**重启**

### 4.6 WebUI 功能
- 侧边栏导航 + 暗黑卡片网格
- 实时仪表盘：RAM/Flash/WiFi 状态磁贴
- WiFi 扫描 + 连接管理
- BLE 扫描
- 端口扫描（Web 端）
- Web 文件管理器：上传/下载/删除/目录浏览
- 一键 OTA 固件更新
- BadUSB 脚本编辑/保存
- 终端 WebSocket

### 4.7 WebUI API 端点
| 端点 | 方法 | 功能 |
|------|------|------|
| `/` | GET | 主页（HTML） |
| `/api/sysinfo` | GET | 系统信息（RAM/Flash/WiFi/版本/状态） |
| `/api/wifi_scan` | GET | WiFi 扫描结果 |
| `/api/wifi_connect` | GET | 连接 WiFi（?ssid=&pass=） |
| `/api/wifi_mode` | GET | 查询/设置 WiFi 模式 |
| `/api/ble_scan` | GET | BLE 扫描结果 |
| `/api/portscan` | GET | 端口扫描（?ip=&start=&end=） |
| `/api/files` | GET | 列出 SD 文件 |
| `/api/files/delete` | GET | 删除文件（?path=） |
| `/api/files/mkdir` | GET | 创建目录（?path=） |
| `/api/files/download` | GET | 下载文件（?path=） |
| `/api/files/upload` | POST | 上传文件 |
| `/api/badusb_save` | POST | 保存 BadUSB 脚本 |
| `/api/check_update` | GET | 检查固件更新 |
| `/api/do_update` | GET | 执行 OTA 更新 |
| `/api/update_url` | GET | 获取/设置更新 URL（?url=） |
| `/api/sd_bins` | GET | 列出 SD 卡 .bin 文件 |

### 4.8 终端命令
| 命令 | 功能 |
|------|------|
| `ls [path]` | 列出目录 |
| `cat <file>` | 查看文件内容（最多 200 行） |
| `rm <file>` | 删除文件 |
| `mkdir <dir>` | 创建目录 |
| `rmdir <dir>` | 删除目录 |
| `cd <path>` | 切换目录 |
| `pwd` | 当前目录 |
| `wget <url> [name]` | HTTP 下载到 SD（15s 超时） |
| `wifi scan` | WiFi 扫描 |
| `wifi connect <ssid> <pass>` | 连接 WiFi |
| `ifconfig` | 网络接口信息 |
| `portscan <ip> [start] [end]` | 端口扫描 |
| `ntpdate` | NTP 时间同步 |
| `free` | 内存信息 |
| `df` | SD 卡空间 |
| `sysinfo` | 系统信息 |
| `reboot` | 重启 |
| `help` | 帮助 |

### 4.9 开机自动检查更新 (v2.1.3 新增)
```
WiFi 连接成功 →
  请求 g_update_url 获取 version.json →
  解析 latest.version，与 FW_VERSION 比较 →
  有新版本 →
    显示：版本号/代号/日期/大小/更新日志(最多5条) →
    用户选择：A:立即更新 / B:跳过 →
    A → 下载固件 + Update.write() + 进度条 → ESP.restart()
    B → 继续启动
  已是最新 → 显示 "Already latest!" 1秒后继续
```

---

## 5. ZeroTermux 中继系统

### 5.1 架构
```
[ESP32 设备] ←WiFi→ [手机热点] ←→ [ZeroTermux (Termux)]
                                      ├── WebDAV Server (端口 8080)  ← 固件分发
                                      └── Relay Server (端口 8090)   ← 命令中继
```

### 5.2 文件 (release/ 目录)
| 文件 | 功能 |
|------|------|
| `webdav_server.py` | WebDAV 服务器 + 命令中继（Python） |
| `relay_server.sh` | Bash 中继守护进程 |
| `start_all.sh` | 一键整理+启动+开机自启 |
| `zerotermux_setup.sh` | ZeroTermux 环境初始化 |
| `version.json` | 固件版本清单 |
| `firmware/*.bin` | 固件二进制文件（v2.0.0 ~ v2.1.3） |

### 5.3 自动启动
- **Termux:Boot**：手机开机时自动启动 `start_all.sh`
- **.bashrc**：每次打开终端检测服务是否运行，未运行则启动

---

## 6. 构建配置

### 6.1 platformio.ini
```ini
[env:xiaomiao]
platform = espressif32@5.3.0
board = esp32dev
framework = arduino
monitor_speed = 115200
board_build.partitions = partitions_ota.csv
upload_speed = 921600
board_build.flash_mode = dio
board_build.f_flash = 80000000L
```

### 6.2 分区表 (partitions_ota.csv)
| 分区 | 偏移 | 大小 | 用途 |
|------|------|------|------|
| nvs | 0x9000 | 0x5000 (20KB) | 非易失性存储 |
| otadata | 0xe000 | 0x2000 (8KB) | OTA 数据 |
| app0 | 0x10000 | 0x1E0000 (1.875MB) | OTA 分区 0 |
| app1 | 0x1F0000 | 0x1E0000 (1.875MB) | OTA 分区 1 |
| spiffs | 0x3D0000 | 0x20000 (128KB) | SPIFFS |
| coredump | 0x3F0000 | 0x10000 (64KB) | 核心转储 |

### 6.3 依赖库
| 库 | 版本 | 用途 |
|----|------|------|
| TFT_eSPI | ^2.5.43 | ST7735 显示驱动 |
| NimBLE-Arduino | ^2.3.0 (实际安装 2.5.1) | BLE 协议栈 |
| ArduinoJson | ^6.21.6 | JSON 解析/生成 |
| JPEGDecoder | ^1.8.0 | JPEG 壁纸解码 |

### 6.4 编译结果 (v2.1.3)
```
RAM:   17.5% (57,500 / 327,680 bytes)
Flash: 70.6% (1,387,789 / 1,966,080 bytes)
```

### 6.5 编译命令
```bash
cd /workspace/esp32_xiaomiao
platformio run          # 编译
platformio run -t upload  # 编译+上传
```

### 6.6 发布流程
```bash
cd /workspace/esp32_xiaomiao
cp .pio/build/xiaomiao/firmware.bin release/firmware/xiaomiao_V{VERSION}.bin
sha256sum release/firmware/xiaomiao_V{VERSION}.bin
# 更新 release/version.json
```

---

## 7. 版本历史

### v2.1.3 (2026-08-16) "Neo-UI" — 当前版本
- 修复开机白屏闪烁（重排 SD/WiFi 初始化顺序）
- 新增开机后联网自动检查固件更新（显示更新日志 + A:更新/B:跳过）
- OTA 更新进度条显示
- 修复 16 个高危 bug（内存泄漏、空指针崩溃、竞态条件、HTTPS 支持）
- 修复 15 个中危 bug（硬编码值、WiFi 模式恢复、JSON 转义等）

### v2.1.2 (2026-08-16) "Neo-UI"
- 修复 WebUI "未知状态"问题
- 修复 WiFi 扫描结果字段名不匹配
- 修复检查更新 JSON 解析路径错误
- 修复 wifi_connect 覆写 AP 配置

### v2.1.1 (2026-08-16) "Neo-UI"
- 全新 WebUI Neo 设计（侧边栏 + 暗黑卡片）
- 实时仪表盘
- Web 文件管理器升级
- 一键 OTA 固件更新

### v2.1.0 (2026-08-16) "Feature-Plus"
- 新增 BLE BadUSB 键盘注入
- 新增 TCP 端口扫描器
- 新增 NTP 时间同步
- Web 文件管理器

### v2.0.6 (2026-08-15) "Security-Patch"
- ArduinoJson 安全更新 (CVE-2025-53540)
- OTA CSRF 保护
- NVS 初始化修复
- BLE 内存泄漏修复

### v2.0.0 ~ v2.0.5
- 基础功能开发（WiFi/BLE 扫描、攻击、菜单、TFT 显示）

---

## 8. 已修复 Bug 完整清单 (v2.1.3)

### 高危 (16 个)
1. **开机白屏闪烁** — SD/WiFi 初始化在开机动画后执行，SPI 冲突重置 TFT
2. **BadUSB getInputReport 空指针崩溃** — 未检查返回值
3. **BadUSB GUI/CTRL/ALT 空命令越界** — `k[0]` 访问空字符串
4. **boot_check_update WiFiClientSecure 内存泄漏** — `new` 后未 `delete`
5. **Defense promiscuous 回调帧头解析错误** — 读 `buf[0]` 而非 `payload[0]`
6. **流量计数器竞态条件** — 缺少 `volatile`
7. **/api/do_update stream 空指针崩溃** — `getStreamPtr()` 可能返回 null
8. **/api/do_update 未知长度校验错误** — `(size_t)-1` 变巨大数
9. **/api/do_update 不支持 HTTPS** — 使用废弃的 `http.begin(url)`
10. **/api/check_update 不支持 HTTPS**
11. **wget stream 空指针崩溃**
12. **WiFi BSSID 空指针崩溃** — `WiFi.BSSID(sel)` 可能返回 null
13. **BLE 扫描 getDevice 空指针崩溃**
14. **CPU/MEM 使用率硬编码 327680** — 应使用 `ESP.getHeapSize()`
15. **Flash 使用率硬编码 69%** — 应实时计算
16. **BLE 扫描双重 30 秒等待** — `scan->start(30)` 已阻塞又加 `delay(30000)`

### 中危 (15 个)
1. terminal `cat` 截断检测失效（close 后检查 available）
2. terminal `wifi_scan` 恢复 AP 模式不完整
3. terminal `wifi_connect` 失败后 WiFi 模式未恢复
4. WebUI `wifi_connect` 失败后 WiFi 完全关闭
5. wget 未知长度下载无限挂起（新增 15s 超时）
6. JSON 字符串未转义（新增 `jsonEscape()` 函数）
7. `jsonEscape` 的 `\u` 格式不符合 JSON 规范
8. `SD.remove`/`SD.mkdir` 重复调用导致返回 ERR
9. lock_draw 流量计数器用 TX 基准做 RX 差值
10. boot_check_update 冗余 sscanf 变量覆盖
11. 端口扫描 WebUI 阻塞上限 500→100 端口
12. WebUI 状态显示"未知状态"（dash-status 未更新）
13. WiFi 扫描结果字段名不匹配（rssi→r, channel→ch）
14. 检查更新 JSON 路径错误（d.latest → d.latest.version）
15. 电池电压分压注释错误

---

## 9. 已知未修复问题（低优先级）

以下问题在代码审计中发现但尚未修复，优先级较低：

### 代码质量
- `g_sta_ssid`/`g_sta_pass` 硬编码在源码中，应改为 NVS 持久化
- `g_cfg`（亮度/时区/蜂鸣器等）不从 NVS 加载，重启丢失
- `config.h` 中存在重复宏定义（BZR_PIN/BUZZER_PIN 等）
- `PG_NET_TELNET`/`PG_NET_SSH` 枚举值未使用（死代码）
- setup() 中 `pinMode(BZR_PIN, OUTPUT)` 与 `buzzer_init()` 的 ledc 配置冲突

### 安全
- `/api/files/upload` 路径遍历漏洞（未过滤 `../`）
- `/api/check_update` 直接透传远程 JSON 未校验
- Telnet `telnet_line_buf` 无长度限制（可 OOM）

### 功能
- `attk_portal()` 退出后 WiFi 模式不恢复
- `sys_webui()` 退出后 WiFi 模式不恢复
- `/api/wifi_scan` 在 WebUI 运行中切换 WiFi 模式导致 AP 中断
- menu.cpp 向上导航可选中分类标题
- `recon_wifi()` 扫描期间 AP 关闭，Telnet 客户端断开

### 硬件
- `TFT_RST` 与 `SD_MISO` 共用 GPIO19（硬件设计缺陷，软件层通过 `tft_restore()` 缓解）
- `TEMP_PIN` 与 `BAT_PIN` 共用 GPIO39

---

## 10. 待办事项

### 短期
- [ ] 将 `g_sta_ssid`/`g_sta_pass` 迁移到 NVS 持久化
- [ ] 将 `g_cfg`（亮度/时区/蜂鸣器/锁屏超时）迁移到 NVS
- [ ] 修复 `/api/files/upload` 路径遍历漏洞
- [ ] 修复 `sys_webui()` / `attk_portal()` 退出后 WiFi 模式恢复
- [ ] `/api/wifi_scan` 改用 `WIFI_AP_STA` 模式避免 AP 中断

### 中期
- [ ] Telnet 输入缓冲区长度限制
- [ ] 清理 `config.h` 重复宏定义
- [ ] 删除未使用的枚举值
- [ ] Evil Portal 凭证记录到 SD
- [ ] PCAP 抓包功能完善

### 长期
- [ ] WiFi Pineapple 功能（Karma 攻击）
- [ ] BLE 中继攻击
- [ ] WiFi PMKID 抓取
- [ ] 多语言支持
- [ ] 配置导入/导出

---

## 11. 开发环境

```
开发机: TraeWork 远程沙箱
编译器: platformio (espressif32@5.3.0)
ESP32 工具链: xtensa-esp32-elf-gcc 8.4.0
Python: 3.10 (用于 WebDAV 服务器和发布脚本)
项目路径: /workspace/esp32_xiaomiao/
固件输出: /workspace/esp32_xiaomiao/.pio/build/xiaomiao/firmware.bin
发布目录: /workspace/esp32_xiaomiao/release/
```

### 编译验证
```bash
cd /workspace/esp32_xiaomiao && platformio run 2>&1 | tail -5
# 预期输出:
# RAM:   17.5% (57,500 bytes)
# Flash: 70.6% (1,387,789 bytes)
# ========================= [SUCCESS] Took ~17s =========================
```

---

## 12. 关键代码位置索引

| 功能 | 文件 | 大致行号 |
|------|------|---------|
| 版本定义 | config.h | 14 |
| 全局变量 | main.cpp | 80-110 |
| `jsonEscape()` | main.cpp | 3100 |
| `boot_screen()` | main.cpp | 156 |
| `boot_check_update()` | main.cpp | 182 |
| `lock_draw()` | main.cpp | 503 |
| `recon_wifi()` | main.cpp | 830 |
| `recon_ble()` | main.cpp | 890 |
| `attk_deauth()` | main.cpp | ~1200 |
| `attk_beacon()` | main.cpp | ~1340 |
| `attk_portal()` | main.cpp | ~1430 |
| `attk_badusb()` | main.cpp | ~1800 |
| `attk_defense()` | main.cpp | ~2060 |
| `sys_webui()` | main.cpp | 3117 |
| WebUI HTML (嵌入) | main.cpp | ~2900 |
| API 端点注册 | main.cpp | 3184+ |
| `setup()` | main.cpp | 4109 |
| `loop()` | main.cpp | 4201 |
| `cmd_portscan()` | terminal.cpp | 356 |
| `cmd_wifi_scan()` | terminal.cpp | 229 |
| `cmd_wifi_connect()` | terminal.cpp | 266 |
| `cmd_wget()` | terminal.cpp | 297 |
| 菜单定义 | menu.cpp | 9 |
| 状态栏 | screen.cpp | 230 |
| `tft_restore()` | screen.cpp | ~70 |

---

## 13. 接手指南

如果你是接手此项目的 AI，请按以下步骤开始：

1. **阅读本文档**了解项目全貌
2. **编译验证**：`cd /workspace/esp32_xiaomiao && platformio run`
3. **阅读 config.h**了解硬件引脚和全局变量
4. **阅读 main.cpp 的 setup() 和 loop()**了解主流程
5. **注意 GPIO19 冲突**：TFT_RST 和 SD_MISO 共用，所有 SD 操作后必须 `tft_restore()`
6. **注意 volatile**：`g_traffic_rx`、`g_traffic_tx`、`g_packets_blocked` 在中断回调中写入
7. **注意 WiFi 模式恢复**：任何切换 WiFi 模式的操作（scan/connect）都必须保存并恢复之前的模式
8. **注意 HTTPS**：所有 HTTP 请求必须根据 URL scheme 选择 `WiFiClient` 或 `WiFiClientSecure`
9. **注意 JSON 转义**：所有写入 JSON 的字符串必须通过 `jsonEscape()` 转义
10. **修改后必须编译验证**：`platformio run` 必须无错误
