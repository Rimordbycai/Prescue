#!/usr/bin/env python3
"""PRESCUE V2 local Wi-Fi + USB telemetry gateway.

Wi-Fi GET is the preferred pressure/actuator state source; USB V2 gives phases,
results, and operator-gated commands through the unchanged bridge.
Requires the existing patched PRESCUE V2 firmware and prescue_usb_bridge_v2.py.
Not medical equipment. No physical-safety verification is implied by telemetry.
"""
import argparse
from collections import deque
import json
import math
import threading
import time
from urllib.request import urlopen
from urllib.parse import urlsplit

import prescue_usb_bridge_v2 as usb

DEFAULT_WIFI = 'http://192.168.4.1'
WIFI_FRESH_SEC = 3.5
POLL_SEC = 0.6
MAX_POINTS = 400

class Fusion:
    def __init__(self):
        self.lock = threading.RLock()
        self.wifi = None
        self.wifi_last_at = 0
        self.wifi_error = 'Wi-Fi belum dibaca'
        self.points = deque(maxlen=MAX_POINTS)
        self.history = deque(maxlen=20)
        self.session_started = time.monotonic()
        self.current_run = None
        self.next_id = 0
        self.last_phase = 'IDLE'
        self.last_plot_at = 0
        self.last_serial_at = 0
        self.last_usb_pressure_at = 0
        self.last_live_result = None
        self.last_run_end_at = 0
        self.wifi_host = DEFAULT_WIFI

    def wifi_fresh(self):
        return bool(self.wifi is not None and self.wifi.get('connected') is True
                    and time.monotonic() - self.wifi_last_at < WIFI_FRESH_SEC)

    def start_run_if_needed(self):
        # Called under fusion lock. Original bridge's own lock protects USB state.
        with usb.S.lock:
            running, mode = usb.S.busy, usb.S.mode
            command, target = usb.S.last_command, usb.S.target
        if running and mode in ('G','L','O') and self.current_run is None:
            self.next_id += 1
            now = time.monotonic()
            self.current_run = {
                'id': self.next_id, 'name': mode, 'command': command,
                'target_mmHg': target, 'started_at': now,
                'started_wall': time.strftime('%Y-%m-%d %H:%M:%S'),
            }
            self.session_started = now
            self.points.clear()
            self.last_plot_at = 0
            self.last_live_result = None
            self.last_phase = 'STARTING'
        elif not running and self.current_run is not None:
            with usb.S.lock:
                result = usb.S.test_result
                phase = usb.S.phase
                leak = usb.S.leak_rate
                err = usb.S.last_error
            run = self.current_run
            outcome = result or ('SELESAI' if phase == 'DONE' else 'TIDAK DIKONFIRMASI')
            row = {
                'id': run['id'], 'name': run['name'], 'command': run['command'],
                'target_mmHg': run['target_mmHg'],
                'phase': phase, 'result': outcome,
                'leak_rate_mmHg_min': leak,
                'duration_seconds': round(max(0,time.monotonic()-run['started_at']),1),
                'finished_at': time.strftime('%Y-%m-%d %H:%M:%S'),
                'error': err,
            }
            self.history.appendleft(row)
            self.last_live_result = row
            self.last_run_end_at = time.monotonic()
            self.current_run = None
            self.last_phase = phase

    def note_pressure(self, pressure, phase, source):
        if not isinstance(pressure,(int,float)) or not math.isfinite(pressure):
            return
        now = time.monotonic()
        # No more than 2 points per second per source.
        if now - self.last_plot_at < 0.46:
            return
        self.points.append({
            'elapsed_seconds': round(max(0,now-self.session_started),3),
            'pressure_mmHg': round(pressure,3), 'phase': phase or 'LIVE',
            'source': source
        })
        self.last_plot_at = now

    def accept_wifi(self, data):
        if not isinstance(data,dict) or data.get('connected') is not True:
            raise ValueError('Status Wi-Fi bukan respons ESP32 aktif')
        p = data.get('pressure_mmHg')
        if p is not None and (not isinstance(p,(int,float)) or not math.isfinite(p)):
            raise ValueError('Tekanan Wi-Fi tidak valid')
        with self.lock:
            self.wifi = data
            self.wifi_last_at = time.monotonic()
            self.wifi_error = None
            self.start_run_if_needed()
            with usb.S.lock:
                usb_phase = usb.S.phase if usb.S.connected else None
            phase = usb_phase if usb_phase not in (None,'IDLE','DISCONNECTED') else data.get('phase','LIVE')
            if p is not None:
                self.note_pressure(p,phase,'wifi')

    def fail_wifi(self, error):
        with self.lock:
            self.wifi_error = str(error)[:180]

    def note_serial(self, line):
        with self.lock:
            self.start_run_if_needed()
            with usb.S.lock:
                pressure = usb.S.pressure
                phase = usb.S.phase
                # Serial output G/L/O confirms the firmware really sampled pressure.
                is_measurement = line.startswith(('G,FILL,','L,SETTLE,','L,RECORD,','T,'))
            if is_measurement:
                self.last_usb_pressure_at = time.monotonic()
                if not self.wifi_fresh():
                    self.note_pressure(pressure, phase or 'LIVE','usb')
            self.start_run_if_needed()

    def snapshot(self):
        with self.lock:
            self.start_run_if_needed()
            base = usb.snapshot()
            now = time.monotonic()
            wifi_ok = self.wifi_fresh()
            w = self.wifi if wifi_ok else {}
            usb_ok = bool(base.get('connected') and base.get('firmware_ready'))
            wifi_pressure = w.get('pressure_mmHg') if w.get('sensor_ok') is True else None
            with usb.S.lock:
                usb_valid = (usb.S.sensor_ok and usb.S.pressure is not None
                             and math.isfinite(usb.S.pressure)
                             and (now-usb.S.last_probe < usb.PROBE_FRESH or testing_active(usb.S)))
                usb_raw_pressure = usb.S.pressure
            usb_pressure = usb_raw_pressure if usb_ok and usb_valid else None
            if isinstance(wifi_pressure,(int,float)) and math.isfinite(wifi_pressure):
                pressure,source = wifi_pressure,'wifi'
            elif isinstance(usb_pressure,(int,float)) and math.isfinite(usb_pressure):
                pressure,source = usb_pressure,'usb'
            else:
                pressure,source = None,None
            with usb.S.lock:
                testing = usb.S.busy
                mode = usb.S.mode
                phase = usb.S.phase
            # Never take a firmware actuator state from an earlier/stale Wi-Fi response.
            result = base.get('test_result')
            leak = base.get('leak_rate_mmHg_min')
            if result is None and self.last_live_result and not testing:
                result = self.last_live_result.get('result')
            if leak is None and self.last_live_result and not testing:
                leak = self.last_live_result.get('leak_rate_mmHg_min')
            if usb_ok and (testing or mode):
                current_phase = phase
            else:
                current_phase = w.get('phase') if wifi_ok else ('USB READY' if usb_ok else 'DISCONNECTED')
            return {
                **base,
                'connected': wifi_ok or usb_ok,
                'usb_connected': usb_ok,
                'wifi_connected': wifi_ok,
                'integration_ready': wifi_ok and usb_ok,
                'is_simulated': False,
                'api_mode': 'wifi_usb_unified_v2',
                'pressure_mmHg': pressure,
                'pressure_source': source,
                'target_mmHg': base.get('target_mmHg'),
                'phase': current_phase,
                'test_mode': mode if usb_ok else None,
                'pump_on': w.get('pump_on') if wifi_ok else None,
                'valve_open': w.get('valve_open') if wifi_ok else None,
                'sensor_ok': bool((w.get('sensor_ok') is True) if wifi_ok else (usb_valid if usb_ok else False)),
                'adc_raw': w.get('adc_raw') if wifi_ok else base.get('adc_raw'),
                'sample_count': len(self.points),
                'elapsed_seconds': round(now-self.session_started,2),
                'leak_rate_mmHg_min': leak,
                'test_result': result,
                'busy': testing or (w.get('busy') is True if wifi_ok else False),
                'wifi_error': None if wifi_ok else self.wifi_error or 'Respons Wi-Fi kedaluwarsa',
                'usb_error': base.get('last_error'),
                'event_count': len(base.get('events',[])),
                'history_count': len(self.history),
                'remote_commands_enabled': bool(base.get('remote_commands_enabled') and wifi_ok),
                # Explicit: software diagnostic is not physical safety certification.
                'safety_ok': False,
                'physical_safety_verified': False,
                'can_pressurize': bool(base.get('can_pressurize') and wifi_ok and w.get('sensor_ok') is True),
                'note': ('Data tekanan Wi-Fi lebih diutamakan; USB memberi fase/hasil G/L/O. '
                         'Pengaman fisik tidak dapat dibuktikan oleh dashboard.'),
            }

