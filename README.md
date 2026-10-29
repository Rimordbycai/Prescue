# PRESCUE — Dashboard Prototype NIBP Simulator

**PRESCUE:** Simulator Verifikasi Tekanan Darah Portabel Berbiaya Rendah untuk Kondisi Darurat Bencana.

Starter frontend **tanpa framework, npm, database, atau koneksi internet wajib**. Ditujukan untuk pemantauan prototipe tekanan berbasis ESP32 dan MPX5050DP, dengan fokus pada kemampuan yang sudah diperlihatkan dalam log pengujian tim.

## Isi ZIP

- `index.html` — satu dashboard panjang yang dapat discroll; sidebar menuju ringkasan, monitoring, pengujian, riwayat, integrasi API.
- `css/styles.css` — desain modern, responsive, light blue & white.
- `js/config.js` — konfigurasi default ESP32 dan interval polling.
- `js/demo-data.js` — **cuplikan data riil dari log yang diberikan**, bukan nilai live dan bukan seluruh 300 sampel.
- `js/api.js` — lapisan adaptasi JSON dan pemanggilan API ESP32. **Di sinilah nama field dapat dipetakan tanpa mengubah UI.**
- `js/chart.js` — grafik Canvas offline, tanpa Chart.js/CDN.
- `js/app.js` — perilaku dashboard, koneksi, replay contoh, tabel dan CSV.
- `assets/prescue-hero.png` — foto hardware dari PDF dokumentasi kelompok, digunakan samar sebagai hero background.
- `docs/API_CONTRACT.md` — contoh respons untuk rekan yang mengerjakan firmware.

## Menjalankan frontend

1. Ekstrak ZIP.
2. Buka `index.html` di browser untuk tampilan **Demo / Rekaman** (langsung tampil dengan data contoh).
3. Disarankan menjalankan HTTP server lokal ketika menguji API:

   ```bash
   cd PRESCUE-dashboard
   python3 -m http.server 5500
   ```

   Buka `http://localhost:5500/` di browser. Jika menjalankan dengan VS Code, extension Live Server juga bisa dipakai.
4. Masuk ke **Integrasi API**, isi alamat ESP32, dan klik **Tes koneksi**. Aktifkan **Live ESP32 API** jika firmware benar-benar sudah menyediakan JSON endpoint.

## Hasil pengujian yang dipakai sebagai contoh

Dari log tim yang diberikan:

- Mode `G` mengisi tekanan ke 150,044 mmHg dalam 8,781 detik untuk target 150 mmHg.
- Mode `L` berisi FILL, jeda awal 2 detik, masa SETTLE 30 detik, dan RECORD sekitar 60 detik (300 sampel dilaporkan).
- Awal RECORD 139,437 mmHg; akhir 129,648 mmHg.
- Laju kebocoran hasil regresi firmware: 8,5736 mmHg/menit, **GAGAL** untuk batas internal ≤ 5 mmHg/menit.
- Perubahan tekanan mendadak dekat 55,683 s terlihat pada data, tetapi **penyebab fisiknya belum diketahui**.
- Percobaan SETTLE 120 detik masih **usulan tes berikutnya**, bukan hasil tes yang sudah selesai.
- Mode `E` dan `F` tersedia sebagai perintah di `.ino`, **belum ada hasil uji yang diberikan**.

Angka **45% progres** di dashboard berasal dari rekap laporan kelompok pekan 7, **bukan** dibaca dari ESP32. Karena rekap manual, jangan disamakan dengan status monitoring hardware realtime.

## Batasan versi ini

- Firmware `1tahap0.ino` yang diberikan pengguna saat pembuatan proyek menggunakan **Serial Monitor**, belum HTTP Web Server. ZIP **tidak** mengubah firmware dan **tidak** bisa langsung menghubungkan ESP32 tanpa implementasi endpoint.
- Dashboard ini **monitoring-only**; tidak ada pengiriman perintah yang menyalakan pompa/solenoid. Keselamatan perangkat dan mode darurat tetap wajib dijalankan di firmware, tidak hanya di UI.
- Mode Demo memperlihatkan **rekaman/cuplikan**, bukan simulasi data live yang diklaim sungguhan.
- Riwayat live tidak dibuat-buat; di mode Live riwayat kosong sampai endpoint histori benar-benar diimplementasikan.
- Bila API belum tersedia, dashboard akan menunjukkan **Terputus** dan tidak diam-diam berganti ke data demo.
- Foto dokumentasi berasal dari file pengguna, untuk konteks proyek ini.

## Ketika integrasi firmware sudah siap

**Paling simpel:** frontend di-host dari filesystem ESP32 (mis. LittleFS), dan GET `/api/status` dipanggil sebagai same-origin. Atur `baseUrl` di halaman Integrasi API menjadi origin ESP32. Jika frontend berada di laptop (origin berbeda), firmware harus mengizinkan CORS atau gunakan development proxy. Untuk pengujian pertama, pastikan perangkat benar-benar terhubung pada Wi-Fi yang sama atau hotspot AP ESP32.

Baca `docs/API_CONTRACT.md` untuk contoh JSON dan detail mapping.
