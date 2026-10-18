# 开发进度与阶段验收

调试与变更记录：`docs/debug-log.md`（Git 已初始化；运行日志和本地配置仍被 `.gitignore` 排除）。

当前状态：**阶段 1 已通过真实硬件验证**（COM6；ESP32-S3 QFN56 rev 0.2、双核 240 MHz、16 MB Flash、8 MB OPI PSRAM；启动横幅、`Status: RUNNING` 与持续递增运行秒数已确认）。**阶段 2 验收项已完成**：OLED 最小显示与状态集成、Wi-Fi 成功连接与失败重试上限、按键 GPIO39/40、RGB 状态灯 GPIO48 均已验证。唯一未单独实测的是“连接建立后掉线”路径；当前代码检测到掉线后会停止重试，需复位后重来。阶段 3 已通过（I2S 麦克风录音数据、采样参数和稳定性已验证）。阶段 4 已通过（MAX98357A 音调序列，用户确认听到）。阶段 5 已通过（Python 服务来源校验有效，ESP32 心跳 HTTP 200）。阶段 6 音频回环已通过：ESP32 录音→HTTP 上传→本地 faster-whisper small ASR→SAPI TTS→ESP32 播放；实机两次识别为“你好,设备测试”，ASR+TTS 约 2.1 s。GPIO0/BOOT 复位时必须松开，否则进入下载模式。阶段 7 只读流程已通过：任务 ID 绑定、60 秒过期、单一任务、来源校验；GPIO39 确认/GPIO40 取消；只读 Codex 调度与结果 TTS 回传已验证（task 659384）。交互式 Codex TUI 已实机启动（task 666000），但模型请求超时（Reconnecting/Request timed out）；原生界面已确认，执行待重试。阶段 7 主流程（ChatGPT 输入框）已通过：设备语音→Whisper→ChatGPT 草稿→GPIO39 发送→回答 TTS 播放；GPIO40 清空草稿（task 454728）。录音绿灯、SEND 绿闪 2 次、CLEAR 黄闪 2 次、ChatGPT 回答 128×32 中文位图显示到 OLED 已实现（task 529388/604133 显示回答并播放 TTS）。本文区分实测结果、历史记录与未验证项。

| 阶段 | 内容 | 验收标准 | 状态 |
| --- | --- | --- | --- |
| 1 | ESP32-S3 最小程序的编译、上传与串口运行验证 | 编译、上传成功；串口启动信息与运行秒数可读 | **已通过真实硬件验证**：COM6；ESP32-S3 QFN56 rev 0.2、双核 240 MHz、16 MB Flash、8 MB OPI PSRAM；使用 `FlashSize=16M,PSRAM=opi`；启动横幅、`Status: RUNNING` 与递增运行秒数已确认 |
| 2 | Wi-Fi、OLED（首项：最小显示验证）、按键、状态灯 | Wi-Fi状态、IP、RSSI可读；断网后重试有上限；OLED 最小显示可读；按键/状态灯分项验证 | **验收项已完成**：OLED 最小显示/状态集成、Wi-Fi 成功连接（IP `192.168.2.78`、RSSI 约 -35~-53 dBm）+ 失败重试上限、按键 GPIO39/40、RGB GPIO48 均验证；连接建立后掉线路径未单独实测（代码停止重试，需复位） |
| 3 | I²S 麦克风 | 单独验证录音数据、采样参数和稳定性 | **已通过真实硬件验证**：WS/SCK/SD=GPIO4/5/6；16 kHz、32-bit、mono/left；约 16,128 样本/秒；静音 RMS 约 2.3–2.7k，说话/拍手 RMS 40k–174k、p-p 最高约 12.6M；连续 39 个窗口稳定 |
| 4 | MAX98357A 扬声器播放 | 单独验证播放、音量和稳定性 | **已通过真实硬件验证**：DIN/BCLK/LRC=GPIO7/15/16；24 kHz、16-bit、stereo；440/660/880 Hz + 静音循环；用户确认听到对应音调（音量主观可闻，未做定量测量） |
| 5 | Python 服务与局域网通信 | 服务来源校验有效；设备与服务消息交换符合协议 | **已通过真实硬件验证**：服务仅用 Python 标准库；localhost health 200；电脑 LAN IP 192.168.2.72 请求 403；ESP32 192.168.2.78 每 5 秒 heartbeat，HTTP 200 ACK；设备 ID `esp32-s3-24ec4a125c54`；无命令执行 |
| 6 | 语音识别与合成 | 输入输出链路按设计验证，错误状态可识别 | **音频回环已通过**：本地 faster-whisper small ASR + SAPI TTS；PC 端 1.38–2.67 s；ESP32 录音 2.24–3.01 s → HTTP 200 → 播放 46k–66k 帧；实机两次识别“你好,设备测试”；阶段 7 未开始 |
| 7 | Codex CLI 调度与安全确认 | 保留 CLI 原生权限/审批；确认只针对当前明确展示的任务 | **ChatGPT 输入框流程已通过**：设备语音→Whisper→ChatGPT 桌面输入框草稿；GPIO39 发送并捕获回答 TTS（task 286459，74,480 帧）；GPIO40 清空草稿（task 454728）；录音绿灯、SEND 绿闪、CLEAR 黄闪、OLED 中文回答显示已实现（task 529388/604133）；Codex CLI 只读替代流程通过；阶段 8 进行中，见后文 |
| 8 | 整机联调与故障恢复 | 端到端运行及断网、重连等恢复场景通过 | 进行中：正常端到端、草稿过期、服务不可用/恢复、ChatGPT 应用关闭/恢复已通过；Wi-Fi OLED 状态/SSID 与回答 10 秒保留已实现并烧录；待 Wi-Fi 断网目视验证和长时间运行 |

