#!/usr/bin/env python3
"""PRESCUE USB API bridge V2, for 1tahap0_WEB_SAFE.ino.

Starts LOCKED. After hardware inspection, sensor ready and firmware Q
handshake, operator explicitly types ARM in THIS terminal. Browser cannot ARM.
Local host only: http://127.0.0.1:8000. Requires pyserial.

This is an operator-gated prototype controller, NOT medical equipment,
NOT a replacement for an independent emergency stop/pressure relief.
"""
import argparse
import json
import math
import re
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    raise SystemExit("Install pyserial: .venv/bin/pip install pyserial")

BAUD = 115200
MAX_TARGET = 150
MIN_TARGET = 20
ARM_SECONDS = 600  # ten minutes, or until disarmed / port lost
PRESSURE_FRESH_SEC = 8
FIRMWARE_FRESH_SEC = 8
ALLOWED_ORIGINS = {
    "http://localhost:5500", "http://127.0.0.1:5500", "http://[::1]:5500",
    "http://[::]:5500",
}

PRESS = re.compile(r"^# tekanan\s*=\s*(-?\d+(?:\.\d+)?)\s+mmHg,\s*mentah\s+(-?\d+(?:\.\d+)?)", re.I)
G_SAMPLE = re.compile(r"^G,FILL,([\d.]+),(-?[\d.]+)$")
L_SAMPLE = re.compile(r"^L,(SETTLE|RECORD),([\d.]+),(-?[\d.]+)$")
L_RESULT = re.compile(r"^# L awal=.*?\bn=(\d+)\s+bocor bertanda=(-?[\d.]+) mmHg/menit:\s*(LOLOS|GAGAL)", re.I)
G_RESULT = re.compile(r"^# G (.*?); durasi", re.I)
FW_READY = re.compile(
    r"^# PRESCUE_WEB_SAFE_V1 sensor_ok=([01]) pressure_mmHg=(-?[\d.]+) raw_adc=([\d.]+) sd=([\d.]+) hard_limit=180 target_limit=150$"
)
T_VENT = re.compile(r"^T,(-?[\d.]+)$")


class State:
    def __init__(self):
        self.lock = threading.RLock()
        self.write_lock = threading.Lock()
        self.samples = deque(maxlen=1600)
        self.events = deque(maxlen=45)
        self.serial = None
        self.connected = False
        self.serial_error = None
        self.pressure = None
        self.raw_adc = None
        self.last_pressure_at = None
        self.firmware_ok = False
        self.firmware_at = None
        self.fw_pressure = None
        self.arm_until = 0
        self.phase = "IDLE"
        self.test_mode = None
        self.target = None
        self.pump = None  # firmware does not report pin states; never invent
        self.valve = None
        self.leak_rate = None
        self.test_result = None
        self.sample_count = 0
        self.started_at = None
        self.testing = False
        self.last_seen = None
        self.last_t_poll = 0
        self.last_q_poll = 0
        self.prev_phase = None
        self.phase_base = 0
        self.last_elapsed = 0
        self.cmd_pending_at = None
        self.last_command = None
        self.last_error = None

s = State()


def pressure_fresh(now):
    return s.last_pressure_at is not None and now - s.last_pressure_at < PRESSURE_FRESH_SEC


def firmware_fresh(now):
    return s.firmware_at is not None and now - s.firmware_at < FIRMWARE_FRESH_SEC


def sensor_ready(now):
    return bool(s.connected and firmware_fresh(now) and s.firmware_ok
                and pressure_fresh(now) and s.pressure is not None
                and -10 <= s.pressure <= 170
                and s.raw_adc is not None and 80 <= s.raw_adc <= 4050)


def commands_ready(now):
    return bool(sensor_ready(now) and not s.testing and s.arm_until > now)


def status_snapshot():
    with s.lock:
        now = time.monotonic()
        is_ready = sensor_ready(now)
        controls = commands_ready(now)
        return {
            "connected": s.connected,
            "pressure_mmHg": s.pressure if pressure_fresh(now) else None,
            "target_mmHg": s.target,
            "phase": s.phase,
            "test_mode": s.test_mode,
            "pump_on": s.pump,
            "valve_open": s.valve,
            "sensor_ok": is_ready,
            "leak_rate_mmHg_min": s.leak_rate,
            "test_result": s.test_result,
            "adc_raw": s.raw_adc if pressure_fresh(now) else None,
            "sample_count": s.sample_count,
            "remote_commands_enabled": controls,
            "safety_ok": controls,  # software conditions+operator, not independent physical safety
            "operator_armed": s.arm_until > now,
            "firmware_ready": bool(s.firmware_ok and firmware_fresh(now)),
            "can_pressurize": bool(controls and s.pressure is not None and -10 <= s.pressure <= 20),
            "busy": s.testing,
            "last_command": s.last_command,
            "serial_error": s.serial_error,
            "is_simulated": False,
            "note": ("Perintah diizinkan oleh operator dan diagnostik firmware; "
                     "bukan sertifikasi keselamatan. Wajib uji tanpa pasien, "
                     "dengan manometer acuan dan pelepasan tekanan mandiri."),
        }


