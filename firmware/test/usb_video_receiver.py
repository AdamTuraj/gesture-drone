#!/usr/bin/env python3
"""Receive ESP32-S3 camera JPEG chunks and serve them as a local MJPEG webpage."""

from __future__ import annotations

import argparse
import json
import struct
import sys
import threading
import time
import webbrowser
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:
    raise SystemExit("Install pyserial first: python -m pip install pyserial") from exc


MAGIC = b"CAM0"
HEADER = struct.Struct("<IIHHH")
MAX_PAYLOAD = 1200
MAX_CHUNKS = 4096
STREAM_BOUNDARY = b"frame"


INDEX_HTML = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 USB Video</title>
  <style>
    :root {
      color-scheme: dark;
      font-family: Inter, Segoe UI, system-ui, sans-serif;
      background: #111816;
      color: #f0f7f3;
    }

    * {
      box-sizing: border-box;
    }

    body {
      margin: 0;
      min-height: 100vh;
      display: grid;
      grid-template-rows: auto 1fr;
      background:
        radial-gradient(circle at 15% 0%, rgba(48, 132, 102, 0.18), transparent 34rem),
        #111816;
    }

    header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 1rem;
      padding: 0.9rem 1rem;
      border-bottom: 1px solid rgba(255, 255, 255, 0.08);
      background: rgba(15, 22, 20, 0.82);
      backdrop-filter: blur(10px);
    }

    h1 {
      margin: 0;
      font-size: 1rem;
      font-weight: 650;
    }

    .stats {
      display: flex;
      flex-wrap: wrap;
      justify-content: flex-end;
      gap: 0.4rem;
      font-variant-numeric: tabular-nums;
      font-size: 0.82rem;
      color: #d7e5dc;
    }

    .stats span {
      min-width: 5.5rem;
      padding: 0.34rem 0.55rem;
      border: 1px solid rgba(255, 255, 255, 0.09);
      border-radius: 6px;
      background: rgba(255, 255, 255, 0.05);
      text-align: center;
    }

    main {
      display: grid;
      place-items: center;
      padding: 1rem;
    }

    .viewport {
      width: min(100%, 1100px);
      aspect-ratio: 4 / 3;
      display: grid;
      place-items: center;
      overflow: hidden;
      border: 1px solid rgba(255, 255, 255, 0.12);
      border-radius: 8px;
      background: #050806;
      box-shadow: 0 24px 70px rgba(0, 0, 0, 0.35);
    }

    img {
      width: 100%;
      height: 100%;
      object-fit: contain;
      display: block;
    }

    @media (max-width: 640px) {
      header {
        align-items: stretch;
        flex-direction: column;
      }

      .stats {
        justify-content: stretch;
      }

      .stats span {
        flex: 1 1 30%;
        min-width: 0;
      }
    }
  </style>
</head>
<body>
  <header>
    <h1>ESP32 USB Video</h1>
    <div class="stats">
      <span id="frames">0 frames</span>
      <span id="fps">0.0 fps</span>
      <span id="size">0 KB</span>
      <span id="age">waiting</span>
    </div>
  </header>
  <main>
    <div class="viewport">
      <img src="/stream.mjpg" alt="ESP32 camera stream">
    </div>
  </main>
  <script>
    const fmt = new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 });

    async function updateStatus() {
      try {
        const response = await fetch('/status.json', { cache: 'no-store' });
        const status = await response.json();
        document.getElementById('frames').textContent = `${status.frames} frames`;
        document.getElementById('fps').textContent = `${fmt.format(status.fps)} fps`;
        document.getElementById('size').textContent = `${fmt.format(status.latest_size / 1024)} KB`;
        document.getElementById('age').textContent = status.latest_age === null
          ? 'waiting'
          : `${fmt.format(status.latest_age)} s`;
      } catch (error) {
        document.getElementById('age').textContent = 'offline';
      }
    }

    updateStatus();
    setInterval(updateStatus, 500);
  </script>