每阶段记录实际测试方法、结果、硬件/软件配置和未解决问题。当前阶段验收完成前不得进入下一阶段。

## 阶段 1 状态明细（2026-10-17；后续运行状态据用户反馈更新）

- **代码已生成：是。** 新增 `firmware/esp32/stage1_minimal_test/` 独立 Arduino 草图；默认串口模式，OLED 代码留作确认硬件后启用。
- **已编译：是。** 2026-10-17 使用 Arduino IDE 2.3.10，在 `ESP32S3 Dev Module`、16 MB Flash、OPI PSRAM、Flash Mode QIO 80MHz、USB CDC On Boot Enabled 配置下，对 OLED 关闭的串口模式成功执行“验证”。输出：Sketch 303,360 bytes（23%），全局变量 22,808 bytes（6%）。未安装依赖；此模式不需要 OLED 库。Partition Scheme 为界面所示的 Default 4MB with SPIFFS，因此此次编译按该分区的 1.25 MB 应用空间上限完成。
- **已上传到真实硬件：是。** 2026-10-17 Arduino IDE 通过 COM6 写入固件，输出显示 `Hash of data verified` 并执行 `Hard resetting via RTS pin`。
- **实机运行验收（2026-10-17 当时记录）：** 用户报告草图持续输出 `Status: RUNNING` 和递增秒数。2026-10-18 本助手已亲自上传并读取串口复测，结果见下方归档；OLED 未启用/验证。串口监视器设为 115200 baud。
- **当前输出：** 串口启动信息、芯片型号、运行状态和每秒更新的运行秒数。OLED 启用后显示项目名、运行状态和运行秒数。
- **接线图已由用户确认：** 麦克风 GPIO4/5/6、MAX98357A GPIO7/15/16、I²C 显示 SDA GPIO41/时钟 GPIO42。该接线尚未等同于实际通电验证。
- **实物验证结果（2026-10-18 更新）：** 开发板型号与 Flash/PSRAM 已由实测确认（ESP32-S3 QFN56 rev 0.2、16 MB Flash、8 MB OPI PSRAM），串口通过 COM6/115200 验证；详见下方“阶段 1 串口最小测试结果归档”。仍待阶段 2 验证：OLED 控制器、I²C 地址及 GPIO42 模块端信号命名（接线图写 SCK）、OLED 实际显示；电源规格与 USB 串口方式的最终记录待后续核对。
- **Arduino IDE 建议：** `ESP32S3 Dev Module` 为文档暂定的通用板型。N16R8 标记通常对应 16 MB Flash 与 8 MB OPI PSRAM，但须从实际模组丝印/板卡资料确认；Flash 模式和 USB/串口菜单也依实物确认。不要照搬通用板默认值。官方菜单说明要求按实物 Flash/PSRAM 配置，R8 对应 8 MB OPI PSRAM。
- **OLED 依赖：** 开启 `ENABLE_OLED` 后需 `Adafruit GFX Library` 和 `Adafruit SSD1306`；`Wire` 随 Arduino-ESP32 core 提供。库未确认、未安装。控制器须与 SSD1306 库兼容，并由用户确认 I²C 地址后再启用。

## 阶段 1 串口最小测试结果归档（2026-10-18）

**归档结论：阶段 1 已通过真实硬件验证。** 验收范围按 `AGENTS.md` 第 5 节：ESP32-S3 最小程序的编译、上传与串口运行验证。

### 实机信息（本次实测确认）

- 芯片：ESP32-S3（QFN56），revision v0.2。
- CPU：双核 240 MHz。
- Flash：16 MB。
- PSRAM：8 MB，OPI（执行 `PSRAM=opi` 构建后固件报告 8 MB；esptool 报告 `Embedded PSRAM 8MB (AP_3v3)`）。
- 软件：Arduino-ESP32 Core 3.3.12，固件打印 SDK `v5.5.5`。
- 串口：COM6，115200 baud，启动横幅与每秒运行状态均可读取。

