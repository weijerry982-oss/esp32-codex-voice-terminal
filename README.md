# ESP32-Codex-Voice-Terminal

基于 ESP32-S3 N16R8 的 Wi-Fi 语音终端项目，目标是让设备与 Windows 电脑上的 Python 服务协作，并在保留 Codex CLI 原生权限与审批机制的前提下提交语音交互任务。

## 目录结构

- `docs/`：硬件、架构、通信协议和进度文档。
- `firmware/esp32/`：ESP32 Arduino IDE 固件工程及配置示例。
- `host/python/`：Windows 电脑端 Python 服务。
- `tests/`：各阶段验收测试计划与后续测试。
- `scripts/`：项目辅助脚本。

## 开发顺序

按 `docs/progress.md` 中阶段 1 至阶段 8 顺序推进。每个阶段通过对应验收后再进入下一阶段：先验证开发板编译、烧录和串口，再验证 Wi-Fi/OLED/按键/状态灯、麦克风、扬声器、局域网服务、语音能力、Codex CLI 安全交互，最后整机联调。

引脚分配目前是待验证配置，以 `docs/hardware.md` 为准；确认后不得擅自更改。实体按钮只确认当前明确展示的待确认任务，不能成为通用批准开关；不得关闭 Codex CLI 的审批机制。

## 阶段 1：串口最小测试（已实机验证）

测试草图位于 `firmware/esp32/stage1_minimal_test/`，默认 `ENABLE_OLED=0`，仅使用 Arduino-ESP32 Core 自带 API，不依赖外部库。实测确认实机为 ESP32-S3 QFN56 rev 0.2、16 MB Flash、8 MB OPI PSRAM。

后续编译固定使用 FQBN `esp32:esp32:esp32s3` 加板卡选项 `FlashSize=16M,PSRAM=opi`，并使用独立系统临时构建目录（不在项目源目录生成构建文件）：

```powershell
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi'
$build = Join-Path $env:TEMP 'esp32-stage1-build'
& $cli compile --fqbn $fqbn --build-path $build firmware/esp32/stage1_minimal_test
```

默认 `esp32:esp32:esp32s3` 的 `PSRAM=disabled`、`FlashSize=4M` 不适用于本 N16R8 实物：按默认配置运行时固件会显示 `PSRAM size: 0 MB`。上传、复位和串口监视需按项目规则经明确批准后单独执行。

## 阶段 2：Wi-Fi 连接测试

阶段二当前验证 ESP32-S3 连接 Wi-Fi、串口日志和 OLED 状态显示，不启用麦克风、功放或按钮。测试草图位于 `firmware/esp32/stage2_wifi_test/`，使用 Arduino-ESP32 Core 自带的 `WiFi.h` 与 `Wire`（SSD1306 直接驱动，无外部库）。

本地 `wifi_config.h` 已创建；请只在本机填写 `WIFI_SSID` 和 `WIFI_PASSWORD`。该文件已加入 `.gitignore`；不要把真实凭据写入模板或提交到版本库。缺少本地配置时草图仍可编译，但只会提示配置缺失，不会尝试连接。

串口使用 115200 baud。草图最多尝试连接 3 次，每次最多等待 15 秒，失败后间隔 5 秒；成功时输出状态、IP 地址和 RSSI，之后每秒输出运行状态和运行秒数。连续失败后须复位开发板才会重新尝试。

阶段 2 Wi-Fi 编译命令已与阶段 1/OLED 已验证配置统一：FQBN `esp32:esp32:esp32s3` 加板卡选项 `FlashSize=16M,PSRAM=opi`，并使用系统临时构建目录。以下为统一后的编译/上传/监视命令（已按该配置实测）：

```powershell
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi'
$build = Join-Path $env:TEMP 'esp32-stage2-wifi-build'
& $cli compile --fqbn $fqbn --build-path $build firmware/esp32/stage2_wifi_test
& $cli upload -p COM6 --fqbn $fqbn --input-dir $build
& $cli monitor -p COM6 --config baudrate=115200
```

