# 调试与变更日志

本文件补充 Git 之外的人工可追溯记录。Git 已初始化（`main`），初始基线提交见 `git log`；以后每次改动可正常 diff/提交。运行日志和本地配置仍被 `.gitignore` 排除。

## 当前环境

- 日期：2026-10-18
- 版本控制：Git，`main` 分支；初始基线已提交
- 设备：ESP32-S3 N16R8，COM6，FQBN `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi`
- 设备地址：192.168.2.78（本地 DHCP）
- PC 服务：`http://192.168.2.72:8765`，`task_backend=chatgpt`
- 本地 ASR/TTS：faster-whisper small（CPU int8）/ Windows SAPI Huihui
- ChatGPT 桌面应用：UIA 草稿、发送、回答捕获

## 日志位置

| 日志 | 路径 | 说明 |
| --- | --- | --- |
| 当前 Python 服务 stdout/stderr | `host/python/data/stage8_service2.out.log`、`stage8_service2.err.log` | 含任务 ID、转录、回答预览、HTTP 状态；本地、被 `.gitignore` 排除 |
| 上一轮服务日志 | `host/python/data/stage8_service.err.log` | 503 修复、task 586618/297165/893283 等记录 |
| 稳定性采样 | `host/python/data/stage8_longrun.log` | 早期 10 分钟采样，已停止，仅 90 字节 |
| 设备串口 | 未自动落盘 | 用下面的复位捕获命令临时读取 |
| 任务产物 | `host/python/data/task_*.wav`、`*_reply.wav`、`*_screen.bin`、`*_prompt.bin` | 本地任务音频/回答/摘要位图；被 `.gitignore` 排除 |

服务日志包含语音转录和回答，属于本地隐私数据，不要提交或外传。

## 串口调试日志捕获

下面的命令会通过 DTR/RTS 复位设备并读取 15 秒启动日志。GPIO0/BOOT 必须保持松开，否则会进入下载模式。

```powershell
$port = New-Object System.IO.Ports.SerialPort "COM6",115200,None,8,one
$port.ReadTimeout = 500
try {
    $port.Open()
    $port.DtrEnable = $false
    $port.RtsEnable = $true
    Start-Sleep -Milliseconds 250
    $port.RtsEnable = $false
    $end = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $end) {
        $chunk = $port.ReadExisting()
        if ($chunk) { [Console]::Out.Write($chunk) }
        Start-Sleep -Milliseconds 100
    }
} finally {
    if ($port.IsOpen) { $port.Close() }
}
```

2026-10-18 实测启动日志包含：

```text
WiFi: connected; SSID=<redacted> IP=192.168.2.78 RSSI=-35 dBm
PSRAM: record=256000 reply=7340032
Status: RUNNING
```

## 2026-10-18 变更与实测记录

### Git 基线

- 初始化 Git（`main`），初始提交 `2acb09d`，共 40 个项目文件。
- `wifi_config.h`、`voice_config.h`、`lan_config.h`、`config.local.json`、`host/python/data/` 均未进入提交；示例配置只含占位值或局域网地址。
- 首次提交包含 `docs/OLED` 与 `docs/主控资料` 下的 PDF；仓库约 54 MB，后续推送到远端时注意大文件。

### 服务端

- 修复 ChatGPT 应用关闭时的未捕获 UIA 异常：`create_pending_task()` 将草稿写入失败统一映射为 `chatgpt_unavailable`，HTTP 返回 503；修复前为空回复/HTTP 000。
- 实测：关闭 ChatGPT → 503；重开约 15 秒后 task `346995` 创建成功，取消后输入框清空。
- 新增 `GET /api/v1/tasks/{id}/prompt`：生成并缓存 128×32、512 字节的任务摘要位图（任务 ID + 转录摘要），供设备确认前显示。
- 实测：task `782728` 的 `/prompt` 返回 200、512 字节，预览可读。

### 设备固件

- OLED 增加 Wi-Fi 状态页：`WIFI CONNECT`、`WIFI OK`、`WIFI LOST`，显示实际 SSID 和 IP；待机页第三行显示 SSID。
- 增强重连：运行中掉线后按 `RECONNECT_INTERVAL_MS` 重试；重连成功恢复原屏幕。
- 回答屏显示后保留 10 秒再回待机；若 TTS 超过 10 秒，在播放结束后回待机。
- 回答音频接收缓冲从 2 MB 提高到 7 MB（`PSRAM: reply=7340032`），并提供 6/5/4/2 MB 回退；下载超时提高到 30 秒，增加“音频过大/下载不完整”串口日志。
- 增加中文按钮状态位图：GPIO0 按住“正在录音”、松开“正在上传”；GPIO39 “正在发送”；GPIO40 “正在取消发送”。
- 待确认页交替显示任务摘要位图和 `TASK <id> / 39=SEND 40=CLEAR` 提示页；摘要不可用时禁止 GPIO39 确认；在提示页按 GPIO39 会先切换到摘要页，第二次按下才发送。
- 空闲 60 秒熄屏：IDLE 无操作后发送 SSD1306 `0xAE` 关屏，RGB 绿色 LED 1 秒亮/1 秒灭；按键、Wi-Fi 状态变化或任务屏幕更新会唤醒并重置计时。息屏后的第一次按键只唤醒并恢复待机，GPIO0 需松开后再次按住才录音。串口已捕获 `Display: idle sleep; green LED blinks every 1 s.`；OLED/LED 视觉效果待目视确认。
- 编译/烧录：最新固件 990,163 字节（75% Flash），烧录 hash 校验通过，设备重新联网，服务健康 200。

### 已验证的故障场景

- 草稿过期：task `339638` 55 秒后设备自动 cancel，ChatGPT 输入框清空。
- Python 服务不可用：连续两次 `Task request HTTP -1`，设备未崩溃；重启服务后 task `604654` 完成，TTS 播放 537,760 帧。
- ChatGPT 应用关闭/恢复：关闭返回 503；重开后 task `346995` 创建成功。
- 短回答音频：task `586618`，563,406 字节，正常。
- 长回答音频超限：task `297165` 2,977,166 字节、task `893283` 3,166,446 字节，旧 2 MB 缓冲拒绝播放；已提高到 7 MB，待重新提问验证。
- 中文状态页/任务摘要/二次确认：已烧录，待 OLED 目视验证。

## 未解决与待验证

- Wi-Fi 断网/恢复：OLED 状态和 SSID 显示已实现；待关闭设备 2.4G AP 约 20 秒做目视验证。
- 长回答 TTS 重新播放验证（7 MB 缓冲）。
- 中文“正在录音/发送/取消发送/上传”实际 OLED 可读性。
- 待确认页摘要与提示页交替、二次确认行为。
- 长时间运行稳定性（复位、死锁、任务泄漏）。
- 安全边界：当前为明文 HTTP + 来源 IP 允许列表，没有 TLS、设备令牌或 HMAC；仅适用于受信局域网。生产加固需要 TLS（或加密隧道）和设备级认证。
- Git 已初始化（`main`），初始基线已提交；后续改动请继续小步提交，便于 diff。
- 之前一次工具输出意外带出 Wi-Fi 凭据；建议更换密码并更新本地配置。本日志不复述凭据。

## 追加模板

```text
### YYYY-MM-DD HH:MM
- 目标：
- 修改文件：
- 命令/操作：
- 结果（HTTP/task ID/串口关键行）：
- 未解决问题：
```