</body>
</html>
"""


@dataclass
class PartialFrame:
    chunk_count: int
    created_at: float
    chunks: list[bytes | None]
    received: int = 0


class FrameStore:
    def __init__(self) -> None:
        self._condition = threading.Condition()
        self._latest_jpeg: bytes | None = None
        self._latest_frame_id = 0
        self._latest_size = 0
        self._sequence = 0
        self._complete_frames = 0
        self._started_at = time.monotonic()
        self._updated_at: float | None = None

    def update(self, frame_id: int, jpeg: bytes) -> None:
        with self._condition:
            self._latest_jpeg = jpeg
            self._latest_frame_id = frame_id
            self._latest_size = len(jpeg)
            self._sequence += 1
            self._complete_frames += 1
            self._updated_at = time.monotonic()
            self._condition.notify_all()

    def wait_for_next(self, last_sequence: int, timeout: float) -> tuple[int, int, bytes] | None:
        with self._condition:
            ready = self._condition.wait_for(
                lambda: self._latest_jpeg is not None and self._sequence != last_sequence,
                timeout=timeout,
            )
            if not ready or self._latest_jpeg is None:
                return None

            return self._sequence, self._latest_frame_id, self._latest_jpeg

    def snapshot(self) -> tuple[int, bytes] | None:
        with self._condition:
            if self._latest_jpeg is None:
                return None

            return self._latest_frame_id, self._latest_jpeg

    def status(self) -> dict[str, int | float | None]:
        with self._condition:
            now = time.monotonic()
            elapsed = max(now - self._started_at, 0.001)
            latest_age = None if self._updated_at is None else now - self._updated_at
            return {
                "frames": self._complete_frames,
                "frame_id": self._latest_frame_id,
                "fps": self._complete_frames / elapsed,
                "latest_size": self._latest_size,
                "latest_age": latest_age,
            }


class CameraHttpServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        server_address: tuple[str, int],
        frame_store: FrameStore,
        http_log: bool,
    ) -> None:
        self.frame_store = frame_store
        self.http_log = http_log
        super().__init__(server_address, CameraHttpHandler)


class CameraHttpHandler(BaseHTTPRequestHandler):
    server: CameraHttpServer
    protocol_version = "HTTP/1.1"

    def log_message(self, format: str, *args: object) -> None:
        if self.server.http_log:
            super().log_message(format, *args)

    def do_GET(self) -> None:
        path = self.path.split("?", 1)[0]
        if path == "/":
            self._send_bytes(INDEX_HTML.encode("utf-8"), "text/html; charset=utf-8")
        elif path == "/stream.mjpg":
            self._stream_mjpeg()
        elif path == "/snapshot.jpg":
            self._send_snapshot()
        elif path == "/status.json":
            self._send_json(self.server.frame_store.status())
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def _send_bytes(self, body: bytes, content_type: str, status: HTTPStatus = HTTPStatus.OK) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_json(self, data: dict[str, int | float | None]) -> None:
        self._send_bytes(json.dumps(data).encode("utf-8"), "application/json")

    def _send_snapshot(self) -> None:
        snapshot = self.server.frame_store.snapshot()
        if snapshot is None:
            self.send_error(HTTPStatus.SERVICE_UNAVAILABLE, "No frame received yet")
            return

        _, jpeg = snapshot
        self._send_bytes(jpeg, "image/jpeg")

    def _stream_mjpeg(self) -> None:
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", f"multipart/x-mixed-replace; boundary={STREAM_BOUNDARY.decode()}")
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

        last_sequence = 0
        while True:
            frame = self.server.frame_store.wait_for_next(last_sequence, timeout=10.0)
            if frame is None:
                continue

            last_sequence, frame_id, jpeg = frame
            header = (
                b"--"
                + STREAM_BOUNDARY
                + b"\r\nContent-Type: image/jpeg\r\nContent-Length: "
                + str(len(jpeg)).encode("ascii")
                + b"\r\nX-Frame-Id: "
                + str(frame_id).encode("ascii")
                + b"\r\n\r\n"
            )

            try:
                self.wfile.write(header)
                self.wfile.write(jpeg)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
                return


def find_default_port() -> str | None:
    candidates = []
    for port in list_ports.comports():
        text = " ".join(
            str(value).lower()
            for value in (port.device, port.description, port.manufacturer, port.hwid)
            if value
        )
        if any(token in text for token in ("usb jtag", "usb serial", "usb_serial", "esp32", "espressif")):
            candidates.append(port.device)

    if len(candidates) == 1:
        return candidates[0]

    return None


def list_serial_ports() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return

    for port in ports:
        print(f"{port.device}: {port.description} [{port.hwid}]")


def read_exact(port: serial.Serial, size: int) -> bytes | None:
    data = bytearray()
    deadline = time.monotonic() + max(port.timeout or 0, 0.1)

    while len(data) < size:
        chunk = port.read(size - len(data))
        if chunk:
            data.extend(chunk)
            deadline = time.monotonic() + max(port.timeout or 0, 0.1)
        elif time.monotonic() >= deadline:
            return None

    return bytes(data)


def read_chunk(port: serial.Serial) -> tuple[int, int, int, bytes] | None:
    sync = bytearray()

    while True:
        byte = port.read(1)
        if not byte:
            return None

        sync.extend(byte)
        if len(sync) > len(MAGIC):
            del sync[0]

        if bytes(sync) != MAGIC:
            continue

        header_tail = read_exact(port, HEADER.size - len(MAGIC))
        if header_tail is None:
            return None

        magic, frame_id, chunk_id, chunk_count, payload_len = HEADER.unpack(MAGIC + header_tail)
        if (
            magic != 0x304D4143
            or chunk_count == 0
            or chunk_count > MAX_CHUNKS
            or chunk_id >= chunk_count
            or payload_len > MAX_PAYLOAD
        ):
            sync.clear()
            continue

        payload = read_exact(port, payload_len)
        if payload is None:
            return None

        return frame_id, chunk_id, chunk_count, payload


def display_host(host: str) -> str:
    return "localhost" if host in ("", "0.0.0.0", "::") else host


def receive(args: argparse.Namespace) -> int:
    port_name = args.port or find_default_port()
    if not port_name:
        print("Could not auto-detect the ESP32 USB serial port. Use --list-ports or pass --port COMx.")
        return 2

    output_dir = Path(args.output)
    if args.save:
        output_dir.mkdir(parents=True, exist_ok=True)

    frame_store = FrameStore()
    httpd = CameraHttpServer((args.http_host, args.http_port), frame_store, args.http_log)
    http_thread = threading.Thread(target=httpd.serve_forever, name="mjpeg-http", daemon=True)
    http_thread.start()

    actual_host, actual_port = httpd.server_address
    url = f"http://{display_host(actual_host)}:{actual_port}/"
    print(f"Web stream: {url}")
    if args.open_browser:
        webbrowser.open(url)

    partial_frames: dict[int, PartialFrame] = {}
    started_at = time.monotonic()

    print(f"Opening {port_name} at {args.baud} baud...")
    try:
        with serial.Serial(port_name, args.baud, timeout=args.timeout) as port:
            port.reset_input_buffer()
            print("Receiving camera frames. Press Ctrl+C to stop.")

            while args.max_frames <= 0 or frame_store.status()["frames"] < args.max_frames:
                chunk = read_chunk(port)
                if chunk is None:
                    continue

                frame_id, chunk_id, chunk_count, payload = chunk
                frame = partial_frames.get(frame_id)
                if frame is None or frame.chunk_count != chunk_count:
                    frame = PartialFrame(
                        chunk_count=chunk_count,
                        created_at=time.monotonic(),
                        chunks=[None] * chunk_count,
                    )
                    partial_frames[frame_id] = frame

                if frame.chunks[chunk_id] is None:
                    frame.chunks[chunk_id] = payload
                    frame.received += 1

                if frame.received == frame.chunk_count:
                    jpeg = b"".join(part for part in frame.chunks if part is not None)
                    frame_store.update(frame_id, jpeg)
                    status = frame_store.status()

                    if args.save and (status["frames"] % args.save_every) == 0:
                        (output_dir / f"frame_{frame_id:08d}.jpg").write_bytes(jpeg)

                    elapsed = max(time.monotonic() - started_at, 0.001)
                    print(
                        f"\rframes={status['frames']} latest_id={frame_id} "
                        f"size={len(jpeg)} fps={status['frames'] / elapsed:.1f}",
                        end="",
                        flush=True,
                    )

                    del partial_frames[frame_id]

                now = time.monotonic()
                stale_ids = [
                    fid for fid, frame in partial_frames.items() if now - frame.created_at > args.stale_seconds
                ]
                for fid in stale_ids:
                    del partial_frames[fid]
    finally:
        httpd.shutdown()
        httpd.server_close()

    print()
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial port, for example COM7 on Windows or /dev/ttyACM0 on Linux.")
    parser.add_argument("--baud", type=int, default=921600, help="Serial baud value; USB Serial/JTAG ignores it.")
    parser.add_argument("--timeout", type=float, default=1.0, help="Serial read timeout in seconds.")
    parser.add_argument("--http-host", default="127.0.0.1", help="HTTP bind host.")
    parser.add_argument("--http-port", type=int, default=8080, help="HTTP bind port; use 0 for an automatic port.")
    parser.add_argument("--http-log", action="store_true", help="Print HTTP request logs.")
    parser.add_argument("--open-browser", action="store_true", help="Open the webpage in the default browser.")
    parser.add_argument("--save", action="store_true", help="Also write JPEG frames to disk.")
    parser.add_argument("--no-save", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--output", default=str(Path(__file__).with_name("captures")), help="JPEG output folder.")
    parser.add_argument("--save-every", type=int, default=1, help="Save every Nth complete frame when --save is used.")
    parser.add_argument("--max-frames", type=int, default=0, help="Stop after N complete frames; 0 means forever.")
    parser.add_argument("--stale-seconds", type=float, default=2.0, help="Discard incomplete frames after this age.")
    parser.add_argument("--list-ports", action="store_true", help="List available serial ports and exit.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.list_ports:
        list_serial_ports()
        return 0

    if args.no_save:
        args.save = False

    if args.save_every < 1:
        print("--save-every must be 1 or greater.")
        return 2

    try:
        return receive(args)
    except KeyboardInterrupt:
        print()
        return 0


if __name__ == "__main__":
    sys.exit(main())
