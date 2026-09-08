from __future__ import annotations

from collections import deque
from datetime import datetime, timezone
import os
import subprocess
import sys
import threading
from typing import Any

from .config import PipelineConfig, REPO_ROOT, build_pipeline_argv, validate_config
from .profile_parser import parse_profile_line


class ProcessManager:
    def __init__(self, *, log_capacity: int = 5000, error_capacity: int = 300) -> None:
        self._lock = threading.RLock()
        self._process: subprocess.Popen[str] | None = None
        self._started_at: str | None = None
        self._current_config: dict[str, Any] | None = None
        self._current_argv: list[str] | None = None
        self._last_returncode: int | None = None
        self._logs: deque[str] = deque(maxlen=log_capacity)
        self._errors: deque[str] = deque(maxlen=error_capacity)
        self._last_profile: dict[str, Any] | None = None

    def is_running(self) -> bool:
        with self._lock:
            return self._process is not None and self._process.poll() is None

    def start(self, config_data: dict[str, Any] | None) -> dict[str, Any]:
        with self._lock:
            if self.is_running():
                raise RuntimeError("Pipeline is already running. Use /restart to replace it.")

            config = validate_config(PipelineConfig.from_dict(config_data))
            pipeline_argv = build_pipeline_argv(config)
            argv = [sys.executable, "-u", *pipeline_argv]

            env = os.environ.copy()
            env["PYTHONUNBUFFERED"] = "1"
            env.setdefault("PYTHONPATH", str(REPO_ROOT))

            self._logs.clear()
            self._errors.clear()
            self._last_profile = None
            self._last_returncode = None

            process = subprocess.Popen(
                argv,
                cwd=str(REPO_ROOT),
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,
                universal_newlines=True,
            )
            self._process = process
            self._started_at = datetime.now(timezone.utc).isoformat()
            self._current_config = config.to_dict()
            self._current_argv = argv

            if process.stdout is not None:
                threading.Thread(
                    target=self._read_stream,
                    args=(process.stdout, "stdout"),
                    name="soundbubble-stdout-reader",
                    daemon=True,
                ).start()
            if process.stderr is not None:
                threading.Thread(
                    target=self._read_stream,
                    args=(process.stderr, "stderr"),
                    name="soundbubble-stderr-reader",
                    daemon=True,
                ).start()
            return self.status()

    def stop(self) -> dict[str, Any]:
        with self._lock:
            process = self._process
            if process is None:
                return self.status()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5.0)
                except subprocess.TimeoutExpired:
                    self._append_log("[controller] process did not terminate within 5s; killing", "stderr")
                    process.kill()
                    process.wait(timeout=5.0)
            self._last_returncode = process.returncode
            self._process = None
            return self.status()

    def restart(self, config_data: dict[str, Any] | None) -> dict[str, Any]:
        self.stop()
        return self.start(config_data)

    def status(self) -> dict[str, Any]:
        with self._lock:
            pid: int | None = None
            running = False
            returncode = self._last_returncode
            if self._process is not None:
                pid = self._process.pid
                polled = self._process.poll()
                running = polled is None
                if polled is not None:
                    returncode = polled
                    self._last_returncode = polled
                    self._process = None
                    pid = None
            return {
                "running": running,
                "pid": pid,
                "returncode": returncode,
                "started_at": self._started_at,
                "args": self._current_argv,
                "config": self._current_config,
                "last_profile": self._last_profile,
                "recent_errors": list(self._errors)[-50:],
            }

    def logs(self, tail: int = 200) -> dict[str, Any]:
        tail = max(1, min(int(tail), self._logs.maxlen or 5000))
        with self._lock:
            return {"lines": list(self._logs)[-tail:]}

    def _read_stream(self, stream, stream_name: str) -> None:
        try:
            for line in iter(stream.readline, ""):
                self._append_log(line.rstrip("\n"), stream_name)
        finally:
            try:
                stream.close()
            except Exception:
                pass

    def _append_log(self, line: str, stream_name: str) -> None:
        stamped = f"[{stream_name}] {line}"
        with self._lock:
            self._logs.append(stamped)
            if stream_name == "stderr" or "error" in line.lower() or "exception" in line.lower():
                self._errors.append(stamped)
            parsed = parse_profile_line(line)
            if parsed is not None:
                parsed["received_at"] = datetime.now(timezone.utc).isoformat()
                self._last_profile = parsed