该配置对应 `ESP32S3 Dev Module`、16 MB Flash、OPI PSRAM、Flash Mode QIO 80MHz、Default 4MB with SPIFFS、USB CDC On Boot Disabled 及 921600 上传速率。此前命令中的 `USBMode=hwcdc,CDCOnBoot=cdc` 等选项已移除，避免与已验证串口路径不一致。草图仍保留 UART0 日志镜像；在该配置下 `Serial` 与 `Serial0` 可能指向同一 UART0，实际运行若出现重复日志，再关闭镜像。

**供电要求（2026-10-18 实测）：** Wi-Fi 射频启动电流较大；仅接单路弱 USB 供电时可能触发 `BROWNOUT_RST`。实测给主模块两路 Type-C 同时供电后连接正常（IP `192.168.2.78`，RSSI 约 -35~-53 dBm）。仍建议使用稳定 5V/1A+ 电源；若出现 BOD，先检查供电。详见 `docs/progress.md`。

**失败路径验证：** 可用 `--build-property "compiler.cpp.extra_flags=-DWIFI_TEST_FORCE_FAILURE"` 构建不可用 SSID 的测试固件；实测 3 次尝试、每次 15 秒、间隔 5 秒，第 3 次后停止重试，设备继续运行。

## 阶段 2：按键 + RGB 状态灯测试

测试草图位于 `firmware/esp32/stage2_button_led_test/`。GPIO39、GPIO40 外接微动按钮到 GND（`INPUT_PULLUP`，低电平有效，30 ms 去抖）；GPIO48 接 `XL-5050RGBC-WS2812B`（WS2812B 兼容三色 RGB，小智 bread-compact-wifi 的 `BUILTIN_LED_GPIO`），使用 Arduino-ESP32 core 自带 `rgbLedWrite`。按 GPIO39 为红、GPIO40 为蓝、双按为绿、松开熄灭；串口 115200 输出按键事件。2026-10-18 实测 GPIO48 RGB 正常；若颜色顺序不符，再调整颜色顺序参数。

## 阶段 3：I²S 麦克风录音测试

测试草图位于 `firmware/esp32/stage3_mic_test/`，使用 Arduino-ESP32 core 自带 `ESP_I2S`，不依赖外部库。麦克风 WS/SCK/SD = GPIO4/5/6；配置 16 kHz、32-bit slot、mono、left slot。串口 115200 每秒输出样本数、RMS、峰峰值、最小/最大值、均值和 ZCR。实测静音 RMS 约 2.3–2.7k，说话/拍手时升至约 40k–174k。

```powershell
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi'
$build = Join-Path $env:TEMP 'esp32-stage3-mic-build'
& $cli compile --fqbn $fqbn --build-path $build firmware/esp32/stage3_mic_test
& $cli upload -p COM6 --fqbn $fqbn --input-dir $build
& $cli monitor -p COM6 --config baudrate=115200
```

## 阶段 4：MAX98357A 扬声器播放测试

测试草图位于 `firmware/esp32/stage4_speaker_test/`，使用 Arduino-ESP32 core 自带 `ESP_I2S`，不依赖外部库。MAX98357A DIN/BCLK/LRC = GPIO7/15/16；配置 24 kHz、16-bit、stereo，播放 440/660/880 Hz 各 1 秒后静音 1 秒，循环。串口 115200 输出当前音调。2026-10-18 实测用户确认听到对应音调。

```powershell
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi'
$build = Join-Path $env:TEMP 'esp32-stage4-speaker-build'
& $cli compile --fqbn $fqbn --build-path $build firmware/esp32/stage4_speaker_test
& $cli upload -p COM6 --fqbn $fqbn --input-dir $build
& $cli monitor -p COM6 --config baudrate=115200
```

## 阶段 5：Python 服务与局域网通信

服务位于 `host/python/app.py`，仅使用 Python 标准库。阶段 5 只接受 JSON 心跳并返回 ACK，不执行命令、不调用 Codex CLI。协议见 `docs/protocol.md`。

```powershell
python host/python/app.py --config host/python/config.local.json
```

`config.local.json` 已被 `.gitignore` 排除。阶段 5 实测绑定 `0.0.0.0:8765`，允许设备 IP `192.168.2.78`，localhost 仅用于诊断；其他来源返回 403。

设备端测试草图位于 `firmware/esp32/stage5_lan_client/`，复用阶段 2 的 Wi-Fi 配置，服务地址在本地 `lan_config.h`（gitignored），每 5 秒发送 heartbeat。