### 板卡选项（重要）

- 默认 FQBN `esp32:esp32:esp32s3` 对应 `PSRAM=disabled`、`FlashSize=4M`，**不适用于本 N16R8 实物**。按默认配置上传后程序仍可运行，但启动横幅显示 `PSRAM size: 0 MB`，即 PSRAM 未初始化。
- 实机成功使用的板卡选项：`FlashSize=16M,PSRAM=opi`。
- 完整 FQBN：`esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi`。
- 其余保持默认：Flash Mode QIO 80MHz、Partition Scheme Default 4MB with SPIFFS、USB CDC On Boot Disabled、上传速率 921600。
- ROM 首段启动日志显示 `mode:DIO` 属正常现象（ROM bootloader 初始阶段）；二级 bootloader 按 QIO 配置运行。

### 推荐编译命令（仅编译，构建目录在系统临时目录）

```powershell
& 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' compile `
  --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi' `
  --build-path (Join-Path $env:TEMP 'esp32-stage1-build') `
  'E:\ESP32-Codex-Voice-Terminal\firmware\esp32\stage1_minimal_test'
```

上传、复位和串口监视需按项目规则经明确批准后单独执行。

### 实测启动输出（RTS 复位后读取）

```text
ESP32-Codex-Voice-Terminal
Stage 1 minimal hardware test
Chip: ESP32-S3
Cores: 2, Revision: 2
Flash size: 16 MB
PSRAM size: 8 MB
SDK: v5.5.5
Status: STARTING
Serial baud: 115200
OLED: disabled; running serial-only test.
Status: RUNNING
Status: RUNNING | Uptime: 0 s
Status: RUNNING | Uptime: 1 s
...（持续每秒递增）
```

### 本次相关草图改动（已实机验证）

- `setup()` 中 `Serial.begin()` 后的固定 `delay(300)` 改为有界等待（最多约 2 秒），避免复位/USB CDC 重枚举后启动横幅丢失。
- 启动横幅新增 `Cores/Revision`、`Flash size`、`PSRAM size`、`SDK` 四行，用于核对板卡配置。
- `ENABLE_OLED` 仍为 0；未安装任何库；未修改 OLED 配置和 GPIO 分配。

### 验收范围说明（2026-10-18 用户裁决）

- 以 `AGENTS.md` 第 5 节的阶段定义为准：阶段 1 为开发板编译、烧录和串口测试；OLED 属于阶段 2。
- OLED 最小显示验证不是阶段 1 验收项，已移入阶段 2，作为 OLED 外设验证的第一项。
- OLED 相关验证已在阶段 2 完成：控制器 SSD1306、I²C 地址 0x3C、实际显示均已确认；GPIO42 模块端信号命名按接线图使用（图中标 `SCK`，I²C 时钟通常称 `SCL`）。

## 阶段 2（验收完成）

> **状态：验收项已完成。** 阶段 1 已通过真实硬件验证。阶段 2 按 `AGENTS.md` 包含 Wi-Fi、OLED、按键和状态灯；四项均已分项验证。唯一未单独实测的是“连接建立后掉线”路径，当前代码在掉线时停止重试并要求复位。

> **板卡选项已统一（2026-10-18）：** 根目录 `README.md` 的阶段 2 Wi-Fi 编译命令已改为与阶段 1/OLED 相同的 `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi`，并使用系统临时构建目录；旧命令中的 `USBMode=hwcdc,CDCOnBoot=cdc` 等选项已移除。

- 2026-10-18 使用该统一配置编译成功：Sketch 849,114 bytes（64%），全局变量 44,932 bytes（13%）。后续已上传实测，结果见下。

### 阶段 2 Wi-Fi 实机测试（2026-10-18）

