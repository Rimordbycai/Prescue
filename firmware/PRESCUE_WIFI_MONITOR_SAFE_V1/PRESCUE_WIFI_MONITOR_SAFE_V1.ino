#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <WiFi.h>
#include <WebServer.h>
#include "wifi_settings.h"

// PRESCUE UART DSP V1
#define DSP_UART_RX 16
#define DSP_UART_TX 17
#define DSP_UART_BAUD 115200


bool isiSampai(float target, int duty);

#define PAKAI_TACHO       0        // 1 bila sensor optik terpasang
#define PAKAI_ARUS        0        // 1 bila resistor shunt terpasang

#define PIN_MOTOR         26       // ke ENA
#define PIN_KATUP         25       // ke ENB
#define PIN_TACHO         18
#define PIN_SHUNT         32
#define PIN_SENSOR        34       // titik tengah pembagi tegangan sensor
#define PIN_LED           2

#define AMBANG_MENTAH     2800.0f  // batas nilai mentah saat memompa dengan P
#define BATAS_GOYANG      25.0f    // simpangan baku maksimum, satuan hitungan ADC
#define BATAS_KERAS       180.0f   // mmHg; penurunan khusus untuk tahap uji 150 mmHg

#define PULSA_PER_PUTARAN 1
#define R_SHUNT           0.10f
#define V_REF             3.10f

#define PWM_RES           8
#define PWM_FREK_MOTOR    1000
#define PWM_FREK_KATUP    20000
#define KANAL_MOTOR       0        // timer 0
#define KANAL_KATUP       2        // timer 1, supaya frekuensinya tidak bertabrakan
#define DUTY_KATUP_MAKS   88
#define DURASI_UJI1_DETIK 20
#define DURASI_BUKA_MANUAL_MS 15000UL
#define JEDA_KATUP_MANUAL_MS  1000UL
#define WAKTU_STABILISASI_L_MS 2000UL
#define BATAS_BUANG_L_MS       15000UL

uint8_t DUTY_KATUP = 84;           // sekitar 6 V rata rata, setel dengan O:angka
uint8_t dutyIsi = 70;

volatile uint32_t cacahPulsa = 0;
bool dibatalkan = false;
uint32_t katupManualBolehDibuka = 0;

/* ---- variabel kalibrasi MPX ---- */
float Vs   = 4.95f;         // tegangan catu sensor, diisi tetap sesuai hasil ukur
float K  = 0.690f;                 // rasio pembagi, perintah K:
float vA = 0, rA = 0, vB = 0, rB = 0;
bool  adaA = false, adaB = false;
float kalM = .00077712f;
float kalC = .100339f;
float KAL_A = 0;
float KAL_B = 0;

void hitungKoefisien() {
  KAL_A = 7.50062f * kalM / (0.018f * K * Vs);
  KAL_B = 7.50062f * (kalC / (K * Vs) - 0.04f) / 0.018f;
}
bool  kalDariUkur = true;

void IRAM_ATTR isrTacho() { cacahPulsa++; }


/* ==============================================================
 * PRESCUE Wi-Fi MONITOR V1 — read-only, no remote actuator commands.
 * ESP32 hosts its own access point. GET endpoints are available
 * throughout G/L Serial tests because WebServer runs on core 0.
 * Hardware control stays exclusively inside the original serial
 * firmware, with the existing pressure protections.
 * ============================================================== */
struct WifiPoint {
  float elapsed;
  float pressure;
  char phase[12];
};
struct WifiTelemetry {
  float pressure;
  float raw;
  float target;
  float leakRate;
  bool sensorValid;
  bool pumping;
  bool valveOpen;
  bool busy;
  bool leakKnown;
  bool resultKnown;
  bool pressureKnown;
  uint32_t updatedAt;
  unsigned long count;
  char phase[12];
  char mode[8];
  char result[20];
};

static portMUX_TYPE wifiLock = portMUX_INITIALIZER_UNLOCKED;
static WifiTelemetry wifiState = {};
static const size_t WIFI_SAMPLE_LIMIT = 360;
static WifiPoint wifiSamples[WIFI_SAMPLE_LIMIT];
static size_t wifiHead = 0;
static size_t wifiSize = 0;
static float wifiPhaseOffset = 0;
static float wifiLastElapsed = 0;
static char wifiPrevPhase[12] = "";
static WebServer rescueWeb(80);
static bool wifiStarted = false;

void wifiPumpState(bool on) {
  portENTER_CRITICAL(&wifiLock);
  wifiState.pumping = on;
  portEXIT_CRITICAL(&wifiLock);
}
void wifiValveState(bool on) {
  portENTER_CRITICAL(&wifiLock);
  wifiState.valveOpen = on;
  portEXIT_CRITICAL(&wifiLock);
}
void wifiBeginRun(char mode, float target) {
  portENTER_CRITICAL(&wifiLock);
  wifiState.busy = true;
  wifiState.target = target;
  wifiState.leakRate = 0;
  wifiState.leakKnown = false;
  wifiState.resultKnown = false;
  wifiState.count = 0;
  strncpy(wifiState.mode, mode == 'G' ? "G" : mode == 'L' ? "L" : mode == 'O' ? "O" :
                          mode == 'E' ? "E" : "F", sizeof(wifiState.mode) - 1);
  strncpy(wifiState.phase, mode == 'O' ? "VENT" : "FILL", sizeof(wifiState.phase) - 1);
  wifiHead = 0;
  wifiSize = 0;
  wifiPhaseOffset = 0;
  wifiLastElapsed = 0;
  wifiPrevPhase[0] = 0;
  portEXIT_CRITICAL(&wifiLock);
}
void wifiFinishRun() {
  portENTER_CRITICAL(&wifiLock);
  wifiState.busy = false;
  if (strcmp(wifiState.phase, "FAULT") != 0 && strcmp(wifiState.phase, "STOPPED") != 0) {
    strncpy(wifiState.phase, "IDLE", sizeof(wifiState.phase) - 1);
  }
  portEXIT_CRITICAL(&wifiLock);
}
void wifiSetFault(const char *reason, const char *phase) {
  portENTER_CRITICAL(&wifiLock);
  wifiState.pumping = false;
  wifiState.sensorValid = false;
  wifiState.pressureKnown = false;
  wifiState.resultKnown = true;
  strncpy(wifiState.result, reason, sizeof(wifiState.result) - 1);
  strncpy(wifiState.phase, phase, sizeof(wifiState.phase) - 1);
  portEXIT_CRITICAL(&wifiLock);
}
void wifiSetLeakResult(float rate, bool pass) {
  portENTER_CRITICAL(&wifiLock);
  if (isfinite(rate)) {
    wifiState.leakRate = rate;
    wifiState.leakKnown = true;
  }
  wifiState.resultKnown = true;
  strncpy(wifiState.result, pass ? "LOLOS" : "GAGAL", sizeof(wifiState.result) - 1);
  portEXIT_CRITICAL(&wifiLock);
}
void wifiSetFillResult(bool pass) {
  portENTER_CRITICAL(&wifiLock);
  if (strcmp(wifiState.mode, "G") == 0) {
    wifiState.resultKnown = true;
    strncpy(wifiState.result, pass ? "TERCAPAI" : "GAGAL ISI", sizeof(wifiState.result)-1);
  }
  portEXIT_CRITICAL(&wifiLock);
}
void wifiRecordSample(const char *phase, float elapsed, float pressure) {
  if (!isfinite(elapsed) || !isfinite(pressure)) return;
  portENTER_CRITICAL(&wifiLock);
  if (strncmp(wifiPrevPhase, phase, sizeof(wifiPrevPhase)) != 0) {
    if (wifiPrevPhase[0] != 0) wifiPhaseOffset = wifiLastElapsed;
    strncpy(wifiPrevPhase, phase, sizeof(wifiPrevPhase) - 1);
    wifiPrevPhase[sizeof(wifiPrevPhase) - 1] = 0;
  }
  float totalTime = wifiPhaseOffset + elapsed;
  if (totalTime < wifiLastElapsed) totalTime = wifiLastElapsed;
  wifiLastElapsed = totalTime;
  WifiPoint &pt = wifiSamples[wifiHead];
  pt.elapsed = totalTime;
  pt.pressure = pressure;
  strncpy(pt.phase, phase, sizeof(pt.phase) - 1);
  pt.phase[sizeof(pt.phase) - 1] = 0;
  wifiHead = (wifiHead + 1) % WIFI_SAMPLE_LIMIT;
  if (wifiSize < WIFI_SAMPLE_LIMIT) ++wifiSize;
  wifiState.count++;
  wifiState.pressure = pressure;
  wifiState.raw = 0; // ADC raw is not sampled here; never show a stale raw as fresh.
  wifiState.pressureKnown = true;
  wifiState.updatedAt = millis();
  strncpy(wifiState.phase, phase, sizeof(wifiState.phase) - 1);
  wifiState.phase[sizeof(wifiState.phase) - 1] = 0;
  if (pressure < -10 || pressure > 170) wifiState.sensorValid = false;
  portEXIT_CRITICAL(&wifiLock);
}

