/* PRESCUE SERIAL COMMAND UI V4 */
(function () {
  'use strict';

  const api = window.PRESCUE_API;
  const byId = id => document.getElementById(id);
  const panel = byId('pcControls');

  if (!panel) return;

  const targetInput = byId('pcTarget');
  const commandText = byId('pcCommandText');
  const feedbackBox = byId('pcFeedback');
  const state = byId('pcState');
  const actions = panel.querySelector('.pc-actions');

  const stop = document.createElement('button');
  stop.type = 'button';
  stop.className = 'button secondary pc-stop';
  stop.id = 'pcStop';
  stop.textContent = 'X · Hentikan proses';
  actions.appendChild(stop);

  let selectedCommand = '';

  function feedback(message) {
    feedbackBox.textContent = message;
  }

  function showCommand(command, description) {
    selectedCommand = command;
    commandText.textContent = command;

    feedback(
      description +
      ' Ini hanya pratinjau perintah. ' +
      'Tidak ada perintah yang dikirim ke ESP32.'
    );
  }

  function target() {
    const raw = targetInput.value.trim();
    const n = Number(raw);

    if (
      !/^[0-9]+$/.test(raw) ||
      !Number.isInteger(n) ||
      n < 20 ||
      n > 150
    ) {
      feedback(
        'Target dashboard harus bilangan bulat 20–150 mmHg. ' +
        'Rentang ini lebih konservatif daripada parser firmware.'
      );
      return null;
    }

    return n;
  }


  // PRESCUE LIVE COMMAND GATE V5
  // Kontrol fisik hanya melalui firmware yang mengizinkan.
  let sending = false;

  async function runLiveCommand(code) {
    if (!api || sending) return;

    let command;

    if (code === 'G' || code === 'L') {
      const n = target();
      if (n === null) return;
      command = code + ':' + n;
    } else if (code === 'O' || code === 'X') {
      command = code;
    } else {
      feedback('Perintah tidak dikenal.');
      return;
    }

    sending = true;
    feedback('Memeriksa koneksi dan pengaman ESP32...');
    state.textContent = 'Memeriksa izin firmware...';

    try {
      const raw = await api.requestJSON(
        api.getSettings().statusPath
      );

      const d =
        raw &&
        raw.data &&
        typeof raw.data === 'object'
          ? raw.data
          : raw;


      // PRESCUE COMMAND DRY RUN V3
      if (
        d.connected === true &&
        d.dry_run_commands_enabled === true &&
        d.remote_commands_enabled === false &&
        ['monitor_dry_run', 'mock_dry_run'].includes(d.api_mode)
      ) {
        const response = await api.sendCommand(command);

        if (
          response?.accepted !== true ||
          response?.dry_run !== true ||
          response?.executed !== false
        ) {
          throw new Error('Respons dry-run tidak valid.');
        }

        state.textContent = d.is_simulated
          ? 'SIMULASI · Dry-run berhasil'
          : 'ESP32 terhubung · Dry-run berhasil';

        feedback(
          'DRY-RUN BERHASIL: ' + command +
          ' diterima dan divalidasi oleh API. ' +
          'Pompa dan katup TIDAK dijalankan.'
        );
        return;
      }

      if (d.connected !== true || d.is_simulated === true) {
        throw new Error(
          'ESP32 tidak terverifikasi sebagai perangkat fisik.'
        );
      }

      if (d.remote_commands_enabled !== true) {
        throw new Error(
          'ESP32 terhubung, tetapi firmware ' +
          'belum mengizinkan kontrol jarak jauh.'
        );
      }

      if (d.safety_ok !== true) {
        throw new Error(
          'Firmware belum mengonfirmasi pengaman siap.'
        );
      }

      if (code === 'G' || code === 'L') {
        if (
          d.sensor_ok !== true ||
          d.can_pressurize !== true
        ) {
          throw new Error(
            'Sensor atau izin pengisian tekanan belum siap.'
          );
        }
      }

      if (code === 'X') {
        if (d.busy !== true) {
          throw new Error(
            'Firmware tidak melaporkan proses aktif. ' +
            'Gunakan pengaman fisik jika kondisi alat tidak pasti.'
          );
        }
      } else if (d.busy === true) {
        throw new Error(
          'Pengujian masih berlangsung. ' +
          'Perintah baru tidak dikirim.'
        );
      }

      const description = code === 'X'
        ? 'Hentikan proses? X bukan pelepasan tekanan darurat.'
        : code === 'O'
        ? 'Buka katup? Pastikan pelepasan tekanan aman.'
        : 'Jalankan ' + command +
          '? Pastikan rangkaian tanpa pasien, ' +
          'pengaman tekanan independen dan STOP fisik ' +
          'sudah diuji.';

      if (!window.confirm(description)) {
        feedback('Dibatalkan. Perintah tidak dikirim.');
        return;
      }

      if (api.getSettings().mode !== 'live') {
        throw new Error('Mode Live sudah berubah.');
      }

      const result = await api.sendCommand(command);

      if (result?.accepted !== true) {
        throw new Error(
          result?.error ||
          'Firmware tidak mengonfirmasi perintah.'
        );
      }

      state.textContent = 'Perintah diterima API';

      feedback(
        command +
        ' diterima API. Ini bukan bukti pengujian berhasil. ' +
        'Pantau sensor dan kondisi fisik alat.'
      );

    } catch (error) {
      state.textContent = 'Kontrol belum siap';

      feedback(
        'Perintah ' + command +
        ' tidak dikonfirmasi: ' + error.message +
        ' Jika terjadi timeout, periksa kondisi alat ' +
        'sebelum mencoba kembali.'
      );
    } finally {
      sending = false;
    }
  }

  function chooseMain(code) {
    if (code === 'O') {
      showCommand(
        'O',
        'O membuka katup maksimal 15 detik. ' +
        'Perintah x dapat menghentikan pembukaan.'
      );
      return;
    }

    const n = target();
    if (n === null) return;

    if (code === 'G') {
      showCommand(
        'G:' + n,
        'G mengisi tekanan sampai target menggunakan duty terakhir. ' +
        'Firmware memiliki batas waktu pengisian 90 detik.'
      );
    }

    if (code === 'L') {
      showCommand(
        'L:' + n,
        'L menjalankan pengisian, SETTLE 120 detik, ' +
        'kemudian RECORD 60 detik.'
      );
    }
  }

  panel.querySelectorAll('[data-pc-command]').forEach(button => {
    button.addEventListener('click', async () => {
      const code = button.dataset.pcCommand;

      if (api && api.getSettings().mode === 'live') {
        await runLiveCommand(code);
      } else {
        chooseMain(code);
      }
    });
  });

  stop.addEventListener('click', async () => {
    if (api && api.getSettings().mode === 'live') {
      await runLiveCommand('X');
      return;
    }

    showCommand(
      'x',
      'PERINGATAN: Tombol website ini TIDAK menghentikan alat. ' +
      'Perintah x pada firmware menghentikan proses, ' +
      'mematikan pompa dan menutup katup, ' +
      'tetapi tekanan sisa tidak otomatis dibuang. ' +
      'Untuk keadaan darurat gunakan pengaman fisik.'
    );
  });

  const descriptions = {
    'T': 'Membaca tekanan terkalibrasi satu kali.',
    'S': 'Memantau sensor berkelanjutan lewat Serial Monitor.',
    'N:1': 'Preset Normal 120/80 mmHg, 75 BPM.',
    'N:2': 'Preset Hipertensi 150/100 mmHg, 75 BPM.',
    'N:3': 'Preset Hipotensi 90/60 mmHg, 75 BPM.',
    'Q:40': 'Uji dorong pompa 40 ms dengan rekaman 100 Hz.',
    'U': 'Rangkaian otomatis target 65, 105, 145 mmHg dan uji Q.'
  };

  panel.querySelectorAll('[data-pc-serial]').forEach(button => {
    button.addEventListener('click', () => {
      const cmd = button.dataset.pcSerial;

      if (!Object.prototype.hasOwnProperty.call(descriptions, cmd)) {
        feedback('Perintah tidak dikenali.');
        return;
      }

      showCommand(cmd, descriptions[cmd]);
    });
  });

  targetInput.addEventListener('input', () => {
    if (
      selectedCommand.startsWith('G:') ||
      selectedCommand.startsWith('L:')
    ) {
      const code = selectedCommand[0];
      selectedCommand = '';
      commandText.textContent = '—';
      feedback(
        'Target berubah. Klik G atau L kembali ' +
        'untuk memperbarui perintah.'
      );
    }
  });

  byId('pcCopy').addEventListener('click', async () => {
    if (!selectedCommand) {
      feedback('Pilih perintah terlebih dahulu.');
      return;
    }

    try {
      await navigator.clipboard.writeText(selectedCommand);
      feedback(
        'Perintah "' + selectedCommand +
        '" berhasil disalin. ' +
        'Belum dikirim ke ESP32. ' +
        'Jangan menjalankan perintah aktuator sebelum ' +
        'rangkaian dan pengaman fisik divalidasi.'
      );
    } catch (_) {
      feedback(
        'Clipboard tidak tersedia. ' +
        'Salin manual teks: ' + selectedCommand
      );
    }
  });

  state.textContent = 'Firmware Serial · Web tidak mengendalikan alat';

  byId('pcCheck').addEventListener('click', async () => {
    if (!api) {
      feedback('Adapter API tidak tersedia.');
      return;
    }

    state.textContent = 'Memeriksa HTTP API...';

    try {
      const settings = api.getSettings();
      const raw = await api.requestJSON(settings.statusPath);

      const data =
        raw &&
        raw.data &&
        typeof raw.data === 'object'
          ? raw.data
          : raw;

      if (data.is_simulated === true) {
        if (data.dry_run_commands_enabled === true) {
          state.textContent = 'SIMULASI · Dry-run tersedia';
          feedback('Perintah dapat diuji tanpa menggerakkan hardware.');
          return;
        }
        state.textContent = 'SIMULASI · Bukan ESP32';
        feedback('API simulasi terhubung. Tombol tidak mengirim perintah fisik.');
        return;
      }
      if (data.connected === true) {
        if (data.dry_run_commands_enabled === true &&
            data.remote_commands_enabled === false) {
          state.textContent = 'ESP32 terhubung · Dry-run tersedia';
          feedback('API dapat menguji G/L/O/X. Aktuator fisik tetap terkunci.');
          return;
        }
        const allowed =
          data.is_simulated !== true &&
          data.remote_commands_enabled === true &&
          data.safety_ok === true;

        state.textContent = allowed
          ? 'ESP32 terhubung · Izin kontrol tersedia'
          : 'ESP32 terhubung · Kontrol terkunci';

        feedback(
          allowed
            ? 'API terhubung dan izin firmware tersedia. ' +
              'Setiap perintah tetap harus lolos ' +
              'pemeriksaan sensor serta konfirmasi operator.'
            : 'Monitoring ESP32 terhubung. ' +
              'Kontrol belum diizinkan firmware. ' +
              'remote_commands_enabled=' +
              String(data.remote_commands_enabled) +
              ', safety_ok=' +
              String(data.safety_ok)
        );
      } else {
        state.textContent = 'API merespons · ESP32 belum terverifikasi';

        feedback(
          'Endpoint HTTP merespons tetapi tidak ' +
          'mengonfirmasi ESP32 terhubung.'
        );
      }
    } catch (error) {
      state.textContent = 'HTTP API tidak tersedia';

      feedback(
        'Tidak dapat menjangkau HTTP API: ' +
        error.message +
        '. Firmware terbaru yang dikirim tidak ' +
        'menyediakan endpoint Wi-Fi. ' +
        'Gunakan Serial Monitor untuk komunikasi langsung.'
      );
    }
  });
})();