- **上传：** 使用统一 FQBN `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi` 上传 COM6 成功，`Hash of data verified`。
- **串口结果：** 启动横幅正常；打印 `WiFi: waiting 1 s for the power rail to stabilize.` 后出现 `E BOD: Brownout detector was triggered` 与 `rst:0x3 (RTC_SW_SYS_RST)`；65 秒内约 122 次欠压复位，从未进入 `WiFi: starting attempt`，未取得 IP/RSSI。
- **已做的软件缓解：** 关闭 `MIRROR_LOG_TO_UART0`（修复 `Serial`/`Serial0` 重复日志）；Wi-Fi 初始化前等待 1 秒；曾尝试把发射功率设为 8.5 dBm，但 `WiFi.setTxPower` 返回失败、实际未生效，该调用已移除。上述措施未消除 BOD，最终通过两路 Type-C 供电解决。
- **诊断：** BOD 发生在 Wi-Fi 射频初始化阶段，属供电余量/瞬态电流问题；OLED 最小显示测试可运行，说明 I²C/显示代码本身无问题。当前不能判定 Wi-Fi 连接功能失败，而是未能在现有供电下启动射频。
- **供电修复后复测（2026-10-18）：** 用户给主模块两路 Type-C 同时供电后，Wi-Fi 在 attempt 1/3 连接成功；IP `192.168.2.78`，RSSI 约 -35~-48 dBm；连续观察 65 秒（uptime 70→135 s）稳定，无 BOD。
- **失败/重试路径验证（2026-10-18）：** 使用编译期开关 `WIFI_TEST_FORCE_FAILURE` 构建不存在的 SSID（未修改本地 `wifi_config.h`）。实测 attempt 1/3、2/3、3/3 均为 `NO_SSID_AVAILABLE`，每次 15 秒超时、间隔 5 秒；第 3 次后输出 `retry limit reached; no more attempts until reset.`，设备保持 `Status: RUNNING | WiFi: DISCONNECTED | Attempt: 3/3`，不再重试；无 BOD。
- **结论：** Wi-Fi 成功连接路径与失败/重试上限均已验证，Wi-Fi 子项通过；正式固件已恢复并重新连接（IP `192.168.2.78`）。按键随后验证，状态灯最终改用 GPIO48 并目视确认正常。

### 阶段 2 OLED + Wi-Fi 状态集成（2026-10-18）

- **代码：** `stage2_wifi_test.ino` 扩展为 Wi-Fi + SSD1306 状态显示；仍只使用 core 自带 `Wire`/`WiFi.h`，未安装外部库。
- **启动日志：** `OLED: SSD1306 ready at 0x3C.`；Wi-Fi `CONNECTED`（attempt 1/3），IP `192.168.2.78`，RSSI 约 -45~-53 dBm；无 BOD。
- **OLED 预期内容：** 第一行 `WIFI OK`，第二行 `IP 192.168.2.78`，第三行 `RSSI -xx`；连接中显示 `WIFI WAIT` + `TRY n/3`，失败显示 `WIFI FAIL` + `TRY n/3`。
- **目视确认：** 用户已确认 OLED 状态画面显示正常（WIFI OK / IP / RSSI）。

### 阶段 2 按键 + RGB 状态灯测试（2026-10-18）

- **代码：** 新增 `firmware/esp32/stage2_button_led_test/stage2_button_led_test.ino`；不启用 Wi-Fi/OLED，OLED 保留上一画面。
- **接线：** GPIO48 → WS2812 兼容三色 RGB，使用 core 自带 `rgbLedWrite`；GPIO39、GPIO40 → 微动按钮到 GND，`INPUT_PULLUP`、低电平有效、30 ms 去抖。
- **编译/上传：** 使用统一 FQBN `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi` 成功，COM6 上传并校验通过。
- **串口结果：** GPIO39 与 GPIO40 的 `PRESSED`/`RELEASED` 事件均被正确记录；双键同时按下也有事件；心跳稳定，无 BOD。
- **LED 映射：** GPIO39 按下=红，GPIO40 按下=蓝，双按=绿，松开=灭。
- **引脚修正（2026-10-18）：** 核对小智源码 `main/boards/bread-compact-wifi/config.h`：`BUILTIN_LED_GPIO=GPIO48`，按键 GPIO39/40，OLED SDA41/SCL42 128×32，麦克风 GPIO4/5/6，功放 GPIO7/15/16，与实板一致。此前使用的 GPIO8 来自 ESP32-C3 原理图，不是本 S3 板；改用 GPIO48 复测并通过。
- **目视确认：** 2026-10-18 用户确认 GPIO48 RGB 自检（红/绿/蓝/白/灭）及按键颜色映射正常。

### 阶段 2 范围（按 `AGENTS.md`）

1. Wi-Fi：已通过（成功连接 IP/RSSI + 3 次失败重试上限）。
2. OLED：最小显示已通过；OLED+Wi-Fi 状态集成已通过（用户目视确认 WIFI OK / IP / RSSI）。
3. 按键：GPIO39/40 串口事件验证通过（低电平有效、30 ms 去抖）；与业务逻辑/OLED 的整合留待后续。
4. 状态灯：GPIO48 RGB 已通过，用户目视确认自检和按键颜色正常。

### 阶段 2 OLED 最小显示测试（2026-10-18）