void wifiCaptureIdle(float pressure, float raw, bool valid) {
  portENTER_CRITICAL(&wifiLock);
  wifiState.pressure = pressure;
  wifiState.raw = raw;
  wifiState.pressureKnown = valid;
  wifiState.sensorValid = valid;
  wifiState.updatedAt = millis();
  portEXIT_CRITICAL(&wifiLock);
}

// No ADC reads from the network task: all sensor sampling is on the loop core.
// This function's body is defined after the existing sensor helpers.
void wifiSampleIdle();

void wifiApiHeaders() {
  rescueWeb.sendHeader("Cache-Control", "no-store");
  rescueWeb.sendHeader("Access-Control-Allow-Origin", "*");  // GET only; no credentials
  rescueWeb.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  rescueWeb.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void wifiApiStatus() {
  WifiTelemetry t;
  portENTER_CRITICAL(&wifiLock);
  t = wifiState;
  portEXIT_CRITICAL(&wifiLock);
  bool fresh = t.updatedAt != 0 && (uint32_t)(millis() - t.updatedAt) < 5000UL;
  bool valid = fresh && t.sensorValid && t.pressureKnown;
  String j;
  j.reserve(550);
  j += "{\"connected\":true,\"connection_type\":\"wifi_ap\",\"pressure_mmHg\":";
  j += valid ? String(t.pressure, 3) : "null";
  j += ",\"target_mmHg\":";
  j += t.busy ? String(t.target, 0) : "null";
  j += ",\"phase\":\""; j += t.phase[0] ? t.phase : "IDLE";
  j += "\",\"test_mode\":";
  if (t.mode[0]) { j += "\""; j += t.mode; j += "\""; } else j += "null";
  j += ",\"pump_on\":"; j += t.pumping ? "true" : "false";
  j += ",\"valve_open\":"; j += t.valveOpen ? "true" : "false";
  j += ",\"sensor_ok\":"; j += valid ? "true" : "false";
  j += ",\"adc_raw\":"; j += (fresh && t.raw > 0) ? String(t.raw, 1) : "null";
  j += ",\"leak_rate_mmHg_min\":"; j += t.leakKnown ? String(t.leakRate, 4) : "null";
  j += ",\"test_result\":";
  if (t.resultKnown) { j += "\""; j += t.result; j += "\""; } else j += "null";
  j += ",\"sample_count\":"; j += String(t.count);
  j += ",\"busy\":"; j += t.busy ? "true" : "false";
  j += ",\"remote_commands_enabled\":false,\"safety_ok\":false,\"can_pressurize\":false,";
  j += "\"is_simulated\":false,\"note\":\"WiFi monitoring only. Remote motor/valve controls disabled.\"}";
  wifiApiHeaders();
  rescueWeb.send(200, "application/json", j);
}

void wifiApiSamples() {
  size_t size, head;
  portENTER_CRITICAL(&wifiLock);
  size = wifiSize;
  head = wifiHead;
  portEXIT_CRITICAL(&wifiLock);
  String j;
  j.reserve(30000);
  j += "{\"samples\":[";
  size_t start = (head + WIFI_SAMPLE_LIMIT - size) % WIFI_SAMPLE_LIMIT;
  for (size_t i = 0; i < size; i++) {
    WifiPoint pt;
    portENTER_CRITICAL(&wifiLock);
    pt = wifiSamples[(start + i) % WIFI_SAMPLE_LIMIT];
    portEXIT_CRITICAL(&wifiLock);
    if (i) j += ',';
    j += "{\"phase\":\"";
    j += pt.phase;
    j += "\",\"elapsed_seconds\":";
    j += String(pt.elapsed, 3);
    j += ",\"pressure_mmHg\":";
    j += String(pt.pressure, 3);
    j += '}';
  }
  j += "]}";
  wifiApiHeaders();
  rescueWeb.send(200, "application/json", j);
}

void wifiApiCapabilities() {
  wifiApiHeaders();
  rescueWeb.send(200, "application/json",
                 "{\"wifi_monitoring\":true,\"remote_commands_enabled\":false,\"reason\":\"Physical controls locked pending hardware validation\"}");
}
void wifiApiCommandLocked() {
  wifiApiHeaders();
  rescueWeb.send(423, "application/json",
                 "{\"accepted\":false,\"error\":\"Kontrol fisik via Wi-Fi belum diaktifkan. Uji sensor dan pengaman dulu.\"}");
}
void wifiApiOptions() {
  wifiApiHeaders();
  rescueWeb.send(204, "text/plain", "");
}
void wifiServeTask(void *) {
  rescueWeb.begin();
  for (;;) {
    rescueWeb.handleClient();
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}
void wifiStart() {
  if (strcmp(PRESCUE_AP_PASSWORD, "GANTI_PASSWORD_UNIK_DISINI") == 0 ||
      strlen(PRESCUE_AP_PASSWORD) < 8) {
    Serial.println("# Wi-Fi tidak dimulai: ganti password di wifi_settings.h (minimal 8 karakter).");
    return;
  }
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(PRESCUE_AP_SSID, PRESCUE_AP_PASSWORD)) {
    Serial.println("# GAGAL mengaktifkan hotspot PRESCUE.");
    return;
  }
  rescueWeb.on("/api/status", HTTP_GET, wifiApiStatus);
  rescueWeb.on("/api/samples", HTTP_GET, wifiApiSamples);
  rescueWeb.on("/api/capabilities", HTTP_GET, wifiApiCapabilities);
  rescueWeb.on("/api/command", HTTP_POST, wifiApiCommandLocked);
  rescueWeb.on("/api/command", HTTP_OPTIONS, wifiApiOptions);
  rescueWeb.onNotFound([]() {
    wifiApiHeaders();
    rescueWeb.send(404, "application/json", "{\"error\":\"Endpoint tidak tersedia\"}");
  });
  wifiStarted = xTaskCreatePinnedToCore(wifiServeTask, "prescue_web", 8192, nullptr,
                                       1, nullptr, 0) == pdPASS;
  if (wifiStarted) {
    Serial.printf("# PRESCUE Wi-Fi SSID: %s, IP: %s (read-only)\n",
                  PRESCUE_AP_SSID, WiFi.softAPIP().toString().c_str());
  } else {
    WiFi.softAPdisconnect(true);
    Serial.println("# GAGAL membuat task HTTP. Kontrol serial tetap tersedia.");
  }
}

/* ================= PWM, POMPA, DAN KATUP ================= */
void pwmSetup() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttachChannel(PIN_MOTOR, PWM_FREK_MOTOR, PWM_RES, KANAL_MOTOR);
  ledcAttachChannel(PIN_KATUP, PWM_FREK_KATUP, PWM_RES, KANAL_KATUP);
#else
  ledcSetup(KANAL_MOTOR, PWM_FREK_MOTOR, PWM_RES);
  ledcAttachPin(PIN_MOTOR, KANAL_MOTOR);
  ledcSetup(KANAL_KATUP, PWM_FREK_KATUP, PWM_RES);
  ledcAttachPin(PIN_KATUP, KANAL_KATUP);
#endif
}