def testing_active(s):
    return bool(s.busy and time.monotonic()-s.last_activity < 5.0)

F = Fusion()
_original_ready = usb.ready_locked

def unified_ready(now):
    # Stronger than old bridge: a new G/L/O needs live Wi-Fi visibility too.
    # Does NOT apply to X, so stop remains available if Wi-Fi fails.
    return _original_ready(now) and F.wifi_fresh()

usb.ready_locked = unified_ready
_original_parse = usb.parse_line

def parse_line_and_fuse(line):
    _original_parse(line)
    F.note_serial(line)

usb.parse_line = parse_line_and_fuse
_original_get = usb.Handler.do_GET
_original_post = usb.Handler.do_POST


def unified_get(self):
    if not self.origin_ok():
        return self.respond(403,{'error':'Origin not allowed'})
    path = urlsplit(self.path).path
    if path == '/api/status':
        return self.respond(200,F.snapshot())
    if path == '/api/samples':
        with F.lock: rows = list(F.points)
        return self.respond(200,{'samples':rows})
    if path == '/api/history':
        with F.lock: rows = list(F.history)
        return self.respond(200,{'history':rows})
    if path == '/api/events':
        with usb.S.lock: rows = list(usb.S.events)[-80:]
        return self.respond(200,{'events':rows})
    if path == '/health':
        d=F.snapshot()
        return self.respond(200,{'service':'prescue_unified_v2',
                                 'usb_connected':d['usb_connected'],
                                 'wifi_connected':d['wifi_connected'],
                                 'integration_ready':d['integration_ready']})
    return _original_get(self)