- **代码：** 新增 `firmware/esp32/stage2_oled_test/stage2_oled_test.ino`；仅使用 Arduino-ESP32 core 自带 `Wire`，不依赖外部库；直接发送 SSD1306 初始化/显示命令。
- **硬件：** SSD1306 128×32；SDA GPIO41、SCL GPIO42；VCC 3V3、GND；模块 SDA/SCL 4.7K 上拉（模块原理图 R3/R4）。
- **编译：** 使用 FQBN `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi` 成功；Sketch 308,543 bytes（23%），全局变量 24,036 bytes（7%）。
- **上传：** COM6 成功，`Hash of data verified`，RTS 复位。
- **串口结果：** I2C 扫描找到 `0x3C`；SSD1306 初始化命令发送成功；全屏点亮、清屏、测试画面传输均成功；`Status: RUNNING`，运行秒数持续递增（连续观察 35 秒稳定）。
- **屏幕内容：** 2026-10-18 用户目视确认屏幕显示 `OLED OK`、`SSD1306`、`0x3C` 及闪烁块。
- **复位观察：** 使用手动 RTS 脉冲捕获启动日志时曾出现多次 `BROWNOUT_RST`；改用 esptool 标准 hard reset 后启动干净、运行稳定。该现象按测试方法/供电余量记录，暂不判定为 OLED 故障；若再次复现需检查供电与 USB 端口。
- **结论：** OLED 最小显示子项已通过（串口侧 + 目视确认）；阶段 2 继续进行 Wi-Fi/按键/状态灯。

### 阶段 2 历史记录：Wi-Fi 连接测试（2026-10-18，未通过串口/实机验证）

> 以下为早期记录；当时的编译选项（USB CDC On Boot Enabled）已不再使用，当前统一配置见上文。

- **目标范围：** 仅测试 STA Wi-Fi 连接与串口日志；不启用 OLED、麦克风、MAX98357A、按键、状态灯、录音、语音或 Codex 命令执行。
- **代码已生成：是。** 新增 `firmware/esp32/stage2_wifi_test/stage2_wifi_test.ino`，仅用 Arduino-ESP32 Core 自带 `WiFi.h`。连接最多尝试 3 次，每次等待最多 15 秒，失败间隔 5 秒；最终失败后停止重试，复位后重来。运行日志每秒输出状态/秒数，成功时输出 Wi-Fi 状态、IP 和 RSSI。
- **本地凭据：** 复制 `wifi_config.example.h` 为 `wifi_config.h` 并填写 SSID/密码；真实配置文件路径已在 `.gitignore` 中排除。没有配置文件时草图可编译，运行时提示且不发起连接。
- **编译状态：成功。** 使用 Arduino IDE 自带的 `arduino-cli` 1.5.1、Arduino-ESP32 Core 3.3.12，以 `ESP32S3 Dev Module`、16MB Flash、OPI PSRAM、QIO 80MHz、USB CDC On Boot Enabled、Default 4MB with SPIFFS 配置进行全量干净编译。输出：Sketch 869,047 bytes（66%），全局变量 45,332 bytes（13%）。首次增量构建在 Arduino 临时缓存 `core.a` 处发生链接损坏；使用 `--clean` 后未修改源代码即编译成功。
- **上传状态：成功。** 初次上传因 COM6 被 Arduino IDE 串口监视器占用而失败；用户关闭占用后，使用 COM6 成功上传。esptool 检测到 ESP32-S3、240MHz 和 8MB 内嵌 PSRAM，写入后的 hash 已验证，并通过 RTS 复位。
- **串口与 Wi-Fi 实机验证：尚未完成。** 115200 baud 监视器可连接至 COM6 并读到 ESP-ROM 启动日志和应用入口，但连续 40 秒未收到草图应有的 `Status: RUNNING` 或 Wi-Fi 日志。本地 `wifi_config.h` 尚不存在，因此即便草图串口恢复，也预计先报告缺少本地配置而不会连接 Wi-Fi。
- **验收方法：** 按根目录 `README.md` 中命令，以 COM6 上传、115200 baud 监视；验证 `CONNECTED`、分配 IP、RSSI 和持续运行秒数。使用错误密码/不可用 AP 验证至多三次尝试、每次最长 15 秒且间隔 5 秒，最终出现 retry limit 并保持运行日志。
- **剩余确认：** 用户本地填写要测试的 Wi-Fi SSID 和密码；保证所选网络及 ESP32 所在位置可接入 2.4 GHz Wi-Fi（ESP32-S3 不支持 5 GHz）。

## 阶段 3：I²S 麦克风录音测试（2026-10-18）

- **代码：** 新增 `firmware/esp32/stage3_mic_test/stage3_mic_test.ino`；仅使用 Arduino-ESP32 core 自带 `ESP_I2S`，不依赖外部库。
- **接线：** 麦克风 WS=GPIO4、SCK=GPIO5、SD=GPIO6（用户已确认）。
- **配置：** I2S 标准模式，16000 Hz，32-bit slot，mono，left slot（适配 INMP441/ICS43434，L/R 接 GND）。
- **编译/上传：** 统一 FQBN `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi` 成功，COM6 上传并校验通过。
- **实测结果：** 约 16,128 样本/秒（1 秒统计窗口）；静音基线 RMS 约 2,300–2,700、p-p 约 15k–18k；说话/拍手时 RMS 升至约 40k–174k、p-p 最高约 12.6M；ZCR 随信号变化；连续 39 个窗口无读空/错误。
- **结论：** I2S 麦克风录音数据、采样参数和运行稳定性已验证，阶段 3 通过。
- **下一步：** 阶段 4 MAX98357A 扬声器播放测试。