void tulisPwm(uint8_t pin, uint8_t kanal, uint8_t persen) {
  if (persen > 100) persen = 100;
  uint32_t n = (uint32_t)persen * 255 / 100;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)kanal;
  ledcWrite(pin, n);
#else
  (void)pin;
  ledcWrite(kanal, n);
#endif
}

bool pompaAktif = false;
void motor(uint8_t persen) { pompaAktif = persen > 0; wifiPumpState(pompaAktif); tulisPwm(PIN_MOTOR, KANAL_MOTOR, persen); }
void motorMati()           { motor(0); }

void katupBuka() {
  uint8_t d = DUTY_KATUP;
  if (d > DUTY_KATUP_MAKS) d = DUTY_KATUP_MAKS;
  tulisPwm(PIN_KATUP, KANAL_KATUP, d);
  wifiValveState(true);
}
void katupTutup() { tulisPwm(PIN_KATUP, KANAL_KATUP, 0); wifiValveState(false); }
void semuaMati()  { motorMati(); katupTutup(); }

/* ================= SENSOR MPX ================= */
float bacaCepat() {
  uint32_t j = 0;
  for (uint8_t i = 0; i < 8; i++) { j += analogRead(PIN_SENSOR); delayMicroseconds(150); }
  return (float)j / 8.0f;
}

void bacaStat(float &rata, float &simpangan) {
  const uint16_t N = 64;
  float m = 0, s = 0;
  for (uint16_t i = 1; i <= N; i++) {
    float v = analogRead(PIN_SENSOR);
    float d = v - m;
    m += d / i;
    s += d * (v - m);
    delayMicroseconds(150);
  }
  rata = m;
  simpangan = sqrtf(s / (N - 1));
}

void ledStatus(float sd) {
  static bool nyala = false;
  if (sd < BATAS_GOYANG) nyala = true;
  else nyala = !nyala;
  digitalWrite(PIN_LED, nyala ? HIGH : LOW);
}

float keMmHg(float mentah) { return KAL_A * mentah + KAL_B; }

float bacaMilivolt() {
  uint32_t j = 0;
  for (uint8_t i = 0; i < 32; i++) { j += analogReadMilliVolts(PIN_SENSOR); delayMicroseconds(150); }
  return (float)j / 32.0f;
}

float mmHgMudah() {
  float vPin = bacaMilivolt() / 1000.0f;
  float vOut = vPin / K;
  return 7.50062f * (vOut / Vs - 0.04f) / 0.018f;
}

void tekananMudah() {
  float vPin = bacaMilivolt() / 1000.0f;
  float vOut = vPin / K;
  float mmHg = 7.50062f * (vOut / Vs - 0.04f) / 0.018f;
  Serial.printf("# pin %.3f V, Vout %.3f V, tekanan %.1f mmHg\n", vPin, vOut, mmHg);
}

// Pengaman: matikan pompa dan buang tekanan lewat katup bila melewati batas
bool lewatBatas() {
  float raw = bacaCepat();
  // Kegagalan sensor yang jelas ketika pompa ON mematikan pompa; tidak
  // menggantikan relief valve mekanis atau proteksi kegagalan analog.
  if (pompaAktif && (!isfinite(raw) || raw < 80.0f || raw > 4050.0f)) {
    motorMati();
    dibatalkan = true;
    wifiSetFault("SENSOR ERROR", "FAULT");
    Serial.println("# SENSOR TIDAK VALID: pompa dimatikan; inspeksi sensor dan buang tekanan secara manual.");
    return true;
  }
  float p = mmHgMudah();
  if (p > BATAS_KERAS) {
    motorMati();
    wifiSetFault("DARURAT", "FAULT");
    Serial.printf("# HENTI DARURAT M=%.1f. Buang sampai M<20, maksimum 15 detik; x tidak memotong pelepasan.\n", p);
    katupBuka();
    uint32_t mulai = millis();
    while (p >= 20 && millis() - mulai < 15000UL) {
      while (Serial.available()) Serial.read();
      delay(5);
      p = mmHgMudah();
    }
    katupTutup();
    if (p >= 20) Serial.printf("# PERINGATAN: pelepasan darurat TIMEOUT, sisa M=%.1f mmHg! Buang manual dengan aman.\n", p);
    else Serial.printf("# Pelepasan darurat selesai, M=%.1f mmHg.\n", p);
    dibatalkan = true;
    return true;
  }
  return false;
}

bool cekBatal() {
  if (dibatalkan) return true;
  bool batal = false;
  while (Serial.available()) {
    if (tolower(Serial.read()) == 'x') batal = true;
  }
  if (batal) {
    semuaMati();
    dibatalkan = true;
    wifiSetFault("DIHENTIKAN", "STOPPED");
    Serial.println("# DIHENTIKAN");
    return true;
  }
  return false;
}

bool tungguAman(uint32_t lama) {
  uint32_t mulai = millis();
  while (millis() - mulai < lama) {
    if (lewatBatas() || cekBatal()) return false;
    delay(2);
  }
  return true;
}

void bacaSekali() {
  float r, sd; bacaStat(r, sd); ledStatus(sd);
  Serial.printf("# mentah = %.1f, simpangan = %.2f, %s\n",
                r, sd, sd < BATAS_GOYANG ? "STABIL" : "GOYANG");
  if (r > 4050) Serial.println("# PERINGATAN: ADC jenuh");
  if (r < 50)   Serial.println("# catatan: di bawah jangkauan ADC, normal pada tekanan atmosfer");
}

void bacaTekanan() {
  float r, sd; bacaStat(r, sd); ledStatus(sd);
  Serial.printf("# tekanan = %.2f mmHg, mentah %.1f, %s, kalibrasi %s\n",
                keMmHg(r), r, sd < BATAS_GOYANG ? "stabil" : "goyang",
                kalDariUkur ? "hasil ukur" : "teoretis");
}

// Status diagnostik untuk USB API Bridge. Ini BUKAN sertifikasi keselamatan
// fisik: operator wajib cek manometer, stop darurat, sambungan dan katup.
void statusBridgeWeb() {
  float raw, sd; bacaStat(raw, sd);
  float p = keMmHg(raw);
  float cadangan = mmHgMudah();
  bool valid = isfinite(raw) && isfinite(p) && isfinite(cadangan)
       && raw >= 80.0f && raw <= 4050.0f
       && sd <= BATAS_GOYANG && p >= -10.0f && p <= 170.0f
       && cadangan >= -10.0f && cadangan <= 170.0f
       && fabsf(cadangan - p) <= 25.0f;
  Serial.printf("# PRESCUE_WEB_SAFE_V1 sensor_ok=%d pressure_mmHg=%.3f raw_adc=%.1f sd=%.1f hard_limit=180 target_limit=150\n",
      valid ? 1 : 0, p, raw, sd);
}