def unified_post(self):
    # No control implementation here: rely entirely on existing gated USB handler.
    result = _original_post(self)
    with F.lock:
        F.start_run_if_needed()
    return result

usb.Handler.do_POST = unified_post
usb.Handler.do_GET = unified_get


def wifi_worker(host):
    # Wi-Fi is READ-ONLY. Never send POST to ESP32 from this gateway.
    url = host.rstrip('/') + '/api/status'
    while True:
        try:
            with urlopen(url, timeout=1.2) as r:
                if r.status != 200:raise RuntimeError('HTTP '+str(r.status))
                payload = r.read(8192)
                obj=json.loads(payload)
            F.accept_wifi(obj)
        except Exception as exc:
            F.fail_wifi(exc)
        time.sleep(POLL_SEC)


def main():
    parser=argparse.ArgumentParser(description='PRESCUE integrated USB+Wi-Fi dashboard, benchtop only')
    parser.add_argument('--list',action='store_true',help='List serial ports, no device control')
    parser.add_argument('--port',help='USB ESP32 device, e.g. /dev/cu.usbserial-0001')
    parser.add_argument('--wifi-url',default=DEFAULT_WIFI,help='ESP32 access point URL')
    parser.add_argument('--http-port',type=int,default=8000)
    args=parser.parse_args()
    if args.list:
        for p in usb.list_ports.comports():print(p.device,'|',p.description)
        return
    if not args.port:parser.error('Set --port /dev/cu... from --list')
    F.wifi_host=args.wifi_url
    threading.Thread(target=wifi_worker,args=(args.wifi_url,),daemon=True).start()
    threading.Thread(target=usb.serial_worker,args=(args.port,),daemon=True).start()
    threading.Thread(target=usb.operator_console,daemon=True).start()
    server=usb.ThreadingHTTPServer(('127.0.0.1',args.http_port),usb.Handler)
    print('[UNIFIED] Wi-Fi GET:',args.wifi_url+'/api/status',flush=True)
    print('[UNIFIED] Dashboard API: http://127.0.0.1:%d'%args.http_port,flush=True)
    print('[LOCKED] Command control still requires local ARM BENCH and physical checks.',flush=True)
    try:server.serve_forever()
    except KeyboardInterrupt:print('\n[STOP] Gateway stopped; inspect physical pressure.',flush=True)
    finally:server.server_close()

if __name__=='__main__':main()
