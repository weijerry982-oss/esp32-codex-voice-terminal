#!/usr/bin/env python3
"""Local service for the ESP32 voice terminal.

Stage 5: JSON heartbeat exchange with source-IP validation.
Stage 6: WAV upload -> Windows zh-CN ASR -> fixed business reply -> Windows
         zh-CN TTS -> WAV response.

Only the Python standard library and Windows System.Speech are used.
The service does not execute commands and does not invoke Codex CLI.
"""

from __future__ import annotations

import argparse
import array
import ipaddress
import json
import logging
import math
import random
import re
import struct
import subprocess
import threading
import time
import urllib.parse
import uuid
import wave
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

DEFAULT_CONFIG: dict[str, Any] = {
    "bind_host": "127.0.0.1",
    "port": 8765,
    "allowed_device_ips": [],
    "allow_localhost": True,
    "expected_device_id": "",
    "audio_max_bytes": 1024 * 1024,
    "asr_timeout_s": 30,
    "tts_timeout_s": 30,
    "whisper_model": "small",
    "whisper_device": "cpu",
    "whisper_compute_type": "int8",
    "task_ttl_s": 60,
    "codex_timeout_s": 180,
    "codex_binary": "codex",
    "project_dir": "",
    "task_backend": "chatgpt",
    "chatgpt_timeout_s": 240,
}

PROTOCOL_VERSION = 1
MAX_HEARTBEAT_BYTES = 4096
CONFIG: dict[str, Any] = dict(DEFAULT_CONFIG)

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
ASR_SCRIPT = BASE_DIR / "speech_asr.ps1"
TTS_SCRIPT = BASE_DIR / "speech_tts.ps1"
CHATGPT_SET_DRAFT = BASE_DIR / "chatgpt_set_draft.ps1"
CHATGPT_CLEAR_DRAFT = BASE_DIR / "chatgpt_clear_draft.ps1"
CHATGPT_SEND_CAPTURE = BASE_DIR / "chatgpt_send_capture.ps1"
RENDER_OLED = BASE_DIR / "render_oled.ps1"
RENDER_OLED_PAGES = BASE_DIR / "render_oled_pages.ps1"

WHISPER_MODEL: Any = None
WHISPER_LOCK = threading.Lock()

TASKS: dict[str, dict[str, Any]] = {}
TASKS_LOCK = threading.Lock()
TASK_CREATION_LOCK = threading.Lock()
CURRENT_TASK_ID: str | None = None


def load_config(path: Path | None) -> dict[str, Any]:
    config = dict(DEFAULT_CONFIG)
    if path is not None and path.exists():
        with path.open("r", encoding="utf-8") as handle:
            loaded = json.load(handle)
        if not isinstance(loaded, dict):
            raise ValueError("config root must be a JSON object")
        config.update(loaded)
    return config


def normalize_ip(value: str) -> str:
    return value[7:] if value.startswith("::ffff:") else value


def source_allowed(source_ip: str) -> bool:
    source_ip = normalize_ip(source_ip)
    if CONFIG.get("allow_localhost", True) and source_ip in ("127.0.0.1", "::1"):
        return True
    return source_ip in CONFIG.get("allowed_device_ips", [])


def validate_heartbeat(data: Any) -> str | None:
    if not isinstance(data, dict):
        return "body must be a JSON object"
    if data.get("protocol_version") != PROTOCOL_VERSION:
        return "protocol_version must be 1"
    if data.get("message_type") != "heartbeat":
        return "message_type must be heartbeat"

    device_id = data.get("device_id")
    if not isinstance(device_id, str) or not 1 <= len(device_id) <= 64:
        return "device_id must be a string of 1-64 characters"

    expected_device_id = CONFIG.get("expected_device_id", "")
    if expected_device_id and device_id != expected_device_id:
        return "device_id is not the expected device"

    uptime_s = data.get("uptime_s")
    if not isinstance(uptime_s, int) or isinstance(uptime_s, bool) or uptime_s < 0:
        return "uptime_s must be a non-negative integer"

    wifi_rssi = data.get("wifi_rssi")
    if not isinstance(wifi_rssi, int) or isinstance(wifi_rssi, bool) or not -127 <= wifi_rssi <= 0:
        return "wifi_rssi must be an integer in [-127, 0]"

    ip_value = data.get("ip")
    if not isinstance(ip_value, str):
        return "ip must be a string"
    try:
        ipaddress.ip_address(ip_value)
    except ValueError:
        return "ip must be a valid IPv4 or IPv6 address"

    firmware = data.get("firmware")
    if not isinstance(firmware, str) or not 1 <= len(firmware) <= 64:
        return "firmware must be a string of 1-64 characters"

    return None


def parse_wav(data: bytes) -> dict[str, int]:
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")

    pos = 12
    fmt: dict[str, int] | None = None
    data_offset: int | None = None
    data_size = 0

    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        chunk_size = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if chunk_id == b"fmt ":
            if chunk_size < 16 or body + 16 > len(data):
                raise ValueError("invalid fmt chunk")
            audio_format, channels, sample_rate, _byte_rate, _block_align, bits = struct.unpack_from(
                "<HHIIHH", data, body
            )
            fmt = {
                "audio_format": audio_format,
                "channels": channels,
                "sample_rate": sample_rate,
                "bits": bits,
            }
        elif chunk_id == b"data":
            data_offset = body
            data_size = min(chunk_size, len(data) - body)
            break
        pos = body + chunk_size + (chunk_size & 1)

    if fmt is None or data_offset is None:
        raise ValueError("missing fmt/data chunk")
    if fmt["audio_format"] != 1 or fmt["bits"] != 16:
        raise ValueError("only PCM 16-bit WAV is supported")
    if fmt["channels"] not in (1, 2):
        raise ValueError("only mono/stereo WAV is supported")
    if fmt["sample_rate"] != 16000:
        raise ValueError("only 16 kHz WAV is supported")

    return {**fmt, "data_offset": data_offset, "data_size": data_size}