// IDLE: update pressure at most ~2x/second for the Wi-Fi dashboard.
// Sensor validity is only an electrical plausibility check, not calibration
// proof or proof of correct physical connection. NEVER use it to arm a pump.
void wifiSampleIdle() {
  static uint32_t last = 0;
  if (!wifiStarted || millis() - last < 600UL) return;
  last = millis();
  float raw, sd;
  bacaStat(raw, sd);
  float p = keMmHg(raw);
  float backup = mmHgMudah();
  bool valid = isfinite(raw) && isfinite(sd) && isfinite(p) && isfinite(backup)
               && raw >= 80 && raw <= 4050 && sd <= BATAS_GOYANG
               && p >= -10 && p <= 170 && backup >= -10 && backup <= 170
               && fabsf(p - backup) <= 25;
  wifiCaptureIdle(p, raw, valid);
}


// PRESCUE UART DSP V1
// Telemetri pasif saat sistem idle.
// Tidak mengendalikan pompa atau solenoid.
void dspUartKirim() {
  static uint32_t terakhir = 0;

  if (millis() - terakhir < 1000UL) return;
  terakhir = millis();

  float raw, sd;
  bacaStat(raw, sd);

  float tekanan = keMmHg(raw);
  float pembanding = mmHgMudah();

  bool valid =
      isfinite(raw) &&
      isfinite(sd) &&
      isfinite(tekanan) &&
      isfinite(pembanding) &&
      raw >= 80.0f &&
      raw <= 4050.0f &&
      sd <= BATAS_GOYANG &&
      tekanan >= -10.0f &&
      tekanan <= 170.0f &&
      pembanding >= -10.0f &&
      pembanding <= 170.0f &&
      fabsf(tekanan - pembanding) <= 25.0f;

  char data[80];

  if (valid) {
    snprintf(
        data,
        sizeof(data),
        "PRESCUE_PRESSURE,%.2f,1",
        tekanan
    );
  } else {
    snprintf(
        data,
        sizeof(data),
        "PRESCUE_PRESSURE,NA,0"
    );
  }

  Serial2.println(data);

  // Salinan ke USB Serial untuk debugging.
  Serial.print("# UART2 TX ");
  Serial.println(data);
}

void pompaSebentar(uint16_t lamaMs) {
  if (bacaCepat() > AMBANG_MENTAH) { Serial.println("# Sudah cukup tinggi, jangan ditambah."); return; }
  if (lamaMs < 80)  lamaMs = 80;
  if (lamaMs > 500) lamaMs = 500;
  if (lewatBatas() || cekBatal()) return;
  motor(100);
  uint32_t t = millis();
  while (millis() - t < lamaMs) {
    if (lewatBatas() || cekBatal()) return;
    if (bacaCepat() > AMBANG_MENTAH) break;
  }
  motorMati();
  delay(500);
  float r, sd; bacaStat(r, sd); ledStatus(sd);
  Serial.printf("# pompa %u ms, mentah %.1f, simpangan %.2f, tekanan %.1f mmHg\n",
                lamaMs, r, sd, mmHgMudah());
}

void akhirOtomatis() {
  semuaMati();
  Serial.println("# Pompa mati, katup tertutup. PERINGATAN: tekanan sisa mungkin ada; buang dengan O secara aman.");
}

bool siapPadaTarget(float target, int duty) {
  if (!isiSampai(target, duty)) return false;
  Serial.printf("# Menunggu stabilisasi tekanan selama %.1f detik.\n",
                WAKTU_STABILISASI_L_MS / 1000.0f);
  uint32_t stabilMulai = millis();
  while (millis() - stabilMulai < WAKTU_STABILISASI_L_MS) {
    if (lewatBatas() || cekBatal()) {
      akhirOtomatis();
      return false;
    }
    delay(2);
  }

  float p = keMmHg(bacaCepat());
  if (p > target + 2) {
    Serial.printf("# T=%.2f masih melebihi target+2; membuang tekanan dengan pulsa katup.\n", p);
    uint32_t buangMulai = millis();
    while (p > target + 2 && millis() - buangMulai < BATAS_BUANG_L_MS) {
      if (lewatBatas() || cekBatal()) {
        akhirOtomatis();
        return false;
      }
      katupBuka();
      delay(100);
      katupTutup();
      delay(200);
      p = keMmHg(bacaCepat());
    }
    katupTutup();
    if (p > target + 2) {
      Serial.printf("# DITOLAK: T=%.2f masih melebihi target+2 setelah pelepasan; buang dengan O.\n", p);
      akhirOtomatis();
      return false;
    }
    Serial.printf("# Pelepasan koreksi selesai, T=%.2f mmHg.\n", p);
  }
  return true;
}

void ujiTahan(float target, int duty) {
  if (!siapPadaTarget(target, duty)) return;
  Serial.println("L,fase,detik,T_mmHg");
  uint32_t mulai = millis(), sampel = mulai;
  while (millis() - mulai < 30000UL) {
    if (lewatBatas() || cekBatal()) { akhirOtomatis(); return; }
    if (millis() - sampel >= 200) {
      sampel = millis();
      float currentPressure = keMmHg(bacaCepat());
      float sec = (sampel - mulai) / 1000.0f;
      wifiRecordSample("SETTLE", sec, currentPressure);
      Serial.printf("L,SETTLE,%.3f,%.3f\n", sec, currentPressure);
    }
    delay(2);
  }
  mulai = millis();
  sampel = mulai;
  unsigned long n = 0;
  double rataT = 0, rataP = 0, sTT = 0, sTP = 0;
  float awal = 0, akhir = 0;
  for (;;) {
    if (lewatBatas() || cekBatal()) { akhirOtomatis(); return; }
    uint32_t kini = millis();
    if (n == 0 || kini - sampel >= 200) {
      sampel = kini;
      akhir = keMmHg(bacaCepat());
      double t = (kini - mulai) / 1000.0;
      if (n == 0) awal = akhir;
      ++n;
      double dt = t - rataT, dp = akhir - rataP;
      rataT += dt / n;
      rataP += dp / n;
      sTT += dt * (t - rataT);
      sTP += dt * (akhir - rataP);
      wifiRecordSample("RECORD", (float)t, akhir);
      Serial.printf("L,RECORD,%.3f,%.3f\n", t, akhir);
      if (kini - mulai >= 60000UL) break;
    }
    delay(2);
  }
  double bocor = -60.0 * sTP / sTT;
  Serial.printf("# L awal=%.3f akhir=%.3f n=%lu bocor bertanda=%.4f mmHg/menit: %s (<=5)\n",
                awal, akhir, n, bocor, bocor <= 5.0 ? "LOLOS" : "GAGAL");
  wifiSetLeakResult((float)bocor, bocor <= 5.0);
  if (bocor < 0) Serial.println("# Laju negatif berarti tekanan naik; periksa kestabilan suhu/tekanan sebelum menafsirkan hasil.");
  akhirOtomatis();
}

void pantau() {
  Serial.println("# Mode pantau. Ketik huruf apa saja lalu Enter untuk berhenti.");
  Serial.println("PANTAU,detik,mentah,simpangan,mmhg_kalibrasi,mmhg_mudah,status");
  uint32_t t0 = millis();
  while (!Serial.available()) {
    if (lewatBatas()) return;
    float r, sd; bacaStat(r, sd); ledStatus(sd);
    Serial.printf("PANTAU,%.1f,%.1f,%.2f,%.2f,%.2f,%s\n",
                  (millis() - t0) / 1000.0f, r, sd, keMmHg(r), mmHgMudah(),
                  sd < BATAS_GOYANG ? "STABIL" : "GOYANG");
    delay(300);
  }
  while (Serial.available()) Serial.read();
  Serial.println("# Mode pantau berhenti.");
}

