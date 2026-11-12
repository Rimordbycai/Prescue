#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <WiFi.h>
#include <WebServer.h>
#include "wifi_settings.h"

bool isiSampai(float target, int duty);
bool dutyValid(float d);
void ujiDorong(float ms);
void rangkaianUjiOtomatis(int duty);

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
#define BATAS_KERAS       300.0f   // mmHg, di atas ini pompa mati otomatis

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

#define UJI_AUTO_DUTY       80        // duty pengisian G pada rangkaian otomatis
#define UJI_AUTO_JEDA_MS    60000UL   // jeda setelah target G tercapai sebelum Q:20
#define UJI_AUTO_Q_JARAK_MS 5000UL    // jarak antarawal perintah Q

uint8_t DUTY_KATUP = 84;           // sekitar 6 V rata rata, setel dengan O:angka
uint8_t dutyIsi = 70;

volatile uint32_t cacahPulsa = 0;
bool dibatalkan = false;
uint32_t katupManualBolehDibuka = 0;

/* ---- variabel kalibrasi MPX ---- */
float Vs   = 4.91f;         // tegangan catu sensor, diisi tetap sesuai hasil ukur
float K  = 0.667f;                 // rasio pembagi, perintah K:
float vA = 0, rA = 0, vB = 0, rB = 0;
bool  adaA = false, adaB = false;
float kalM = 0.000733038f;
float kalC = 0.122760318f;
float KAL_A = 0;
float KAL_B = 0;

void hitungKoefisien() {
  KAL_A = 7.50062f * kalM / (0.018f * K * Vs);
  KAL_B = 7.50062f * (kalC / (K * Vs) - 0.04f) / 0.018f;
}
bool  kalDariUkur = true;

void bacaStat(float &rata, float &simpangan);

/* PRESCUE LAB Wi-Fi API — MONITOR ONLY V2
 * Based on the latest team serial firmware (2026-11-12).
 * This code NEVER receives actuator commands over Wi-Fi.
 * A network connection is NOT proof of safe hardware operation.
 */

static WebServer prescueServer(80);
static portMUX_TYPE prescueMux = portMUX_INITIALIZER_UNLOCKED;
static constexpr size_t PRESCUE_RING_CAP = 120;
struct PrescuePoint { float elapsed; float pressure; };
static PrescuePoint prescueRing[PRESCUE_RING_CAP];
static size_t prescueHead=0, prescueCount=0;
struct PrescueLive {
  float pressure=0, raw=0;
  uint32_t updatedAt=0;
  bool sensorValid=false;
  bool pumpOn=false, valveOpen=false, busy=false;
  char command='-';
};
static PrescueLive prescueLive;
static bool prescueWifiStarted=false;

void prescuePump(bool on){
  portENTER_CRITICAL(&prescueMux);
  prescueLive.pumpOn=on;
  portEXIT_CRITICAL(&prescueMux);
}
void prescueValve(bool on){
  portENTER_CRITICAL(&prescueMux);
  prescueLive.valveOpen=on;
  portEXIT_CRITICAL(&prescueMux);
}
void prescueBusy(bool busy, char command){
  portENTER_CRITICAL(&prescueMux);
  prescueLive.busy=busy;
  if(busy) prescueLive.command=command;
  portEXIT_CRITICAL(&prescueMux);
}
struct PrescueSerialBusyGuard {
  explicit PrescueSerialBusyGuard(char command){prescueBusy(true,command);}
  ~PrescueSerialBusyGuard(){prescueBusy(false,'-');}
};

// Record values already measured by the ORIGINAL firmware. No ADC reads
// occur in HTTP callbacks, so the Wi-Fi task does not contend for the ADC.
void prescueNoteRaw(float raw, float sd){
  static uint32_t last=0;
  uint32_t now=millis();
  if((uint32_t)(now-last)<600UL) return;
  last=now;
  float pressure=KAL_A*raw+KAL_B;
  bool valid=isfinite(raw) && isfinite(pressure) &&
             raw>=80.0f && raw<=4050.0f &&
             pressure>=-10.0f && pressure<=300.0f &&
             (sd<0 || (isfinite(sd) && sd<BATAS_GOYANG));
  portENTER_CRITICAL(&prescueMux);
  prescueLive.raw=raw;
  prescueLive.pressure=pressure;
  prescueLive.sensorValid=valid;
  prescueLive.updatedAt=now;
  if(valid){
    prescueRing[prescueHead]={now/1000.0f,pressure};
    prescueHead=(prescueHead+1)%PRESCUE_RING_CAP;
    if(prescueCount<PRESCUE_RING_CAP) ++prescueCount;
  }
  portEXIT_CRITICAL(&prescueMux);
}

