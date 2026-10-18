# 设备与电脑服务通信协议（v1，阶段 8）

本文定义 ESP32 终端与 Windows Python 服务之间的最小局域网通信。当前阶段 8 仍只验证受信局域网内的语音任务、确认和恢复；不执行任意 Windows 命令。

## 传输与安全边界

- 传输：HTTP/1.1 over TCP，请求和响应均为 UTF-8 JSON。
- 默认端口：`8765`。
- 默认监听：`127.0.0.1`（仅本机）。可信局域网测试必须显式配置监听地址，并配置来源 IP 允许列表。
- 来源校验：服务端检查 TCP 来源 IP；不在允许列表中的请求返回 `403`。
- **当前安全边界（阶段 8）：** 明文 HTTP + 来源 IP 允许列表；没有传输加密、设备令牌或 HMAC。只适用于受信局域网，不得暴露到公网。生产部署需要 TLS（或加密隧道）和设备级认证。
- localhost（`127.0.0.1` / `::1`）仅用于本机诊断，可由本地配置关闭。
- 请求体上限：4096 字节。
- 阶段 5 不定义命令通道；任何请求都不能直接作为 Windows 命令或 Codex CLI 参数执行。
- 不以明文形式在 Git 中保存 Wi-Fi 密码、令牌或密钥。`config.local.json` 与设备本地配置均被 `.gitignore` 排除。

## 端点

### GET /api/v1/health

用于本机/允许来源的连通性检查。

响应 `200`：

```json
{"ok": true, "service": "codex-voice-service", "protocol_version": 1}
```

### POST /api/v1/device/heartbeat

设备定期上报状态。服务端只记录并返回 ACK，不执行任何操作。

请求头：

```text
Content-Type: application/json
```

请求体：

```json
{
  "protocol_version": 1,
  "device_id": "esp32-s3-24ec4a125c54",
  "message_type": "heartbeat",
  "uptime_s": 123,
  "wifi_rssi": -45,
  "ip": "192.168.2.78",
  "firmware": "stage5_lan_client"
}
```

字段约束：

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `protocol_version` | integer | 必须为 `1` |
| `device_id` | string | 1–64 字符 |
| `message_type` | string | 必须为 `heartbeat` |
| `uptime_s` | integer | ≥ 0 |
| `wifi_rssi` | integer | -127–0 |
| `ip` | string | 合法 IP 地址 |
| `firmware` | string | 1–64 字符 |

成功响应 `200`：

```json
{
  "ok": true,
  "protocol_version": 1,
  "server_time": "2026-10-18T12:34:56+00:00",
  "message": "heartbeat accepted"
}
```

### POST /api/v1/audio/utterance

设备上传一段录音，服务端完成中文 ASR、固定业务回复和中文 TTS，并把回复音频直接返回给设备。阶段 6 只做回声/状态回复，不执行识别文本，不调用 Codex CLI。

请求头：`Content-Type: audio/wav`；可选 `X-Device-Id`。

请求体：16 kHz、16-bit PCM、mono 或 stereo WAV；最大 1 MB（20 秒 mono WAV 约 640 KB）。

响应 `200`：`Content-Type: audio/wav`；响应头 `X-Transcript` 与 `X-Reply-Text` 为 URL 编码的 UTF-8 文本；响应体为 16 kHz、16-bit mono WAV。

错误：`400`（非 WAV/不支持的格式）、`403`（来源不允许）、`413`（音频过大）、`415`（Content-Type 不是 `audio/wav`）。

### 任务确认接口（阶段 7）

阶段 7 的 Codex 任务必须先创建为 pending 任务，再由设备对当前任务 ID 做实体确认。服务端一次只允许一个 pending/running/interactive 任务；pending 任务 60 秒过期。

#### POST /api/v1/tasks/request?mode=read_only|interactive

请求体与 `POST /api/v1/audio/utterance` 相同的 16 kHz WAV；服务端做 ASR 后创建任务，不执行 Codex。

成功响应 `200`：

```json
{
  "ok": true,
  "task_id": "414963",
  "status": "pending",
  "mode": "read_only",
  "transcript": "...",
  "summary": "...",
  "expires_in_s": 60
}
```

已有 pending/running 任务时返回 `409 task_in_progress`。

#### POST /api/v1/tasks/text

PC 端测试/诊断入口，JSON：

```json
{"text": "Read docs/progress.md and summarize the current stage.", "mode": "read_only"}
```

同样只创建 pending 任务，不执行 Codex。

#### POST /api/v1/tasks/{id}/confirm

无请求体。只有当 `id` 等于当前 pending 任务 ID 且未过期时才接受；服务端随后：
- `read_only`：运行 `codex exec --sandbox read-only --skip-git-repo-check`，捕获输出；
- `interactive`：打开原生 Codex TUI 窗口，保留 Codex 自身审批；
- `task_backend=chatgpt`（默认）：把 request 阶段写入 ChatGPT 桌面输入框的草稿发送出去，捕获 ChatGPT 回答并生成 TTS 音频；cancel 清空输入框。该模式要求 ChatGPT 桌面应用已打开，服务只做 UI 文本操作，不执行 Windows 命令。ChatGPT 捕获默认等待上限 240 秒，并忽略“正在思考/正在搜索/时间戳”等临时 UI 文本，避免把占位提示当成回答。

服务端不得使用 `--dangerously-bypass-approvals-and-sandbox`、`--approve-for-me` 或 `danger-full-access`。识别文本只作为 Codex prompt 或 ChatGPT 输入，不作为 Windows 命令执行。

#### POST /api/v1/tasks/{id}/cancel

仅取消当前 pending 任务；running/interactive 任务不能取消。

#### GET /api/v1/tasks/{id}

返回任务状态、模式、转录文本、结果预览和错误信息。设备可轮询。

#### GET /api/v1/tasks/{id}/prompt

返回 512 字节、128×32、page-major 的 1-bit 任务摘要位图，内容为任务 ID 和转录摘要。设备在确认前交替显示该摘要与 `TASK <id> / 39=SEND 40=CLEAR` 提示页；摘要不可用时禁止 GPIO39 确认。位图在首次请求时生成并缓存。

#### GET /api/v1/tasks/{id}/screen

返回 512 字节的回答位图（ChatGPT 回答摘要），供 OLED 显示。

#### GET /api/v1/tasks/{id}/audio

返回 TTS WAV。设备当前回答缓冲上限 6 MB（PSRAM），超过 20 秒录音和 640 KB WAV 缓冲后仍有约 0.8 MB 余量；超限或下载不完整时拒绝播放并记录串口日志。

## 错误响应

| 状态码 | 场景 |
| --- | --- |
| `400` | JSON 无效、字段缺失或格式错误 |
| `403` | 来源 IP 不在允许列表 |
| `404` | 未知路径 |
| `413` | 请求体超过 4096 字节 |
| `415` | `Content-Type` 不是 `application/json` |
| `405` | 不支持的方法（阶段 5 仅实现 GET/POST 端点） |

错误响应示例：

```json
{"ok": false, "error": "source_not_allowed"}
```

## 设备端行为

- 连接 Wi-Fi 后每 5 秒发送一次 heartbeat。
- 打印 HTTP 状态码和响应体。
- 网络错误时保留设备运行并继续重试；阶段 5 不做复杂恢复，阶段 8 再完善。
- 设备端不解析或执行任何返回文本。

## 后续扩展（不在阶段 5 实现）

- 设备令牌或 HMAC 签名（令牌只保存在本地配置，不进入 Git）。
- 任务/确认消息、语音数据通道和 Codex CLI 调度。
- 速率限制、重放保护和更细的来源策略。