def normalize_for_asr(source: Path, target: Path) -> tuple[Path, float]:
    with wave.open(str(source), "rb") as handle:
        params = handle.getparams()
        frames = handle.readframes(handle.getnframes())

    samples = array.array("h")
    samples.frombytes(frames)
    if not samples:
        return source, 1.0

    mean = sum(samples) / len(samples)
    centered = [sample - mean for sample in samples]
    peak = max(max(centered), -min(centered))
    if peak <= 0:
        return source, 1.0

    gain = min(40.0, 30000.0 / peak)
    if gain < 1.5:
        return source, 1.0

    scaled = array.array(
        "h",
        (max(-32768, min(32767, int(round(sample * gain)))) for sample in centered),
    )
    with wave.open(str(target), "wb") as handle:
        handle.setparams(params)
        handle.writeframes(scaled.tobytes())
    return target, gain


def run_powershell(script: Path, arguments: list[str], timeout_s: int) -> str:
    result = subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
            *arguments,
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout_s,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    if result.returncode != 0:
        detail = (result.stderr or "").strip() or (result.stdout or "").strip()
        raise RuntimeError(detail or f"PowerShell exited with {result.returncode}")
    return result.stdout.lstrip("\ufeff").strip()


def transcribe_wav(path: Path) -> str:
    if WHISPER_MODEL is None:
        return run_powershell(ASR_SCRIPT, [str(path)], int(CONFIG.get("asr_timeout_s", 30)))

    with WHISPER_LOCK:
        segments, info = WHISPER_MODEL.transcribe(
            str(path),
            language="zh",
            beam_size=5,
            vad_filter=True,
        )
        text = "".join(segment.text for segment in segments).strip()
    logging.info(
        "whisper transcript=%r language=%s probability=%.3f",
        text,
        info.language,
        info.language_probability,
    )
    return text


def generate_tone_wav(path: Path, frequency: int = 880, duration_s: float = 1.0) -> None:
    sample_rate = 16000
    frames = bytearray()
    for index in range(int(sample_rate * duration_s)):
        value = int(5000 * math.sin(2.0 * math.pi * frequency * index / sample_rate))
        frames += struct.pack("<h", value)
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(sample_rate)
        handle.writeframes(frames)


def task_public(task: dict[str, Any]) -> dict[str, Any]:
    now = time.time()
    remaining = 0
    if task.get("status") == "pending":
        remaining = max(0, int(task.get("expires_at", now) - now))
    return {
        "ok": True,
        "task_id": task["id"],
        "status": task["status"],
        "mode": task["mode"],
        "transcript": task.get("transcript", ""),
        "summary": task.get("summary", ""),
        "expires_in_s": remaining,
        "result": task.get("result", "")[-2000:],
        "error": task.get("error", "")[-1000:],
        "reply_text": task.get("reply_text", "")[-500:],
        "screen_ready": bool(task.get("screen_path")),
    }


def expire_tasks_locked() -> None:
    global CURRENT_TASK_ID
    now = time.time()
    for task_id, task in list(TASKS.items()):
        if task["status"] == "pending" and now >= task["expires_at"]:
            task["status"] = "expired"
            task["finished_at"] = now
            if CURRENT_TASK_ID == task_id:
                CURRENT_TASK_ID = None


def new_task_id_locked() -> str:
    for _ in range(50):
        candidate = f"{random.randint(100000, 999999)}"
        if candidate not in TASKS:
            return candidate
    raise RuntimeError("unable to allocate task id")


def sanitize_task_text(text: str) -> str:
    cleaned = "".join(
        character
        for character in text
        if character in ("\n", "\t") or ord(character) >= 32
    )
    return " ".join(cleaned.split())[:300]


def extract_reply_text(output: str) -> str:
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", output)
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        return "任务完成。"
    return lines[-1][:400]


def active_task_locked() -> bool:
    if CURRENT_TASK_ID is None:
        return False
    current = TASKS.get(CURRENT_TASK_ID)
    return current is not None and current["status"] in ("pending", "running", "interactive")