void prescueHttpHeaders(){
  prescueServer.sendHeader("Access-Control-Allow-Origin","*");
  prescueServer.sendHeader("Access-Control-Allow-Methods","GET,POST,OPTIONS");
  prescueServer.sendHeader("Access-Control-Allow-Headers","Content-Type");
  prescueServer.sendHeader("Cache-Control","no-store");
}

void prescueHttpStatus(){
  PrescueLive t;
  size_t count;
  portENTER_CRITICAL(&prescueMux);
  t=prescueLive;
  count=prescueCount;
  portEXIT_CRITICAL(&prescueMux);
  bool fresh=t.updatedAt!=0 && (uint32_t)(millis()-t.updatedAt)<5000UL;
  bool valid=fresh && t.sensorValid;
  String s;
  s.reserve(560);
  s+="{\"connected\":true,\"connection_type\":\"wifi_ap\",\"pressure_mmHg\":";
  s+= valid ? String(t.pressure,3) : "null";
  s+=",\"adc_raw\":";
  s+= fresh ? String(t.raw,1) : "null";
  s+=",\"target_mmHg\":null,\"phase\":\"";
  s+=t.busy ? "SERIAL_TEST" : "IDLE";
  s+="\",\"test_mode\":\"";s+=String(t.command);s+="\"";
  s+=",\"pump_on\":";s+=t.pumpOn ? "true" : "false";
  s+=",\"valve_open\":";s+=t.valveOpen ? "true" : "false";
  s+=",\"sensor_ok\":";s+=valid ? "true" : "false";
  s+=",\"busy\":";s+=t.busy ? "true" : "false";
  s+=",\"sample_count\":";s+=String((unsigned)count);
  s+=",\"elapsed_seconds\":";s+=String(millis()/1000.0f,2);
  s+=",\"leak_rate_mmHg_min\":null,\"test_result\":null";
  s+=",\"remote_commands_enabled\":false,\"safety_ok\":false,\"can_pressurize\":false";
  s+=",\"dry_run_commands_enabled\":true";
  s+=",\"is_simulated\":false,\"api_mode\":\"monitor_dry_run\"";
  s+=",\"note\":\"Latest serial firmware with read-only WiFi. No remote actuation.\"}";
  prescueHttpHeaders();
  prescueServer.send(200,"application/json",s);
}

void prescueHttpSamples(){
  PrescuePoint snapshot[PRESCUE_RING_CAP];
  size_t n;
  portENTER_CRITICAL(&prescueMux);
  n=prescueCount;
  size_t start=(prescueHead+PRESCUE_RING_CAP-n)%PRESCUE_RING_CAP;
  for(size_t i=0;i<n;i++) snapshot[i]=prescueRing[(start+i)%PRESCUE_RING_CAP];
  portEXIT_CRITICAL(&prescueMux);
  String s="{\"samples\":[";
  s.reserve(9000);
  for(size_t i=0;i<n;i++){
    if(i)s+=",";
    s+="{\"phase\":\"LIVE\",\"elapsed_seconds\":";
    s+=String(snapshot[i].elapsed,3);
    s+=",\"pressure_mmHg\":";
    s+=String(snapshot[i].pressure,3);
    s+="}";
  }
  s+="]}";
  prescueHttpHeaders();
  prescueServer.send(200,"application/json",s);
}


// PRESCUE COMMAND DRY RUN V3
// No pump, valve, or Serial command is executed over Wi-Fi.
void prescueHttpCapabilities(){
  prescueHttpHeaders();
  prescueServer.send(200, "application/json",
    "{\"wifi_monitoring\":true,"
    "\"dry_run_commands_enabled\":true,"
    "\"remote_commands_enabled\":false,"
    "\"serial_commands_available\":true,"
    "\"reason\":\"DRY RUN only; physical actuation locked\"}");
}

