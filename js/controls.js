/* PRESCUE live command UI V2: requires terminal ARM + firmware Q diagnostics. */
(function () {
  'use strict';
  const api = window.PRESCUE_API;
  const byId = (id) => document.getElementById(id);
  const panel = byId('pcControls');
  if (!panel || !api) return;
  const buttons = Array.from(panel.querySelectorAll('[data-pc-command]'));
  const actions = panel.querySelector('.pc-actions');
  const stopButton = document.createElement('button');
  stopButton.type = 'button';
  stopButton.id = 'pcStop';
  stopButton.className = 'button secondary pc-stop';
  stopButton.textContent = 'X · Hentikan proses';
  stopButton.title = 'Mengirim x untuk membatalkan proses. Ini TIDAK membuang tekanan.';
  stopButton.disabled = true;
  if (actions) actions.appendChild(stopButton);

  const targetInput = byId('pcTarget');
  targetInput.min = '20';
  targetInput.max = '150';
  targetInput.step = '1';
  let verified = false;
  let checking = false;
  let sending = false;
  let current = null;
  const live = () => api.getSettings().mode === 'live';
  function feedback(text) { byId('pcFeedback').textContent = text; }
  function render() {
    const active = live() && verified && !checking && !sending &&
                   current?.connected && current?.remote_commands_enabled === true &&
                   current?.sensor_ok === true && current?.safety_ok === true &&
                   current?.busy !== true && current?.is_simulated !== true;
    for (const button of buttons) {
      const cmd = button.dataset.pcCommand;
      button.disabled = !active || ((cmd === 'G' || cmd === 'L') && current?.can_pressurize !== true);
    }
    stopButton.disabled = !live() || !current?.connected || !current?.busy || sending;
    const stateText = !live() ? 'Rekaman · kontrol nonaktif' :
      !current?.connected ? 'ESP32 belum terhubung' :
      current?.busy ? 'Pengujian berjalan' :
      active ? 'Siap menerima perintah' :
      current?.operator_armed ? 'Menunggu sensor / firmware' :
      'Terkunci · ARM di terminal';
    byId('pcState').textContent = stateText;
  }
  async function refresh(showFeedback = false) {
    if (checking) return;
    if (!live()) {
      current = null;
      verified = false;
      if (showFeedback) feedback('Pilih Live ESP32 API terlebih dahulu.');
      render();
      return;
    }
    checking = true;
    render();
    try {
      const raw = await api.requestJSON(api.getSettings().statusPath);
      current = raw && typeof raw.data === 'object' && raw.data ? raw.data : raw;
      if (current.is_simulated === true) throw new Error('API simulasi ditolak.');
      verified = current.connected === true && current.remote_commands_enabled === true &&
                 current.sensor_ok === true && current.safety_ok === true;
      if (showFeedback) {
        if (current.busy) feedback('Pengujian berjalan; pantau status. Tombol X hanya membatalkan, tidak membuang tekanan.');
        else if (verified) feedback('Izin operator dan diagnostik firmware OK. Pastikan rangkaian TANPA pasien, katup dan manometer diawasi.');
        else feedback('Belum siap: firmware WEB_SAFE, sensor dan ARM di terminal harus terpenuhi. Klik Periksa lagi setelah ARM.');
      }
    } catch (error) {
      current = null;
      verified = false;
      if (showFeedback) feedback(`Kontrol terkunci: ${error.message}`);
    } finally {
      checking = false;
      render();
    }
  }
  byId('pcCheck').addEventListener('click', () => refresh(true));
  for (const button of buttons) {
    button.addEventListener('click', async () => {
      if (button.disabled || sending) return;
      const code = button.dataset.pcCommand;
      const raw = targetInput.value.trim();
      const target = Number(raw);
      if (code !== 'O' && (!/^\d+$/.test(raw) || !Number.isInteger(target) || target < 20 || target > 150)) {
        feedback('Target wajib bilangan bulat 20–150 mmHg.');
        return;
      }
      const command = code === 'O' ? 'O' : `${code}:${target}`;
      await refresh(false);
      if (!verified || !live() || current?.busy || button.disabled) {
        feedback('Perintah tidak dikirim: koneksi, sensor, atau izin operator sudah berubah.');
        return;
      }
      const message = code === 'O'
        ? 'Buka katup maksimal 15 detik? Pastikan alat tanpa pasien, operator mengawasi dan pelepasan tekanan aman.'
        : `Kirim ${command} ke ESP32 untuk mengaktifkan rangkaian pneumatik? Pastikan tanpa pasien dan stop fisik tersedia.`;
      if (!window.confirm(message)) return;
      sending = true;
      render();
      try {
        // Request is still revalidated by bridge immediately before UART write.
        const result = await api.sendCommand(command);
        if (result?.accepted !== true) throw new Error(result?.error || 'Server tidak memberi konfirmasi accepted');
        feedback(`${command} DIKIRIM. Ini bukan tanda uji berhasil. Pantau grafik dan siapkan stop fisik.`);
        verified = false;
      } catch (error) {
        feedback(`Perintah GAGAL/belum pasti dikirim: ${error.message}. Periksa keadaan alat sebelum mencoba lagi.`);
      } finally {
        sending = false;
        await refresh(false);
        render();
      }
    });
  }
  stopButton.addEventListener('click', async () => {
    if (stopButton.disabled || sending) return;
    if (!window.confirm('Kirim x untuk HENTIKAN proses? x mematikan pompa tetapi TIDAK mengosongkan tekanan. Awasi pelepasan fisik.')) return;
    sending = true;
    render();
    try {
      const result = await api.sendCommand('X');
      if (result?.accepted !== true) throw new Error(result?.error || 'Tidak diterima');
      feedback('Perintah x dikirim. Tunggu konfirmasi DIHENTIKAN di monitoring dan periksa tekanan sisa secara fisik.');
    } catch (error) {
      feedback('Hentikan via website gagal: ' + error.message + '. Gunakan stop fisik.');
    } finally {
      sending = false;
      await refresh(false);
    }
  });
  for (const id of ['modeDemoBtn', 'modeLiveBtn']) {
    byId(id)?.addEventListener('click', () => {
      verified = false;
      current = null;
      render();
    });
  }
  // Keep controls locked when the operator revokes ARM, sensor drops, or USB disconnects.
  window.setInterval(() => { if (live()) refresh(false); else { verified = false; current = null; render(); } }, 2500);
  render();
})();
