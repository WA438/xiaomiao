XiaoMiaoOS项目进度全览本文档供开发使用，包含完整项目状态、架构、已修复 bug 清单、已知问题和待办事项。
最后更新：2026-08-16|当前版本：v2.1.3|固件大小：1，393，568bytes(闪存70.6%)

1. 项目概述
XiaoMiaoOS是一款基于ESP32的手持渗透测试设备固件，风格参考ESP32掠夺者/布鲁斯.运行在自定义硬件上(ESP32+ST7735128x160TFT+6按键+SD卡+蜂鸣器），提供WiFi/BLE侦察、攻击、防御、网络工具、WebUI管理等功能.

设备通过ZeroTermux(安卓上的Termux)作为中继服务器，实现远程命令执行和固件OTA更新.

2.硬件配置
MCU:ESP32(ESP32-dev，4MB闪存)
屏幕：ST7735128x160(SPI，旋转90°)
按键：6个(UP/DOWN/LEFT/RIGHT/A/B)
SD卡：SPI接口
蜂鸣器：GPIO14(LEDC PWM)
电池：ADC GPIO39(2:1分压)
引脚定义(config.h)
功能GPIO说明
TFT_CS5TFT片选
TFT_DC4TFT数据/命令
TFT_RST19TFT复位(与SD_MISO共用！)
SD_CS22SD片选
SD_MOSI	23	SPI MOSI
SD_MISO19SPI MISO(与TFT_RST共用！)
SD_SCK	18	SPI 时钟
BTN_UP2上
BTN_DOWN13下
BTN_LEFT27左
BTN_RIGHT35右
BTN_A	34	A (确认)
BTN_B	12	B (返回)
蜂鸣器14蜂鸣器
bat_PIN39电池电压ADC
重要硬件约束：TFT_RST(GPIO19)与SD_MISO(GPIO19)共用同一引脚.SD卡初始化会导致TFT复位(白屏闪烁).所有使用SD的功能在初始化后必须调用TFT_restore()恢复显示。

3. 软件架构
3.1 源文件结构
src/
├--config.h(210行)硬件配置、枚举、结构体、全局变量声明
├--main.cpp(4556行)主程序：UI、攻击、防御、WebUI、OTA、启动
├--终端.CPP(672行)telnet服务器、终端命令处理
├--screen.cpp(309行)屏幕初始化、状态栏、文本裁剪、TFT恢复
├--menu.cpp(215行)菜单导航系统
├--按钮.CPP(132行)按键驱动(去抖、长按、重复)
├--蜂鸣器驱动(LEDC PWM)
├--按钮.H(67行)按键接口
├-蜂鸣器.h(58行)蜂鸣器接口
├--菜单.H(11行)菜单接口
├--screen.h(22行)屏幕接口
└--终端.H(21行)终端接口
总计约 6,369 行代码。

3.2 关键全局变量
//WiFi状态
Page g_page;              // 当前页面
WifiMode g_wifi_mode；//WM_OFF/WM_STA/WM_AP
bool g_wifi_conn；//WiFi STA是否已连接
bool GBLE_on；//BLE是否已初始化
AtkState g_wifi_atk；//WiFi攻击状态
AtkState g_ble_atk；//BLE攻击状态
AtkState g_wifi_def；//防御模式状态

//计数器(挥发性-在混杂的回调中写入)
volatile uint32_t g_packets_blocked；//被阻断的攻击包数
volatile uint32_t g_traffic_rx；//接收流量计
volatile uint32_t g_traffic_tx；//传输流量计
uint32_t g_packets_sent；//发送的攻击包数
uint32_t g_beacons_sent；//发送的beacon数
uint32_t g_ble_spam_cnt；//BLE spam计数

//AP配置(非静态的，供终端.cpp引用)
char g_web_ap_ssid[33]="XiaoMiao-CFG"；
char g_web_ap_pass[33]="xiaomiao123"；
uint8_t g_web_ap_ch=1；
uint8_t g_web_ap_max_cli=8；
bool g_web_ap_hidden=false；

//STA凭据(家庭WiFi)
char g_st_ssid[33]="ye"；//←硬编码，应改为NVS持久化
char g_sta_pass[33]= "82813269";

//ota更新url(通过nvs持久化)
char g_update_url[256];

//sd卡状态
XiaoMiaoOS项目进度全览本文档供开发使用，包含完整项目状态、架构、已修复 bug 清单、已知问题和待办事项。
XiaoMiaoOS是一款基于ESP32的手持渗透测试设备固件，风格参考ESP32掠夺者/布鲁斯.运行在自定义硬件上(ESP32+ST7735128x160TFT+6按键+SD卡+蜂鸣器），提供WiFi/BLE侦察、攻击、防御、网络工具、WebUI管理等功能.
设备通过ZeroTermux(安卓上的Termux)作为中继服务器，实现远程命令执行和固件OTA更新.
按键：6个(UP/DOWN/LEFT/RIGHT/A/B)
PG_MENU主菜单导航
PG_RECON_WIFI WiFi扫描+详情查看
PG_RECON_BLE BLE扫描
PG_RECON_WARD Wardrive(GPS记录，需硬件支持）
PG_attk_deauth deauth攻击
PG_attk_BEACON信标垃圾邮件
PG_attk_PORTAL邪恶门户(强制门户)
PG_attk_BLE BLE垃圾邮件
PG_attk_BadUSB BLE BadUSB HID键盘注入
PG_attk_DEFENSE防御模式（deauth检测）
PG_NET_HOST主机扫描(arp)
PG_NET_TCP TCP探测
PG_NET_TRAFFIC流量监控
PG_EXPLOIT PCAP/凭证查看
PG_SYS_FILES SD文件浏览器
PG_SYS_WebUI WebUI(AP模式+Web服务器)
PG_SYS_TERM Telnet终端
PG_SYS_TIME时间/NTP设置
PG_SYS_BRIGHT亮度调节
PG_SYS_BUZZER蜂鸣器开关
PG_SYS_REBOOT重启
设备通过ZeroTermux(安卓上的Termux)作为中继服务器，实现远程命令执行和固件OTA更新.
重要硬件约束：TFT_RST(GPIO19)与SD_MISO(GPIO19)共用同一引脚.SD卡初始化会导致TFT复位(白屏闪烁).所有使用SD的功能在初始化后必须调用TFT_restore()恢复显示。
1.serial初始化(115200)
2.按钮_init()
3.buzzer_init()
4.SCR_init()←TFT初始化
5.SD_init()←SD卡初始化(会重置TFT)
6.TFT_restore()←恢复TFT显示
7.boot_screen()←开机动画(此时显示已稳定)
8.WiFi STA连接(10s超时)
├--成功→NTP同步→boot_check_update()→tft_restore()
└--失败→回退AP模式→TFT_restore()
9.menu_init()→PG_LOCK
4. 功能清单
4.1 侦察
WiFi扫描：扫描附近AP，显示SSID/BSSID/信道/RSSI/加密类型，可选中查看详情
BLE Scan:NimBLE被动扫描30秒，显示设备名/RSSI
Wardrive:GPS记录(需硬件gps模块)
4.2 攻击
deauth：发送802.11deauth帧，断开目标WiFi连接
信标垃圾邮件：广播大量伪造SSID的灯塔帧
邪恶门：捕鱼门钓鱼(dns劫持+web表格)
BLE垃圾邮件：BLE广播垃圾包
BadUSB:BLE HID键盘注入，支持Ducky Script(SD:/BadUSB_script.txt)

指令：string：、DELAY：、ENTER、TAB、ESC、GUI、CTRL、ALT


4.3 防御
防御模式：淫乱模式监测deauth/Disassoc带、蜂鸣器报警+计数
4.4 网络工具
主机扫描：ARP扫描局域网存活主机
TCP探测：TCP端口探测
Traffic Monitor：实时流量监控(RX/TX包计数)
端口扫描：终端portscan<互联网协议>[开始] [结束]，最多 100 端口
4.5 系统功能
SD 文件浏览器：浏览/查看/删除文件
WebUI:AP模式+Web服务器(端口80)，Neo暗黑主题设计
telnet终端：端口23，远程命令执行
NTP时间同步：终端ntpdate
亮度调节、蜂鸣器开关、重启
4.6WebUI功能
侧边栏导航 + 暗黑卡片网格
实时仪表盘：RAM/闪存/WiFi状态磁贴
WiFi扫描+连接管理
BLE扫描
端口扫描(Web端)
Web 文件管理器：上传/下载/删除/目录浏览
一键 OTA 固件更新
BadUSB剧本编辑/保存
终端WebSocket
4.7WebUI API端点
端点	方法	功能
/GET主页(HTML)
/api/sysinfo GET系统信息(RAM/闪存/WiFi/版本/状态）
/api/wifi_scan GET WiFi扫描结果
/api/wifi_connect GET连接WiFi(？SSID=&pass=)
/api/wifi_mode GET查询/设置WiFi模式
/api/ble_scan GET BLE扫描结果
/api/portscan GET端口扫描(？ip=&start=&end=)
/api/files GET列出SD文件
/api/files/delete GET删除文件（？路径=)
/api/文件/mkdir GET创建目录（？路径=）
/api/files/download GET下载文件（？路径=)
/api/文件/上传POST上传文件
/api/badusb_save POST保存BadUSB脚本
/api/check_update GET检查固件更新
/api/do_update GET执行OTA更新
/api/update_url GET获取/设置更新URL(？url=)
/api/sd_bin GET列出SD卡.bin文件
4.8 终端命令
命令	功能
LS[路径]	列出目录
猫<文件>	查看文件内容（最多 200 行）
RM<文件>	删除文件
mkdir<Dir>创建目录
rmdir<Dir>删除目录
CD<路径>切换目录
pwd	当前目录
wget<URL> [姓名]HTTP下载到SD(15s超时）
WiFi扫描WiFi扫描
WiFi连接<SSID><通过>连接WiFi
ifconfig网络接口信息
portscan<互联网协议>[开始] [结束]	端口扫描
ntpdate NTP时间同步
免费的内存信息
DF SD卡空间
sysinfo系统信息
重新启动重启
帮助帮助
4.9 开机自动检查更新 (v2.1.3 新增)
WiFi连接成功→
请求g_update_url获取version.json→
解析最新版本，与FW_VERSION比较→
有新版本 →
    显示：版本号/代号/日期/大小/更新日志(最多5条) →
用户选择：A：立即更新/B：跳过→
a→下载固件+Update.write()+进度条→ESP.restart()
B→继续启动
已是最新→显示"已最新！"1秒后继续
5.ZeroTermux中继系统
5.1 架构
[ESP32设备]←WiFi→[手机热点]←→[ZeroTermux(Termux)]
├--WebDAV服务器(端口8080)←固件分发
└-relay服务器(端口8090)←指令中继
5.2文件(发布/目录)
文件	功能
WebDAV_server.py WebDAV服务器+命令中继(Python)
relay_server.sh Bash中继守护进程
start_all.sh一键整理+启动+开机自启
zerotermux_setup.sh ZeroTermux环境初始化
version.json固件版本清单
固件/*.bin固件二进制文件(v2.0.0~v2.1.3)
5.3 自动启动
Termux：引导：手机开机时自动启动start_all.sh
.bashrc：每次打开终端检测服务是否运行，未运行则启动
6. 构建配置
6.1platformio.ini
[env：小苗]
平台=espressif32@5.3.0
board=esp32dev
framework=arduino
monitor_speed=115200
WiFi扫描+连接管理
upload_speed=921600
board_build.flash_mode=dio
board_build.f_flash=80000000L
6.2分区表(partitions_ota.csv)
分区	偏移	大小	用途
nvs0x90000x5000(20kb)非易失性存赚
otadata	0xe000	0x2000 (8KB)	OTA 数据
app0	0x10000	0x1E0000(1.875MB)OTA分区0
app10x1F000000x1E0000(1.875MB)OTA分区1
spiffs0x3D000000x20000(128KB)spiffs
coredump0x3F000000x10000(64KB)核心转储
6.3 依赖库
库	版本	用途
TFT_eSPI	^2.5.43	ST7735 显示驱动
NimBLE-Arduino	^2.3.0 (实际安装 2.5.1)	BLE 协议栈
ArduinoJson	^6.21.6	JSON 解析/生成
JPEGDecoder	^1.8.0	JPEG 壁纸解码
6.4 编译结果 (v2.1.3)
RAM:   17.5% (57,500 / 327,680 bytes)
Flash: 70.6% (1,387,789 / 1,966,080 bytes)
6.5 编译命令
cd /workspace/esp32_xiaomiao
platformio run          # 编译
platformio run -t upload  # 编译+上传
6.6 发布流程
cd /workspace/esp32_xiaomiao
cp .pio/build/xiaomiao/firmware.bin release/firmware/xiaomiao_V{VERSION}.bin
sha256sum release/firmware/xiaomiao_V{VERSION}.bin
# 更新 release/version.json
7. 版本历史
v2.1.3 (2026-08-16) "Neo-UI" — 当前版本
修复开机白屏闪烁（重排 SD/WiFi 初始化顺序）
新增开机后联网自动检查固件更新（显示更新日志 + A:更新/B:跳过）
OTA 更新进度条显示
修复 16 个高危 bug（内存泄漏、空指针崩溃、竞态条件、HTTPS 支持）
修复 15 个中危 bug（硬编码值、WiFi 模式恢复、JSON 转义等）
v2.1.2 (2026-08-16) "Neo-UI"
修复 WebUI "未知状态"问题
修复 WiFi 扫描结果字段名不匹配
修复检查更新 JSON 解析路径错误
修复 wifi_connect 覆写 AP 配置
v2.1.1 (2026-08-16) "Neo-UI"
全新 WebUI Neo 设计（侧边栏 + 暗黑卡片）
实时仪表盘
Web 文件管理器升级
一键 OTA 固件更新
v2.1.0 (2026-08-16) "Feature-Plus"
新增 BLE BadUSB 键盘注入
新增 TCP 端口扫描器
新增 NTP 时间同步
Web 文件管理器
v2.0.6 (2026-08-15) "Security-Patch"
ArduinoJson 安全更新 (CVE-2025-53540)
OTA CSRF 保护
NVS 初始化修复
BLE 内存泄漏修复
v2.0.0 ~ v2.0.5
基础功能开发（WiFi/BLE 扫描、攻击、菜单、TFT 显示）
8. 已修复 Bug 完整清单 (v2.1.3)
高危 (16 个)
开机白屏闪烁 — SD/WiFi 初始化在开机动画后执行，SPI 冲突重置 TFT
BadUSB getInputReport 空指针崩溃 — 未检查返回值
BadUSB GUI/CTRL/ALT 空命令越界 — k[0] 访问空字符串
boot_check_update WiFiClientSecure 内存泄漏 — new 后未 delete
Defense promiscuous 回调帧头解析错误 — 读 buf[0] 而非 payload[0]
流量计数器竞态条件 — 缺少 volatile
/api/do_update stream 空指针崩溃 — getStreamPtr() 可能返回 null
/api/do_update 未知长度校验错误 — (size_t)-1 变巨大数
/api/do_update 不支持 HTTPS — 使用废弃的 http.begin(url)
/api/check_update 不支持 HTTPS
wget stream 空指针崩溃
WiFi BSSID 空指针崩溃 — WiFi.BSSID(sel) 可能返回 null
BLE 扫描 getDevice 空指针崩溃
CPU/MEM 使用率硬编码 327680 — 应使用 ESP.getHeapSize()
Flash 使用率硬编码 69% — 应实时计算
BLE 扫描双重 30 秒等待 — scan->start(30) 已阻塞又加 delay(30000)
中危 (15 个)
terminal cat 截断检测失效（close 后检查 available）
terminal wifi_scan 恢复 AP 模式不完整
terminal wifi_connect 失败后 WiFi 模式未恢复
WebUI wifi_connect 失败后 WiFi 完全关闭
wget 未知长度下载无限挂起（新增 15s 超时）
JSON 字符串未转义（新增 jsonEscape() 函数）
jsonEscape 的 \u 格式不符合 JSON 规范
SD.remove/SD.mkdir 重复调用导致返回 ERR
lock_draw 流量计数器用 TX 基准做 RX 差值
boot_check_update 冗余 sscanf 变量覆盖
端口扫描 WebUI 阻塞上限 500→100 端口
WebUI 状态显示"未知状态"（dash-status 未更新）
WiFi 扫描结果字段名不匹配（rssi→r, channel→ch）
检查更新 JSON 路径错误（d.latest → d.latest.version）
电池电压分压注释错误
9. 已知未修复问题（低优先级）
以下问题在代码审计中发现但尚未修复，优先级较低：

代码质量
G_sta_ssid/g_sta_pass硬编码在源代码中，应改为NVS持久化
g_cfg（亮度/时区/蜂鸣器等）不从 NVS 加载，重启丢失
config.h中存在重复宏定义(BZR_PIN/蜂鸣器_PIN等)
PG_NET_TELNET/PG_NET_SSH枚举值未使用（死代码）
setup()中pinMode(BZR_PIN，OUTPUT)与buzer_init()的LEDC配置冲突
安全
/api/文件/上传路径遍历漏洞（未过滤../）
/api/check_update直接透过远程JSON未校验
telnet telnet_line_buf无长度限制(可OOM)
功能
attk_portal()退出后WiFi模式不恢复
sys_webui()退出后WiFi模式不恢复
/api/wifi_scan在WebUI运行中切换WiFi模式导致AP中断
menu.cpp 向上导航可选中分类标题
Recon_wifi()扫描期间AP关闭，telnet客户端断开
硬件
TFT_RST与SD_MISO共用GPIO19（硬件设计缺陷，软件层通过TFT_restore()缓解）
temp_PIN与BAT_PIN共用GPIO39
10. 待办事项
短期
将g_sta_ssid/g_sta_pass穿越到nvs持久化
将g_cfg(亮度/时区/蜂鸣器/锁屏超时)迁移到NVS
修复/API/文件/上传路径遍历漏洞
修复sys_webui()/attk_portal()退出后WiFi模式恢复
/api/wifi_scan改用WiFi_AP_STA模式避免AP中断
中期
telnet输入缓冲区长度限制
清理config.h重复宏定义
 删除未使用的枚举值
vehicle Portal凭证记录到SD
pcap抓包功能完善
长期
WiFi菠萝功能(因果报应攻击)
BLE中继攻击
WiFi PMKID抓取
 多语言支持
配置导入/导出
11. 开发环境
开发机：TraeWork远程沙箱
编译器：PlatformIO(espressif32@5.3.0)
ESP32工具链：xtensa-esp32-elf-gcc8.4.0
Python:3.10(用于WebDAV服务器和发布脚本)
项目路径：/workspace/esp32_Xiaomiao/
固件输出：/workspace/esp32_Xiaomiao/.Pio/build/Xiaomiao/firmware。bin
发布目录：/workspace/esp32_Xiaomiao/release/
编译验证
CD/workspace/esp32_Xiaomiao&&PlatformIO run2>&1|tail-5
#预期输出:
内存数：17.5%(57，500字节)RAM:17.5%(57，500bytes)
flash:70.6%(1，387，789字节)flash:70.6%(1，387，789bytes)
#=========================[成功]耗时约17秒 ========================= =========================[成功]耗时约17秒=========================
12. 关键代码位置索引 关键代码位置索引
功能	文件	大致行号
版本定义config.h14
全局变量main.cpp80-110
jsonEscape()main.cpp3100
boot_screen()main.cpp156
boot_check_update()main.cpp182
lock_draw()main.cpp503
Recon_wifi()main.cpp830
Recon_ble()main.cpp890
attk_deauth()main.cpp~1200
attk_beacon()main.cpp~1340
attk_portal()	main.cpp	~1430
attk_BadUSB()main.cpp~1800
attk_defense()main.cpp~2060
sys_webui()main.cpp3117
WebUI HTML(嵌入)main.cpp~2900
API端点注册main.cpp3184+
setup()main.cpp4109
loop()main.cpp4201
CMD_portscan()terminal.cpp356
CMD_wifi_scan()terminal.cpp229
CMD_wifi_connect()terminal.cpp266
CMD_wget()terminal.cpp297
菜单定义menu.cpp9
状态栏screen.cpp230
TFT_restore()screen.cpp~70
13. 接手指南 接手指南
如果你是接手此项目的 AI，请按以下步骤开始：

阅读本文档了解项目全貌
编译验证：CD/workspace/esp32_Xiaomiao&&PlatformIO run
阅读 config.h了解硬件引脚和全局变量
**阅读main.cpp的setup()和loop()**了解主流程
注意GPIO19冲突：TFT_RST和SD_MISO共用，所有SD操作后必须TFT_restore()
注意volatile:g_traffic_rx、g_traffic_tx、g_packets_blocked在中断回调中写入
注意WiFi模式恢复：任何切换WiFi模式的操作(扫描/连接)都必须保存并恢复之前的模式
注意https：所有http请求必根据URL scheme选择WiFiClient或WiFiClientSecure
注意JSON转义：所有写入JSON的字符串必须通过jsonEscape()转义
修改后必须编译实验证明：PlatformIO运行必须错误