void prescueHttpCommandDryRun(){
  String body = prescueServer.arg("plain");
  body.trim();

  if(body.length() > 80){
    prescueHttpHeaders();
    prescueServer.send(400, "application/json",
      "{\"accepted\":false,\"error\":\"Payload too large\"}");
    return;
  }

  String compact;
  compact.reserve(body.length());

  for(size_t i=0; i<body.length(); i++){
    char c=body.charAt(i);
    if(c!=' ' && c!='\n' && c!='\r' && c!='\t')
      compact += c;
  }

  const String prefix="{\"command\":\"";

  if(!compact.startsWith(prefix) || !compact.endsWith("\"}")){
    prescueHttpHeaders();
    prescueServer.send(400, "application/json",
      "{\"accepted\":false,\"error\":\"Invalid JSON command format\"}");
    return;
  }

  String cmd=compact.substring(prefix.length(),compact.length()-2);
  bool valid=(cmd=="O" || cmd=="X");

  if(!valid && cmd.length()>=4 &&
     (cmd[0]=='G' || cmd[0]=='L') && cmd[1]==':'){
    String number=cmd.substring(2);
    bool digits=number.length()>=2 && number.length()<=3;

    for(size_t i=0;i<number.length();i++){
      if(number[i]<'0' || number[i]>'9') digits=false;
    }

    int target=number.toInt();
    valid=digits && target>=20 && target<=150;
  }

  if(!valid){
    prescueHttpHeaders();
    prescueServer.send(422, "application/json",
      "{\"accepted\":false,\"error\":\"Unsupported command\"}");
    return;
  }

  // Acknowledgement only. NO physical execution.
  String response=
    "{\"accepted\":true,\"executed\":false,"
    "\"dry_run\":true,\"command\":\"" + cmd +
    "\",\"message\":\"Command validated; actuators NOT activated\"}";

  prescueHttpHeaders();
  prescueServer.send(200, "application/json", response);
}

