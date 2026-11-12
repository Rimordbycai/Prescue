#!/usr/bin/env python3
"""PRESCUE local fake ESP32 API for browser UI testing; NO physical actuation."""
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import json, time, math, re
from urllib.parse import urlparse

START = time.monotonic()

class Handler(BaseHTTPRequestHandler):
    def send_json(self, status, obj):
        body = json.dumps(obj, separators=(',', ':')).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_json(200, {'mock': True})

    def do_GET(self):
        path = urlparse(self.path).path
        elapsed = time.monotonic() - START
        pressure = round(112 + 2.5 * math.sin(elapsed / 7), 3)
        if path == '/api/status':
            self.send_json(200, {
                'connected': True, 'connection_type': 'local_mock',
                'is_simulated': True, 'api_mode': 'mock_dry_run',
                'pressure_mmHg': pressure, 'target_mmHg': None,
                'phase': 'SIMULASI', 'test_mode': None,
                'pump_on': False, 'valve_open': False,
                'sensor_ok': True, 'adc_raw': 1822.0,
                'busy': False, 'sample_count': int(elapsed / 2) + 1,
                'elapsed_seconds': elapsed,
                'leak_rate_mmHg_min': None, 'test_result': None,
                'remote_commands_enabled': False, 'dry_run_commands_enabled': True, 'safety_ok': False,
                'can_pressurize': False,
                'note': 'SIMULASI LOKAL. Tidak terhubung ke ESP32.'
            })
        elif path == '/api/samples':
            n = min(120, int(elapsed / 2) + 1)
            samples = [
                {'phase': 'SIMULASI', 'elapsed_seconds': round(elapsed - 2*i,3),
                 'pressure_mmHg': round(112 + 2.5 * math.sin((elapsed - 2*i) / 7),3)}
                for i in range(n-1, -1, -1)
            ]
            self.send_json(200, {'samples': samples})
        elif path == '/api/capabilities':
            self.send_json(200, {'wifi_monitoring': True, 'is_simulated': True,
                                 'remote_commands_enabled': False})
        else:
            self.send_json(404, {'error': 'Mock endpoint tidak ditemukan'})


    # PRESCUE COMMAND DRY RUN V3
    def do_POST(self):
        if urlparse(self.path).path != '/api/command':
            self.send_json(404, {'accepted': False, 'error': 'Not found'})
            return

        try:
            size = int(self.headers.get('Content-Length', '0'))
            if size < 1 or size > 100:
                raise ValueError('Invalid payload size')

            data = json.loads(self.rfile.read(size))
            command = data.get('command')

            valid = isinstance(command, str) and (
                command in ('O', 'X') or (
                    re.fullmatch(r'[GL]:[0-9]{2,3}', command) is not None
                    and 20 <= int(command[2:]) <= 150
                )
            )

            if not valid:
                raise ValueError('Unsupported command')

            print('DRY-RUN:', command, flush=True)

            self.send_json(200, {
                'accepted': True,
                'executed': False,
                'dry_run': True,
                'command': command,
                'message': 'No hardware actuation'
            })

        except (ValueError, TypeError, json.JSONDecodeError) as exc:
            self.send_json(400, {
                'accepted': False,
                'error': str(exc)
            })

if __name__ == '__main__':
    server = ThreadingHTTPServer(('127.0.0.1', 8765), Handler)
    print('MOCK PRESCUE API: http://127.0.0.1:8765')
    print('SIMULASI SAJA | Tidak ada akses Serial, ESP32, pompa, atau katup.')
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nStopped')
