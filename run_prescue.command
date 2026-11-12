#!/bin/zsh
set -e
cd "${0:A:h}"

echo "=== PRESCUE V2: Wi-Fi + USB terpadu ==="
echo "Gunakan hanya untuk pengujian rangkaian tanpa manusia."
echo "Jangan ARM sebelum STOP fisik, pelepasan darurat, sensor, dan kalibrasi diuji."

echo ""
python3 prescue_unified.py --list

echo ""
read -r 'PORT?Masukkan port USB ESP32 dari daftar di atas (contoh /dev/cu.usbserial-0001): '
if [[ -z "$PORT" || "$PORT" != /dev/cu.* ]]; then
  echo "STOP: Port harus /dev/cu.*; tidak ada perangkat yang dibuka."
  exit 1
fi

if lsof -nP -iTCP:8000 -sTCP:LISTEN >/dev/null 2>&1; then
  echo "STOP: Port 8000 dipakai. Tutup bridge USB lama dahulu."
  exit 1
fi

SERVER_5500=""
if ! lsof -nP -iTCP:5500 -sTCP:LISTEN >/dev/null 2>&1; then
  python3 -m http.server 5500 > /tmp/prescue-web.log 2>&1 &
  SERVER_5500=$!
fi

cleanup(){
  if [[ -n "$SERVER_5500" ]]; then kill "$SERVER_5500" 2>/dev/null || true; fi
}
trap cleanup EXIT

(sleep 1; open 'http://localhost:5500/#integration') >/dev/null 2>&1 &
echo ""
echo "Dashboard: http://localhost:5500"
echo "Pilih tombol 'Gunakan Wi-Fi + USB Terpadu' di Integrasi API."
echo "Ketik STATUS pada terminal ini untuk pemeriksaan koneksi."
echo "ARM BENCH hanya setelah pengaman fisik benar-benar divalidasi."
echo ""
python3 prescue_unified.py --port "$PORT" --wifi-url 'http://192.168.4.1'
