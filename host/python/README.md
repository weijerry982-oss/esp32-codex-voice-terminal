# Windows Python 服务

阶段 5 最小局域网服务：只接受允许来源的 JSON 心跳，返回 ACK；不执行命令，也不调用 Codex CLI。

## 依赖

- Python 3.10 及以上。
- 仅使用 Python 标准库；`requirements.txt` 无外部包。

## 运行

```powershell
python host/python/app.py --config host/python/config.local.json
```

- `config.example.json` 是安全默认示例：只监听 `127.0.0.1`。
- 局域网测试使用 `config.local.json`（已被 `.gitignore` 排除）。阶段 5 实测配置：
  - `bind_host`: `0.0.0.0`（仅用于可信局域网测试）
  - `port`: `8765`
  - `allowed_device_ips`: `["192.168.2.78"]`
  - `allow_localhost`: `true`（本机诊断用）
- 如果设备 IP 变化，需要更新允许列表；不要把允许列表扩大到不可信网段。

## 端点

- `GET /api/v1/health`
- `POST /api/v1/device/heartbeat`

完整字段、错误码和安全边界见 `docs/protocol.md`。

## 安全边界

- 每个请求校验 TCP 来源 IP；非允许来源返回 `403` 并记录日志。
- 请求体上限 4096 字节；只接受 `application/json`；校验必需字段。
- 阶段 5 没有命令通道；服务不会执行任意 Windows 命令，也不会调用 Codex CLI。
- Wi-Fi 密码、令牌和密钥不得写入 Git；`config.local.*`、`secrets.*` 等已被 `.gitignore` 排除。

## 阶段 5 实测（2026-10-18）

- localhost `GET /api/v1/health`：`200`。
- 电脑局域网 IP `192.168.2.72` 发起的请求：`403 source_not_allowed`。
- ESP32 `192.168.2.78` 每 5 秒发送 heartbeat：`HTTP 200` + `heartbeat accepted`；服务端记录 `device_id=esp32-s3-24ec4a125c54`。

## 阶段 6 音频端点（已通过）

- `POST /api/v1/audio/utterance`：接收 `audio/wav`，16 kHz、16-bit、mono/stereo，上限 512 KB。
- 处理：本地 `faster-whisper small`（CPU int8）中文 ASR → 固定回声/状态回复 → Windows SAPI zh-CN TTS；响应 `audio/wav`，并带 URL 编码的 `X-Transcript` 和 `X-Reply-Text` 头。
- faster-whisper 模型缓存在用户目录，不在 Git；SAPI 作为 Whisper 不可用时的回退。不执行识别文本或任意命令。
- 2026-10-18 本机/设备实测：设备录音识别为“你好,设备测试”；模型加载 2.24 s，ASR+TTS 2.09–2.67 s；TTS 播放 46k–66k 帧。

## 阶段 7 任务确认 / ChatGPT 输入框（已通过）

- 默认 `task_backend=chatgpt`：task request 将 ASR 文本写入 ChatGPT 桌面输入框（草稿）；confirm 发送并捕获回答；cancel 清空输入框；`GET /api/v1/tasks/{id}/audio` 返回回答 TTS。
- UIA 脚本：`chatgpt_set_draft.ps1`、`chatgpt_clear_draft.ps1`、`chatgpt_send_capture.ps1`；要求 ChatGPT 桌面应用已打开。
- 安全：草稿在 ChatGPT 输入框中可见；GPIO39 才发送；GPIO40 清空；55 秒设备过期并清空；来源 IP 允许列表；服务不执行 Windows 命令，只朗读回答。
- 实测：task `286459` 设备语音→ChatGPT 草稿→GPIO39 发送→回答 TTS 74,480 帧；task `454728` GPIO40 清空成功；task `446779` 取消端点把输入框恢复为“询问 ChatGPT”。
- Codex CLI 替代流程仍可用：read-only `codex exec --sandbox read-only`；交互式 TUI 启动验证但模型请求超时。
- 已知限制：ChatGPT UIA 依赖 Chromium/Electron 可访问性；应用更新可能导致脚本失效；代码块可能被截断；TTS 截断 400 字符。
- OLED 回答：`render_oled.ps1` 生成 128×32 1-bit 位图，`GET /api/v1/tasks/{id}/screen` 返回 512 字节；设备显示中文回答。
- 视觉实测：task `529388`/`604133` 显示回答，TTS 182,640/95,440 帧；task `555209` GPIO40 清空成功。