/* ================= KALIBRASI TAHAP 0 ================= */
bool simpanTitik(float volt, float &vSimpan, float &rSimpan, bool &tanda, const char *nama) {
  float r, sd; bacaStat(r, sd); ledStatus(sd);
  if (sd >= BATAS_GOYANG) {
    Serial.printf("# DITOLAK. Titik %s tidak disimpan, simpangan %.2f terlalu besar.\n", nama, sd);
    Serial.println("# Tunggu tekanan diam, pantau dengan S, lalu ulangi.");
    return false;
  }
  if (r > 4050) { Serial.println("# DITOLAK. ADC jenuh, turunkan tekanan."); return false; }
  vSimpan = volt; rSimpan = r; tanda = true;
  Serial.printf("# Titik %s tersimpan, mentah %.1f, simpangan %.2f, %.4f V\n", nama, r, sd, volt);
  return true;
}

void hitung() {
  if (!adaA || !adaB) { Serial.println("# Titik A atau B belum diisi."); return; }
  if (fabs(rB - rA) < 200) {
    Serial.println("# Beda kedua titik terlalu kecil. Ulangi titik B pada tekanan lebih tinggi.");
    return;
  }
  float m = (vB - vA) / (rB - rA);
  float c = vA - m * rA;
  float A = 7.50062f * m / (0.018f * K * Vs);
  float B = 7.50062f * (c / (K * Vs) - 0.04f) / 0.018f;

  Serial.println("\n# ---------- HASIL KOREKSI ----------");
  Serial.printf("# Vs = %.3f V, K = %.4f\n", Vs, K);
  Serial.printf("# titik A : mentah %.1f, %.4f V\n", rA, vA);
  Serial.printf("# titik B : mentah %.1f, %.4f V\n", rB, vB);
  Serial.printf("# m = %.8f volt per hitungan\n", m);
  Serial.printf("# c = %.6f volt\n", c);
  Serial.printf("float KAL_A = %.6f;\n", A);
  Serial.printf("float KAL_B = %.4f;\n", B);

  Serial.println("# ---------- PEMERIKSAAN ----------");
  bool wajar = true;
  if (!isfinite(m) || !isfinite(c) || !isfinite(A) || !isfinite(B) || m <= 0) {
    Serial.println("# GAGAL: kemiringan negatif, titik A dan B kemungkinan tertukar.");
    wajar = false;
  }
  if (A < 0.08f || A > 0.14f) {
    Serial.println("# PERIKSA: KAL_A di luar kisaran wajar 0.08 sampai 0.14.");
    wajar = false;
  }
  if (fabs(rB - rA) < 1500) {
    Serial.println("# PERIKSA: rentang kedua titik sempit, ketelitian berkurang.");
    wajar = false;
  }
  Serial.printf("# Rentang tercakup %.0f hitungan, setara sekitar %.0f mmHg\n",
                fabs(rB - rA), fabs(rB - rA) * A);

  if (wajar) {
    kalM = m; kalC = c; hitungKoefisien(); kalDariUkur = true;
    Serial.println("# HASIL LAYAK DIPAKAI. Aktif di RAM saja; reset mengembalikan nilai kode.");
    Serial.printf("float kalM = %.9ff;\nfloat kalC = %.9ff;\n", kalM, kalC);
    Serial.println("# Salin kalM dan kalC di atas ke kode untuk menyimpan permanen.");
  } else {
    Serial.println("# HASIL BELUM LAYAK, kalibrasi lama tetap dipakai. Ulangi titik yang bermasalah.");
  }
}

void statusKalibrasi() {
  Serial.println("\n# ---------- STATUS ----------");
  Serial.printf("# kalM=%.9f kalC=%.9f; perubahan kalibrasi/K/V hanya RAM.\n", kalM, kalC);
  Serial.println("# G/L/E/F memakai T; pengaman independen M>180, pelepasan M<20 / 15s.");
  Serial.println("# E/F: operator harus memastikan L lulus dahulu (tidak dikunci perangkat lunak).");
  Serial.printf("# Vs = %.3f V, K = %.4f\n", Vs, K);
  Serial.printf("# titik A : %s", adaA ? "" : "belum diisi\n");
  if (adaA) Serial.printf("mentah %.1f, %.4f V\n", rA, vA);
  Serial.printf("# titik B : %s", adaB ? "" : "belum diisi\n");
  if (adaB) Serial.printf("mentah %.1f, %.4f V\n", rB, vB);
  Serial.printf("# KAL_A = %.6f, KAL_B = %.4f, sumber %s\n",
                KAL_A, KAL_B, kalDariUkur ? "hasil ukur" : "teoretis");
  Serial.printf("# duty katup = %u persen, batas %u persen\n", DUTY_KATUP, DUTY_KATUP_MAKS);
}

/* ================= KATUP ================= */
void bukaKatupManual(int dutyBaru) {
  if ((int32_t)(millis() - katupManualBolehDibuka) < 0) {
    Serial.printf("# katup masih jeda, tunggu %lu ms.\n",
                  (unsigned long)(katupManualBolehDibuka - millis()));
    return;
  }
  motorMati();  // Pelepasan manual tidak boleh bersamaan dengan pemompaan.
  if (dutyBaru > 0) {
    if (dutyBaru > DUTY_KATUP_MAKS) dutyBaru = DUTY_KATUP_MAKS;
    DUTY_KATUP = (uint8_t)dutyBaru;
  }
  Serial.printf("# katup terbuka, duty %u persen. Ketik x lalu Enter untuk menutup; maksimum 15 detik.\n", DUTY_KATUP);
  katupBuka();
  uint32_t mulai = millis(), sampel = mulai;
  while (millis() - mulai < DURASI_BUKA_MANUAL_MS) {
    if (cekBatal()) break;
    uint32_t kini = millis();
    if (kini - sampel >= 200UL) {
      sampel = kini;
      float p = keMmHg(bacaCepat());
      wifiRecordSample("VENT", (kini - mulai) / 1000.0f, p);
      Serial.printf("T,%.3f\n", p);
    }
    delay(2);
  }
  katupTutup();
  katupManualBolehDibuka = millis() + JEDA_KATUP_MANUAL_MS;
  Serial.println("# katup tertutup; jeda 1 detik sebelum O berikutnya");
}

/* ================= UJI MOTOR ================= */
float ukurRpm(uint16_t jendelaMs) {
#if PAKAI_TACHO
  cacahPulsa = 0;
  if (!tungguAman(jendelaMs)) return -1;
  uint32_t n = cacahPulsa;
  return (float)n * 60000.0f / ((float)jendelaMs * PULSA_PER_PUTARAN);
#else
  if (!tungguAman(jendelaMs)) return -1;
  return -1;
#endif
}

float ukurArus() {
#if PAKAI_ARUS
  uint32_t j = 0;
  for (uint8_t i = 0; i < 32; i++) { j += analogRead(PIN_SHUNT); delayMicroseconds(150); }
  return ((float)j / 32.0f) * V_REF / 4095.0f / R_SHUNT;
#else
  return -1;
#endif
}

void ujiPutaran() {
  Serial.println("\n# ===== UJI 1 : PUTARAN DASAR =====");
  Serial.printf("# Pompa menyala %d detik. Ketik x untuk berhenti.\n", DURASI_UJI1_DETIK);
  delay(2000);
  if (lewatBatas() || cekBatal()) return;
  motor(100);
  for (int d = DURASI_UJI1_DETIK; d > 0; d--) {
    Serial.printf("# menyala, sisa %d detik, tekanan %.1f mmHg\n", d, mmHgMudah());
    if (!tungguAman(1000)) return;
  }
  float rpm = ukurRpm(1000);
  float arus = ukurArus();
  motorMati();
  Serial.println("HASIL_1,rpm,arus_a");
  Serial.printf("HASIL_1,%.1f,%.3f\n", rpm, arus);
}

