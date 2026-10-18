# ESP32 Arduino IDE 工程

使用 Arduino IDE 建立并打开 ESP32 工程。当前目录用于存放 `.ino` 主文件、配置示例及后续固件源文件；请先阅读项目根目录 `AGENTS.md` 与 `docs/hardware.md`。

开发板为 `ESP32S3 Dev Module`（ESP32-S3 N16R8）。阶段 1 实测确认实机为 ESP32-S3 QFN56 rev 0.2、16 MB Flash、8 MB OPI PSRAM；后续编译固定使用 FQBN `esp32:esp32:esp32s3` 加板卡选项 `FlashSize=16M,PSRAM=opi`（完整写作 `esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi`），并使用独立系统临时构建目录。默认 `PSRAM=disabled`、`FlashSize=4M` 不适用于本实物。编译命令见根目录 `README.md`。不要将个人 Wi-Fi 凭据或密钥写入版本控制；本地配置应基于示例文件另行保存并确保被 `.gitignore` 排除。

阶段 1 已通过真实硬件验证；阶段 2 验收项已完成：OLED 最小显示与状态集成、Wi-Fi 成功连接与失败重试上限、按键 GPIO39/40、RGB GPIO48 均已验证（用户目视确认 RGB 正常）；唯一未单独实测的是连接建立后掉线路径。阶段 3 已通过：I2S 麦克风 GPIO4/5/6，16 kHz、32-bit、mono/left，静音 RMS 约 2.3–2.7k，说话/拍手时约 40k–174k。阶段 4 已通过：MAX98357A 音调序列（440/660/880 Hz）用户已确认听到。阶段 5 已通过：Python 服务来源校验（192.168.2.78 allowed、192.168.2.72 403）和 ESP32 心跳 HTTP 200。阶段 6 已通过：ESP32 录音→HTTP→本地 faster-whisper small ASR + SAPI TTS→MAX98357A 播放，实机两次识别“你好,设备测试”。阶段 7 ChatGPT 输入框流程已通过：设备语音→ChatGPT 草稿→GPIO39 发送→回答 TTS 播放；GPIO40 清空草稿；OLED 显示 `39=SEND 40=CLEAR`。录音期间 RGB 绿灯，SEND 绿闪 2 次，CLEAR 黄闪 2 次；ChatGPT 回答以 128×32 中文位图显示在 OLED。Codex CLI 只读替代流程通过，交互式 TUI 启动验证但模型超时。阶段 8 进行中：正常端到端、草稿过期、服务不可用/恢复、ChatGPT 应用关闭/恢复已通过；阶段 7 固件已新增 OLED Wi-Fi 状态/SSID 显示与回答屏保留 10 秒（已烧录，Wi-Fi 断网目视验证待做）；长时间运行待测。阶段 2 Wi-Fi 使用与阶段 1 相同的 FQBN 选项，命令见根目录 `README.md`。GPIO 接线遵循 `docs/hardware.md` 中最终确认的配置；引脚连通性与模块电气参数仍待实物验证。