## 阶段 4：MAX98357A 扬声器播放测试（2026-10-18）

- **代码：** 新增 `firmware/esp32/stage4_speaker_test/stage4_speaker_test.ino`；仅使用 Arduino-ESP32 core 自带 `ESP_I2S`，不依赖外部库。
- **接线：** MAX98357A DIN=GPIO7、BCLK=GPIO15、LRC=GPIO16；VIN/GND=3V3/GND，SD 接 VIN，GAIN 接 GND。
- **配置：** I2S 标准模式，24000 Hz，16-bit，stereo；播放 440/660/880 Hz 各 1 秒，随后 1 秒静音，循环。
- **编译/上传：** 统一 FQBN `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi` 成功；COM6 上传并校验通过。
- **串口结果：** 音调序列日志循环正常：`440 Hz → 660 Hz → 880 Hz → silence`。
- **听觉确认：** 2026-10-18 用户确认听到 440/660/880 Hz 对应音调，序列循环稳定；音量主观可闻，未做定量测量。
- **结论：** 阶段 4 通过；阶段 5 尚未开始。

## 阶段 5：Python 服务与局域网通信（2026-10-18）

- **服务：** `host/python/app.py`，仅使用 Python 标准库；本地配置 `host/python/config.local.json`（gitignored），测试时绑定 `0.0.0.0:8765`，允许设备 IP `192.168.2.78`，localhost 仅用于诊断。
- **协议：** `docs/protocol.md` v1；`GET /api/v1/health` 和 `POST /api/v1/device/heartbeat`。
- **来源校验：** localhost health 返回 200；电脑局域网 IP `192.168.2.72` 的请求返回 403；ESP32 `192.168.2.78` 被允许。
- **设备端：** `firmware/esp32/stage5_lan_client/stage5_lan_client.ino`；复用阶段 2 的 Wi-Fi 配置，服务地址在本地 `lan_config.h`（gitignored）；每 5 秒发送 heartbeat。
- **实测日志：** ESP32 连续返回 `HTTP 200` 与 `heartbeat accepted`；服务端记录 `source=192.168.2.78 device_id=esp32-s3-24ec4a125c54 uptime_s=1..36 rssi=-41..-45 firmware=stage5_lan_client`。
- **安全边界：** 只接受状态心跳；不执行命令、不调用 Codex CLI；请求体上限 4096 字节；凭据/本地配置不进入 Git。
- **结论：** 阶段 5 通过；阶段 6 音频回环随后已通过，见下。

## 阶段 6：语音识别与合成（2026-10-18）

- **PC 端端点：** 新增 `POST /api/v1/audio/utterance`，接收 16 kHz、16-bit、mono/stereo WAV（上限 512 KB）；ASR 使用本地 `faster-whisper small`（CPU int8，模型缓存），TTS 使用 Windows SAPI zh-CN（Microsoft Huihui Desktop）；只做固定回声/状态回复，不执行命令。
- **PC 端实测：** 上传 147,406 字节 WAV；SAPI 阶段识别为“你好设备可是今天天气怎么样”；升级 Whisper 后对设备录音识别为 **“你好,设备测试”**；模型加载 2.24 s，ASR+TTS 总耗时 2.67 s。
- **设备端：** 新增 `firmware/esp32/stage6_voice_loop/stage6_voice_loop.ino`；GPIO0（BOOT）按住录音、松开上传、接收 WAV 并用 MAX98357A 播放；编译/上传成功。
- **设备端实测：** 多次录音（1.84–3.01 s）均返回 `HTTP audio POST: 200`，播放 46,640–66,240 帧 16 kHz 音频；Whisper 升级后两次识别均为 **“你好,设备测试”**，服务端 ASR+TTS 约 2.09–2.14 s。
- **录音电平与归一化：** 设备 WAV 峰值约 600–2,802、RMS 85–388；服务端对 ASR 输入做去直流和 10.7–40 倍增益归一化（峰值到 30,000），Whisper 识别效果显著改善。
- **启动注意：** GPIO0/BOOT 作为语音键；复位/上电时按住会进入下载模式。松开 BOOT 后重新上电即可正常启动；运行期按住录音不影响。
- **结论：** 音频回环（录音→上传→ASR→TTS→播放）已通过；ASR 为本地 faster-whisper small，SAPI 保留为回退；阶段 6 已通过，阶段 7/8 见后文。

## 阶段 7：Codex CLI 调度与安全确认（2026-10-18，进行中）