void ujiZonaMati() {
#if !PAKAI_TACHO
  Serial.println("# UJI 2 memerlukan tachometer.");
  Serial.println("# Tanpa tachometer, cari duty mulai secara manual dengan D40, D50, D60, dan seterusnya.");
  return;
#else
  Serial.println("\n# ===== UJI 2 : ZONA MATI DAN KURVA RPM =====");
  Serial.println("UJI_2,arah,duty_persen,rpm,arus_a,berputar");
  uint8_t dutyMulai = 0, dutyBerhenti = 0;

  for (uint8_t d = 2; d <= 100 && !dibatalkan; d += 2) {
    motorMati(); delay(600);
    if (lewatBatas() || cekBatal()) return;
    motor(d);
    float rpm = ukurRpm(800);
    float arus = ukurArus();
    bool jalan = (rpm > 5);
    Serial.printf("UJI_2,NAIK,%u,%.1f,%.3f,%d\n", d, rpm, arus, jalan);
    if (jalan && dutyMulai == 0) { dutyMulai = d; break; }
    if (lewatBatas() || cekBatal()) return;
  }
  if (dutyMulai == 0) { motorMati(); Serial.println("# Pompa tidak pernah berputar"); return; }

  for (int d = 100; d >= 2 && !dibatalkan; d -= 5) {
    if (lewatBatas() || cekBatal()) return;
    motor(d);
    float rpm = ukurRpm(1000);
    float arus = ukurArus();
    bool jalan = (rpm > 5);
    Serial.printf("UJI_2,TURUN,%d,%.1f,%.3f,%d\n", d, rpm, arus, jalan);
    if (!jalan) { dutyBerhenti = d; break; }
    if (lewatBatas() || cekBatal()) return;
  }
  motorMati();
  Serial.println("RINGKASAN_2,duty_mulai,duty_berhenti,histeresis");
  Serial.printf("RINGKASAN_2,%u,%u,%d\n", dutyMulai, dutyBerhenti, dutyMulai - dutyBerhenti);
#endif
}

void ujiCoasting() {
#if !PAKAI_TACHO
  Serial.println("# UJI 3 memerlukan tachometer.");
  return;
#else
  Serial.println("\n# ===== UJI 3 : WAKTU COASTING =====");
  if (lewatBatas() || cekBatal()) return;
  motor(100);
  if (!tungguAman(3000)) return;
  cacahPulsa = 0;
  uint32_t t0 = micros();
  motorMati();
  uint32_t pulsaAkhir = 0, waktuAkhir = t0;
  while (micros() - t0 < 5000000UL) {
    if (cacahPulsa != pulsaAkhir) { pulsaAkhir = cacahPulsa; waktuAkhir = micros(); }
    if (micros() - waktuAkhir > 500000UL) break;
    delay(2);
  }
  Serial.println("RINGKASAN_3,waktu_coasting_ms,putaran_sisa");
  Serial.printf("RINGKASAN_3,%.0f,%.2f\n",
                (waktuAkhir - t0) / 1000.0f, (float)pulsaAkhir / PULSA_PER_PUTARAN);
#endif
}

void ujiKetahanan(uint16_t siklus) {
  if (siklus == 0 || siklus > 500) siklus = 30;
  Serial.println("\n# ===== UJI 4 : KETAHANAN =====");
  Serial.printf("# %u siklus: jalan 5 detik, berhenti 10 detik.\n", siklus);
  Serial.println("UJI_4,siklus,rpm,arus_a,mmhg,menit");
  uint32_t awal = millis();
  for (uint16_t i = 1; i <= siklus && !dibatalkan; i++) {
    if (lewatBatas() || cekBatal()) return;
    motor(100);
    for (uint8_t k = 0; k < 30; k++) {
      delay(100);
      if (lewatBatas() || cekBatal()) return;
    }
    float rpm = ukurRpm(1000);
    float arus = ukurArus();
    float p = mmHgMudah();
    if (!tungguAman(1000)) return;
    motorMati();
    Serial.printf("UJI_4,%u,%.1f,%.3f,%.1f,%.2f\n", i, rpm, arus, p, (millis() - awal) / 60000.0f);
    for (uint8_t d = 0; d < 100; d++) { if (!tungguAman(100)) return; }
  }
  motorMati();
  Serial.println("# Selesai. Bila sistem tertutup, buka katup atau stopcock untuk membuang tekanan.");
}

bool isiSampai(float target, int duty) {
  semuaMati();
  if (!isfinite(target) || target < 20 || target > 150 || duty < 1 || duty > 100) {
    Serial.println("# target 20..150, duty 1..100"); return false;
  }
  dutyIsi = duty;
  uint32_t t0 = millis(), cek = t0, log = t0;
  bool berhasil = false, melewati20 = false;
  float patokan = 0;
  const char *hasil = "batas 90 detik";
  Serial.println("G,fase,detik,T_mmHg");
  if (lewatBatas() || cekBatal()) { akhirOtomatis(); return false; }
  float p = keMmHg(bacaCepat());
  if (p > target + 2) {
    Serial.printf("# DITOLAK: T=%.2f lebih dari target+2; buang dahulu dengan O.\n", p);
    akhirOtomatis();
    return false;
  }
  if (p >= target) {
    Serial.printf("# T=%.2f sudah >= target; tidak memompa.\n", p);
    berhasil = true; hasil = "sudah mencapai target";
  } else {
    melewati20 = p > 20;
    patokan = p;
    motor(duty);
    while (millis() - t0 < 90000UL) {
      if (lewatBatas() || cekBatal()) { hasil = "darurat/batal"; break; }
      p = keMmHg(bacaCepat());
      uint32_t kini = millis();
      if (p >= target) { berhasil = true; hasil = "target tercapai"; break; }
      if (!melewati20) {
        if (p > 20) { melewati20 = true; cek = kini; patokan = p; }
        else if (kini - t0 >= 30000UL) { hasil = "manset lepas / kebocoran besar: belum >20 dalam 30s"; break; }
      } else if (kini - cek >= 5000UL) {
        if (p - patokan < 2) { hasil = "kenaikan <2 mmHg/5s, cek kebocoran/duty"; break; }
        cek = kini; patokan = p;
      }
      if (kini - log >= 2000UL) {
        log = kini;
        float sec = (kini - t0) / 1000.0f;
        wifiRecordSample("FILL", sec, p);
        Serial.printf("G,FILL,%.3f,%.3f\n", sec, p);
      }
      delay(2);
    }
  }
  motorMati();
  wifiSetFillResult(berhasil);
  Serial.printf("# G %s; durasi %.3fs, T=%.3f\n", hasil, (millis() - t0) / 1000.0f, p);
  akhirOtomatis();
  return berhasil;
}

void ujiPertahankan(float target, float durasi, int duty) {
  if (!siapPadaTarget(target, duty)) return;
  Serial.println("E,detik,T_mmHg,pompa,jumlah_ON,toleransi_2mmHg");
  uint32_t mulai = millis(), log = mulai, onMulai = mulai;
  unsigned long jumlah = 0;
  bool hidup = false, pertama = true;
  while (millis() - mulai < (uint32_t)(durasi * 1000)) {
    if (lewatBatas() || cekBatal()) break;
    float p = keMmHg(bacaCepat());
    uint32_t kini = millis();
    if (hidup && p >= target) { motorMati(); hidup = false; }
    if (!hidup && p <= target - 1) {
      if (lewatBatas() || cekBatal()) break;
      motor(duty); hidup = true; ++jumlah;
      onMulai = kini;
    }
    // Koreksi normal hanya sekitar 1 mmHg: jangan terapkan syarat G sebesar 2 mmHg/5s.
    if (hidup && kini - onMulai >= 30000UL) {
      Serial.println("# E gagal: pompa ON mencapai 30s tanpa mencapai target"); break;
    }
    if (pertama || kini - log >= 1000UL) {
      pertama = false; log = kini;
      Serial.printf("E,%.3f,%.3f,%d,%lu,%s\n", (kini - mulai) / 1000.0f, p, hidup, jumlah,
                    fabsf(p - target) <= 2 ? "OK" : "DI_LUAR");
    }
    delay(2);
  }
  Serial.printf("# E selesai, durasi %.3fs, jumlah ON=%lu; tidak ada koreksi katup.\n", (millis() - mulai) / 1000.0f, jumlah);
  akhirOtomatis();
}