def parse_line(line):
    now = time.monotonic()
    with s.lock:
        s.last_seen = now
        s.events.append(line)
        match = FW_READY.match(line)
        if match:
            s.firmware_ok = match.group(1) == "1"
            s.firmware_at = now
            s.fw_pressure = float(match.group(2))
            s.pressure = s.fw_pressure
            s.raw_adc = float(match.group(3))
            s.last_pressure_at = now
            return
        match = PRESS.match(line)
        if match:
            s.pressure = float(match.group(1))
            s.raw_adc = float(match.group(2))
            s.last_pressure_at = now
            return
        match = G_SAMPLE.match(line)
        if match:
            add_sample("FILL", float(match.group(1)), float(match.group(2)), now)
            s.test_mode = "G" if s.test_mode != "L" else "L"
            return
        match = L_SAMPLE.match(line)
        if match:
            add_sample(match.group(1), float(match.group(2)), now)
            s.test_mode = "L"
            return
        match = T_VENT.match(line)
        if match:
            value = float(match.group(1))
            if math.isfinite(value):
                s.pressure = value
                s.last_pressure_at = now
                s.phase = "VENT"
            return
        match = L_RESULT.match(line)
        if match:
            s.test_mode = "L"
            s.sample_count = int(match.group(1))
            s.leak_rate = float(match.group(2))
            s.test_result = match.group(3).upper()
            s.testing = False
            s.phase = "IDLE"
            return
        match = G_RESULT.match(line)
        if match:
            success = ("target tercapai" in match.group(1).lower()
                       or "sudah mencapai target" in match.group(1).lower())
            # L first calls isiSampai(); the G result is NOT end of L.
            if s.test_mode == "L" and success:
                return
            s.testing = False
            s.phase = "IDLE"
            if not success:
                s.test_result = "GAGAL ISI"
                s.arm_until = 0
            return
        if "# HENTI DARURAT" in line or "# SENSOR TIDAK VALID" in line:
            s.testing = False
            s.phase = "FAULT"
            s.test_result = "DARURAT"
            s.arm_until = 0
            s.last_error = line
        elif "# DIHENTIKAN" in line:
            s.testing = False
            s.phase = "STOPPED"
            s.test_result = "DIHENTIKAN"
            s.arm_until = 0
        elif "# katup tertutup" in line and s.test_mode == "O":
            s.testing = False
            s.phase = "IDLE"
        elif "# Pompa mati, katup tertutup" in line and s.test_mode != "L":
            s.testing = False
            s.phase = "IDLE"
        elif "# DITOLAK" in line or "# Argumen tidak valid" in line:
            s.testing = False
            s.phase = "IDLE"
            s.test_result = "DITOLAK"
            s.last_error = line
            s.arm_until = 0


def add_sample(phase, seconds, pressure, now):
    if not (math.isfinite(seconds) and math.isfinite(pressure)):
        return
    if s.prev_phase != phase:
        s.phase_base = s.last_elapsed if s.prev_phase is not None else 0.0
        s.prev_phase = phase
    elapsed = s.phase_base + seconds
    s.last_elapsed = max(s.last_elapsed, elapsed)
    s.testing = True
    s.phase = phase
    s.pressure = pressure
    s.last_pressure_at = now
    s.sample_count += 1
    s.samples.append({"phase": phase, "elapsed_seconds": elapsed,
                      "pressure_mmHg": pressure})


def serial_write(command):
    with s.write_lock:
        with s.lock:
            ser = s.serial
            active = s.connected
        if not active or ser is None:
            raise RuntimeError("ESP32 tidak tersambung")
        ser.write((command + "\n").encode("ascii"))
        ser.flush()