void prescueWifiTask(void*){
  prescueServer.begin();
  for(;;){
    prescueServer.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
void prescueWifiStart(){
  if(strcmp(PRESCUE_AP_PASSWORD,"GANTI_PASSWORD_UNIK_DISINI")==0 ||
     strlen(PRESCUE_AP_PASSWORD)<8){
    Serial.println("# PRESCUE WiFi OFF: set a unique password in wifi_settings.h");
    return;
  }
  WiFi.mode(WIFI_AP);
  if(!WiFi.softAP(PRESCUE_AP_SSID,PRESCUE_AP_PASSWORD)){
    Serial.println("# PRESCUE WiFi AP failed");
    return;
  }
  prescueServer.on("/api/status",HTTP_GET,prescueHttpStatus);
  prescueServer.on("/api/samples",HTTP_GET,prescueHttpSamples);
  prescueServer.on("/api/capabilities",HTTP_GET,prescueHttpCapabilities);
  prescueServer.on("/api/command",HTTP_POST,prescueHttpCommandDryRun);
  prescueServer.on("/api/command",HTTP_OPTIONS,[]{prescueHttpHeaders();prescueServer.send(204,"text/plain","");});
  prescueServer.onNotFound([](){prescueHttpHeaders();prescueServer.send(404,"application/json","{\"error\":\"Not found\"}");});
  prescueWifiStarted=xTaskCreatePinnedToCore(
    prescueWifiTask,"prescue_wifi",8192,nullptr,1,nullptr,0)==pdPASS;
  if(prescueWifiStarted){
    Serial.printf("# WiFi AP SSID: %s IP: %s (MONITOR ONLY)\n",
       PRESCUE_AP_SSID,WiFi.softAPIP().toString().c_str());
  }else{
    WiFi.softAPdisconnect(true);
    Serial.println("# PRESCUE WiFi HTTP task failed");
  }
}

void prescueIdleSample(){
  static uint32_t last=0;
  uint32_t now=millis();
  if((uint32_t)(now-last)<800UL)return;
  last=now;
  float r,sd;
  bacaStat(r,sd);
}


void IRAM_ATTR isrTacho() { cacahPulsa++; }

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

void motor(uint8_t persen) { tulisPwm(PIN_MOTOR, KANAL_MOTOR, persen); prescuePump(persen > 0); }
void motorMati()           { motor(0); }

void katupBuka() {
  uint8_t d = DUTY_KATUP;
  if (d > DUTY_KATUP_MAKS) d = DUTY_KATUP_MAKS;
  tulisPwm(PIN_KATUP, KANAL_KATUP, d);
  prescueValve(true);
}
void katupTutup() { tulisPwm(PIN_KATUP, KANAL_KATUP, 0); prescueValve(false); }
void semuaMati()  { motorMati(); katupTutup(); }

/* ================= SENSOR MPX ================= */
float bacaCepat() {
  uint32_t j = 0;
  for (uint8_t i = 0; i < 8; i++) { j += analogRead(PIN_SENSOR); delayMicroseconds(150); }
  float raw = (float)j / 8.0f;
  prescueNoteRaw(raw, -1.0f);
  return raw;
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
  prescueNoteRaw(rata, simpangan);
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
  float p = mmHgMudah();
  if (p > BATAS_KERAS) {
    motorMati();
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
      delay(8);
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
  while (millis() - mulai < 120000UL) {
    if (lewatBatas() || cekBatal()) { akhirOtomatis(); return; }
    if (millis() - sampel >= 200) {
      sampel = millis();
      Serial.printf("L,SETTLE,%.3f,%.3f\n", (sampel - mulai) / 1000.0f, keMmHg(bacaCepat()));
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
      Serial.printf("L,RECORD,%.3f,%.3f\n", t, akhir);
      if (kini - mulai >= 60000UL) break;
    }
    delay(2);
  }
  double bocor = -60.0 * sTP / sTT;
  Serial.printf("# L awal=%.3f akhir=%.3f n=%lu bocor bertanda=%.4f mmHg/menit: %s (<=5)\n",
                awal, akhir, n, bocor, bocor <= 5.0 ? "LOLOS" : "GAGAL");
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

void rekamCepat(float detik) {
  if (detik <= 0 || detik > 60) detik = 40;
  Serial.println("# Rekam cepat 100 Hz, 10 bacaan per baris. Ketik huruf apa saja lalu Enter untuk berhenti.");
  Serial.println("W,ms_awal,10 bacaan T_mmHg berjarak 10 ms");
  uint32_t t0 = millis(), berikut = t0, awalBaris = 0;
  float buf[10];
  uint8_t n = 0;
  while (millis() - t0 < (uint32_t)(detik * 1000)) {
    if (Serial.available()) break;
    uint32_t kini = millis();
    if ((int32_t)(kini - berikut) < 0) continue;
    berikut += 10;                                   // satu bacaan setiap 10 ms
    uint32_t j = 0;
    for (uint8_t i = 0; i < 4; i++) j += analogRead(PIN_SENSOR);
    if (n == 0) awalBaris = kini - t0;
    buf[n++] = keMmHg(j / 4.0f);
    if (n == 10) {                                   // satu baris setiap 100 ms
      Serial.printf("W,%lu", (unsigned long)awalBaris);
      for (uint8_t i = 0; i < 10; i++) Serial.printf(",%.1f", buf[i]);
      Serial.println();
      n = 0;
      if (lewatBatas()) return;                      // pengaman tiap 100 ms
    }
  }
  while (Serial.available()) Serial.read();
  Serial.println("# Rekam cepat selesai.");
}

// Q:ms  dorong pompa sesaat di tekanan sekarang, rekam 100 Hz
void ujiDorong(float ms) {
  if (ms < 5 || ms > 200) ms = 40;
  Serial.printf("# Q:%.0f mulai, T=%.2f mmHg\n", ms, keMmHg(bacaCepat()));
  const int N = 150;                 // 1,5 detik
  float buf[N];
  uint32_t t0 = millis(), berikut = t0;
  bool nyala = false, sudah = false, hentak = false ;
  for (int k = 0; k < N; k++) {
    while ((int32_t)(millis() - berikut) < 0) {
      uint32_t t = millis() - t0;
      if (!sudah && !nyala && t >= 300) { motor(100); nyala = true; hentak = true; }
      if (nyala && hentak && t >= 320) { motor(80); hentak = false; }
      if (nyala && t >= 300 + (uint32_t)ms) { motorMati(); nyala = false; sudah = true; }
    }
    berikut += 10;
    uint32_t j = 0;
    for (uint8_t i = 0; i < 4; i++) j += analogRead(PIN_SENSOR);
    buf[k] = keMmHg(j / 4.0f);
    if (lewatBatas() || cekBatal()) { motorMati(); return; }
  }
  motorMati();
  float awal = 0, akhir = 0, puncak = -1e9;
  for (int k = 0; k < 25; k++) awal += buf[k];
  awal /= 25;
  for (int k = 30; k < 80; k++) puncak = fmaxf(puncak, buf[k]);
  for (int k = N - 30; k < N; k++) akhir += buf[k];
  akhir /= 30;
  Serial.printf("# Q dorong %.0f ms: awal=%.2f puncak=%.2f (+%.2f) akhir=%.2f (+%.2f)\n",
                ms, awal, puncak, puncak - awal, akhir, akhir - awal);
}

/* ================= RANGKAIAN OTOMATIS U ================= */
// Untuk setiap target: isi G dengan duty 80, lalu mulai Q:20/40/60/80
// pada detik 60, 65, 70, 75 dihitung dari saat target tersebut tercapai.
const float  UJI_AUTO_TARGET[]  = {65, 105, 145};
const uint16_t UJI_AUTO_Q_MS[]  = {20, 40, 60, 80};
const uint8_t UJI_AUTO_Q_JML    = sizeof(UJI_AUTO_Q_MS) / sizeof(UJI_AUTO_Q_MS[0]);

bool tungguHingga(uint32_t acuan, uint32_t targetMs) {
  uint32_t log = millis();
  for (;;) {
    uint32_t lalu = millis() - acuan;
    if (lalu >= targetMs) return true;
    if (lewatBatas() || cekBatal()) return false;
    uint32_t kini = millis();
    if (kini - log >= 10000UL) {
      log = kini;
      Serial.printf("# U tunggu %.0f/%.0f s, T=%.2f mmHg\n",
                    lalu / 1000.0f, targetMs / 1000.0f, keMmHg(bacaCepat()));
    }
    delay(5);
  }
}

void rangkaianUjiOtomatis(int duty) {
  if (!dutyValid(duty)) duty = UJI_AUTO_DUTY;
  const uint8_t jmlTarget = sizeof(UJI_AUTO_TARGET) / sizeof(UJI_AUTO_TARGET[0]);
  Serial.printf("\n# ===== RANGKAIAN OTOMATIS: %u target, duty G %d persen =====\n",
                (unsigned)jmlTarget, duty);
  Serial.printf("# Setiap target: G lalu Q:%u,%u,%u,%u pada detik %.0f, %.0f, %.0f, %.0f setelah target.\n",
                (unsigned)UJI_AUTO_Q_MS[0], (unsigned)UJI_AUTO_Q_MS[1],
                (unsigned)UJI_AUTO_Q_MS[2], (unsigned)UJI_AUTO_Q_MS[3],
                UJI_AUTO_JEDA_MS / 1000.0f,
                (UJI_AUTO_JEDA_MS + UJI_AUTO_Q_JARAK_MS) / 1000.0f,
                (UJI_AUTO_JEDA_MS + 2 * UJI_AUTO_Q_JARAK_MS) / 1000.0f,
                (UJI_AUTO_JEDA_MS + 3 * UJI_AUTO_Q_JARAK_MS) / 1000.0f);

  for (uint8_t i = 0; i < jmlTarget; i++) {
    float target = UJI_AUTO_TARGET[i];
    Serial.printf("\n# ---- TARGET %u/%u : G:%.0f,%d ----\n", i + 1, jmlTarget, target, duty);
    if (!isiSampai(target, duty)) {
      Serial.println("# Rangkaian dihentikan: pengisian G gagal.");
      akhirOtomatis();
      return;
    }
    uint32_t acuan = millis();
    Serial.printf("# Target %.0f tercapai; Q pertama pada detik %.0f.\n",
                  target, UJI_AUTO_JEDA_MS / 1000.0f);
    bool lanjut = true;
    for (uint8_t q = 0; q < UJI_AUTO_Q_JML; q++) {
      uint32_t jadwal = UJI_AUTO_JEDA_MS + (uint32_t)q * UJI_AUTO_Q_JARAK_MS;
      if (!tungguHingga(acuan, jadwal)) { lanjut = false; break; }
      Serial.printf("# ---- Q:%u (target %.0f, detik %.0f) ----\n",
                    (unsigned)UJI_AUTO_Q_MS[q], target, jadwal / 1000.0f);
      ujiDorong((float)UJI_AUTO_Q_MS[q]);
      if (dibatalkan) { lanjut = false; break; }
    }
    if (!lanjut) {
      Serial.println("# Rangkaian dihentikan pada tahap Q.");
      break;
    }
  }

  akhirOtomatis();
  if (dibatalkan) Serial.println("# Rangkaian otomatis berhenti karena batal/darurat.");
  else Serial.println("# Rangkaian otomatis selesai. Buang tekanan sisa dengan O secara aman.");
}

const float Q_P[3] = {62, 100, 137};
const float Q_MS[4] = {20, 40, 60, 80};
const float Q_PUNCAK[3][4] = {
  {2.97f, 4.16f, 5.25f, 6.40f},
  {2.78f, 3.84f, 5.38f, 6.78f},
  {1.94f, 3.98f, 4.25f, 5.21f}
};
const float AMP_MAKS = 5.0f;
const float A_MIN = 0.35f;
const float Z_SYS = 0.811f;
const float Z_DIA = 0.611f;

float puncakTabel(float ms, float p) {
  p = constrain(p, Q_P[0], Q_P[2]);
  ms = constrain(ms, Q_MS[0], Q_MS[3]);
  uint8_t i = 0, j = 0;
  while (i < 1 && p > Q_P[i + 1]) ++i;
  while (j < 2 && ms > Q_MS[j + 1]) ++j;
  float rp = (p - Q_P[i]) / (Q_P[i + 1] - Q_P[i]);
  float rm = (ms - Q_MS[j]) / (Q_MS[j + 1] - Q_MS[j]);
  float bawah = Q_PUNCAK[i][j] + rm * (Q_PUNCAK[i][j + 1] - Q_PUNCAK[i][j]);
  float atas = Q_PUNCAK[i + 1][j] + rm * (Q_PUNCAK[i + 1][j + 1] - Q_PUNCAK[i + 1][j]);
  return bawah + rp * (atas - bawah);
}

int msUntuk(float puncak, float p) {
  for (int ms = 20; ms <= 80; ms++) {
    if (puncakTabel(ms, p) >= puncak) return ms;
  }
  return 80;
}

// N:1 = 120/80, N:2 = 150/100, N:3 = 90/60, denyut 75 per menit, rekam 100 Hz
void modeDenyut(int preset) {
  float SYS = 120, DIA = 80;
  if (preset == 2) { SYS = 150; DIA = 100; }
  else if (preset == 3) { SYS = 90; DIA = 60; }
  const float HR = 75;
  float MAP = DIA + (SYS - DIA) / 3.0f;
  float sAtas = (SYS - MAP) / Z_SYS;
  float sBawah = (MAP - DIA) / Z_DIA;
  uint32_t jarak = (uint32_t)(60000.0f / HR);
  semuaMati();
  Serial.printf("# Mode denyut %.0f/%.0f, HR %.0f, MAP %.1f. Tekan START di Omron. x untuk berhenti.\n",
                SYS, DIA, HR, MAP);
  Serial.printf("# Puncak target maksimum %.1f mmHg pada MAP %.1f mmHg.\n", AMP_MAKS, MAP);
  Serial.println("# Z,ms_awal,bacaan 10 ms | N,detik,T,rasio,puncak_target,ms");
  uint32_t t0 = millis(), berikutDenyut = t0, berikutSampel = t0, awalBaris = 0, totalNyala = 0;
  float buf[10]; uint8_t n = 0;
  float pTerakhir = 0;
  bool pernahTinggi = false;

  auto cetakBuf = [&]() {
    if (n == 0) return;
    Serial.printf("Z,%lu", (unsigned long)awalBaris);
    for (uint8_t i = 0; i < n; i++) Serial.printf(",%.1f", buf[i]);
    Serial.println();
    n = 0;
  };

  while (millis() - t0 < 120000UL) {
    uint32_t kini = millis();
    if ((int32_t)(kini - berikutSampel) >= 0) {          // sampel 100 Hz
      berikutSampel += 10;
      uint32_t j = 0;
      for (uint8_t i = 0; i < 4; i++) j += analogRead(PIN_SENSOR);
      pTerakhir = keMmHg(j / 4.0f);
      if (n == 0) awalBaris = kini - t0;
      buf[n++] = pTerakhir;
      if (n == 10) {
        cetakBuf();
        if (lewatBatas() || cekBatal()) break;
      }
    }
    if ((int32_t)(kini - berikutDenyut) >= 0) {          // satu denyut
      berikutDenyut += jarak;
      float p = pTerakhir;
      if (p > 60) pernahTinggi = true;
      if (pernahTinggi && p < 20) { cetakBuf(); Serial.println("# Omron selesai membuang."); break; }
      float z = (p - MAP) / (p >= MAP ? sAtas : sBawah);
      float a = expf(-0.5f * z * z);
      float target = 0; int ms = 0;
      if (p >= 20 && a >= A_MIN) {
        target = a * AMP_MAKS;
        ms = msUntuk(target, p);
        cetakBuf();
        if (lewatBatas() || cekBatal()) break;
        motor(100); delay(20);                    // hentakan awal
        if (ms > 20) { motor(80); delay(ms - 20); }
        motorMati();
        totalNyala += ms;
        berikutSampel = millis();
      }
      Serial.printf("N,%.2f,%.2f,%.2f,%.2f,%d\n", (millis() - t0) / 1000.0f, p, a, target, ms);
    }
  }
  motorMati();
  cetakBuf();
  Serial.printf("# Mode denyut selesai, total pompa menyala %.1f s\n", totalNyala / 1000.0f);
  akhirOtomatis();
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
  Serial.println("# G/L/E/F memakai T; pengaman independen M>300, pelepasan M<20 / 15s.");
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
      Serial.printf("T,%.3f\n", keMmHg(bacaCepat()));
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

// Baca tekanan dengan pompa dijeda sebentar, supaya bebas gangguan listrik pompa
float bacaJeda(uint16_t jedaMs = 80) {
  motorMati();
  delay(jedaMs);
  float r, sd; bacaStat(r, sd);
  return keMmHg(r);
}

bool isiSampai(float target, int duty) {
  semuaMati();
  if (!isfinite(target) || target < 20 || target > 280 || duty < 1 || duty > 100) {
    Serial.println("# target 20..280, duty 1..100"); return false;
  }
  dutyIsi = duty;
  uint32_t t0 = millis(), cek = t0, log = t0;
  bool berhasil = false, melewati20 = false;
  float patokan = 0;
  const char *hasil = "batas 90 detik";
  Serial.println("G,fase,detik,T_mmHg");
  if (lewatBatas() || cekBatal()) { akhirOtomatis(); return false; }
  float r0, sd0; bacaStat(r0, sd0);                 // pompa masih mati, baca rata rata 64 sampel
  float p = keMmHg(r0);
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
    uint16_t lamaNyala = (target - p > 20) ? 300 : 80;  // BARU: lama pompa menyala per dorongan
    motor(duty);
    uint32_t nyala = millis();                           // BARU
    while (millis() - t0 < 90000UL) {
      if (lewatBatas() || cekBatal()) { hasil = "darurat/batal"; break; }
      if (millis() - nyala < lamaNyala) { delay(2); continue; }  // BARU: biarkan pompa jalan dulu
      p = bacaJeda();                                             // BARU: pompa dijeda, lalu baca
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
        Serial.printf("G,FILL,%.3f,%.3f\n", (kini - t0) / 1000.0f, p);
      }
      lamaNyala = (target - p > 20) ? 300 : 80;  // BARU: dekat target, dorongan dipendekkan
      motor(duty);                               // BARU: lanjut memompa
      nyala = millis();
    }
  }
  motorMati();
  Serial.printf("# G %s; durasi %.3fs, T=%.3f\n", hasil, (millis() - t0) / 1000.0f, p);
  akhirOtomatis();
  return berhasil;
}

void ujiPertahankan(float target, float durasi, int duty) {
  if (!siapPadaTarget(target, duty)) return;
  Serial.println("E,detik,T_mmHg,jumlah_tambah,toleransi_2mmHg");
  uint32_t mulai = millis(), log = mulai;
  unsigned long jumlah = 0;
  bool pertama = true, gagal = false;
  float p = target;
  while (millis() - mulai < (uint32_t)(durasi * 1000)) {
    if (lewatBatas() || cekBatal()) break;
    float r, sd; bacaStat(r, sd);
    p = keMmHg(r);                              // pompa mati, bacaan bersih
    if (p <= target - 1) {                      // tekanan turun, perlu ditambah
      ++jumlah;
      uint32_t tambahMulai = millis();
      while (p < target) {
        if (lewatBatas() || cekBatal()) break;
        if (millis() - tambahMulai >= 30000UL) { gagal = true; break; }
        motor(duty);
        delay(60);                              // dorongan pendek
        p = bacaJeda();                         // pompa dijeda, lalu baca
      }
      motorMati();
      if (gagal) { Serial.println("# E gagal: 30 detik menambah tanpa mencapai target"); break; }
      if (dibatalkan) break;
    }
    uint32_t kini = millis();
    if (pertama || kini - log >= 1000UL) {
      pertama = false; log = kini;
      Serial.printf("E,%.3f,%.3f,%lu,%s\n", (kini - mulai) / 1000.0f, p, jumlah,
                    fabsf(p - target) <= 2 ? "OK" : "DI_LUAR");
    }
    delay(20);
  }
  Serial.printf("# E selesai, durasi %.3fs, jumlah penambahan=%lu\n",
                (millis() - mulai) / 1000.0f, jumlah);
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
  if (!tungguAman(2000)) { akhirOtomatis(); return; }  // tunggu lonjakan setelah pompa mati reda
  float r, sd; bacaStat(r, sd);                         // rata rata 64 sampel, lebih stabil
  float awal = keMmHg(r);
  if (awal > awalTarget + 8) {
    Serial.println("# F ditolak: tekanan awal > start+8."); akhirOtomatis(); return;
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
      int dipakaiUs = 0;
      if (galat > 0) {
        dipakaiUs = constrain((int)(1500 + 1500 * galat), 2000, 15000);  // 2 sampai 15 ms
        katupBuka();
        delayMicroseconds(dipakaiUs);   // panjang pulsa tepat, tidak bergantung loop
        katupTutup();
      }
      Serial.printf("F,%.3f,%.3f,%.3f,%.1f\n", t, p, lintasan, dipakaiUs / 1000.0f);
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
  Serial.println("# L:150[,duty] isi G, stabilisasi 2s, koreksi katup bila >target+2, settle 120s, rekam T 60s/200ms");
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
  Serial.println("# M        tekanan dengan kalibrasi pabrik ESP32");
  Serial.println("# S        mode pantau berkelanjutan");
  Serial.println("# W:60     rekam cepat 100 Hz selama 60 detik");
  Serial.println("# N:1/2/3  denyut 120/80, 150/100, 90/60; HR 75, puncak pada MAP, rekam 100 Hz");
  Serial.println("# Q        dorong pompa 40 ms, rekam 100 Hz 1,5 detik");
  Serial.println("# Q:ms     dorong pompa sepanjang ms (5..200), lalu cetak awal/puncak/akhir");
  Serial.println("# U[:duty] rangkaian otomatis G:65, G:105, G:145 duty 80");
  Serial.println("# U        tiap target: tunggu 60s, lalu Q:20, Q:40, Q:60, Q:80 tiap 5s");
  Serial.println("# G:150,60 isi otomatis sampai 150 mmHg dengan duty 60, lalu pompa mati");
  Serial.println("# G:150    sama, memakai duty terakhir; T terkalibrasi, maksimum 90s, log 2s");
  Serial.println("# G: harus >20 dalam 30s, kemudian naik >=2 setiap 5s. M>300 selalu prioritas darurat.");
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
  prescueWifiStart();
  Serial.println("\n# UJI POMPA, KATUP, DAN SENSOR MPX DENGAN L298N - Kelompok 18");
  Serial.printf("# pin pompa=%d  pin katup=%d  pin sensor=%d\n", PIN_MOTOR, PIN_KATUP, PIN_SENSOR);
  bantuan();
}

void loop() {
  if (!Serial.available()) { prescueIdleSample(); delay(10); return; }
  String b = Serial.readStringUntil('\n'); b.trim();
  if (b.length() == 0) return;
  char c = toupper(b.charAt(0));
  PrescueSerialBusyGuard prescueBusyGuard(c);
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
    valid = valid && a[0] >= 20 && a[0] <= 280;
    if (!valid) { Serial.println("# Argumen tidak valid. Lihat ?; angka lengkap, target 20..280, duty bulat 1..100."); return; }
    if (c == 'G') isiSampai(a[0], duty);
    else if (c == 'L') ujiTahan(a[0], duty);
    else if (c == 'E') ujiPertahankan(a[0], durasi, duty);
    else ujiTurun(a[0], a[1], laju, duty);
  }
  else if (c == 'O') bukaKatupManual(p > 0 ? (int)arg : 0);
  else if (c == 'R') bacaSekali();
  else if (c == 'T') bacaTekanan();
  else if (c == 'M') tekananMudah();
  else if (c == 'S') pantau();
  else if (c == 'W') rekamCepat(p > 0 ? arg : 60);
  else if (c == 'Q') ujiDorong(p > 0 ? arg : 40);
  else if (c == 'U') {
    float a[1]; int n;
    int duty = UJI_AUTO_DUTY;
    if (p > 0) {
      if (!bacaArgumen(b, a, 1, 1, n) || !dutyValid(a[0])) {
        Serial.printf("# Format U atau U:duty, duty bulat 1..100.\n"); return;
      }
      duty = (int)a[0];
    }
    rangkaianUjiOtomatis(duty);
  }
  else if (c == 'N') modeDenyut( p > 0 ? (int)arg : 1 );
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