void ujiTurun(float awalTarget, float akhirTarget, float laju, int duty) {
  semuaMati();
  if (lewatBatas() || cekBatal()) { akhirOtomatis(); return; }
  if (keMmHg(bacaCepat()) > awalTarget + 2) {
    Serial.println("# F ditolak: T awal > start+2; buang tekanan dahulu dengan O.");
    akhirOtomatis(); return;
  }
  if (!isiSampai(awalTarget, duty)) return;
  float awal = keMmHg(bacaCepat());
  if (awal > awalTarget + 2) {
    Serial.println("# F ditolak: overshoot pengisian > start+2."); akhirOtomatis(); return;
  }
  uint32_t mulai = millis(), kontrol = mulai, buka = mulai;
  uint32_t batas = (uint32_t)(((awal - akhirTarget) / laju + 15) * 1000);
  bool terbuka = false, pertama = true, tercapai = false;
  float pulsa = 20, p = awal;
  double jumlahKuadrat = 0;
  unsigned long n = 0;
  Serial.println("F,detik,T_mmHg,lintasan_mmHg,pulsa_ms");
  while (millis() - mulai <= batas) {
    if (lewatBatas() || cekBatal()) break;
    uint32_t kini = millis();
    if (terbuka && kini - buka >= (uint32_t)pulsa) { katupTutup(); terbuka = false; }
    if (pertama || kini - kontrol >= 100UL) {
      pertama = false; kontrol = kini;
      p = keMmHg(bacaCepat());
      float t = (kini - mulai) / 1000.0f;
      float lintasan = fmaxf(akhirTarget, awal - laju * t);
      float galat = p - lintasan;
      jumlahKuadrat += (double)galat * galat; ++n;
      if (p <= akhirTarget) {
        tercapai = true; katupTutup();
        Serial.printf("F,%.3f,%.3f,%.3f,0\n", t, p, lintasan);
        break;
      }
      pulsa = fmaxf(10, fminf(60, pulsa + 3 * galat));
      int dipakai = 0;
      if (galat > 0) { katupBuka(); terbuka = true; buka = millis(); dipakai = (int)pulsa; }
      else { katupTutup(); terbuka = false; }
      Serial.printf("F,%.3f,%.3f,%.3f,%d\n", t, p, lintasan, dipakai);
    }
    delay(2);
  }
  semuaMati();
  float lama = (millis() - mulai) / 1000.0f;
  Serial.printf("# F %s, %.3fs, awal=%.3f akhir=%.3f, laju rata-rata=%.3f mmHg/s, RMS=%.3f mmHg\n",
                tercapai ? "tercapai" : "batal/darurat/timeout", lama, awal, p,
                lama > 0 ? (awal - p) / lama : 0, n ? sqrt(jumlahKuadrat / n) : 0);
  akhirOtomatis();
}

// Semua ruas harus angka finite lengkap; tidak menerima koma kosong/argumen tambahan.
bool bacaArgumen(const String &b, float *nilai, int minimal, int maksimal, int &jumlah) {
  jumlah = 0;
  if (b.length() < 3 || b.charAt(1) != ':') return false;
  const char *s = b.c_str() + 2;
  for (;;) {
    while (isspace((unsigned char)*s)) ++s;
    if (!*s || jumlah >= maksimal) return false;
    char *akhir;
    float v = strtof(s, &akhir);
    if (akhir == s || !isfinite(v)) return false;
    while (isspace((unsigned char)*akhir)) ++akhir;
    nilai[jumlah++] = v;
    if (!*akhir) return jumlah >= minimal;
    if (*akhir != ',') return false;
    s = akhir + 1;
  }
}

bool dutyValid(float d) { return d >= 1 && d <= 100 && floorf(d) == d; }

/* ================= ANTARMUKA ================= */
void bantuan() {
  Serial.println("\n# ---- POMPA ----");
  Serial.println("# 1        putaran dasar 20 detik");
  Serial.println("# 2        zona mati, perlu tachometer");
  Serial.println("# 3        waktu coasting, perlu tachometer");
  Serial.println("# 4:50     ketahanan 50 siklus");
  Serial.println("# D60      pompa pada duty 60 persen, ketik m untuk baca tekanan, x untuk berhenti");
  Serial.println("# P:120    pompa sebentar 120 ms");
  Serial.println("# L:150[,duty] isi G, stabilisasi 2s, koreksi katup bila >target+2, settle 30s, rekam T 60s/200ms");
  Serial.println("# E:120[,detik[,duty]] isi G lalu pertahankan, default/maks 300s; pompa ON<=target-1 OFF>=target");
  Serial.println("# E hanya pompa, tanpa koreksi katup; CSV 1s dengan toleransi +/-2 mmHg dan jumlah ON");
  Serial.println("# F:180,40[,laju] isi G lalu turun dengan pulsa katup, default 2.5, rentang 2..3 mmHg/s");
  Serial.println("# F menolak T awal >start+2; tidak memompa saat di bawah lintasan; timeout ideal+15s");
  Serial.println("# Sebelum E/F operator wajib memastikan L lulus; ini prasyarat alur kerja, bukan kunci software.");
  Serial.println("# ---- KATUP ----");
  Serial.println("# O        buka katup sampai x, paling lama 15 detik; cetak T tiap 200 ms");
  Serial.println("# O:55     setel duty katup 55 persen lalu buka");
  Serial.println("# ---- SENSOR ----");
  Serial.println("# R        nilai mentah dan kestabilan");
  Serial.println("# T        tekanan dengan kalibrasi KAL_A dan KAL_B");
  Serial.println("# Q        diagnostik aman bridge (sensor/rentang/kalibrasi); tanpa aktuasi");
  Serial.println("# M        tekanan dengan kalibrasi pabrik ESP32");
  Serial.println("# S        mode pantau berkelanjutan");
  Serial.println("# G:150,60 isi otomatis sampai 150 mmHg dengan duty 60, lalu pompa mati");
  Serial.println("# G:150    sama, memakai duty terakhir; T terkalibrasi, maksimum 90s, log 2s");
  Serial.println("# G: harus >20 dalam 30s, kemudian naik >=2 setiap 5s. M>180 selalu prioritas darurat.");
  Serial.println("# Darurat: pompa OFF, katup buka sampai M<20 atau 15s; x tidak memotong pelepasan.");
  Serial.println("# x membatalkan semua tahap normal: pompa OFF, katup tutup. Tekanan sisa harus dibuang aman.");
  Serial.println("# ---- KALIBRASI TAHAP 0 ----");
  Serial.println("# K:0.676  rasio pembagi hasil ukur, 0<K<=1; T langsung dihitung ulang");
  Serial.println("# V:4.95   catu sensor dalam volt, >0; T langsung dihitung ulang");
  Serial.println("# K/V/H hanya tersimpan di RAM. H mencetak kalM/kalC untuk disalin ke kode.");
  Serial.println("# A:0.462  simpan titik A, isi tegangan di GPIO34");
  Serial.println("# B:1.873  simpan titik B, isi tegangan di GPIO34");
  Serial.println("# H        hitung KAL_A dan KAL_B");
  Serial.println("# I        lihat status");
  Serial.println("# C:1.234  cek kalibrasi, isi tegangan GPIO34 dari multimeter");
  Serial.println("# ---- UMUM ----");
  Serial.println("# X        matikan pompa dan tutup katup");
  Serial.println("# ?        bantuan");
}