def serial_worker(port):
    try:
        with serial.Serial(port=port, baudrate=BAUD, timeout=0.2, write_timeout=1) as ser:
            with s.lock:
                s.serial = ser
                s.connected = True
            time.sleep(2.0)  # USB serial may reset ESP32
            ser.reset_input_buffer()
            print(f"[USB] Terhubung {port} @ {BAUD}. Sensor poll dan Q handshake aktif.", flush=True)
            while True:
                try:
                    line = ser.readline().decode("utf-8", errors="replace").strip()
                    if line:
                        if line.startswith(("# PRESCUE_WEB_SAFE", "# G ", "# L ", "# HENTI", "# DIHENTIKAN", "# DITOLAK", "# SENSOR")):
                            print("[ESP32]", line, flush=True)
                        parse_line(line)
                    now = time.monotonic()
                    with s.lock:
                        # During tests, polling would be consumed by cekBatal(); suppress.
                        idle = not s.testing and (s.last_seen is None or now - s.last_seen >= 0.3)
                        poll_t = idle and now - s.last_t_poll >= 2
                        poll_q = idle and now - s.last_q_poll >= 3
                        if poll_t: s.last_t_poll = now
                        if poll_q: s.last_q_poll = now
                        # A missing reply should not leave the UI stuck indefinitely.
                        if s.testing and s.cmd_pending_at is not None and now - s.cmd_pending_at > 240:
                            s.last_error = "Waktu pengujian melewati 240 detik; periksa alat secara fisik"
                            s.arm_until = 0
                            s.cmd_pending_at = None
                    if poll_q:
                        serial_write("Q")
                    elif poll_t:
                        serial_write("T")
                except (serial.SerialException, OSError, RuntimeError) as e:
                    with s.lock:
                        s.serial_error = str(e)
                    print("[USB] Terputus:", e, flush=True)
                    break
    except Exception as e:
        with s.lock:
            s.serial_error = str(e)
        print("[USB] Gagal membuka port:", e, flush=True)
    finally:
        with s.lock:
            s.connected = False
            s.serial = None
            s.arm_until = 0
            s.firmware_ok = False
            s.testing = False
            s.phase = "DISCONNECTED"
        print("[SAFE] USB putus: kontrol terkunci. Periksa tekanan secara fisik.", flush=True)


def operator_console():
    print("\n[LOCKED] Sebelum ARM: rangkaian tanpa pasien, sambungan benar,"
          " kalibrasi dengan manometer acuan, katup/stop darurat independen"
          " sudah diuji, dan baterai aman.", flush=True)
    print("[OPERATOR] Ketik STATUS, ARM, atau DISARM di terminal ini."
          " ARM berlaku 10 menit, tanpa otomatis mengirim perintah.", flush=True)
    for line in sys.stdin:
        command = line.strip().upper()
        if command == "STATUS":
            data = status_snapshot()
            print("[STATUS]", json.dumps({k: data[k] for k in (
                "connected", "pressure_mmHg", "sensor_ok", "firmware_ready",
                "operator_armed", "remote_commands_enabled", "busy")}), flush=True)
        elif command == "DISARM":
            with s.lock: s.arm_until = 0
            print("[SAFE] DISARM: perintah baru dikunci. Jika alat sedang bekerja,"
                  " DISARM tidak menghentikan pompa; gunakan x/stop fisik.", flush=True)
        elif command == "ARM":
            with s.lock:
                now = time.monotonic()
                ready = sensor_ready(now) and not s.testing
                if ready: s.arm_until = now + ARM_SECONDS
            if ready:
                print("[ARMED] Kontrol web diizinkan 10 menit."
                      " Setiap G/L butuh tekanan awal <=20 mmHg dan konfirmasi di web.", flush=True)
            else:
                print("[LOCKED] Sensor / firmware / USB belum siap;"
                      " cek STATUS, tunggu pembacaan Q dan T.", flush=True)
        elif command:
            print("[OPERATOR] Ketik STATUS, ARM, atau DISARM.", flush=True)