def create_pending_task(
    transcript: str, mode: str, source_ip: str, device_id: str
) -> dict[str, Any]:
    global CURRENT_TASK_ID
    ttl = int(CONFIG.get("task_ttl_s", 60))
    backend = str(CONFIG.get("task_backend", "chatgpt"))

    task: dict[str, Any] | None = None
    with TASK_CREATION_LOCK:
        with TASKS_LOCK:
            expire_tasks_locked()
            if active_task_locked():
                raise RuntimeError("task_in_progress")

        if backend == "chatgpt":
            try:
                draft_result = run_powershell(CHATGPT_SET_DRAFT, [transcript], 20)
            except Exception as error:  # noqa: BLE001 - map all UIA failures to unavailable
                logging.warning("ChatGPT draft unavailable: %s", error)
                raise RuntimeError("chatgpt_unavailable") from error
            if not draft_result.startswith("OK"):
                raise RuntimeError("chatgpt_unavailable")
            mode = "chatgpt"

        busy = False
        with TASKS_LOCK:
            expire_tasks_locked()
            if active_task_locked():
                busy = True
            else:
                task_id = new_task_id_locked()
                now = time.time()
                task = {
                    "id": task_id,
                    "status": "pending",
                    "mode": mode,
                    "transcript": transcript,
                    "summary": transcript[:120],
                    "source_ip": source_ip,
                    "device_id": device_id,
                    "created_at": now,
                    "expires_at": now + ttl,
                    "result": "",
                    "error": "",
                    "pid": None,
                }
                TASKS[task_id] = task
                CURRENT_TASK_ID = task_id

        if busy:
            if backend == "chatgpt":
                try:
                    run_powershell(CHATGPT_CLEAR_DRAFT, [], 20)
                except Exception as error:  # noqa: BLE001 - best effort cleanup
                    logging.warning("ChatGPT draft cleanup failed: %s", error)
            raise RuntimeError("task_in_progress")

    assert task is not None
    logging.info("=" * 60)
    logging.info("PENDING TASK %s mode=%s backend=%s", task["id"], mode, backend)
    logging.info("TEXT: %s", transcript)
    if backend == "chatgpt":
        logging.info("Draft written to the ChatGPT input box; confirm to send.")
    logging.info("Confirm only for task ID %s; expires in %d s.", task["id"], ttl)
    logging.info("=" * 60)
    return task


def run_codex_task(task_id: str) -> None:
    global CURRENT_TASK_ID
    with TASKS_LOCK:
        task = dict(TASKS.get(task_id, {}))
    if not task:
        return

    mode = task["mode"]
    transcript = task["transcript"]
    project_dir = str(CONFIG.get("project_dir") or BASE_DIR.parent.parent)
    codex_binary = str(CONFIG.get("codex_binary", "codex"))
    timeout_s = int(CONFIG.get("codex_timeout_s", 180))

    if mode == "read_only":
        prompt = "This is a read-only task. Do not modify files. " + transcript
        command = [
            codex_binary,
            "exec",
            "--sandbox",
            "read-only",
            "--skip-git-repo-check",
            "-C",
            project_dir,
            prompt,
        ]
        logging.info("codex exec read-only task=%s", task_id)
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout_s,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
            output = (result.stdout or "").strip()
            error_text = (result.stderr or "").strip()
            reply_text = (
                extract_reply_text(output)
                if result.returncode == 0
                else "Codex 任务失败，请检查电脑端日志。"
            )
            audio_path = DATA_DIR / f"task_{task_id}_reply.wav"
            try:
                run_powershell(
                    TTS_SCRIPT,
                    [reply_text, str(audio_path)],
                    int(CONFIG.get("tts_timeout_s", 30)),
                )
            except Exception as tts_error:  # noqa: BLE001 - task still completes
                logging.warning("task %s TTS failed: %s", task_id, tts_error)
            audio_ready = audio_path.exists() and audio_path.stat().st_size > 44

            with TASKS_LOCK:
                entry = TASKS.get(task_id)
                if entry is not None:
                    entry["status"] = "done" if result.returncode == 0 else "error"
                    entry["result"] = output[-4000:]
                    entry["error"] = error_text[-1500:] if result.returncode != 0 else ""
                    entry["reply_text"] = reply_text
                    entry["audio_path"] = str(audio_path) if audio_ready else ""
                    entry["finished_at"] = time.time()
                    if CURRENT_TASK_ID == task_id:
                        CURRENT_TASK_ID = None
            logging.info(
                "codex exec task=%s exit=%s reply=%r audio=%s",
                task_id,
                result.returncode,
                reply_text,
                audio_ready,
            )
        except Exception as error:  # noqa: BLE001 - report to caller
            with TASKS_LOCK:
                entry = TASKS.get(task_id)
                if entry is not None:
                    entry["status"] = "error"
                    entry["error"] = str(error)[:1500]
                    entry["finished_at"] = time.time()
                    if CURRENT_TASK_ID == task_id:
                        CURRENT_TASK_ID = None
            logging.warning("codex exec task=%s failed: %s", task_id, error)
        return

    command = [codex_binary, "-C", project_dir, transcript]
    logging.info("launching interactive codex task=%s", task_id)
    try:
        process = subprocess.Popen(
            command,
            cwd=project_dir,
            creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0),
        )
        with TASKS_LOCK:
            entry = TASKS.get(task_id)
            if entry is not None:
                entry["status"] = "interactive"
                entry["pid"] = process.pid
        return_code = process.wait()
        with TASKS_LOCK:
            entry = TASKS.get(task_id)
            if entry is not None:
                entry["status"] = "done" if return_code == 0 else "error"
                entry["result"] = "interactive Codex window closed"
                entry["finished_at"] = time.time()
                if CURRENT_TASK_ID == task_id:
                    CURRENT_TASK_ID = None
        logging.info("interactive codex task=%s exit=%s", task_id, return_code)
    except Exception as error:  # noqa: BLE001 - report to caller
        with TASKS_LOCK:
            entry = TASKS.get(task_id)
            if entry is not None:
                entry["status"] = "error"
                entry["error"] = str(error)[:1500]
                entry["finished_at"] = time.time()
                if CURRENT_TASK_ID == task_id:
                    CURRENT_TASK_ID = None
        logging.warning("interactive codex task=%s failed: %s", task_id, error)


