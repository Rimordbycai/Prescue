#!/usr/bin/env python3
"""PRESCUE V2 USB serial bridge: benchtop prototype, hardware safety NOT certified.

Control requires physical operator confirmation (ARM BENCH). Wi-Fi remains
read-only telemetry. V2 firmware must have USB_V2 probe and heartbeat patch.
Not for human use; independent physical emergency stop and pressure relief needed.
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
    raise SystemExit('Install pyserial: python3 -m pip install pyserial')

BAUD = 115200
ARM_SECONDS = 300
PROBE_FRESH = 4.0
ALLOWED_ORIGINS = {'http://localhost:5500', 'http://127.0.0.1:5500'}
PROBE = re.compile(r'^# PRESCUE_USB_V2 sensor_ok=(0|1) pressure_mmHg=(-?[\d.]+) raw_adc=([\d.]+) sd=([\d.]+) independent_mmHg=(-?[\d.]+) hard_limit=180 target_limit=150$')
G_SAMPLE = re.compile(r'^G,FILL,([\d.]+),(-?[\d.]+)$')
L_SAMPLE = re.compile(r'^L,(SETTLE|RECORD),([\d.]+),(-?[\d.]+)$')
G_RESULT = re.compile(r'^# G (.*?); durasi')
L_RESULT = re.compile(r'^# L awal=.*?\bn=(\d+)\s+bocor bertanda=(-?[\d.]+) mmHg/menit:\s*(LOLOS|GAGAL)')
VENT = re.compile(r'^T,(-?[\d.]+)$')

class State:
    def __init__(self):
        self.lock = threading.RLock()
        self.write_lock = threading.Lock()
        self.serial = None
        self.connected = False
        self.fw_ready = False
        self.last_probe = 0.0
        self.sensor_ok = False
        self.pressure = None
        self.raw_adc = None
        self.independent = None
        self.arm_until = 0.0
        self.busy = False
        self.starting = False
        self.mode = None
        self.phase = 'DISCONNECTED'
        self.target = None
        self.last_command = None
        self.last_error = None
        self.test_result = None
        self.leak_rate = None
        self.samples = deque(maxlen=400)
        self.events = deque(maxlen=40)
        self.last_activity = 0.0

S = State()

def ready_locked(now):
    return (S.connected and S.fw_ready and S.sensor_ok and
            now - S.last_probe < PROBE_FRESH and
            S.pressure is not None and math.isfinite(S.pressure) and
            S.independent is not None and math.isfinite(S.independent))

def snapshot():
    with S.lock:
        now = time.monotonic()
        ready = ready_locked(now)
        armed = S.arm_until > now
        return {
            'connected': S.connected,
            'connection_type': 'usb_serial_v2',
            'is_simulated': False,
            'api_mode': 'usb_operator_gated',
            'firmware_ready': S.fw_ready and (S.busy or now-S.last_probe < PROBE_FRESH),
            'sensor_ok': ready,
            'operator_armed': armed,
            'remote_commands_enabled': ready and armed and not S.busy,
            # Software checks are NOT equivalent to verified physical safety.
            'safety_ok': False,
            'physical_safety_verified': False,
            'can_pressurize': ready and armed and not S.busy and -10 <= S.pressure <= 20,
            'pressure_mmHg': S.pressure,
            'independent_pressure_mmHg': S.independent,
            'adc_raw': S.raw_adc,
            'phase': S.phase,
            'test_mode': S.mode,
            'target_mmHg': S.target,
            'busy': S.busy,
            'sample_count': len(S.samples),
            'test_result': S.test_result,
            'leak_rate_mmHg_min': S.leak_rate,
            'last_command': S.last_command,
            'last_error': S.last_error,
            'events': list(S.events)[-8:],
            'note': 'USB sends commands only with operator ARM BENCH. Physical safety is unverified.'
        }

def record(phase, seconds, pressure):
    if not (math.isfinite(seconds) and math.isfinite(pressure)):
        return
    S.pressure = pressure
    S.phase = phase
    S.samples.append({'phase': phase, 'elapsed_seconds': seconds, 'pressure_mmHg': pressure})

def parse_line(line):
    with S.lock:
        S.events.append(line)
        S.last_activity = time.monotonic()
        probe = PROBE.match(line)
        if probe:
            ok, p, raw, sd, ind = probe.groups()
            p, raw, sd, ind = map(float, (p,raw,sd,ind))
            S.fw_ready = True
            S.last_probe = time.monotonic()
            S.sensor_ok = ok == '1' and all(map(math.isfinite, (p,raw,sd,ind)))
            S.pressure, S.raw_adc, S.independent = p, raw, ind
            return
        if line.startswith('# REMOTE_ACCEPT '):
            S.starting = False
            return
        g = G_SAMPLE.match(line)
        if g:
            record('FILL', float(g.group(1)), float(g.group(2)))
            return
        l = L_SAMPLE.match(line)
        if l:
            record(l.group(1), float(l.group(2)), float(l.group(3)))
            return
        t = VENT.match(line)
        if t:
            try: S.pressure = float(t.group(1))
            except ValueError: pass
            S.phase = 'VENT'
            return
        result = L_RESULT.match(line)
        if result:
            S.leak_rate = float(result.group(2))
            S.test_result = result.group(3)
            S.busy = False
            S.phase = 'DONE'
            S.arm_until = 0
            return
        result = G_RESULT.match(line)
        if result:
            success = 'target tercapai' in result.group(1) or 'sudah mencapai target' in result.group(1)
            if S.mode == 'L' and success:
                return
            S.busy = False
            S.phase = 'DONE' if success else 'FAULT'
            S.test_result = 'TARGET' if success else 'GAGAL ISI'
            S.arm_until = 0
            return
        if '# katup tertutup' in line and S.mode == 'O':
            S.busy = False
            S.phase = 'DONE'
            S.arm_until = 0
            return
        if ('# DIHENTIKAN' in line or '# REMOTE_LINK_LOST' in line or
            '# SENSOR FAULT' in line or '# HENTI DARURAT' in line or
            '# REMOTE_REJECT' in line or '# DITOLAK' in line or
            '# Argumen tidak valid' in line):
            S.busy = False
            S.arm_until = 0
            S.phase = 'FAULT' if 'DIHENTIKAN' not in line else 'STOPPED'
            S.test_result = S.phase
            S.last_error = line

def write_serial(command):
    with S.write_lock:
        with S.lock:
            handle = S.serial
            connected = S.connected
        if not connected or handle is None:
            raise RuntimeError('USB tidak tersambung')
        handle.write((command + '\n').encode('ascii'))
        handle.flush()

def serial_worker(port):
    try:
        with serial.Serial(port, BAUD, timeout=0.12, write_timeout=1) as dev:
            with S.lock:
                S.serial = dev
                S.connected = True
                S.phase = 'IDLE'
            print('[USB] Terhubung:', port, '| Serial Monitor Arduino harus ditutup', flush=True)
            time.sleep(2.2)  # Typical ESP32 reset when serial port is opened
            dev.reset_input_buffer()
            last_probe = last_heartbeat = 0.0
            while True:
                line = dev.readline().decode('utf-8', errors='replace').strip()
                if line:
                    parse_line(line)
                    if (line.startswith(('# REMOTE', '# DIHENTIKAN', '# SENSOR FAULT', '# HENTI DARURAT', '# G ', '# L '))):
                        print('[ESP32]', line, flush=True)
                now = time.monotonic()
                with S.lock:
                    running = S.busy
                    armed = S.arm_until > now
                    if S.busy and now - S.last_activity > 240:
                        S.last_error = 'Tidak ada kemajuan: periksa hardware dan STOP fisik'
                        S.arm_until = 0
                if (armed or running) and now-last_heartbeat > 0.45:
                    write_serial('~')
                    last_heartbeat = now
                if not running and now-last_probe > 1.0:
                    write_serial('Z')  # safe diagnostic; NEVER Q
                    last_probe = now
    except (Exception, KeyboardInterrupt) as exc:
        print('[USB] Terputus / gagal:', str(exc), flush=True)
    finally:
        with S.lock:
            S.serial = None
            S.connected = False
            S.arm_until = 0
            S.fw_ready = False
            S.sensor_ok = False
            S.busy = False
            S.phase = 'DISCONNECTED'
        print('[USB] Kontrol dikunci. Kondisi tekanan fisik wajib diperiksa.', flush=True)

def operator_console():
    print('\n[LOCKED] BENCH ONLY: tanpa pasien / orang; manometer acuan, STOP fisik,'
          ' pelepasan tekanan independen, sensor, dan wiring wajib diperiksa.', flush=True)
    print('[OPERATOR] Ketik STATUS, ARM BENCH, atau DISARM di terminal.', flush=True)
    for line in sys.stdin:
        command = line.strip().upper()
        if command == 'STATUS':
            data = snapshot()
            print('[STATUS]', json.dumps({k: data[k] for k in (
                'connected','firmware_ready','sensor_ok','pressure_mmHg',
                'operator_armed','busy','phase','last_error')}), flush=True)
        elif command == 'ARM BENCH':
            with S.lock:
                now = time.monotonic()
                ok = ready_locked(now) and not S.busy and -10 <= S.pressure <= 20
                if ok:
                    S.arm_until = now + ARM_SECONDS
            print('[ARMED] 5 menit: G/L/O diizinkan hanya untuk bench.' if ok
                  else '[LOCKED] USB V2, sensor, tekanan awal atau firmware belum siap.', flush=True)
        elif command == 'DISARM':
            with S.lock: S.arm_until = 0
            print('[DISARM] Hanya menolak perintah baru. TIDAK menghentikan tes aktif!',flush=True)
        elif command:
            print('[OPERATOR] STATUS | ARM BENCH | DISARM',flush=True)

class Handler(BaseHTTPRequestHandler):
    def cors(self):
        origin = self.headers.get('Origin')
        if origin in ALLOWED_ORIGINS:
            self.send_header('Access-Control-Allow-Origin',origin)
            self.send_header('Vary','Origin')
        self.send_header('Access-Control-Allow-Headers','Content-Type')
        self.send_header('Access-Control-Allow-Methods','GET,POST,OPTIONS')
    def respond(self, code, data):
        raw = json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type','application/json; charset=utf-8')
        self.send_header('Cache-Control','no-store')
        self.send_header('Content-Length',str(len(raw)))
        self.cors()
        self.end_headers()
        self.wfile.write(raw)
    def origin_ok(self):
        return self.headers.get('Origin') in (None, *ALLOWED_ORIGINS)
    def do_OPTIONS(self):
        if not self.origin_ok():return self.respond(403,{'error':'Origin not allowed'})
        self.send_response(204); self.cors(); self.end_headers()
    def do_GET(self):
        if not self.origin_ok():return self.respond(403,{'error':'Origin not allowed'})
        path=urlsplit(self.path).path
        if path=='/api/status':return self.respond(200,snapshot())
        if path=='/api/samples':
            with S.lock: samples=list(S.samples)
            return self.respond(200,{'samples':samples})
        if path=='/health':return self.respond(200,{'service':'prescue_usb_v2','connected':S.connected})
        return self.respond(404,{'error':'Not found'})
    def do_POST(self):
        if urlsplit(self.path).path!='/api/command':return self.respond(404,{'error':'Not found'})
        if not self.origin_ok():return self.respond(403,{'error':'Origin not allowed'})
        if self.headers.get('Content-Type','').split(';')[0].lower()!='application/json':
            return self.respond(415,{'error':'JSON required'})
        try:
            n=int(self.headers.get('Content-Length','0'))
            if n<1 or n>90:raise ValueError('Invalid request size')
            obj=json.loads(self.rfile.read(n))
            cmd=obj.get('command') if isinstance(obj,dict) else None
            if not isinstance(cmd,str):raise ValueError('Invalid command')
        except (ValueError,TypeError):return self.respond(400,{'error':'Invalid payload'})
        if cmd=='X':
            with S.lock:
                if not S.connected or not S.busy:
                    return self.respond(409,{'error':'Tidak ada pengujian USB aktif'})
            try:write_serial('x')
            except Exception as e:return self.respond(503,{'error':str(e)})
            return self.respond(202,{'accepted':True,'forwarded_to_usb':True,'command':'X',
                                     'message':'x dikirim. Tidak membuang tekanan sisa; periksa fisik.'})
        m=re.fullmatch(r'([GL]):([0-9]{2,3})',cmd)
        if m:
            target=int(m.group(2));mode=m.group(1)
            if not 20<=target<=150:return self.respond(400,{'error':'Target 20..150 mmHg'})
        elif cmd=='O':
            target=None;mode='O'
        else:return self.respond(400,{'error':'Hanya G:20..150, L:20..150, O, X'})
        with S.lock:
            now=time.monotonic()
            if not ready_locked(now) or not S.arm_until>now or S.busy:
                return self.respond(423,{'error':'Belum ARM BENCH, firmware V2/sensor tidak siap, atau sibuk'})
            if mode in ('G','L') and not -10<=S.pressure<=20:
                return self.respond(409,{'error':'Tekanan awal harus <=20 mmHg'})
            S.busy=True
            S.starting=True
            S.mode=mode
            S.phase='STARTING' if mode!='O' else 'VENT'
            S.target=target
            S.last_command=cmd
            S.test_result=None
            S.leak_rate=None
            S.last_error=None
            S.samples.clear()
        try:
            # Firmware requires fresh heartbeat and !-prefix for bridge commands.
            write_serial('~')
            time.sleep(0.1)
            write_serial('!'+cmd)
        except Exception as e:
            with S.lock:
                S.busy=False;S.arm_until=0;S.last_error=str(e);S.phase='FAULT'
            return self.respond(503,{'error':'USB write failed: '+str(e)})
        print('[USB COMMAND]',cmd,'sent (NOT confirmation of actuation)',flush=True)
        return self.respond(202,{'accepted':True,'forwarded_to_usb':True,'command':cmd,
                                 'message':'Sent via USB; check dashboard Wi-Fi telemetry and physical device.'})
    def log_message(self,fmt,*args):
        if len(args)>1 and str(args[1]) not in ('200','204'):
            print('[HTTP]',fmt%args,flush=True)

def main():
    ap=argparse.ArgumentParser(description='PRESCUE V2 USB/Wi-Fi bridge, benchtop only')
    ap.add_argument('--list',action='store_true')
    ap.add_argument('--port',help='e.g. /dev/cu.usbserial-0001')
    ap.add_argument('--http-port',type=int,default=8000)
    args=ap.parse_args()
    if args.list:
        for p in list_ports.comports():print(p.device,'|',p.description)
        return
    if not args.port:ap.error('Need --port /dev/cu...')
    threading.Thread(target=serial_worker,args=(args.port,),daemon=True).start()
    threading.Thread(target=operator_console,daemon=True).start()
    srv=ThreadingHTTPServer(('127.0.0.1',args.http_port),Handler)
    print('[HTTP] http://127.0.0.1:%d/api/status'%args.http_port,flush=True)
    print('[WARN] Do not use with humans; physical E-stop and relief required.',flush=True)
    try:srv.serve_forever()
    except KeyboardInterrupt:print('\n[STOP] Server shutdown. Inspect actual pressure.',flush=True)
    finally:srv.server_close()

if __name__=='__main__':main()