class Handler(BaseHTTPRequestHandler):
    def allowed_origin(self):
        origin = self.headers.get("Origin")
        return origin if origin in ALLOWED_ORIGINS else None

    def add_cors(self):
        origin = self.allowed_origin()
        if origin:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")

    def send_json(self, data, code=200):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.add_cors()
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        if self.headers.get("Origin") and not self.allowed_origin():
            return self.send_json({"error": "Origin tidak diizinkan"}, 403)
        self.send_response(204)
        self.add_cors()
        self.end_headers()

    def path_only(self):
        return urlsplit(self.path).path

    def do_GET(self):
        path = self.path_only()
        if path == "/api/status":
            payload = status_snapshot()
            return self.send_json(payload, 200 if payload["connected"] else 503)
        if path == "/api/samples":
            with s.lock: samples = list(s.samples)
            return self.send_json({"samples": samples})
        if path == "/health":
            with s.lock:
                return self.send_json({"connected": s.connected, "serial_error": s.serial_error})
        return self.send_json({"error": "Not found"}, 404)

    def do_POST(self):
        if self.path_only() != "/api/command":
            return self.send_json({"error": "Not found"}, 404)
        if self.headers.get("Origin") and not self.allowed_origin():
            return self.send_json({"error": "Origin tidak diizinkan"}, 403)
        if self.headers.get("Content-Type", "").split(";")[0].strip().lower() != "application/json":
            return self.send_json({"error": "Content-Type wajib application/json"}, 415)
        try:
            content_length = int(self.headers.get("Content-Length", "0"))
            if content_length < 1 or content_length > 256:
                return self.send_json({"error": "Ukuran JSON tidak valid"}, 413)
            data = json.loads(self.rfile.read(content_length))
            command = data.get("command") if isinstance(data, dict) else None
        except (ValueError, TypeError, UnicodeDecodeError):
            return self.send_json({"error": "JSON invalid"}, 400)
        if not isinstance(command, str):
            return self.send_json({"error": "Field command harus string"}, 400)
        command = command.strip()
        if command.upper() == "X":
            # x is interpreted by firmware's cekBatal() during active G/L/O.
            with s.lock:
                if not s.connected or not s.testing:
                    return self.send_json({"error": "Tidak ada pengujian aktif"}, 409)
            try: serial_write("x")
            except Exception as e: return self.send_json({"error": str(e)}, 503)
            return self.send_json({"accepted": True,
                                   "message": "Pembatalan x dikirim; cek respons firmware. Tidak membuang tekanan."}, 202)

        match = re.fullmatch(r"(G|L):(\d{1,3})", command)
        if match:
            target = int(match.group(2))
            if not MIN_TARGET <= target <= MAX_TARGET:
                return self.send_json({"error": "Target web hanya 20–150 mmHg"}, 400)
            mode = match.group(1)
        elif command == "O":
            target = None
            mode = "O"
        else:
            return self.send_json({"error": "Perintah yang diizinkan: G:20..150, L:20..150, O, X"}, 400)

        with s.lock:
            now = time.monotonic()
            if not commands_ready(now):
                return self.send_json({"error": "Kontrol terkunci: USB/Q/sensor/ARM tidak siap atau pengujian masih aktif"}, 423)
            if mode in ("G", "L") and (s.pressure is None or not -10 <= s.pressure <= 20):
                return self.send_json({"error": "Untuk G/L, tekanan awal harus <=20 mmHg. Buang tekanan dengan aman dulu."}, 409)
            s.testing = True  # lock out a second command BEFORE serial write
            s.phase = "STARTING" if mode != "O" else "VENT"
            s.test_mode = mode
            s.target = target
            s.last_command = command
            s.started_at = now
            s.cmd_pending_at = now
            s.leak_rate = None if mode == "L" else s.leak_rate
            s.test_result = None
            s.prev_phase = None
            s.phase_base = 0
            s.last_elapsed = 0
            s.samples.clear()
            s.sample_count = 0
        try:
            serial_write(command)
        except Exception as e:
            with s.lock:
                s.testing = False
                s.arm_until = 0
                s.phase = "FAULT"
                s.last_error = str(e)
            return self.send_json({"error": f"Perintah gagal ditulis ke USB: {e}"}, 503)
        print("[COMMAND]", command, "dikirim ke ESP32 (belum berarti uji selesai)", flush=True)
        return self.send_json({"accepted": True, "command": command,
                               "message": "Dikirim ke ESP32. Pantau proses/hasil; jaga stop fisik tersedia."}, 202)

    def log_message(self, format, *args):
        status = str(args[1]) if len(args) > 1 else ""
        if status not in ("200", "204"):
            print("[HTTP]", format % args, flush=True)


def main():
    parser = argparse.ArgumentParser(description="PRESCUE operator-gated USB bridge")
    parser.add_argument("--list", action="store_true", help="Daftar port ESP32")
    parser.add_argument("--port", help="Contoh /dev/cu.usbserial-0001")
    parser.add_argument("--http-port", type=int, default=8000)
    args = parser.parse_args()
    if args.list:
        for port in list_ports.comports():
            print(port.device, "|", port.description)
        return
    if not args.port:
        parser.error("Gunakan --port /dev/cu.usbserial-0001")
    threading.Thread(target=serial_worker, args=(args.port,), daemon=True).start()
    threading.Thread(target=operator_console, daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler)
    print(f"[HTTP] http://127.0.0.1:{args.http_port}/api/status", flush=True)
    print("[SAFE] Awalnya LOCKED; hanya firmware WEB_SAFE_V1 + sensor siap + ARM terminal membuka G/L/O.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[SAFE] Server dihentikan. Periksa dan bebaskan tekanan secara fisik.", flush=True)
    finally:
        server.server_close()

if __name__ == "__main__":
    main()