def wav_duration_ms(path: Path) -> int:
    try:
        with wave.open(str(path), "rb") as handle:
            sample_rate = handle.getframerate()
            if sample_rate <= 0:
                return 0
            return int(round(1000 * handle.getnframes() / sample_rate))
    except Exception:  # noqa: BLE001 - duration is optional
        return 0


def render_page_bitmaps(text: str, raw_path: Path) -> int:
    run_powershell(RENDER_OLED_PAGES, [text, str(raw_path), "20"], 30)
    data = raw_path.read_bytes()
    if not data or len(data) % 512 != 0:
        raise RuntimeError("invalid_page_bitmaps")
    count = len(data) // 512
    if count <= 0 or count > 24:
        raise RuntimeError("page_count_out_of_range")
    return count


def build_answer_screens(reply_text: str, audio_path: Path, out_path: Path) -> int:
    pages = [reply_text[index:index + 20] for index in range(0, len(reply_text), 20)]
    if not pages:
        raise RuntimeError("empty_answer_text")
    raw_path = out_path.with_name(out_path.stem + "_raw.bin")
    count = render_page_bitmaps(reply_text, raw_path)
    if count != len(pages):
        raise RuntimeError(f"page_count_mismatch rendered={count} expected={len(pages)}")
    raw = raw_path.read_bytes()
    total_ms = wav_duration_ms(audio_path) if audio_path.exists() else 0
    total_chars = sum(max(1, len(page)) for page in pages)
    durations: list[int] = []
    remaining = total_ms
    for index, page in enumerate(pages):
        if total_ms <= 0:
            duration = 3000
        elif index == len(pages) - 1:
            duration = remaining
        else:
            duration = int(round(total_ms * len(page) / total_chars))
        duration = max(1, min(65535, duration))
        durations.append(duration)
        remaining -= duration
    data = bytearray(struct.pack("<H", len(pages)))
    for index, duration in enumerate(durations):
        data += struct.pack("<H", duration)
        data += raw[index * 512:(index + 1) * 512]
    out_path.write_bytes(data)
    return len(pages)


def build_prompt_pages(transcript: str, out_path: Path) -> int:
    raw_path = out_path.with_name(out_path.stem + "_raw.bin")
    count = render_page_bitmaps(transcript, raw_path)
    raw = raw_path.read_bytes()
    out_path.write_bytes(struct.pack("<H", count) + raw)
    return count


def run_chatgpt_task(task_id: str) -> None:
    global CURRENT_TASK_ID
    with TASKS_LOCK:
        task = dict(TASKS.get(task_id, {}))
    if not task:
        return

    transcript = task["transcript"]
    timeout_s = int(CONFIG.get("chatgpt_timeout_s", 120))
    try:
        answer = run_powershell(
            CHATGPT_SEND_CAPTURE,
            [transcript, str(timeout_s)],
            timeout_s + 15,
        )
        answer = answer.strip()
        if answer.startswith("ERROR:"):
            raise RuntimeError(answer)

        clean_answer = answer
        for label in ("ChatGPT 说：", "ChatGPT 说:", "你说：", "你说:"):
            clean_answer = clean_answer.replace(label, " ")
        reply_text = " ".join(clean_answer.split())[:400]
        if not reply_text:
            reply_text = "ChatGPT 没有返回内容。"

        screen_path = DATA_DIR / f"task_{task_id}_screen.bin"
        try:
            run_powershell(RENDER_OLED, [reply_text[:40], str(screen_path)], 20)
        except Exception as render_error:  # noqa: BLE001 - screen is optional
            logging.warning("task %s OLED render failed: %s", task_id, render_error)
        screen_ready = screen_path.exists() and screen_path.stat().st_size == 512

        audio_path = DATA_DIR / f"task_{task_id}_reply.wav"
        try:
            run_powershell(
                TTS_SCRIPT,
                [reply_text, str(audio_path)],
                int(CONFIG.get("tts_timeout_s", 30)),
            )
        except Exception as tts_error:  # noqa: BLE001 - task still completes
            logging.warning("task %s TTS failed: %s", task_id, tts_error)
        audio_ready = audio_path.exists() and audio_path.stat().st_size > 44

        screens_path = DATA_DIR / f"task_{task_id}_screens.bin"
        screens_ready = False
        screens_pages = 0
        if reply_text:
            try:
                screens_pages = build_answer_screens(reply_text, audio_path, screens_path)
                screens_ready = True
            except Exception as screens_error:  # noqa: BLE001 - single screen remains available
                logging.warning("task %s OLED pages failed: %s", task_id, screens_error)

        with TASKS_LOCK:
            entry = TASKS.get(task_id)
            if entry is not None:
                entry["status"] = "done"
                entry["result"] = answer[-4000:]
                entry["error"] = ""
                entry["reply_text"] = reply_text
                entry["audio_path"] = str(audio_path) if audio_ready else ""
                entry["screen_path"] = str(screen_path) if screen_ready else ""
                entry["screens_path"] = str(screens_path) if screens_ready else ""
                entry["finished_at"] = time.time()
                if CURRENT_TASK_ID == task_id:
                    CURRENT_TASK_ID = None
        logging.info(
            "chatgpt task=%s done; reply=%r audio=%s screen=%s screens=%s pages=%d",
            task_id,
            reply_text,
            audio_ready,
            screen_ready,
            screens_ready,
            screens_pages,
        )
    except Exception as error:  # noqa: BLE001 - report to caller
        with TASKS_LOCK:
            entry = TASKS.get(task_id)
            if entry is not None:
                entry["status"] = "error"
                entry["error"] = str(error)[:1500]
                entry["finished_at"] = time.time()
                if CURRENT_TASK_ID == task_id:
                    CURRENT_TASK_ID = None
        logging.warning("chatgpt task=%s failed: %s", task_id, error)


