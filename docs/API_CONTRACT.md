# PRESCUE — Rancangan API untuk teman yang mengerjakan ESP32

**Status:** usulan kontrak, **belum diimplementasikan di `1tahap0.ino`**. Nama endpoint dan field boleh diubah; cukup sesuaikan `js/api.js` (fungsi `normalizeStatus`, `normalizeSamples`) atau ubah path via form dashboard.

## GET /api/status (wajib untuk live)

Contoh **format** JSON, memakai angka **dari rekaman sebelumnya** untuk menjelaskan bentuk data; **bukan respons dari perangkat live**:

```json
{
  "pressure_mmHg": 129.648,
  "target_mmHg": 150,
  "phase": "RECORD",
  "test_mode": "L",
  "pump_on": false,
  "valve_open": false,
  "sensor_ok": true,
  "leak_rate_mmHg_min": 8.5736,
  "test_result": "GAGAL",
  "adc_raw": null,
  "sensor_voltage": null,
  "elapsed_seconds": 60.105,
  "sample_count": 300
}
```

`pressure_mmHg` adalah satu-satunya field **wajib** di adapter saat ini; jika tidak ada, UI menampilkan error agar tidak mengarang nilai. Sisanya opsional (UI menampilkan `—`/`Tidak dikirim`). Jika belum tersedia, **kirim `null`**, bukan angka dummy yang tampak benar.

### Field map

| Field | Tipe | Keterangan |
| --- | --- | --- |
| `pressure_mmHg` | number | tekanan terkalibrasi dari MPX5050DP, mmHg |
| `target_mmHg` | number/null | target tekanan mode saat itu |
| `phase` | string/null | `FILL`, `SETTLE`, `RECORD`, `IDLE` atau fase lain yang memang dipakai firmware |
| `test_mode` | string/null | `G`, `L`, `S`, `E`, `F`, dst. |
| `pump_on` | boolean/null | status fisik perintah pompa; jika belum dibaca, null |
| `valve_open` | boolean/null | status perintah katup; jika belum dibaca, null |
| `sensor_ok` | boolean/null | true hanya jika firmware punya pemeriksaan sensor yang sah |
| `leak_rate_mmHg_min` | number/null | hasil regresi saat tes L selesai; null saat belum selesai |
| `test_result` | string/null | `LOLOS`, `GAGAL`, atau null bila belum ada hasil |
| `adc_raw` | number/null | pembacaan ADC mentah, opsional |
| `sensor_voltage` | number/null | tegangan sensor, volt, opsional |
| `elapsed_seconds` | number/null | waktu kumulatif sejak tes/session dimulai, jangan reset jika grafik live ingin kontinu |
| `sample_count` | number/null | jumlah sampel yang dikumpulkan firmware, jika relevan |

Adapter mengizinkan alias beberapa nama field, contoh `pressure`, `T_mmHg`, `fase`, `mentah`. Untuk skema khusus, edit fungsi mapping di `js/api.js`.

## GET /api/samples (opsional)

Agar grafik bisa menampilkan riwayat utuh dari ESP32, kembalikan array yang mencakup maksimal 300 sampel terbaru:

```json
{
  "samples": [
    {"elapsed_seconds": 0.0, "phase": "FILL", "pressure_mmHg": 12.1},
    {"elapsed_seconds": 2.0, "phase": "FILL", "pressure_mmHg": 67.3}
  ]
}
```

Angka pada **contoh skema** ini hanya ilustrasi bentuk respons, **bukan hasil pengukuran aktual**. Jika firmware lebih mudah mengirim data melalui `samples` di dalam respons `/api/status`, frontend juga mendukung pola itu. Kosongkan field path sampel di form bila endpoint ini belum dibuat. Jika `/api/samples` tidak ada, frontend dapat membangun grafik dari polling status berkala.

## Mode koneksi

- **Demo/Rekaman** (default) = tidak melakukan request perangkat; menampilkan cuplikan log kelompok yang sudah dikirim pengguna.
- **Live API** = GET `/api/status` setiap 2 detik. Tidak ada fallback diam-diam ke data demo bila fetch gagal.
- **CORS**: jika dashboard di-host oleh ESP32, biasanya lebih mudah same-origin. Jika dashboard dibuka dari VS Code/Laptop dengan port berbeda, API perlu CORS atau proxy.
- **HTTP/HTTPS**: jika halaman dijalankan via HTTPS tetapi ESP32 hanya HTTP, browser dapat memblokir mixed content. Gunakan HTTP lokal pada development atau proxy aman.
- **Jangan lakukan POST kontrol** sebelum pengujian keselamatan, interlock, validasi pressure limits, dan mekanisme pengaman mandiri pada ESP32 benar-benar siap.

## Pembedaan output firmware yang sekarang

- `G,FILL,detik,T_mmHg` adalah **Serial CSV**, bukan JSON HTTP.
- `L,SETTLE,detik,T_mmHg` dan `L,RECORD,detik,T_mmHg` juga Serial CSV.
- `# L awal=... akhir=... n=... bocor bertanda=...` adalah ringkasan hasil serial.
- Firmware saat ini belum menyediakan GET `/api/status`, `/api/samples`, atau `/api/history`.

Jadi tugas teman firmware adalah menambahkan HTTP/Web Server **tanpa mengganggu timing, pengukuran, atau pengaman hardware** dan melaporkan struktur JSON aslinya. Tugas frontend cukup memetakan field dan menguji pembacaannya.

## Urutan integrasi yang paling aman

1. Firmware menyediakan `/api/status` read-only dengan data tekanan aktual.
2. Tes koneksi memakai tombol **Tes koneksi** pada bagian integrasi dashboard.
3. Aktifkan **Live ESP32 API**; bandingkan nilai yang muncul dengan Serial Monitor.
4. Tambahkan status pompa, katup, dan fase sebenarnya.
5. Jika diperlukan, tambahkan `/api/samples` dengan buffer poin terbatas.
6. Setelah hasil uji L valid dan pengaman teruji, baru bahas antarmuka kontrol sebagai tahap tersendiri.
