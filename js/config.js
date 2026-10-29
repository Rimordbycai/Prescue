/* PRESCUE – konfigurasi sederhana. Boleh diganti teman tim firmware. */
window.PRESCUE_CONFIG = {
  defaultMode: "demo", // "demo" | "live"; tidak pernah otomatis menyamar jadi live
  defaultBaseUrl: "http://192.168.4.1",
  defaultStatusPath: "/api/status",
  defaultSamplesPath: "/api/samples", // opsional; boleh kosong jika status mengandung array samples
  pollIntervalMs: 2000,
  requestTimeoutMs: 5000,
  maxChartPoints: 300,
  leakLimitMmhgPerMin: 5
};