class ServiceHandler(BaseHTTPRequestHandler):
    server_version = "CodexVoiceService/0.2"
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: Any) -> None:
        logging.info("%s - %s", self.address_string(), format % args)

    def send_json(self, status: HTTPStatus, payload: dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_wav(self, wav_bytes: bytes, transcript: str, reply_text: str) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(wav_bytes)))
        self.send_header("X-Transcript", urllib.parse.quote(transcript))
        self.send_header("X-Reply-Text", urllib.parse.quote(reply_text))
        self.end_headers()
        self.wfile.write(wav_bytes)

    def reject_source(self) -> None:
        source_ip = normalize_ip(self.client_address[0])
        logging.warning("rejected source ip=%s method=%s path=%s", source_ip, self.command, self.path)
        self.send_json(HTTPStatus.FORBIDDEN, {"ok": False, "error": "source_not_allowed"})

    def do_GET(self) -> None:
        if not source_allowed(self.client_address[0]):
            self.reject_source()
            return
        path = urllib.parse.urlparse(self.path).path
        if path == "/api/v1/health":
            self.send_json(
                HTTPStatus.OK,
                {"ok": True, "service": "codex-voice-service", "protocol_version": PROTOCOL_VERSION},
            )
            return
        task_match = re.fullmatch(r"/api/v1/tasks/(\d{6})", path)
        if task_match is not None:
            self.handle_task_status(task_match.group(1))
            return
        audio_match = re.fullmatch(r"/api/v1/tasks/(\d{6})/audio", path)
        if audio_match is not None:
            self.handle_task_audio(audio_match.group(1))
            return
        screen_match = re.fullmatch(r"/api/v1/tasks/(\d{6})/screen", path)
        if screen_match is not None:
            self.handle_task_screen(screen_match.group(1))
            return
        prompt_match = re.fullmatch(r"/api/v1/tasks/(\d{6})/prompt", path)
        if prompt_match is not None:
            self.handle_task_prompt(prompt_match.group(1))
            return
        screens_match = re.fullmatch(r"/api/v1/tasks/(\d{6})/screens", path)
        if screens_match is not None:
            self.handle_task_screens(screens_match.group(1))
            return
        prompt_pages_match = re.fullmatch(r"/api/v1/tasks/(\d{6})/prompt-pages", path)
        if prompt_pages_match is not None:
            self.handle_task_prompt_pages(prompt_pages_match.group(1))
            return
        self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "not_found"})

    def do_POST(self) -> None:
        if not source_allowed(self.client_address[0]):
            self.reject_source()
            return
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        if path == "/api/v1/device/heartbeat":
            self.handle_heartbeat()
            return
        if path == "/api/v1/audio/utterance":
            self.handle_audio_utterance()
            return
        if path == "/api/v1/tasks/request":
            self.handle_task_request(parsed.query)
            return
        if path == "/api/v1/tasks/text":
            self.handle_task_text()
            return
        confirm_match = re.fullmatch(r"/api/v1/tasks/(\d{6})/confirm", path)
        if confirm_match is not None:
            self.handle_task_confirm(confirm_match.group(1))
            return
        cancel_match = re.fullmatch(r"/api/v1/tasks/(\d{6})/cancel", path)
        if cancel_match is not None:
            self.handle_task_cancel(cancel_match.group(1))
            return
        self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "not_found"})

    def handle_heartbeat(self) -> None:
        content_type = self.headers.get("Content-Type", "")
        if not content_type.lower().startswith("application/json"):
            self.send_json(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, {"ok": False, "error": "content_type_must_be_json"})
            return

        try:
            content_length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid_content_length"})
            return
        if content_length <= 0 or content_length > MAX_HEARTBEAT_BYTES:
            self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"ok": False, "error": "body_too_large"})
            return

        raw_body = self.rfile.read(content_length)
        try:
            data = json.loads(raw_body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid_json"})
            return

        problem = validate_heartbeat(data)
        if problem is not None:
            self.send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "invalid_request", "detail": problem},
            )
            return

        source_ip = normalize_ip(self.client_address[0])
        logging.info(
            "heartbeat source=%s device_id=%s uptime_s=%s rssi=%s firmware=%s",
            source_ip,
            data["device_id"],
            data["uptime_s"],
            data["wifi_rssi"],
            data["firmware"],
        )
        self.send_json(
            HTTPStatus.OK,
            {
                "ok": True,
                "protocol_version": PROTOCOL_VERSION,
                "server_time": datetime.now(timezone.utc).isoformat(),
                "message": "heartbeat accepted",
            },
        )

    def handle_audio_utterance(self) -> None:
        content_type = self.headers.get("Content-Type", "")
        if not content_type.lower().startswith(("audio/wav", "audio/x-wav")):
            self.send_json(
                HTTPStatus.UNSUPPORTED_MEDIA_TYPE,
                {"ok": False, "error": "content_type_must_be_audio_wav"},
            )
            return

        max_audio_bytes = int(CONFIG.get("audio_max_bytes", 1024 * 1024))
        try:
            content_length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid_content_length"})
            return
        if content_length <= 0 or content_length > max_audio_bytes:
            self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"ok": False, "error": "audio_too_large"})
            return

        raw_audio = self.rfile.read(content_length)
        try:
            wav_info = parse_wav(raw_audio)
        except ValueError as error:
            self.send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "unsupported_wav", "detail": str(error)},
            )
            return

        DATA_DIR.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        utterance_path = DATA_DIR / f"utterance_{stamp}_{uuid.uuid4().hex[:8]}.wav"
        utterance_path.write_bytes(raw_audio)

        source_ip = normalize_ip(self.client_address[0])
        device_id = self.headers.get("X-Device-Id", "unknown")[:64]
        started = datetime.now(timezone.utc)
        logging.info(
            "audio source=%s device_id=%s bytes=%d rate=%d channels=%d bits=%d",
            source_ip,
            device_id,
            len(raw_audio),
            wav_info["sample_rate"],
            wav_info["channels"],
            wav_info["bits"],
        )

        asr_input = utterance_path
        try:
            normalized_path = utterance_path.with_name(utterance_path.stem + "_norm.wav")
            asr_input, gain = normalize_for_asr(utterance_path, normalized_path)
            if gain > 1.5:
                logging.info("ASR normalized input path=%s gain=%.1f", normalized_path, gain)
        except Exception as error:  # noqa: BLE001 - keep original on failure
            logging.warning("ASR normalization failed: %s", error)
            asr_input = utterance_path

        transcript = ""
        try:
            transcript = transcribe_wav(asr_input)
        except Exception as error:  # noqa: BLE001 - log and use fallback
            logging.warning("ASR failed: %s", error)
        transcript = " ".join(transcript.split())[:300]

        if not transcript:
            reply_text = "我没有听清，请再说一遍。"
        elif "状态" in transcript:
            reply_text = "设备在线，服务正常。"
        else:
            reply_text = f"我听到了：{transcript}"
        reply_text = reply_text[:200]

        reply_path = DATA_DIR / f"reply_{stamp}_{uuid.uuid4().hex[:8]}.wav"
        try:
            run_powershell(
                TTS_SCRIPT,
                [reply_text, str(reply_path)],
                int(CONFIG.get("tts_timeout_s", 30)),
            )
        except Exception as error:  # noqa: BLE001 - fallback tone
            logging.warning("TTS failed: %s", error)

        if not reply_path.exists() or reply_path.stat().st_size <= 44:
            generate_tone_wav(reply_path)
            logging.warning("TTS output missing; sent fallback tone")

        reply_bytes = reply_path.read_bytes()
        elapsed = (datetime.now(timezone.utc) - started).total_seconds()
        logging.info(
            "audio reply source=%s transcript=%r reply=%r reply_bytes=%d elapsed_s=%.2f",
            source_ip,
            transcript,
            reply_text,
            len(reply_bytes),
            elapsed,
        )
        self.send_wav(reply_bytes, transcript, reply_text)

    def handle_task_request(self, query: str) -> None:
        params = urllib.parse.parse_qs(query)
        mode = params.get("mode", ["read_only"])[0]
        if mode not in ("read_only", "interactive"):
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid_mode"})
            return

        content_type = self.headers.get("Content-Type", "")
        if not content_type.lower().startswith(("audio/wav", "audio/x-wav")):
            self.send_json(
                HTTPStatus.UNSUPPORTED_MEDIA_TYPE,
                {"ok": False, "error": "content_type_must_be_audio_wav"},
            )
            return

        max_audio_bytes = int(CONFIG.get("audio_max_bytes", 1024 * 1024))
        try:
            content_length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid_content_length"})
            return
        if content_length <= 0 or content_length > max_audio_bytes:
            self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"ok": False, "error": "audio_too_large"})
            return

        raw_audio = self.rfile.read(content_length)
        try:
            parse_wav(raw_audio)
        except ValueError as error:
            self.send_json(
                HTTPStatus.BAD_REQUEST,
                {"ok": False, "error": "unsupported_wav", "detail": str(error)},
            )
            return

        DATA_DIR.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        utterance_path = DATA_DIR / f"task_{stamp}_{uuid.uuid4().hex[:8]}.wav"
        utterance_path.write_bytes(raw_audio)

        asr_input = utterance_path
        try:
            normalized_path = utterance_path.with_name(utterance_path.stem + "_norm.wav")
            asr_input, gain = normalize_for_asr(utterance_path, normalized_path)
            if gain > 1.5:
                logging.info("ASR normalized input path=%s gain=%.1f", normalized_path, gain)
        except Exception as error:  # noqa: BLE001 - keep original on failure
            logging.warning("ASR normalization failed: %s", error)
            asr_input = utterance_path

        transcript = ""
        try:
            transcript = transcribe_wav(asr_input)
        except Exception as error:  # noqa: BLE001 - report failure
            logging.warning("ASR failed: %s", error)
        transcript = sanitize_task_text(transcript)
        if not transcript:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "empty_transcript"})
            return

        source_ip = normalize_ip(self.client_address[0])
        device_id = self.headers.get("X-Device-Id", "unknown")[:64]
        try:
            task = create_pending_task(transcript, mode, source_ip, device_id)
        except RuntimeError as error:
            if str(error) == "task_in_progress":
                self.send_json(HTTPStatus.CONFLICT, {"ok": False, "error": "task_in_progress"})
                return
            if str(error) == "chatgpt_unavailable":
                self.send_json(
                    HTTPStatus.SERVICE_UNAVAILABLE,
                    {"ok": False, "error": "chatgpt_unavailable"},
                )
                return
            raise
        self.send_json(HTTPStatus.OK, task_public(task))

    def handle_task_text(self) -> None:
        content_type = self.headers.get("Content-Type", "")
        if not content_type.lower().startswith("application/json"):
            self.send_json(
                HTTPStatus.UNSUPPORTED_MEDIA_TYPE,
                {"ok": False, "error": "content_type_must_be_json"},
            )
            return
        try:
            content_length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid_content_length"})
            return
        if content_length <= 0 or content_length > MAX_HEARTBEAT_BYTES:
            self.send_json(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, {"ok": False, "error": "body_too_large"})
            return

        try:
            data = json.loads(self.rfile.read(content_length).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid_json"})
            return
        if not isinstance(data, dict):
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid_request"})
            return

        text = sanitize_task_text(str(data.get("text", "")))
        mode = str(data.get("mode", "read_only"))
        if mode not in ("read_only", "interactive"):
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "invalid_mode"})
            return
        if not text:
            self.send_json(HTTPStatus.BAD_REQUEST, {"ok": False, "error": "empty_text"})
            return

        source_ip = normalize_ip(self.client_address[0])
        device_id = self.headers.get("X-Device-Id", "pc-text")[:64]
        try:
            task = create_pending_task(text, mode, source_ip, device_id)
        except RuntimeError as error:
            if str(error) == "task_in_progress":
                self.send_json(HTTPStatus.CONFLICT, {"ok": False, "error": "task_in_progress"})
                return
            if str(error) == "chatgpt_unavailable":
                self.send_json(
                    HTTPStatus.SERVICE_UNAVAILABLE,
                    {"ok": False, "error": "chatgpt_unavailable"},
                )
                return
            raise
        self.send_json(HTTPStatus.OK, task_public(task))

    def handle_task_confirm(self, task_id: str) -> None:
        global CURRENT_TASK_ID
        with TASKS_LOCK:
            expire_tasks_locked()
            task = TASKS.get(task_id)
            if task is None:
                self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "task_not_found"})
                return
            if task["status"] == "expired":
                self.send_json(HTTPStatus.GONE, {"ok": False, "error": "task_expired"})
                return
            if task["status"] != "pending":
                self.send_json(HTTPStatus.CONFLICT, {"ok": False, "error": "task_not_pending"})
                return
            if CURRENT_TASK_ID != task_id:
                self.send_json(HTTPStatus.CONFLICT, {"ok": False, "error": "task_not_current"})
                return
            task["status"] = "running"
            task["confirmed_at"] = time.time()
            mode = task["mode"]

        logging.info(
            "task %s confirmed source=%s device_id=%s mode=%s",
            task_id,
            normalize_ip(self.client_address[0]),
            task.get("device_id", "unknown"),
            mode,
        )
        backend = str(CONFIG.get("task_backend", "chatgpt"))
        target = run_chatgpt_task if backend == "chatgpt" else run_codex_task
        thread = threading.Thread(target=target, args=(task_id,), daemon=True)
        thread.start()
        self.send_json(
            HTTPStatus.OK,
            {"ok": True, "task_id": task_id, "status": "running", "mode": mode},
        )

    def handle_task_cancel(self, task_id: str) -> None:
        global CURRENT_TASK_ID
        with TASKS_LOCK:
            expire_tasks_locked()
            task = TASKS.get(task_id)
            if task is None:
                self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "task_not_found"})
                return
            if task["status"] != "pending" or CURRENT_TASK_ID != task_id:
                self.send_json(HTTPStatus.CONFLICT, {"ok": False, "error": "task_not_cancellable"})
                return
            task["status"] = "cancelled"
            task["finished_at"] = time.time()
            CURRENT_TASK_ID = None
        if CONFIG.get("task_backend") == "chatgpt":
            try:
                cleared = run_powershell(CHATGPT_CLEAR_DRAFT, [], 20)
                if not cleared.startswith("OK"):
                    logging.warning("ChatGPT draft clear returned %r", cleared)
            except Exception as error:  # noqa: BLE001 - best effort cleanup
                logging.warning("ChatGPT draft clear failed: %s", error)

        logging.info("task %s cancelled source=%s", task_id, normalize_ip(self.client_address[0]))
        self.send_json(HTTPStatus.OK, {"ok": True, "task_id": task_id, "status": "cancelled"})

    def handle_task_status(self, task_id: str) -> None:
        with TASKS_LOCK:
            expire_tasks_locked()
            task = TASKS.get(task_id)
            if task is None:
                self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "task_not_found"})
                return
            payload = task_public(task)
        self.send_json(HTTPStatus.OK, payload)

    def handle_task_audio(self, task_id: str) -> None:
        with TASKS_LOCK:
            expire_tasks_locked()
            task = TASKS.get(task_id)
            if task is None:
                self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "task_not_found"})
                return
            audio_path = task.get("audio_path", "")
        if not audio_path:
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "audio_not_ready"})
            return
        path = Path(audio_path)
        if not path.exists():
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "audio_not_found"})
            return
        data = path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_task_screen(self, task_id: str) -> None:
        with TASKS_LOCK:
            expire_tasks_locked()
            task = TASKS.get(task_id)
            if task is None:
                self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "task_not_found"})
                return
            screen_path = task.get("screen_path", "")
        if not screen_path:
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "screen_not_ready"})
            return
        path = Path(screen_path)
        if not path.exists() or path.stat().st_size != 512:
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "screen_not_found"})
            return
        data = path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_task_prompt(self, task_id: str) -> None:
        with TASKS_LOCK:
            expire_tasks_locked()
            task = TASKS.get(task_id)
            if task is None:
                self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "task_not_found"})
                return
            prompt_path = task.get("prompt_path", "")
            transcript = task.get("transcript", "")

        if not prompt_path:
            path = DATA_DIR / f"task_{task_id}_prompt.bin"
            try:
                run_powershell(RENDER_OLED, [f"任务{task_id} {transcript}", str(path)], 20)
            except Exception as error:  # noqa: BLE001 - report render failure to the device
                logging.warning("task %s prompt render failed: %s", task_id, error)
                self.send_json(
                    HTTPStatus.INTERNAL_SERVER_ERROR,
                    {"ok": False, "error": "prompt_render_failed"},
                )
                return
            with TASKS_LOCK:
                task = TASKS.get(task_id)
                if task is not None:
                    task["prompt_path"] = str(path)
            prompt_path = str(path)

        path = Path(prompt_path)
        if not path.exists() or path.stat().st_size != 512:
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "prompt_not_found"})
            return
        data = path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_task_screens(self, task_id: str) -> None:
        with TASKS_LOCK:
            expire_tasks_locked()
            task = TASKS.get(task_id)
            if task is None:
                self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "task_not_found"})
                return
            screens_path = task.get("screens_path", "")
        if not screens_path:
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "screens_not_ready"})
            return
        path = Path(screens_path)
        if not path.exists() or path.stat().st_size < 2:
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "screens_not_found"})
            return
        data = path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_task_prompt_pages(self, task_id: str) -> None:
        with TASKS_LOCK:
            expire_tasks_locked()
            task = TASKS.get(task_id)
            if task is None:
                self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "task_not_found"})
                return
            prompt_pages_path = task.get("prompt_pages_path", "")
            transcript = task.get("transcript", "")
        if not prompt_pages_path:
            path = DATA_DIR / f"task_{task_id}_prompt_pages.bin"
            try:
                build_prompt_pages(transcript, path)
            except Exception as error:  # noqa: BLE001 - device falls back to single prompt
                logging.warning("task %s prompt pages failed: %s", task_id, error)
                self.send_json(
                    HTTPStatus.INTERNAL_SERVER_ERROR,
                    {"ok": False, "error": "prompt_pages_failed"},
                )
                return
            with TASKS_LOCK:
                task = TASKS.get(task_id)
                if task is not None:
                    task["prompt_pages_path"] = str(path)
            prompt_pages_path = str(path)
        path = Path(prompt_pages_path)
        if not path.exists() or path.stat().st_size < 2:
            self.send_json(HTTPStatus.NOT_FOUND, {"ok": False, "error": "prompt_pages_not_found"})
            return
        data = path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_PUT(self) -> None:
        self.method_not_allowed()

    def do_PATCH(self) -> None:
        self.method_not_allowed()

    def do_DELETE(self) -> None:
        self.method_not_allowed()

    def method_not_allowed(self) -> None:
        self.send_json(HTTPStatus.METHOD_NOT_ALLOWED, {"ok": False, "error": "method_not_allowed"})


