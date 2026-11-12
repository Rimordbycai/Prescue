/* PRESCUE V2 optional USB physical command path. Explicit opt-in only.
 * Telemetry stays on configured Wi-Fi ESP32 API. USB bridge: localhost:8000.
 * Physical stop/pressure relief are NEVER guaranteed by the browser.
 */
(function () {
  'use strict';
  const BRIDGE = 'http://127.0.0.1:8000';
  const panel = document.getElementById('pcControls');
  const api = window.PRESCUE_API;
  if (!panel || !api) return;

  const form = panel.querySelector('.pc-form');
  const feedback = document.getElementById('pcFeedback');
  const statusText = document.getElementById('pcState');
  const target = document.getElementById('pcTarget');
  if (!form || !feedback || !statusText || !target) return;

  const toggleLabel = document.createElement('label');
  toggleLabel.style.cssText = 'display:flex;gap:10px;align-items:center;font-weight:600;cursor:pointer;padding:8px 0;';
  const toggle = document.createElement('input');
  toggle.type = 'checkbox';
  toggle.id = 'pcUsbV2Enabled';
  toggle.style.cssText = 'width:18px;height:18px;accent-color:#3b82f6;';
  toggleLabel.appendChild(toggle);
  const span = document.createElement('span');
  span.textContent = 'USB fisik V2 (wajib ARM BENCH di Terminal, uji tanpa manusia)';
  toggleLabel.appendChild(span);
  form.appendChild(toggleLabel);

  const liveInfo = document.createElement('p');
  liveInfo.id = 'pcUsbV2Info';
  liveInfo.style.cssText = 'margin:8px 0;color:inherit;font-size:13px;';
  liveInfo.textContent = 'USB V2 NONAKTIF. Tombol mengikuti mode Wi-Fi dry-run.';
  form.appendChild(liveInfo);

  const resultInfo = document.createElement('p');
  resultInfo.id = 'pcUsbV2Result';
  resultInfo.style.cssText = 'font-size:13px;margin:3px 0 8px;white-space:normal;';
  resultInfo.textContent = 'Hasil USB belum tersedia.';
  form.appendChild(resultInfo);

  const logs = document.createElement('details');
  logs.style.cssText = 'font-size:13px;margin:6px 0;';
  const logTitle = document.createElement('summary');
  logTitle.textContent = 'Lihat output Serial USB terbaru';
  const logText = document.createElement('pre');
  logText.style.cssText = 'white-space:pre-wrap;max-height:150px;overflow:auto;padding:8px;font-size:11px;border:1px solid #64748b;border-radius:6px;';
  logText.textContent = 'Belum ada log USB.';
  logs.append(logTitle, logText);
  form.appendChild(logs);

  async function usbJSON(path, options) {
    const controller = new AbortController();
    const id = setTimeout(() => controller.abort(), 4000);
    try {
      const r = await fetch(BRIDGE + path, {
        ...options,
        signal: controller.signal,
        cache: 'no-store'
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(d.error || 'USB API HTTP ' + r.status);
      return d;
    } finally { clearTimeout(id); }
  }

  async function refreshUSB() {
    if (!toggle.checked) return;
    try {
      const d = await usbJSON('/api/status');
      liveInfo.textContent =
        'USB ' + (d.connected ? 'TERHUBUNG' : 'TERPUTUS') +
        ' | V2 ' + (d.firmware_ready ? 'OK' : 'BELUM TERDETEKSI') +
        ' | sensor ' + (d.sensor_ok ? 'OK' : 'TIDAK SIAP') +
        ' | ARM ' + (d.operator_armed ? 'AKTIF' : 'OFF') +
        ' | fase ' + (d.phase || '-') +
        (d.last_error ? ' | ' + d.last_error : '');
      resultInfo.textContent = 'Hasil: ' + (d.test_result || 'belum tersedia') +
        ' | tekanan: ' + (typeof d.pressure_mmHg === 'number' ? d.pressure_mmHg.toFixed(2) + ' mmHg' : '-') +
        ' | kebocoran: ' + (typeof d.leak_rate_mmHg_min === 'number' ? d.leak_rate_mmHg_min.toFixed(3) + ' mmHg/menit' : '-') +
        '. Grafik utama tetap membaca sensor Wi-Fi.';
      logText.textContent = Array.isArray(d.events) && d.events.length
        ? d.events.join('\n') : 'Belum ada log USB.';
    } catch (e) {
      liveInfo.textContent = 'USB bridge tidak terhubung: ' + e.message;
    }
  }

  toggle.addEventListener('change', () => {
    liveInfo.textContent = toggle.checked
      ? 'USB aktif. Periksa Terminal bridge dan status hardware; tanpa pasien.'
      : 'USB nonaktif. Tombol kembali ke Wi-Fi dry-run.';
    refreshUSB();
  });

  // Capture intercept prevents the older controls.js from also sending a command.
  panel.addEventListener('click', async (event) => {
    if (!toggle.checked) return;
    const b = event.target.closest('[data-pc-command],#pcStop,#pcCheck');
    if (!b || !panel.contains(b)) return;
    event.preventDefault();
    event.stopPropagation();
    event.stopImmediatePropagation();

    if (api.getSettings().mode !== 'live') {
      feedback.textContent = 'Pilih Live ESP32 API untuk monitoring Wi-Fi dahulu.';
      return;
    }

    let cmd = null;
    if (b.id === 'pcCheck') {
      await refreshUSB();
      feedback.textContent = 'Status USB diperbarui; monitoring grafik tetap dari Wi-Fi ESP32.';
      return;
    }
    if (b.id === 'pcStop') cmd = 'X';
    else if (b.dataset.pcCommand === 'O') cmd = 'O';
    else if (b.dataset.pcCommand === 'G' || b.dataset.pcCommand === 'L') {
      const raw = target.value.trim();
      const n = Number(raw);
      if (!/^\d+$/.test(raw) || !Number.isInteger(n) || n < 20 || n > 150) {
        feedback.textContent = 'Target wajib angka bulat 20–150 mmHg.';
        return;
      }
      cmd = b.dataset.pcCommand + ':' + n;
    }
    if (!cmd) return;
    b.disabled = true;
    feedback.textContent = 'Memeriksa bridge USB V2...';
    try {
      const d = await usbJSON('/api/status');
      if (!d.connected || (cmd !== 'X' && !d.firmware_ready)) {
        throw new Error('ESP32 USB/V2 belum siap.');
      }
      if (cmd !== 'X') {
        if (!d.operator_armed) throw new Error('Wajib ARM BENCH di Terminal bridge.');
        if (!d.sensor_ok) throw new Error('Sensor tidak siap; perintah ditolak.');
        if (d.busy) throw new Error('Pengujian lain sedang berjalan.');
        if (cmd.startsWith('G:') || cmd.startsWith('L:')) {
          if (d.can_pressurize !== true) throw new Error('Tekanan awal >20 mmHg atau belum aman.');
        }
      } else if (!d.busy) {
        throw new Error('Tidak ada tes USB aktif. X bukan tombol pelepasan tekanan.');
      }
      const question = cmd === 'X'
        ? 'Kirim x melalui USB? X menghentikan pompa dan MENUTUP katup, tidak melepaskan tekanan sisa.'
        : 'Kirim ' + cmd + ' ke ESP32 USB secara FISIK? Hanya untuk bench tanpa pasien; STOP fisik dan relief independen wajib tersedia.';
      if (!window.confirm(question)) {
        feedback.textContent = 'Dibatalkan. Tidak ada perintah dikirim.';
        return;
      }
      const r = await usbJSON('/api/command', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ command: cmd })
      });
      if (r.accepted !== true || r.forwarded_to_usb !== true) {
        throw new Error('Bridge tidak mengonfirmasi pengiriman USB.');
      }
      statusText.textContent = 'USB V2 · Perintah dikirim';
      feedback.textContent = cmd + ' sudah DIKIRIM via USB. Ini BUKAN bukti aktuator berhasil. ' +
        'Amati kondisi fisik alat dan grafik Wi-Fi. STOP fisik tetap utama.';
      await refreshUSB();
    } catch (e) {
      statusText.textContent = 'USB V2 · Perintah tidak dikonfirmasi';
      feedback.textContent = 'Tidak dikonfirmasi: ' + e.message +
        ' Bila timeout, cek kondisi fisik sebelum mencoba ulang.';
    } finally { b.disabled = false; }
  }, true);

  setInterval(refreshUSB, 2500);
})();