- **服务端任务状态机：** `POST /api/v1/tasks/request?mode=read_only|interactive`（音频）或 `POST /api/v1/tasks/text`（PC 测试）创建待确认任务；`POST /api/v1/tasks/{id}/confirm` 确认；`POST /api/v1/tasks/{id}/cancel` 取消；`GET /api/v1/tasks/{id}` 查询状态。
- **安全约束：** 单一开始仅一个 pending/running/interactive 任务；任务 ID 为 6 位数字；60 秒有效期；确认必须匹配当前任务 ID；来源 IP 允许列表；服务端控制台打印完整任务文本和 ID，设备 OLED 交替显示任务 ID + 转录摘要和按键提示；摘要不可用时禁止 GPIO39 确认；识别文本只作为 Codex prompt，不作为 Windows 命令执行。
- **Codex 调度：** read-only 使用原生 `codex.exe exec --sandbox read-only --skip-git-repo-check -C <project> <prompt>`；interactive 使用 `CREATE_NEW_CONSOLE` 打开原生 Codex TUI。未使用 `--dangerously-bypass-approvals-and-sandbox`、`--approve-for-me` 或 `danger-full-access`。
- **实测（read-only）：** 通过 PC 文本入口创建 task `414963`，确认后 `codex exec` 运行约 130 秒并 `exit=0`，返回“当前已完成并实机验证阶段 1–6，语音回环正常，下一阶段为尚未开始的阶段 7…”。沙箱为 read-only，未修改项目文件。
- **本机 Codex 路径：** PATH 上的 `codex` 是 npm `.cmd` shim，服务直接调用会 `WinError 2`；本地 `config.local.json` 已指向 npm 包内的原生 `codex.exe`。该路径为本地配置，不进入 Git。
- **设备端实测：**
  - task `273179`：语音创建的 ASR 文本不理想（“读取docs-prgre ss”），但被确认后只读 Codex 仍正确读取 `docs/progress.md` 并返回结果；结果音频 748,526 B 超过旧 512 KB 缓冲，未播放，随后把设备接收缓冲提升到 2 MB。
  - task `324000`：转录“测试取消”；GPIO40 取消返回 `HTTP 200 status=cancelled`，未派发 Codex。
  - task `659384`：转录“總結當前階段”；GPIO39 确认后 read-only Codex 完成，设备轮询到 `done`，结果 TTS 播放 254,240 帧（约 15.9 秒），串口输出 `Task result audio played.`。
- **任务文本显示：** 服务端可见控制台打印 `PENDING TASK <id>` 与完整文本；设备 OLED 在待确认页交替显示 `任务<id> <转录摘要>` 位图和 `TASK <id> / 39=SEND 40=CLEAR` 提示页。摘要不可用时禁止 GPIO39 确认；在提示页按 GPIO39 会先切回摘要页，第二次按下才发送。task 273179 的误确认说明确认前核对任务内容不可省略；完整文本仍保留在 PC 控制台。
- **交互式模式：** task `666000` 已实机启动原生 Codex TUI，工作目录和 prompt 正确，原生审批界面保留；模型请求出现 `Reconnecting... Request timed out`（Codex 端 app-server/网络超时），任务未完成执行，待网络恢复后重试。
- **状态：** 只读流程通过；交互式 Codex 待演练；阶段 8 进行中，见后文。

### 阶段 7 主流程：ChatGPT 桌面输入框（2026-10-18，已通过）

- **目标流程：** ESP32 录音 → Whisper ASR → 服务端把文本写入 ChatGPT 桌面输入框（草稿）→ GPIO39 发送 → 捕获 ChatGPT 回答 → SAPI TTS → MAX98357A 播放；GPIO40 清空草稿。
- **实现：** 默认 `task_backend=chatgpt`；UIA 脚本 `chatgpt_set_draft.ps1`、`chatgpt_clear_draft.ps1`、`chatgpt_send_capture.ps1`；任务 request 写入草稿，confirm 发送并捕获，cancel 清空。
- **设备实测：**
  - task `286459`：语音转录“请回复”→ChatGPT 草稿→GPIO39 发送→ChatGPT 回答“你想让我回复什么内容？”→TTS 播放 74,480 帧（约 4.65 秒）。
  - task `454728`：GPIO40 清空草稿，返回 `Cancel HTTP 200 status=cancelled`。
  - task `446779`：取消端点验证，ChatGPT 输入框从草稿恢复为占位符“询问 ChatGPT”。
  - 捕获脚本问题已修正：取最后一条用户消息，避免把整个对话历史当作回答。
- **安全：** 草稿在 ChatGPT 输入框中直接可见；GPIO39 只发送当前任务 ID 对应的草稿；GPIO40 清空；55 秒设备侧过期并调用 cancel 清空；来源 IP 校验；服务只做 UIA 文本写入/发送/读取，不执行 Windows 命令；TTS 只朗读回答。
- **已知限制：** ChatGPT 桌面 UIA 依赖 Chromium/Electron 可访问性（ProseMirror 控件）；应用更新可能导致脚本失效；回答按文本控件收集、遇到按钮停止，代码块可能被截断；TTS 截断 400 字符。
- **Codex CLI 替代流程：** read-only `codex exec --sandbox read-only` 已通过；交互式 TUI 启动已验证但模型请求超时。