```powershell
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi'
$build = Join-Path $env:TEMP 'esp32-stage5-lan-build'
& $cli compile --fqbn $fqbn --build-path $build firmware/esp32/stage5_lan_client
& $cli upload -p COM6 --fqbn $fqbn --input-dir $build
& $cli monitor -p COM6 --config baudrate=115200
```

2026-10-18 实测：localhost health 200；电脑 LAN IP `192.168.2.72` 请求 403；ESP32 `192.168.2.78` 每 5 秒 heartbeat 返回 HTTP 200。

## 阶段 6：语音识别与合成

PC 服务新增 `POST /api/v1/audio/utterance`（协议见 `docs/protocol.md`），使用本地 faster-whisper small 中文 ASR + Windows SAPI zh-CN TTS，只做固定回声/状态回复，不执行命令。设备端草图位于 `firmware/esp32/stage6_voice_loop/`，GPIO0（BOOT）按住录音、松开上传并播放返回的 TTS WAV。

2026-10-18 实测：ESP32 录音 2.24–3.01 秒 → HTTP 200 → 播放 TTS 音频；Whisper 两次识别均为“你好,设备测试”；ASR+TTS 约 2.1 秒。服务端对低电平录音做去直流和增益归一化；faster-whisper 模型缓存在用户目录，不在 Git。

注意：GPIO0/BOOT 在复位/上电时必须保持松开，否则会进入下载模式；运行期按住录音正常。

```powershell
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi'
$build = Join-Path $env:TEMP 'esp32-stage6-voice-build'
& $cli compile --fqbn $fqbn --build-path $build firmware/esp32/stage6_voice_loop
& $cli upload -p COM6 --fqbn $fqbn --input-dir $build
& $cli monitor -p COM6 --config baudrate=115200
```

## 阶段 7：Codex/ChatGPT 语音输入（ChatGPT 输入框流程已通过）

主流程：ESP32 录音 → Whisper ASR → 服务端把文本写入 ChatGPT 桌面输入框（草稿）→ GPIO39 发送 → 捕获 ChatGPT 回答 → SAPI TTS → MAX98357A 播放；GPIO40 清空草稿。

2026-10-18 实测：设备语音转录后写入 ChatGPT 输入框，GPIO39 发送并播放回答 TTS 74,480 帧（约 4.65 秒）；GPIO40 清空草稿成功。服务使用 `task_backend=chatgpt` 和 UIA 脚本 `chatgpt_set_draft.ps1`、`chatgpt_clear_draft.ps1`、`chatgpt_send_capture.ps1`，只做 UI 文本操作，不执行 Windows 命令。

视觉反馈：录音期间 RGB 绿灯；GPIO39 SEND 绿闪 2 次（0.5 秒）；GPIO40 CLEAR 黄闪 2 次（0.5 秒）。ChatGPT 回答由 `render_oled.ps1` 渲染为 128×32 位图并显示在 OLED 上；task `529388`/`604133` 已实测显示回答并播放 TTS。

Codex CLI 替代流程仍可用（read-only `codex exec --sandbox read-only`；交互式 TUI 启动验证但模型请求超时）。已知限制：ChatGPT UIA 依赖 Chromium/Electron 可访问性，应用更新可能导致脚本失效；TTS 截断 400 字符。

## 阶段 8：整机联调与故障恢复（进行中）

阶段 8 继续使用 `firmware/esp32/stage7_task_confirm/`。完整流程为：GPIO0 按住录音 → 松开上传并由本地 Whisper 识别 → 服务端写入 ChatGPT 草稿并生成任务摘要位图 → OLED 交替显示任务摘要和 `TASK <id> / 39=SEND 40=CLEAR` → GPIO39 确认发送 → ChatGPT 回答显示在 OLED 并播放 TTS → GPIO40 取消。

已实现并烧录：