void setup() {
  Serial.begin(115200);
  pwmSetup();
  semuaMati();
  delay(400);
  hitungKoefisien();
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
#if PAKAI_TACHO
  pinMode(PIN_TACHO, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_TACHO), isrTacho, FALLING);
#endif
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Serial2.begin(
      DSP_UART_BAUD,
      SERIAL_8N1,
      DSP_UART_RX,
      DSP_UART_TX
  );

  Serial.println("# UART2 DSP siap.");

  wifiStart();
  Serial.println("\n# UJI POMPA, KATUP, DAN SENSOR MPX DENGAN L298N - Kelompok 18");
  Serial.printf("# pin pompa=%d  pin katup=%d  pin sensor=%d\n", PIN_MOTOR, PIN_KATUP, PIN_SENSOR);
  bantuan();
  Serial.println("# Wi-Fi: hotspot PRESCUE untuk monitoring. GET /api/status, /api/samples; POST kontrol terkunci.");
}

void loop() {
  if (!Serial.available()) {
    wifiSampleIdle();
    dspUartKirim();
    delay(10);
    return;
  }
  String b = Serial.readStringUntil('\n'); b.trim();
  if (b.length() == 0) return;
  char c = toupper(b.charAt(0));
  dibatalkan = false;
  int p = b.indexOf(':');
  float arg = (p > 0) ? b.substring(p + 1).toFloat() : 0;

  if      (c == '1') ujiPutaran();
  else if (c == '2') ujiZonaMati();
  else if (c == '3') ujiCoasting();
  else if (c == '4') ujiKetahanan(p > 0 ? b.substring(p + 1).toInt() : 30);
  else if (c == 'P') pompaSebentar(p > 0 ? b.substring(p + 1).toInt() : 120);
  else if (c == 'G' || c == 'L' || c == 'E' || c == 'F') {
    float a[3] = {150, 0, 0}; int n = 0;
    bool valid = (c == 'G' && b.length() == 1) || bacaArgumen(b, a, c == 'F' ? 2 : 1, (c == 'E' || c == 'F') ? 3 : 2, n);
    int duty = dutyIsi;
    float durasi = 300, laju = 2.5f;
    if (c == 'E') {
      if (n >= 2) durasi = a[1];
      if (n >= 3) { valid = valid && dutyValid(a[2]); if (valid) duty = (int)a[2]; }
      valid = valid && durasi > 0 && durasi <= 300;
    } else if (c == 'F') {
      if (n >= 3) laju = a[2];
      valid = valid && a[1] >= 20 && a[1] < a[0] && laju >= 2 && laju <= 3;
    } else if (n >= 2) {
      valid = valid && dutyValid(a[1]); if (valid) duty = (int)a[1];
    }
    valid = valid && a[0] >= 20 && a[0] <= 150;
    if (!valid) { Serial.println("# Argumen tidak valid. Lihat ?; angka lengkap, target 20..150, duty bulat 1..100."); return; }
    wifiBeginRun(c, a[0]);
    if (c == 'G') isiSampai(a[0], duty);
    else if (c == 'L') ujiTahan(a[0], duty);
    else if (c == 'E') ujiPertahankan(a[0], durasi, duty);
    else ujiTurun(a[0], a[1], laju, duty);
    wifiFinishRun();
  }
  else if (c == 'O') { wifiBeginRun('O', 0); bukaKatupManual(p > 0 ? (int)arg : 0); wifiFinishRun(); }
  else if (c == 'R') bacaSekali();
  else if (c == 'T') bacaTekanan();
  else if (c == 'Q') statusBridgeWeb();
  else if (c == 'M') tekananMudah();
  else if (c == 'S') pantau();
  else if (c == 'H') hitung();
  else if (c == 'I') statusKalibrasi();
  else if (c == 'X') { semuaMati(); Serial.println("# pompa mati, katup tertutup"); }
  else if (c == '?') bantuan();
  else if (c == 'K' || c == 'V') {
    float a[1]; int n;
    if (!bacaArgumen(b, a, 1, 1, n) || a[0] <= 0 || (c == 'K' && a[0] > 1)) {
      Serial.println("# Format K:rasio (0<K<=1) atau V:volt (V>0), angka finite."); return;
    }
    float kBaru = c == 'K' ? a[0] : K, vBaru = c == 'V' ? a[0] : Vs;
    float koefA = 7.50062f * kalM / (0.018f * kBaru * vBaru);
    float koefB = 7.50062f * (kalC / (kBaru * vBaru) - 0.04f) / 0.018f;
    if (!isfinite(koefA) || !isfinite(koefB) || koefA <= 0) {
      Serial.println("# Ditolak: koefisien kalibrasi tidak valid."); return;
    }
    K = kBaru; Vs = vBaru; hitungKoefisien();
    Serial.printf("# K=%.4f Vs=%.4f; T dihitung ulang, hanya RAM.\n", K, Vs);
    if (K < 0.5f || K > 0.8f) Serial.println("# PERIKSA: K di luar kisaran wajar 0.5 sampai 0.8");
    if (Vs < 4.5f || Vs > 5.5f) Serial.println("# PERIKSA: Vs di luar kisaran wajar 4.5 sampai 5.5 V");
  }
  else if (c == 'A' || c == 'B') {
    float volt[1]; int n;
    if (!bacaArgumen(b, volt, 1, 1, n) || volt[0] <= 0 || volt[0] > 3.3f) {
      Serial.printf("# Format %c:0.462, tegangan GPIO34 harus >0 sampai 3.3 V.\n", c); return;
    }
    if (c == 'A') simpanTitik(volt[0], vA, rA, adaA, "A");
    else          simpanTitik(volt[0], vB, rB, adaB, "B");
  }
  else if (c == 'D') {
    if (b.length() < 2) { Serial.println("# format D60, duty 0 sampai 100"); return; }
    int v = b.substring(1).toInt();
    if (v < 0 || v > 100) { Serial.println("# duty harus 0 sampai 100"); return; }
    if (lewatBatas() || cekBatal()) return;
    motor(v);
    Serial.printf("# pompa pada duty %d persen.\n", v);
    Serial.println("# ketik m lalu Enter untuk baca tekanan, x lalu Enter untuk berhenti");
    uint32_t mulai = millis(), cek = 0;
    const char *akhir = "batas 60 detik";
    while (millis() - mulai < 60000UL) {
      if (Serial.available()) {
        char k = tolower(Serial.read());
        if (k == 'x') { akhir = "dihentikan dengan x"; break; }
        if (k == 'm') tekananMudah();
      }
      if (millis() - cek >= 20) {
        cek = millis();
        if (lewatBatas()) return;
      }
      delay(5);
    }
    motorMati();
    float lama = (millis() - mulai) / 1000.0f;
    delay(300);
    Serial.printf("# pompa dimatikan (%s). Duty %d persen berjalan %.2f detik, tekanan %.1f mmHg\n",
                  akhir, v, lama, mmHgMudah());
  }
  else if (c == 'C') {
    if (p < 0 || arg <= 0.0f) { Serial.println("# Format C:1.234, isi tegangan GPIO34 dari multimeter"); return; }
    float r, sd; bacaStat(r, sd);
    float pMulti = 7.50062f * (arg / (K * Vs) - 0.04f) / 0.018f;
    float pKal = keMmHg(r);
    Serial.printf("# multimeter %.3f V setara %.1f mmHg, kalibrasi %.1f mmHg (mentah %.1f), selisih %.1f mmHg\n",
                  arg, pMulti, pKal, r, pKal - pMulti);
  }
  else {
    Serial.println("# perintah tidak dikenal, ketik ? untuk bantuan");
  }
}