### 阶段 7 视觉反馈与 OLED 回答（2026-10-18）

- **RGB 反馈：** 录音期间 GPIO48 绿灯常亮；GPIO39 SEND 时绿灯闪 2 次（每次 0.5 秒）；GPIO40 CLEAR 时黄灯闪 2 次（每次 0.5 秒）。
- **OLED 回答：** `render_oled.ps1` 把 ChatGPT 回答渲染为 128×32 1-bit 位图；`GET /api/v1/tasks/{id}/screen` 下发 512 字节；设备显示并保留回答画面。
- **实测：** task `529388` 和 `604133` 均输出 `Answer screen displayed`，TTS 分别播放 182,640 帧（约 11.4 秒）和 95,440 帧（约 6.0 秒）；task `555209` GPIO40 清空成功。
- **待确认：** RGB 颜色/闪烁和 OLED 中文回答的可读性需要用户目视确认。

## 阶段 8：整机联调与故障恢复（2026-10-18，进行中）

测试清单已写入 `tests/README.md`。正常端到端流程已通过（task `529388`/`604133`：ChatGPT 草稿→GPIO39→回答 TTS/OLED）。本轮继续验证：

- 草稿过期自动清理（55 秒，设备 cancel 清空 ChatGPT 输入框）。
- Python 服务不可用与恢复。
- ChatGPT 桌面应用关闭与恢复。
- Wi-Fi 断网与重连。
- 长时间运行稳定性。

**已完成：**

- **正常端到端：** task `529388`/`604133` 完成，回答屏幕和 TTS 正常。
- **草稿过期：** task `339638` 在 55 秒后由设备自动取消，ChatGPT 输入框恢复为“询问 ChatGPT”。
- **服务不可用/恢复：** 停止服务后设备连续两次 `Task request HTTP -1`，未崩溃；重启服务后 task `604654` 完成，OLED 显示回答，TTS 播放 537,760 帧（约 33.6 秒）。
- **ChatGPT 应用关闭/恢复：** 关闭应用后 UIA 草稿脚本失败。修复 `create_pending_task`，将 UIA 异常统一映射为 `chatgpt_unavailable`；修复前为空回复（HTTP 000），修复后返回 503。重新打开应用约 15 秒后 task `346995` 创建成功并取消。
- **Wi-Fi 状态/SSID 显示：** 阶段 7 固件新增 OLED `WIFI CONNECT`、`WIFI OK`、`WIFI LOST` 页面，显示实际 SSID（`WiFi.SSID()`）和 IP；补全小写字母、`_`、`:` 等字体；空闲页第三行显示 SSID。已编译、烧录，设备恢复联网（ping 成功）；断网/恢复的 OLED 目视验证待做。
- **回答保留 10 秒：** 回答屏显示时启动 10 秒计时；TTS 超过 10 秒时在播放结束后回待机；Wi-Fi 重连恢复回答屏时重新计时。已编译、烧录，待端到端目视验证。
- **按钮中文状态页（待目视验证）：** GPIO0 按住显示“正在录音”，松开上传显示“正在上传”；GPIO39 显示“正在发送”；GPIO40 显示“正在取消发送”。使用 16px SimSun 位图 `status_bitmaps.h`，已编译、烧录。
- **确认前任务摘要（待目视验证）：** 新增 `GET /api/v1/tasks/{id}/prompt`，返回任务 ID + 转录摘要的 128×32 位图；设备在待确认页交替显示摘要和 `TASK <id> / 39=SEND 40=CLEAR`。摘要不可用时禁止 GPIO39 确认；在提示页按 GPIO39 会先切换到摘要页，第二次按下才发送。已编译、烧录。
- **安全边界：** 当前仍为明文 HTTP + 来源 IP 允许列表，没有 TLS、设备令牌或 HMAC，仅适用于受信局域网；已记录在 `docs/protocol.md`，生产加固需 TLS 与设备级认证。
- **长回答音频（待复测）：** task `893283`（“牙疼怎么办”）的 TTS WAV 为 3,166,446 字节，旧 2 MB 缓冲区导致设备拒绝播放；task `297165` 为 2,977,166 字节，同样超限。已将 `MAX_REPLY_BYTES` 提高到 7 MB，并提供 6/5/4/2 MB 回退；音频下载超时提高到 30 秒，增加“音频过大/下载不完整”串口日志。已编译、烧录，待重新提问验证播放。

**待测：** Wi-Fi 断网/恢复目视验证（OLED 状态、SSID、恢复后任务）、回答 10 秒回待机、长回答 TTS 播放、长时间运行。阶段 8 通过前不标记完成。