- Wi-Fi 状态页：`WIFI CONNECT` / `WIFI OK` / `WIFI LOST`，显示实际 SSID 和 IP；待机页第三行显示 SSID；运行中掉线后自动重连。
- 空闲 60 秒熄屏：IDLE 无操作 60 秒后关闭 OLED，RGB 绿色 LED 以 1 秒亮/1 秒灭闪烁；按任意键、Wi-Fi 状态变化或任务屏幕更新会唤醒并重置计时。息屏后的第一次按键只唤醒并恢复待机，不执行按键动作；GPIO0 需松开后再次按住才开始录音。
- 中文按钮状态：GPIO0 按住显示“正在录音 + 剩余秒数（20→0）”、松开显示“正在上传”；GPIO39“正在发送”；GPIO40“正在取消发送”。
- 确认前任务摘要：`GET /api/v1/tasks/{id}/prompt` 返回 128×32 位图（任务 ID + 转录摘要）。摘要不可用时禁止 GPIO39 确认；当前显示按键提示页时，第一次按 GPIO39 只切换到摘要页，第二次才发送。
- 回答文字分页：服务端按 20 字/页生成并附带每页时长；设备随 TTS 播放逐页切换 OLED，不再只显示第一屏。
- 录音文字分页：确认前按 20 字/页轮播完整转录，与任务 ID/按键提示交替显示；确认保护逻辑不变。
- 错误恢复：服务端有运行中任务时新录音收到 409，OLED 显示 `BUSY`；任务在服务重启后丢失（404）时显示 `LOST` 并清空；运行超过 5 分钟显示 `TIMEOUT`；上传时服务不可达显示 `SERVICE OFFLINE`；ChatGPT 应用不在聊天视图/草稿不可写（503）时显示 `CHATGPT OFFLINE / OPEN CHAT VIEW`。
- 回答屏最后一页在播放结束后保留 10 秒；TTS 超过 10 秒时不会在播放中途回待机。
- 发送成功后 OLED 每秒显示 `WAITING <秒>S`，直到回答、错误或任务结束。
- ChatGPT 捕获上限提高到 240 秒，并过滤“正在思考/正在搜索/时间戳”等临时 UI 文本，避免把占位提示当成回答。
- 录音最长 20 秒（约 640 KB WAV），服务端音频上传上限提高到 1 MB。
- 回答音频接收缓冲为 6 MB（为 20 秒录音和 WAV 缓冲留出 PSRAM，带 5/4/2 MB 回退），下载超时 30 秒；当前 400 字 TTS 上限约 3.2 MB，6 MB 足够。
- ChatGPT 应用关闭时，服务返回 `503 chatgpt_unavailable`（修复前会断开连接/HTTP 000）。

已通过：

- 正常端到端 task `529388` / `604133`。
- 草稿过期 task `339638`，55 秒后自动取消并清空 ChatGPT 输入框。
- 服务不可用：连续 `Task request HTTP -1`，设备未崩溃；服务重启后 task `604654` 完成。
- ChatGPT 应用关闭/恢复：关闭返回 503；重开后 task `346995` 创建成功。
- 任务摘要端点：task `782728` 返回 200、512 字节，位图预览可读。
- 长回答捕获：task `382014`（西京中学中考）曾因 120 秒超时和无回答占位文本失败；提高捕获时限并过滤临时文本后，task `264302` 捕获到 277 字真实回答并完成。

待实机验证：关闭设备使用的 2.4G Wi-Fi 约 20 秒后的 OLED 状态与恢复、长回答 TTS 重新播放、中文状态/任务摘要/二次确认的目视效果、长时间运行稳定性。

```powershell
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$fqbn = 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi'
$build = Join-Path $env:TEMP 'esp32-stage8-build'
& $cli compile --fqbn $fqbn --build-path $build firmware/esp32/stage7_task_confirm
& $cli upload -p COM6 --fqbn $fqbn --input-dir $build
& $cli monitor -p COM6 --config baudrate=115200
```

GPIO0/BOOT 在复位或上电时必须松开，否则进入下载模式。设备语音只作为 ChatGPT/Codex 输入，不拼接为 Windows 命令。

**安全边界：** 当前为明文 HTTP + 来源 IP 允许列表，没有 TLS、设备令牌或 HMAC；只适用于受信局域网，不得暴露到公网。生产加固需要 TLS（或加密隧道）和设备级认证，详见 `docs/protocol.md`。

**日志：** 调试与变更记录见 `docs/debug-log.md`；Python 服务日志和任务音频位于 `host/python/data/`，已被 `.gitignore` 排除。阶段 8 通过前不标记完成。