class ServiceServer(ThreadingHTTPServer):
    allow_reuse_address = True


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32 voice terminal service")
    parser.add_argument(
        "--config",
        type=Path,
        default=Path(__file__).with_name("config.local.json"),
        help="Path to a local JSON config (default: next to this script)",
    )
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    global CONFIG, WHISPER_MODEL
    CONFIG = load_config(args.config)
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if not CONFIG.get("project_dir"):
        CONFIG["project_dir"] = str(BASE_DIR.parent.parent)

    try:
        from faster_whisper import WhisperModel

        model_name = str(CONFIG.get("whisper_model", "small"))
        model_device = str(CONFIG.get("whisper_device", "cpu"))
        model_compute = str(CONFIG.get("whisper_compute_type", "int8"))
        logging.info(
            "loading whisper model=%s device=%s compute_type=%s",
            model_name,
            model_device,
            model_compute,
        )
        load_started = time.time()
        WHISPER_MODEL = WhisperModel(model_name, device=model_device, compute_type=model_compute)
        logging.info("whisper model loaded in %.2f s", time.time() - load_started)
    except Exception as error:  # noqa: BLE001 - fallback to SAPI ASR
        logging.warning("Whisper unavailable; falling back to SAPI ASR: %s", error)
        WHISPER_MODEL = None

    bind_host = str(CONFIG["bind_host"])
    port = int(CONFIG["port"])
    allowed = CONFIG.get("allowed_device_ips", [])
    logging.info("config=%s", args.config)
    logging.info("listening on %s:%d", bind_host, port)
    logging.info("allowed_device_ips=%s allow_localhost=%s", allowed, CONFIG.get("allow_localhost", True))
    logging.info(
        "asr_script=%s (exists=%s) tts_script=%s (exists=%s)",
        ASR_SCRIPT,
        ASR_SCRIPT.exists(),
        TTS_SCRIPT,
        TTS_SCRIPT.exists(),
    )

    server = ServiceServer((bind_host, port), ServiceHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logging.info("stopping")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
