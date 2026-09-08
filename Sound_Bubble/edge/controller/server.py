from __future__ import annotations

import argparse
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse

from .process_manager import ProcessManager


def query_audio_devices() -> dict[str, Any]:
    try:
        import sounddevice as sd
    except ImportError as exc:
        raise RuntimeError("sounddevice is not installed in this environment") from exc

    devices = sd.query_devices()
    hostapis = sd.query_hostapis()
    result = []
    for index, device in enumerate(devices):
        hostapi_name = hostapis[device["hostapi"]]["name"]
        result.append(
            {
                "index": index,
                "name": device["name"],
                "hostapi": hostapi_name,
                "max_input_channels": int(device["max_input_channels"]),
                "max_output_channels": int(device["max_output_channels"]),
                "default_samplerate": float(device["default_samplerate"]),
            }
        )
    return {"devices": result}


class ControllerServer(ThreadingHTTPServer):
    def __init__(self, server_address, RequestHandlerClass, *, manager: ProcessManager, token: str | None):
        super().__init__(server_address, RequestHandlerClass)
        self.manager = manager
        self.token = token


class Handler(BaseHTTPRequestHandler):
    server: ControllerServer

    def log_message(self, fmt: str, *args) -> None:  # keep stdout clean; journald still captures process logs
        print(f"[controller-http] {self.address_string()} - {fmt % args}")

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        try:
            if parsed.path == "/health":
                self._json(200, {"ok": True})
                return
            self._require_auth()
            if parsed.path == "/status":
                self._json(200, self.server.manager.status())
            elif parsed.path == "/logs":
                qs = parse_qs(parsed.query)
                tail = int(qs.get("tail", ["200"])[0])
                self._json(200, self.server.manager.logs(tail=tail))
            elif parsed.path == "/devices":
                self._json(200, query_audio_devices())
            else:
                self._json(404, {"error": "not found"})
        except Exception as exc:
            self._json(400, {"error": str(exc)})

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        try:
            self._require_auth()
            body = self._read_json_body()
            config = body.get("config") if isinstance(body, dict) else None
            if parsed.path == "/start":
                self._json(200, self.server.manager.start(config))
            elif parsed.path == "/stop":
                self._json(200, self.server.manager.stop())
            elif parsed.path == "/restart":
                self._json(200, self.server.manager.restart(config))
            else:
                self._json(404, {"error": "not found"})
        except Exception as exc:
            self._json(400, {"error": str(exc)})

    def _require_auth(self) -> None:
        token = self.server.token
        if token is None:
            return
        expected = f"Bearer {token}"
        actual = self.headers.get("Authorization", "")
        if actual != expected:
            raise PermissionError("missing or invalid bearer token")

    def _read_json_body(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length == 0:
            return {}
        raw = self.rfile.read(length)
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))

    def _json(self, status: int, payload: dict[str, Any]) -> None:
        data = json.dumps(payload, indent=2, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def serve(host: str, port: int, token: str | None) -> None:
    manager = ProcessManager()
    httpd = ControllerServer((host, port), Handler, manager=manager, token=token)
    print(f"[controller] listening on http://{host}:{port}")
    if token is None:
        print("[controller-warning] bearer token disabled; only use this for local testing")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("[controller] stopping")
    finally:
        manager.stop()
        httpd.server_close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Sound Bubble remote controller HTTP API")
    parser.add_argument("--host", default=os.environ.get("SOUND_BUBBLE_CONTROLLER_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("SOUND_BUBBLE_CONTROLLER_PORT", "8000")))
    parser.add_argument("--allow-no-token", action="store_true", help="Allow unauthenticated non-health endpoints. Use only for local testing.")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    token = os.environ.get("SOUND_BUBBLE_CONTROLLER_TOKEN")
    if not token and not args.allow_no_token:
        raise SystemExit("Set SOUND_BUBBLE_CONTROLLER_TOKEN or pass --allow-no-token for local testing.")
    serve(args.host, args.port, token)

