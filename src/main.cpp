/*
  ESP32 — Niveau d'eau + PIR + Relais (électrovanne/pompe) + OLED + Web en temps réel
  - SIMULATION : capteurs HC-SR04 & SR602 simulés
  - RÉEL       : lecture capteurs
  - Web        : / (page HTML), /events (SSE), /status (JSON)
  - OLED       : RSSI (≤15 px), niveau d'eau (%)
  Librairies : Wire, Adafruit_SSD1306, WiFi, AsyncTCP, ESPAsyncWebServer
*/
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AHTX0.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <EEPROM.h>
#include <time.h>
#include "icons.h"

// ===================== Configuration générale =====================
#define SIMULATION false          // true = simulateur; false = capteurs réels

// === États de la fontaine ===
bool fountainRunning = false;  // Marche/Arrêt global (défaut: OFF)

// === Option: Source d'eau ===
enum WaterSource { SRC_EXTERNAL, SRC_INTERNAL, SRC_AUTO };
WaterSource waterSource = SRC_AUTO;  // Défaut: auto

// === Option: Écoulement ===
enum FlowMode { FLOW_PIR, FLOW_CONTINUOUS };
FlowMode flowMode = FLOW_CONTINUOUS;  // Défaut: continu

// === Option: Vidange ===
enum DrainMode { DRAIN_NEVER, DRAIN_PERIODIC, DRAIN_AT_LEVEL };
DrainMode drainMode = DRAIN_NEVER;  // Défaut: jamais

// Paramètres vidange périodique
enum DrainScheduleType { DRAIN_DAILY, DRAIN_SPECIFIC_DAYS, DRAIN_EVERY_X_HOURS };
DrainScheduleType drainScheduleType = DRAIN_DAILY;
uint8_t drainHour = 3;      // Heure de vidange (0-23)
uint8_t drainMinute = 0;    // Minute de vidange (0-59)
uint8_t drainDays = 0b1111111;  // Bitmask jours (bit0=Lun...bit6=Dim)
uint16_t drainEveryHours = 24;  // Toutes les X heures (1-720)
uint32_t lastDrainTimestamp = 0;  // Epoch dernière vidange

// Paramètres vidange au niveau
uint8_t drainAtLevelPct = 95;  // Vidanger quand niveau atteint X%

// === Calibration des seuils ===
float calibZeroCm = 0.0;    // Distance capteur quand cuve vide (0%)
float calibFullCm = 0.0;    // Distance capteur quand cuve pleine (100%)
bool calibrationDone = false;

// Seuils de fonctionnement (en %)
uint8_t thresholdMin = 25;   // Seuil bas (arrêt pompe si interne)
uint8_t thresholdMax = 90;   // Seuil haut (arrêt remplissage si externe)

// === État vidange en cours ===
bool drainInProgress = false;
bool manualDrainActive = false;

// ===================== EEPROM =====================
#define EEPROM_SIZE 64
#define EEPROM_MAGIC_ADDR       0
#define EEPROM_MAGIC_VALUE      0xA7
#define EEPROM_RUNNING          1   // 1 byte
#define EEPROM_WATER_SOURCE     2   // 1 byte
#define EEPROM_FLOW_MODE        3   // 1 byte
#define EEPROM_DRAIN_MODE       4   // 1 byte
#define EEPROM_DRAIN_SCHEDULE   5   // 1 byte
#define EEPROM_DRAIN_HOUR       6   // 1 byte
#define EEPROM_DRAIN_MINUTE     7   // 1 byte
#define EEPROM_DRAIN_DAYS       8   // 1 byte
#define EEPROM_DRAIN_EVERY_H    9   // 2 bytes (9-10)
#define EEPROM_DRAIN_LEVEL      11  // 1 byte
#define EEPROM_THRESHOLD_MIN    12  // 1 byte
#define EEPROM_THRESHOLD_MAX    13  // 1 byte
#define EEPROM_CALIB_ZERO       14  // 4 bytes (14-17)
#define EEPROM_CALIB_FULL       18  // 4 bytes (18-21)
#define EEPROM_CALIB_DONE       22  // 1 byte
#define EEPROM_LAST_DRAIN       23  // 4 bytes (23-26)


// ---- WiFi ----
const char* WIFI_SSID = "Nian_nian";
const char* WIFI_PASS = "M@rieK3v";
const char* WIFI_HOSTNAME = "Fontaine";
/* IPAddress ip(192, 168, 1, 40); // Adresse IP de la carte
IPAddress gateway (192,168,1,1);
IPAddress subnet  (255,255,255,0);
IPAddress dns1    (192,168,1,1);     // ou 8.8.8.8 / 1.1.1.1
IPAddress dns2    (1,1,1,1); */

// ---- NTP ----
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 3600;        // GMT+1 (France hiver)
const int   DAYLIGHT_OFFSET_SEC = 3600;   // +1h si heure d'été


// === Google Web App ===
const char* GSCRIPT_URL   = "https://script.google.com/macros/s/AKfycbyBtQMShESVRcuFGqsJnEIWSeQ_uYQmR2UhtKyw1khbzB0H2wi5ZUAXzZn7pZnJUOdY7g/exec";
const char* GSCRIPT_TOKEN = "m4rwE7J8XWax57RNmXNsfDsK7BKpbwZC";

// test : https://script.google.com/macros/s/AKfycbwiqfQCXPvX9JzyxIAHO4fmqVBcLC4NWA5T5LPTZtkg5vdU74_oyXNl_0p9z426bFcBuw/exec?token=m4rwE7J8XWax57RNmXNsfDsK7BKpbwZC&payload=%7B%7D

// Cadence d’envoi vers Sheets
const uint32_t SHEET_INTERVAL_MS = 60UL * 15000UL; // 15 minute (ajuste)
unsigned long lastSheetMs = 0;

// ---- OLED SSD1306 (SDA=21, SCL=22) ----
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR   0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_AHTX0 aht;

// ---- Brochage (adapter selon votre câblage réel) ----
const int PIN_ECHO  = 12;   // HC-SR04 ECHO (si réel)
const int PIN_TRIG  = 13;   // HC-SR04 TRIG (si réel)
const int PIN_PIR   = 14;   // SR602 (si réel)
const int PIN_VALVE = 18;   // Relais EV_out
const int PIN_PUMP  = 19;   // Relais pompe + ancienne EV
const bool ACTIVE_LOW = true; // true si relais actifs à LOW
const int PIN_EV1_AIN1 = 33; // EV1
const int PIN_EV1_AIN2 = 32;
const int PIN_EV1_PWMA = 23;
/* 
const int PIN_EV2_BIN1 = 25; //EV2
const int PIN_EV2_BIN2 = 26;
const int PIN_EV2_PWMB = 27; 
*/

const int EV_PULSE_MS = 50; // durée impulsion électrovanne bistable

// ---- Cuve & seuils ----
const float TANK_HEIGHT_CM   = 9.1; // hauteur utile d'eau
const float SENSOR_OFFSET_CM = 2.0;  // distance min capteur->surface pleine

const int LEVEL_TARGET_FILL  = 90; // % à atteindre quand remplissage autorisé
const int PUMP_ON_ABOVE      = 85; // % déclenche pompe au-dessus
const int PUMP_OFF_BELOW     = 25; // % arrêt pompe en redescendant
const int MOTION_HOLD_SECONDS= 3; // s d'autorisation après détection

// ---- Simulation ----
const float SIM_FILL_RATE_PCT_S  = 2.0; // %/s si vanne ouverte
const float SIM_DRAIN_RATE_PCT_S = 5.0; // %/s si pompe ON
const float SIM_LEAK_RATE_PCT_S  = 0.001;  // fuite naturelle
const bool  SIM_FAKE_PIR_BURSTS  = true; // bursts de PIR

// ---- Intervalles non bloquants ----
const uint32_t LOGIC_INTERVAL_MS = 50;
const uint32_t OLED_INTERVAL_MS  = 250;
const uint32_t SSE_INTERVAL_MS   = 3000;


// ===================== Variables d'état =====================
unsigned long lastLogicMs = 0, lastOledMs = 0, lastSseMs = 0;

float levelPct = 10.0f; // simulé; en mode réel remplacé par la mesure
float distanceCm = 0.0f;
float temperatureC = 0.0f;
float humidityPct = 0.0f;
bool ahtOk = false;

bool valveOn = false;
bool pumpOn  = false;
bool VoutOn = false;

unsigned long fillAllowedUntilMs = 0; // fenêtre d'autorisation de remplissage
unsigned long pirSimUntilMs      = 0; // fenêtre d'un burst PIR simulé
bool pirState = false;                // état instantané du PIR (affichage)

unsigned long lastPirDetectMs = 0; // dernière détection PIR (instant)
unsigned long lastValveOnMs   = 0; // dernier passage vanne -> ON
unsigned long lastPumpOnMs    = 0; // dernier passage pompe -> ON

// ===================== Web server (Async) =====================
AsyncWebServer server(80);
AsyncEventSource events("/events");

// Page web minimaliste (SSE) — tout-en-un
static const char index_html[] PROGMEM = R"HTML(
<!doctype html><html lang="fr"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Fontaine</title>
<style>
:root{font:14px system-ui,Segoe UI,Roboto,Ubuntu,Arial}
body{margin:0;background:#0b1220;color:#e8eefc}
header{padding:12px 16px;background:#0f172a;position:sticky;top:0;display:flex;justify-content:space-between;align-items:center}
main{padding:16px;max-width:900px;margin:auto}
.grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
.card{background:#111827;border:1px solid #1f2937;border-radius:12px;padding:14px}
.card h3{margin:0 0 12px;font-size:14px;opacity:.9}
.title{opacity:.8;font-size:12px;margin-bottom:6px}
.big{font-size:36px;margin:4px 0 10px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:6px 0}
.pill{padding:3px 8px;border-radius:999px;background:#1f2937;font-size:12px}
code{background:#0a0f1a;padding:2px 6px;border-radius:6px}
.btn{padding:8px 16px;border:none;border-radius:6px;cursor:pointer;font-size:13px;font-weight:500}
.btn-sm{padding:4px 10px;font-size:12px}
.btn-primary{background:#3b82f6;color:#fff}
.btn-primary:hover{background:#2563eb}
.btn-success{background:#10b981;color:#fff}
.btn-success:hover{background:#059669}
.btn-danger{background:#dc2626;color:#fff}
.btn-danger:hover{background:#b91c1c}
.btn-secondary{background:#4b5563;color:#fff}
.btn-secondary:hover{background:#374151}
.btn.active{background:#10b981!important;color:#000}
.btn-group{display:flex;gap:4px}
.btn-group .btn{border-radius:0}
.btn-group .btn:first-child{border-radius:6px 0 0 6px}
.btn-group .btn:last-child{border-radius:0 6px 6px 0}
select,input[type=number],input[type=time]{background:#1f2937;color:#fff;border:1px solid #374151;padding:6px 10px;border-radius:6px;font-size:13px}
input[type=number]{width:60px}
.toggle{position:relative;width:50px;height:26px;display:inline-block}
.toggle input{opacity:0;width:0;height:0}
.toggle .slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#4b5563;border-radius:26px;transition:.3s}
.toggle input:checked+.slider{background:#10b981}
.toggle .slider:before{content:"";position:absolute;height:20px;width:20px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.3s}
.toggle input:checked+.slider:before{transform:translateX(24px)}
.power-btn{width:60px;height:60px;border-radius:50%;font-size:24px;display:flex;align-items:center;justify-content:center}
.power-btn.off{background:#dc2626}
.power-btn.on{background:#10b981}
.sub-options{margin-left:20px;padding:10px;background:#0a0f1a;border-radius:8px;margin-top:8px}
.hidden{display:none!important}
.status-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;text-align:center}
.status-item{background:#1f2937;padding:8px;border-radius:8px}
.status-item .val{font-size:18px;font-weight:bold}
.status-item .lbl{font-size:11px;opacity:.7}
.presets{display:flex;gap:6px;flex-wrap:wrap;margin-top:12px}
label{display:flex;align-items:center;gap:8px;cursor:pointer}
.checkbox-days{display:flex;gap:4px;flex-wrap:wrap}
.checkbox-days label{background:#1f2937;padding:4px 8px;border-radius:4px;font-size:12px}
.checkbox-days input:checked+span{color:#10b981;font-weight:bold}
</style>
</head>
<body>
<header>
  <strong>Fontaine</strong>
  <div class="row">
    <span id="clock">--:--</span>
    <button id="powerBtn" class="btn power-btn off" onclick="togglePower()">⏻</button>
  </div>
</header>

<main>
  <!-- Status -->
  <div class="card" style="margin-bottom:12px">
    <div class="status-grid">
      <div class="status-item"><div class="val" id="level">--</div><div class="lbl">Niveau %</div></div>
      <div class="status-item"><div class="val" id="temp">--</div><div class="lbl">Temp °C</div></div>
      <div class="status-item"><div class="val" id="hum">--</div><div class="lbl">Humidité %</div></div>
      <div class="status-item"><div class="val" id="ev1">--</div><div class="lbl">EV1</div></div>
      <div class="status-item"><div class="val" id="pump">--</div><div class="lbl">Pompe</div></div>
      <div class="status-item"><div class="val" id="vout">--</div><div class="lbl">Évac.</div></div>
    </div>
  </div>

  <div class="grid">
    <!-- Source d'eau -->
    <div class="card">
      <h3>💧 Source d'eau</h3>
      <div class="btn-group">
        <button class="btn" data-src="0" onclick="setSource(0)">Externe</button>
        <button class="btn" data-src="1" onclick="setSource(1)">Interne</button>
        <button class="btn" data-src="2" onclick="setSource(2)">Auto</button>
      </div>
      <div class="sub-options" id="srcAutoOpts">
        <div class="row">
          <span>Seuils :</span>
          <input type="number" id="threshMin" min="5" max="50" value="25"> % min
          <input type="number" id="threshMax" min="50" max="100" value="90"> % max
          <button class="btn btn-sm btn-secondary" onclick="setThresholds()">OK</button>
        </div>
      </div>
    </div>

    <!-- Écoulement -->
    <div class="card">
      <h3>🌊 Écoulement</h3>
      <div class="btn-group">
        <button class="btn" data-flow="0" onclick="setFlow(0)">PIR</button>
        <button class="btn" data-flow="1" onclick="setFlow(1)">Continu</button>
      </div>
      <div class="row" style="margin-top:8px">
        <span class="pill">PIR: <span id="pirStatus">--</span></span>
        <span class="pill">Depuis: <span id="sincePir">--</span></span>
      </div>
    </div>

    <!-- Vidange -->
    <div class="card">
      <h3>🚿 Vidange</h3>
      <div class="btn-group">
        <button class="btn" data-drain="0" onclick="setDrain(0)">Jamais</button>
        <button class="btn" data-drain="1" onclick="setDrain(1)">Périodique</button>
        <button class="btn" data-drain="2" onclick="setDrain(2)">Au niveau</button>
      </div>
      
      <!-- Options périodique -->
      <div class="sub-options hidden" id="drainPeriodicOpts">
        <div class="row">
          <select id="drainSchedule" onchange="updateDrainUI()">
            <option value="0">Tous les jours à</option>
            <option value="1">Certains jours à</option>
            <option value="2">Toutes les X heures</option>
          </select>
        </div>
        <div class="row" id="drainTimeRow">
          <input type="time" id="drainTime" value="03:00">
        </div>
        <div class="row hidden" id="drainDaysRow">
          <div class="checkbox-days">
            <label><input type="checkbox" name="day" value="0" checked><span>Lun</span></label>
            <label><input type="checkbox" name="day" value="1" checked><span>Mar</span></label>
            <label><input type="checkbox" name="day" value="2" checked><span>Mer</span></label>
            <label><input type="checkbox" name="day" value="3" checked><span>Jeu</span></label>
            <label><input type="checkbox" name="day" value="4" checked><span>Ven</span></label>
            <label><input type="checkbox" name="day" value="5" checked><span>Sam</span></label>
            <label><input type="checkbox" name="day" value="6" checked><span>Dim</span></label>
          </div>
        </div>
        <div class="row hidden" id="drainHoursRow">
          <span>Toutes les</span>
          <input type="number" id="drainHours" min="1" max="720" value="24">
          <span>heures</span>
        </div>
        <button class="btn btn-sm btn-primary" onclick="saveDrainSettings()">Enregistrer</button>
      </div>
      
      <!-- Options au niveau -->
      <div class="sub-options hidden" id="drainLevelOpts">
        <div class="row">
          <span>Vidanger à</span>
          <input type="number" id="drainLevel" min="50" max="100" value="95">
          <span>%</span>
          <button class="btn btn-sm btn-primary" onclick="saveDrainLevel()">OK</button>
        </div>
      </div>
      
      <div class="row" style="margin-top:12px">
        <button class="btn btn-danger" id="drainNowBtn" onclick="drainNow()">Vidanger maintenant</button>
        <span class="pill" id="drainStatus"></span>
      </div>
    </div>

    <!-- Calibration -->
    <div class="card">
      <h3>📏 Calibration</h3>
      <div class="row">
        <span>Distance actuelle: <strong id="currentDist">--</strong> cm</span>
      </div>
      <div class="row">
        <button class="btn btn-secondary" onclick="calibrate(0)">Définir 0%</button>
        <button class="btn btn-secondary" onclick="calibrate(100)">Définir 100%</button>
      </div>
      <div class="row" style="margin-top:8px">
        <span class="pill">0% = <span id="calib0">--</span> cm</span>
        <span class="pill">100% = <span id="calib100">--</span> cm</span>
      </div>
    </div>

    <!-- Préréglages -->
    <div class="card">
      <h3>⚡ Préréglages rapides</h3>
      <div class="presets">
        <button class="btn btn-secondary" onclick="applyPreset(0)">Cycle fermé</button>
        <button class="btn btn-secondary" onclick="applyPreset(1)">Cycle ouvert</button>
        <button class="btn btn-secondary" onclick="applyPreset(2)">Hybride</button>
        <button class="btn btn-secondary" onclick="applyPreset(3)">Éco</button>
      </div>
      <p style="font-size:11px;opacity:.6;margin-top:8px">
        Les préréglages modifient les options ci-dessus selon des configurations typiques.
      </p>
    </div>
  </div>
</main>

<script>
const $=id=>document.getElementById(id);

// SSE
const es = new EventSource('/events');
es.onmessage = e => {
  try {
    const d = JSON.parse(e.data);
    $('level').textContent = d.level?.toFixed(0) ?? '--';
    $('temp').textContent = d.temp?.toFixed(1) ?? '--';
    $('hum').textContent = d.hum?.toFixed(0) ?? '--';
    $('ev1').textContent = d.valve ? 'Ouvert' : 'Fermé';
    $('pump').textContent = d.pump ? 'ON' : 'OFF';
    $('vout').textContent = d.vout ? 'ON' : 'OFF';
    $('pirStatus').textContent = d.pir ? '🟢' : '⚫';
    $('sincePir').textContent = d.sincePir || '--';
    $('currentDist').textContent = d.dist?.toFixed(1) ?? '--';
    $('clock').textContent = d.time || '--:--';
    
    // Power button
    const pb = $('powerBtn');
    if (d.running) {
      pb.classList.remove('off');
      pb.classList.add('on');
    } else {
      pb.classList.remove('on');
      pb.classList.add('off');
    }
    
    // Update UI states
    updateSourceUI(d.waterSource);
    updateFlowUI(d.flowMode);
    updateDrainModeUI(d.drainMode);
    
    $('threshMin').value = d.threshMin || 25;
    $('threshMax').value = d.threshMax || 90;
    $('calib0').textContent = d.calib0?.toFixed(1) ?? '--';
    $('calib100').textContent = d.calib100?.toFixed(1) ?? '--';
    $('drainLevel').value = d.drainAtLevel || 95;
    
    // Drain status
    if (d.drainInProgress) {
      $('drainStatus').textContent = '⏳ Vidange en cours...';
      $('drainNowBtn').disabled = true;
    } else {
      $('drainStatus').textContent = d.nextDrain ? 'Prochaine: ' + d.nextDrain : '';
      $('drainNowBtn').disabled = false;
    }
  } catch(_){}
};

function updateSourceUI(src) {
  document.querySelectorAll('[data-src]').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.src) === src);
  });
  $('srcAutoOpts').classList.toggle('hidden', src !== 2);
}

function updateFlowUI(flow) {
  document.querySelectorAll('[data-flow]').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.flow) === flow);
  });
}

function updateDrainModeUI(mode) {
  document.querySelectorAll('[data-drain]').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.drain) === mode);
  });
  $('drainPeriodicOpts').classList.toggle('hidden', mode !== 1);
  $('drainLevelOpts').classList.toggle('hidden', mode !== 2);
}

function updateDrainUI() {
  const sched = parseInt($('drainSchedule').value);
  $('drainTimeRow').classList.toggle('hidden', sched === 2);
  $('drainDaysRow').classList.toggle('hidden', sched !== 1);
  $('drainHoursRow').classList.toggle('hidden', sched !== 2);
}

function togglePower() {
  fetch('/power').then(r => r.text());
}

function setSource(s) {
  fetch('/setsource?v=' + s).then(r => r.text());
}

function setFlow(f) {
  fetch('/setflow?v=' + f).then(r => r.text());
}

function setDrain(d) {
  fetch('/setdrain?v=' + d).then(r => r.text());
}

function setThresholds() {
  const min = $('threshMin').value;
  const max = $('threshMax').value;
  fetch('/setthresh?min=' + min + '&max=' + max).then(r => r.text());
}

function saveDrainSettings() {
  const sched = $('drainSchedule').value;
  const time = $('drainTime').value;
  let days = 0;
  document.querySelectorAll('[name="day"]:checked').forEach(cb => {
    days |= (1 << parseInt(cb.value));
  });
  const hours = $('drainHours').value;
  fetch('/setdrainsched?type=' + sched + '&time=' + time + '&days=' + days + '&hours=' + hours)
    .then(r => r.text());
}

function saveDrainLevel() {
  const lvl = $('drainLevel').value;
  fetch('/setdrainlevel?v=' + lvl).then(r => r.text());
}

function drainNow() {
  fetch('/drain').then(r => r.text());
}

function calibrate(pct) {
  fetch('/calibrate?pct=' + pct).then(r => r.text());
}

function applyPreset(p) {
  fetch('/preset?v=' + p).then(r => r.text());
}

updateDrainUI();
</script>
</body></html>
)HTML";

// ===================== Outils =====================
// void setRelay(int pin, bool on) {
//   digitalWrite(pin, (ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW)));
// }

void setPump(bool on) {
  digitalWrite(PIN_PUMP, (ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW)));
}

void setEV_out(bool on) {
  digitalWrite(PIN_VALVE, (ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW)));
}

void pulseEV1(bool open) {
  // open=true: ouvrir EV1 | open=false: fermer EV1
  if (open) {
    digitalWrite(PIN_EV1_AIN1, HIGH);
    digitalWrite(PIN_EV1_AIN2, LOW);
  } else {
    digitalWrite(PIN_EV1_AIN1, LOW);
    digitalWrite(PIN_EV1_AIN2, HIGH);
  }
  digitalWrite(PIN_EV1_PWMA, HIGH);
  delay(EV_PULSE_MS);
  digitalWrite(PIN_EV1_PWMA, LOW);
  digitalWrite(PIN_EV1_AIN1, LOW);
  digitalWrite(PIN_EV1_AIN2, LOW);
}

// ===================== FONCTIONS EEPROM =====================

void saveFloatToEEPROM(int addr, float value) {
  uint8_t* p = (uint8_t*)&value;
  for (int i = 0; i < 4; i++) {
    EEPROM.write(addr + i, p[i]);
  }
}

float loadFloatFromEEPROM(int addr) {
  float value;
  uint8_t* p = (uint8_t*)&value;
  for (int i = 0; i < 4; i++) {
    p[i] = EEPROM.read(addr + i);
  }
  return value;
}

void saveUint32ToEEPROM(int addr, uint32_t value) {
  EEPROM.write(addr + 0, (value >> 24) & 0xFF);
  EEPROM.write(addr + 1, (value >> 16) & 0xFF);
  EEPROM.write(addr + 2, (value >> 8) & 0xFF);
  EEPROM.write(addr + 3, value & 0xFF);
}

uint32_t loadUint32FromEEPROM(int addr) {
  uint32_t value = 0;
  value |= ((uint32_t)EEPROM.read(addr + 0)) << 24;
  value |= ((uint32_t)EEPROM.read(addr + 1)) << 16;
  value |= ((uint32_t)EEPROM.read(addr + 2)) << 8;
  value |= ((uint32_t)EEPROM.read(addr + 3));
  return value;
}

void saveAllSettingsToEEPROM() {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC_VALUE);
  EEPROM.write(EEPROM_RUNNING, fountainRunning ? 1 : 0);
  EEPROM.write(EEPROM_WATER_SOURCE, (uint8_t)waterSource);
  EEPROM.write(EEPROM_FLOW_MODE, (uint8_t)flowMode);
  EEPROM.write(EEPROM_DRAIN_MODE, (uint8_t)drainMode);
  EEPROM.write(EEPROM_DRAIN_SCHEDULE, (uint8_t)drainScheduleType);
  EEPROM.write(EEPROM_DRAIN_HOUR, drainHour);
  EEPROM.write(EEPROM_DRAIN_MINUTE, drainMinute);
  EEPROM.write(EEPROM_DRAIN_DAYS, drainDays);
  EEPROM.write(EEPROM_DRAIN_EVERY_H, (drainEveryHours >> 8) & 0xFF);
  EEPROM.write(EEPROM_DRAIN_EVERY_H + 1, drainEveryHours & 0xFF);
  EEPROM.write(EEPROM_DRAIN_LEVEL, drainAtLevelPct);
  EEPROM.write(EEPROM_THRESHOLD_MIN, thresholdMin);
  EEPROM.write(EEPROM_THRESHOLD_MAX, thresholdMax);
  saveFloatToEEPROM(EEPROM_CALIB_ZERO, calibZeroCm);
  saveFloatToEEPROM(EEPROM_CALIB_FULL, calibFullCm);
  EEPROM.write(EEPROM_CALIB_DONE, calibrationDone ? 1 : 0);
  saveUint32ToEEPROM(EEPROM_LAST_DRAIN, lastDrainTimestamp);
  EEPROM.commit();
}

void loadAllSettingsFromEEPROM() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC_VALUE) {
    // Première utilisation : valeurs par défaut
    fountainRunning = false;
    waterSource = SRC_AUTO;
    flowMode = FLOW_CONTINUOUS;
    drainMode = DRAIN_NEVER;
    drainScheduleType = DRAIN_DAILY;
    drainHour = 3;
    drainMinute = 0;
    drainDays = 0b1111111;
    drainEveryHours = 24;
    drainAtLevelPct = 95;
    thresholdMin = 25;
    thresholdMax = 90;
    calibZeroCm = SENSOR_OFFSET_CM + TANK_HEIGHT_CM;  // Défaut
    calibFullCm = SENSOR_OFFSET_CM;                    // Défaut
    calibrationDone = false;
    lastDrainTimestamp = 0;
    saveAllSettingsToEEPROM();
    return;
  }
  
  fountainRunning = false;  // Toujours OFF au démarrage
  waterSource = (WaterSource)constrain(EEPROM.read(EEPROM_WATER_SOURCE), 0, 2);
  flowMode = (FlowMode)constrain(EEPROM.read(EEPROM_FLOW_MODE), 0, 1);
  drainMode = (DrainMode)constrain(EEPROM.read(EEPROM_DRAIN_MODE), 0, 2);
  drainScheduleType = (DrainScheduleType)constrain(EEPROM.read(EEPROM_DRAIN_SCHEDULE), 0, 2);
  drainHour = constrain(EEPROM.read(EEPROM_DRAIN_HOUR), 0, 23);
  drainMinute = constrain(EEPROM.read(EEPROM_DRAIN_MINUTE), 0, 59);
  drainDays = EEPROM.read(EEPROM_DRAIN_DAYS);
  drainEveryHours = (EEPROM.read(EEPROM_DRAIN_EVERY_H) << 8) | EEPROM.read(EEPROM_DRAIN_EVERY_H + 1);
  drainEveryHours = constrain(drainEveryHours, 1, 720);
  drainAtLevelPct = constrain(EEPROM.read(EEPROM_DRAIN_LEVEL), 50, 100);
  thresholdMin = constrain(EEPROM.read(EEPROM_THRESHOLD_MIN), 5, 50);
  thresholdMax = constrain(EEPROM.read(EEPROM_THRESHOLD_MAX), 50, 100);
  calibZeroCm = loadFloatFromEEPROM(EEPROM_CALIB_ZERO);
  calibFullCm = loadFloatFromEEPROM(EEPROM_CALIB_FULL);
  calibrationDone = EEPROM.read(EEPROM_CALIB_DONE) == 1;
  lastDrainTimestamp = loadUint32FromEEPROM(EEPROM_LAST_DRAIN);
}
// ======================= FIN EEPROM =======================

// Force du Wifi
int rssiToQuality(int rssiDbm) {
  // approx: -50 dBm => ~100%, -100 dBm => ~0%
  int q = map(constrain(rssiDbm, -100, -50), -100, -50, 0, 100);
  return constrain(q, 0, 100);
}

// Retourne le pointeur vers l'icône à afficher (20x15px)
const unsigned char* wifiIconForRSSI(int rssiDbm, bool connected) {
  if (!connected) return wifi_none;
  // Seuils typiques : ajustez si besoin
  if (rssiDbm >= -55) return wifi_4;
  if (rssiDbm >= -63) return wifi_3;
  if (rssiDbm >= -70) return wifi_2;
  if (rssiDbm >= -78) return wifi_1;
  return wifi_0;
}

bool readPir() {
  if (!SIMULATION) return digitalRead(PIN_PIR) == HIGH;
  // --- PIR simulé en bursts ---
  static unsigned long nextBurstMs = 0;
  unsigned long now = millis();
  if (SIM_FAKE_PIR_BURSTS && (long)(now - nextBurstMs) >= 0) {
    pirSimUntilMs = now + (unsigned long)random(2000, 20000);   // 2–20 s
    nextBurstMs   = now + (unsigned long)random(20000, 25000);  // 20–25 s
  }
  return (long)pirSimUntilMs - (long)now > 0;
}

void readAHT20() {
  if (!ahtOk) return;
  
  sensors_event_t humidity, temp;
  if (aht.getEvent(&humidity, &temp)) {
    temperatureC = temp.temperature;
    humidityPct = humidity.relative_humidity;
  }
}

// Tri par insertion simple pour tableau de 5 éléments
void sortFloat(float arr[], int n) {
  for (int i = 1; i < n; i++) {
    float key = arr[i];
    int j = i - 1;
    while (j >= 0 && arr[j] > key) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = key;
  }
}

float medianFilter(float samples[], int count) {
  float sorted[count];
  memcpy(sorted, samples, count * sizeof(float));
  sortFloat(sorted, count);
  return sorted[count / 2];  // Valeur médiane
}

float readUltrasonicCm() {
  if (SIMULATION) {
    float waterHeight = (levelPct / 100.0f) * TANK_HEIGHT_CM;
    float dist = SENSOR_OFFSET_CM + (TANK_HEIGHT_CM - waterHeight);
    dist += (random(-5, 6)) * 0.05f;
    dist = constrain(dist, SENSOR_OFFSET_CM, SENSOR_OFFSET_CM + TANK_HEIGHT_CM);
    return dist;
  }
  
  // ===== LECTURES MULTIPLES FILTRÉES =====
  const int NUM_SAMPLES = 5;
  float samples[NUM_SAMPLES];
  int validCount = 0;
  
  for (int i = 0; i < NUM_SAMPLES; i++) {
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    
    unsigned long duration = pulseIn(PIN_ECHO, HIGH, 30000UL);
    
    if (duration > 0) {
      // Compensation température
      float speedSound = 331.3f + (0.606f * temperatureC);
      float cm = (duration * speedSound / 20000.0f);
      
      // Rejet valeurs aberrantes (hors plage physique +10%)
      float minValid = SENSOR_OFFSET_CM * 0.9f;
      float maxValid = (SENSOR_OFFSET_CM + TANK_HEIGHT_CM) * 1.1f;
      
      if (cm >= minValid && cm <= maxValid) {
        samples[validCount++] = cm;
      }
    }
    
    delay(30);  // Pause entre mesures (évite échos résiduels)
  }
  
  // ===== TRAITEMENT =====
  if (validCount == 0) {
    // Aucune mesure valide → garde ancienne valeur
    Serial.println("WARN: Ultrason timeout");
    return distanceCm;  // Retourne dernière mesure connue
  }
  
  if (validCount < 3) {
    // Peu de mesures → moyenne simple
    float sum = 0;
    for (int i = 0; i < validCount; i++) sum += samples[i];
    return sum / validCount;
  }
  
  // 3+ mesures → filtre médian (élimine extrêmes)
  return medianFilter(samples, validCount);
}

int cmToPercent(float cm) {
  if (!calibrationDone) {
    // Utilise les constantes par défaut
    float waterHeight = (SENSOR_OFFSET_CM + TANK_HEIGHT_CM) - cm;
    waterHeight = constrain(waterHeight, 0.0f, TANK_HEIGHT_CM);
    return (int)round((waterHeight / TANK_HEIGHT_CM) * 100.0f);
  }
  
  // Utilise la calibration
  if (calibZeroCm <= calibFullCm) return 0;  // Erreur calibration
  
  float pct = (calibZeroCm - cm) / (calibZeroCm - calibFullCm) * 100.0f;
  return (int)round(constrain(pct, 0.0f, 100.0f));
}

void checkPeriodicDrain() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  
  switch (drainScheduleType) {
    case DRAIN_DAILY:
      // Tous les jours à l'heure spécifiée
      if (timeinfo->tm_hour == drainHour && timeinfo->tm_min == drainMinute) {
        if (now - lastDrainTimestamp > 3600) {  // Évite double déclenchement
          drainInProgress = true;
        }
      }
      break;
      
    case DRAIN_SPECIFIC_DAYS:
      {
        // Jours spécifiques (bit0=Lun, bit6=Dim)
        int weekday = (timeinfo->tm_wday + 6) % 7;  // Convertir (0=Dim) en (0=Lun)
        if (drainDays & (1 << weekday)) {
          if (timeinfo->tm_hour == drainHour && timeinfo->tm_min == drainMinute) {
            if (now - lastDrainTimestamp > 3600) {
              drainInProgress = true;
            }
          }
        }
      }
      break;
      
    case DRAIN_EVERY_X_HOURS:
      {
        uint32_t intervalSec = (uint32_t)drainEveryHours * 3600UL;
        if (now - lastDrainTimestamp >= intervalSec) {
          drainInProgress = true;
        }
      }
      break;
  }
}

void checkDrainTrigger(int levelNow) {
  if (drainInProgress || manualDrainActive) return;
  
  switch (drainMode) {
    case DRAIN_NEVER:
      // Pas de vidange automatique
      break;
      
    case DRAIN_AT_LEVEL:
      // Vidange quand niveau atteint le seuil
      if (levelNow >= drainAtLevelPct) {
        drainInProgress = true;
      }
      break;
      
    case DRAIN_PERIODIC:
      checkPeriodicDrain();
      break;
  }
}

void runLogic(unsigned long dtMs) {
  unsigned long now = millis();

  // === 1) Mesures ===
  bool pir = readPir();
  pirState = pir;
  if (pir) {
    fillAllowedUntilMs = now + (unsigned long)MOTION_HOLD_SECONDS * 1000UL;
    lastPirDetectMs = now;
  }
  bool pirActive = (long)fillAllowedUntilMs - (long)now > 0;

  readAHT20();
  distanceCm = readUltrasonicCm();
  int levelNow = cmToPercent(distanceCm);
  if (!SIMULATION) levelPct = levelNow;

  // === 2) Fontaine arrêtée : tout OFF ===
  if (!fountainRunning) {
    if (valveOn || pumpOn || VoutOn) {
      valveOn = false;
      pumpOn = false;
      VoutOn = false;
      pulseEV1(false);
      setPump(false);
      setEV_out(false);
    }
    return;
  }

  // === 3) Vidange en cours (manuelle ou automatique) ===
  if (manualDrainActive || drainInProgress) {
    valveOn = false;
    pumpOn = true;
    VoutOn = true;
    
    // Arrêt si niveau très bas
    if (levelNow <= 5) {
      manualDrainActive = false;
      drainInProgress = false;
      lastDrainTimestamp = (uint32_t)time(nullptr);
      saveUint32ToEEPROM(EEPROM_LAST_DRAIN, lastDrainTimestamp);
      EEPROM.commit();
      pumpOn = false;
      VoutOn = false;
    }
    
    pulseEV1(valveOn);
    setPump(pumpOn);
    setEV_out(VoutOn);
    return;
  }

  // === 4) Vérification déclenchement vidange ===
  checkDrainTrigger(levelNow);

  // === 5) Logique normale ===
  bool prevValve = valveOn;
  bool prevPump = pumpOn;
  bool prevVout = VoutOn;

  // --- Source d'eau (EV1) ---
  switch (waterSource) {
    case SRC_EXTERNAL:
      // EV1 ouverte, remplissage depuis l'extérieur
      valveOn = (levelNow < thresholdMax);
      break;
      
    case SRC_INTERNAL:
      // EV1 fermée, cycle fermé
      valveOn = false;
      VoutOn = false;  // Pas d'évacuation
      break;
      
    case SRC_AUTO:
      // Automatique selon les seuils
      if (levelNow <= thresholdMin) {
        valveOn = true;   // Ouvrir pour remplir
      } else if (levelNow >= thresholdMax) {
        valveOn = false;  // Fermer, assez plein
      }
      // Entre les deux : maintenir l'état actuel
      break;
  }

  // --- Écoulement (Pompe) ---
  switch (flowMode) {
    case FLOW_PIR:
      // Pompe active seulement si PIR détecté récemment
      if (pirActive && levelNow > thresholdMin) {
        pumpOn = true;
      } else {
        pumpOn = false;
      }
      break;
      
    case FLOW_CONTINUOUS:
      // Pompe toujours active (sauf si niveau trop bas)
      pumpOn = (levelNow > thresholdMin);
      break;
  }

  // --- Arrêts automatiques aux seuils ---
  // Si source externe : arrêt écoulement quand MAX atteint
  if (waterSource == SRC_EXTERNAL && levelNow >= thresholdMax) {
    // On continue la pompe, c'est juste le remplissage qui s'arrête
  }
  
  // Si source interne : arrêt pompe quand MIN atteint
  if (waterSource == SRC_INTERNAL && levelNow <= thresholdMin) {
    pumpOn = false;
  }

  // --- VoutOn suit pumpOn sauf si source interne ---
  if (waterSource != SRC_INTERNAL) {
    VoutOn = pumpOn && valveOn;  // Évacuation si pompe ET remplissage
  } else {
    VoutOn = false;  // Cycle fermé, pas d'évacuation
  }

  // === 6) Appliquer les changements ===
  if (valveOn != prevValve) {
    pulseEV1(valveOn);
    if (valveOn) lastValveOnMs = now;
  }
  if (pumpOn != prevPump) {
    setPump(pumpOn);
    if (pumpOn) lastPumpOnMs = now;
  }
  if (VoutOn != prevVout) {
    setEV_out(VoutOn);
  }

  // === Simulation ===
  if (SIMULATION) {
    float dt = dtMs / 1000.0f;
    if (valveOn) levelPct += SIM_FILL_RATE_PCT_S * dt;
    if (pumpOn && VoutOn) levelPct -= SIM_DRAIN_RATE_PCT_S * dt;
    levelPct -= SIM_LEAK_RATE_PCT_S * dt;
    levelPct = constrain(levelPct, 0.0f, 100.0f);
  }
}

void applyPreset(int preset) {
  switch (preset) {
    case 0:  // Cycle fermé
      waterSource = SRC_INTERNAL;
      flowMode = FLOW_CONTINUOUS;
      drainMode = DRAIN_NEVER;
      break;
      
    case 1:  // Cycle ouvert
      waterSource = SRC_EXTERNAL;
      flowMode = FLOW_PIR;
      drainMode = DRAIN_AT_LEVEL;
      drainAtLevelPct = 95;
      break;
      
    case 2:  // Hybride
      waterSource = SRC_AUTO;
      flowMode = FLOW_CONTINUOUS;
      drainMode = DRAIN_PERIODIC;
      drainScheduleType = DRAIN_DAILY;
      break;
      
    case 3:  // Éco
      waterSource = SRC_AUTO;
      flowMode = FLOW_PIR;
      drainMode = DRAIN_PERIODIC;
      drainScheduleType = DRAIN_EVERY_X_HOURS;
      drainEveryHours = 48;
      break;
  }
  saveAllSettingsToEEPROM();
}

void drawOLED() {
  display.clearDisplay();

  // --- Bandeau Wi-Fi (icône 20x15) ---
  const bool connected = WiFi.isConnected();
  const int  rssi = connected ? WiFi.RSSI() : -100;
  const unsigned char* icon = wifiIconForRSSI(rssi, connected);

  // Icône 20x15 en haut à droite
  display.drawBitmap(SCREEN_WIDTH - 20, 0, icon, 20, 15, SSD1306_WHITE);

  // --- Indicateur ON/OFF en haut à gauche ---
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (fountainRunning) {
    display.print("ON");
  } else {
    display.print("OFF");
  }

  // --- Affichage du mode actif (si préréglage détecté) ---
  // Détecte quel préréglage correspond aux options actuelles
  const char* modeName = nullptr;
  
  if (waterSource == SRC_INTERNAL && flowMode == FLOW_CONTINUOUS && drainMode == DRAIN_NEVER) {
    modeName = "Ferme";
  } else if (waterSource == SRC_EXTERNAL && flowMode == FLOW_PIR && drainMode == DRAIN_AT_LEVEL) {
    modeName = "Ouvert";
  } else if (waterSource == SRC_AUTO && flowMode == FLOW_CONTINUOUS && drainMode == DRAIN_PERIODIC) {
    modeName = "Hybride";
  } else if (waterSource == SRC_AUTO && flowMode == FLOW_PIR && drainMode == DRAIN_PERIODIC) {
    modeName = "Eco";
  }
  
  // Affiche le nom du mode centré en haut (si détecté)
  if (modeName != nullptr) {
    int nameLen = strlen(modeName);
    int nameWidth = nameLen * 6;  // 6 pixels par caractère en taille 1
    display.setCursor((SCREEN_WIDTH - nameWidth) / 2, 0);
    display.print(modeName);
  }

  // --- Image 52x52 selon source d'eau ---
  const unsigned char* srcIcon;
  
  switch (waterSource) {
    case SRC_EXTERNAL:
      srcIcon = robinet;  // Source externe = robinet
      break;
    case SRC_INTERNAL:
      srcIcon = bac;      // Source interne = bac
      break;
    case SRC_AUTO:
    default:
      // En mode auto : affiche selon l'état actuel de EV1
      if (valveOn) {
        srcIcon = robinet;  // EV1 ouverte = remplissage externe
      } else {
        srcIcon = bac;      // EV1 fermée = cycle interne
      }
      break;
  }

  // Position : bas gauche (y = 64 - 52 = 12)
  display.drawBitmap(0, 12, srcIcon, 52, 52, SSD1306_WHITE);

  // --- Niveau d'eau en bas à droite ---
  display.setTextSize(2);

  char levelText[8];
  snprintf(levelText, sizeof(levelText), "%d%%", (int)round(levelPct));

  int textWidth = strlen(levelText) * 12;  // 12 pixels par caractère en taille 2
  display.setCursor(SCREEN_WIDTH - textWidth, SCREEN_HEIGHT - 20);
  display.print(levelText);

  // --- Indicateurs d'état (petits, entre l'icône et le niveau) ---
  display.setTextSize(1);
  int rightCol = 56;  // À droite de l'icône 52px
  
  // Pompe
  display.setCursor(rightCol, 16);
  display.print("P:");
  display.print(pumpOn ? "ON" : "--");
  
  // Écoulement (PIR ou continu)
  display.setCursor(rightCol, 26);
  if (flowMode == FLOW_PIR) {
    display.print("PIR:");
    display.print(pirState ? "!" : "-");
  } else {
    display.print("Cont");
  }
  
  // Vidange en cours
  if (drainInProgress || manualDrainActive) {
    display.setCursor(rightCol, 36);
    display.print("DRAIN");
  }

  display.display();
}

String uptimeStr() {
  uint32_t s = millis()/1000;
  uint32_t h=s/3600, m=(s%3600)/60, sec=s%60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h,m,sec);
  return String(buf);
}

String ipStr() {
  if (WiFi.isConnected()) return WiFi.localIP().toString();
  return String();
}

String fmtHMS(uint32_t sec) {
  uint32_t h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
  char buf[16]; snprintf(buf, sizeof(buf), "%02u:%02u:%02u", h, m, s);
  return String(buf);
}

String agoFrom(unsigned long whenMs) {
  if (whenMs == 0) return String("--:--:--");
  uint32_t sec = (millis() - whenMs) / 1000;
  return fmtHMS(sec);
}

String statusJson() {
  char buf[700];
  
  String sincePir = agoFrom(lastPirDetectMs);
  
  // Calcul prochaine vidange
  String nextDrain = "";
  if (drainMode == DRAIN_PERIODIC && lastDrainTimestamp > 0) {
    uint32_t now = (uint32_t)time(nullptr);
    uint32_t interval = 0;
    
    if (drainScheduleType == DRAIN_EVERY_X_HOURS) {
      interval = (uint32_t)drainEveryHours * 3600UL;
      uint32_t elapsed = now - lastDrainTimestamp;
      if (elapsed < interval) {
        nextDrain = fmtHMS(interval - elapsed);
      }
    }
  }
  
  // Heure actuelle
  struct tm timeinfo;
  char timeStr[6] = "--:--";
  if (getLocalTime(&timeinfo)) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  }
  
  snprintf(buf, sizeof(buf),
    "{\"level\":%.1f,\"temp\":%.1f,\"hum\":%.1f,\"dist\":%.1f,"
    "\"valve\":%s,\"pump\":%s,\"vout\":%s,\"pir\":%s,"
    "\"running\":%s,\"waterSource\":%d,\"flowMode\":%d,\"drainMode\":%d,"
    "\"threshMin\":%d,\"threshMax\":%d,"
    "\"calib0\":%.1f,\"calib100\":%.1f,"
    "\"drainAtLevel\":%d,\"drainInProgress\":%s,"
    "\"sincePir\":\"%s\",\"nextDrain\":\"%s\",\"time\":\"%s\"}",
    levelPct, temperatureC, humidityPct, distanceCm,
    valveOn ? "true" : "false",
    pumpOn ? "true" : "false",
    VoutOn ? "true" : "false",
    pirState ? "true" : "false",
    fountainRunning ? "true" : "false",
    (int)waterSource, (int)flowMode, (int)drainMode,
    thresholdMin, thresholdMax,
    calibZeroCm, calibFullCm,
    drainAtLevelPct,
    (drainInProgress || manualDrainActive) ? "true" : "false",
    sincePir.c_str(), nextDrain.c_str(), timeStr
  );
  
  return String(buf);
}

void pushToGoogleSheet() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client; 
  client.setInsecure(); // (ou client.setCACert(ROOT_CA) si tu veux du TLS strict)

  HTTPClient https;
  https.setConnectTimeout(8000);

  const String url = String(GSCRIPT_URL) + "?token=" + GSCRIPT_TOKEN;
  if (!https.begin(client, url)) return;

  https.addHeader("Content-Type", "application/json");
  String payload = statusJson();

  int code = https.POST(payload);
  String resp = https.getString();

  // Si 302/301/303/307/308 -> on suit manuellement et on RE-POST
  if (code == HTTP_CODE_FOUND || code == HTTP_CODE_MOVED_PERMANENTLY ||
      code == HTTP_CODE_SEE_OTHER || code == HTTP_CODE_TEMPORARY_REDIRECT ||
      code == HTTP_CODE_PERMANENT_REDIRECT) {

    String loc = https.getLocation(); // nouvelle URL
    https.end();

    if (loc.length() && https.begin(client, loc)) {
      https.addHeader("Content-Type", "application/json");
      code = https.POST(payload);     // <-- on POST à nouveau
      resp = https.getString();
      https.end();
    }
  } else {
    https.end();
  }

  //Serial.printf("Sheets %d %s\n", code, resp.c_str());
}


// ================================================= Setup =====================================================

void setup() {
  Serial.begin(115200);

  // ========== Initialisation EEPROM ==========
  EEPROM.begin(EEPROM_SIZE);
  loadAllSettingsFromEEPROM();

  // ========== Détection GPIO0 (bouton BOOT) ==========
  pinMode(0, INPUT_PULLUP);
  delay(100);
  
  if (digitalRead(0) == LOW) {  // Bouton BOOT enfoncé
    Serial.println(F("MODE PROGRAMMATION (BOOT pressé)"));
    while(true) { delay(1000); }  // Blocage
  }
  Serial.println(F("MODE NORMAL : Fontaine active"));
  // ===================================================

  // Baisser la fréquence CPU (80 MHz suffit pour ce projet)
  setCpuFrequencyMhz(80);

  // GPIO
  pinMode(PIN_VALVE, OUTPUT);
  pinMode(PIN_PUMP,  OUTPUT);
  pulseEV1(false);
  setPump(false);
  setEV_out(false);

  // EV1 - Pont en H
  pinMode(PIN_EV1_AIN1, OUTPUT);
  pinMode(PIN_EV1_AIN2, OUTPUT);
  pinMode(PIN_EV1_PWMA, OUTPUT);
  digitalWrite(PIN_EV1_AIN1, LOW);
  digitalWrite(PIN_EV1_AIN2, LOW);
  digitalWrite(PIN_EV1_PWMA, LOW);

  if (!SIMULATION) {
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    pinMode(PIN_PIR,  INPUT);
  } else {
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);
    pinMode(PIN_PIR,  INPUT_PULLUP);
    randomSeed(esp_random());
  }

  // I2C + OLED
  Wire.begin(21, 22); // SDA, SCL
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 non détecté !"));
  } else {
    // display.dim(true); //eteindre l'écran
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.println(F("Boot..."));
    display.display();
  }

  // ===== AHT20 =====
  if (aht.begin()) {
    ahtOk = true;
    Serial.println("AHT20 détecté");
  } else {
    ahtOk = false;
    Serial.println("ERREUR: AHT20 introuvable sur I2C");
  }

  // WiFi
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("WiFi connexion"));
  for (int i=0;i<40 && WiFi.status()!=WL_CONNECTED;i++) {
    delay(250); Serial.print('.');
  }
  Serial.println();
  if (WiFi.isConnected()) {
    Serial.printf("IP:%s | GW:%s | DNS0:%s | DNS1:%s\n", WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP(0).toString().c_str(), WiFi.dnsIP(1).toString().c_str());
  } else {
    Serial.println(F("WiFi non connecté."));
  }
  
  // Configuration NTP
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.print(F("Synchronisation NTP..."));
  int ntpRetry = 0;
  while (time(nullptr) < 100000 && ntpRetry < 20) {
    delay(500);
    Serial.print(".");
    ntpRetry++;
  }

  // Pour réduire la temperature carte
  WiFi.setTxPower(WIFI_POWER_8_5dBm); // limiter le débit wifi
  WiFi.setSleep(true); // autoriser la veille wifi

  // Serveur web
  events.onConnect([](AsyncEventSourceClient *client){
    if(client->connected()){
      client->send(statusJson().c_str(), "status", millis());
    }
  });
  server.addHandler(&events);

  // endpoints serveur
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "text/html", index_html);
  });

  server.on("/power", HTTP_GET, [](AsyncWebServerRequest* r){
    fountainRunning = !fountainRunning;
    saveAllSettingsToEEPROM();
    r->send(200, "text/plain", fountainRunning ? "ON" : "OFF");
  });

  server.on("/setsource", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("v")) {
      int v = r->getParam("v")->value().toInt();
      if (v >= 0 && v <= 2) {
        waterSource = (WaterSource)v;
        saveAllSettingsToEEPROM();
        r->send(200, "text/plain", "OK");
        return;
      }
    }
    r->send(400, "text/plain", "Invalid");
  });

  server.on("/setflow", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("v")) {
      int v = r->getParam("v")->value().toInt();
      if (v >= 0 && v <= 1) {
        flowMode = (FlowMode)v;
        saveAllSettingsToEEPROM();
        r->send(200, "text/plain", "OK");
        return;
      }
    }
    r->send(400, "text/plain", "Invalid");
  });

  server.on("/setdrain", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("v")) {
      int v = r->getParam("v")->value().toInt();
      if (v >= 0 && v <= 2) {
        drainMode = (DrainMode)v;
        saveAllSettingsToEEPROM();
        r->send(200, "text/plain", "OK");
        return;
      }
    }
    r->send(400, "text/plain", "Invalid");
  });

  server.on("/setthresh", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("min") && r->hasParam("max")) {
      int mn = r->getParam("min")->value().toInt();
      int mx = r->getParam("max")->value().toInt();
      if (mn >= 5 && mn <= 50 && mx >= 50 && mx <= 100 && mn < mx) {
        thresholdMin = mn;
        thresholdMax = mx;
        saveAllSettingsToEEPROM();
        r->send(200, "text/plain", "OK");
        return;
      }
    }
    r->send(400, "text/plain", "Invalid");
  });

  server.on("/setdrainsched", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("type")) {
      int t = r->getParam("type")->value().toInt();
      if (t >= 0 && t <= 2) {
        drainScheduleType = (DrainScheduleType)t;
        
        if (r->hasParam("time")) {
          String time = r->getParam("time")->value();
          int h = time.substring(0,2).toInt();
          int m = time.substring(3,5).toInt();
          drainHour = constrain(h, 0, 23);
          drainMinute = constrain(m, 0, 59);
        }
        
        if (r->hasParam("days")) {
          drainDays = r->getParam("days")->value().toInt() & 0x7F;
        }
        
        if (r->hasParam("hours")) {
          drainEveryHours = constrain(r->getParam("hours")->value().toInt(), 1, 720);
        }
        
        saveAllSettingsToEEPROM();
        r->send(200, "text/plain", "OK");
        return;
      }
    }
    r->send(400, "text/plain", "Invalid");
  });

  server.on("/setdrainlevel", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("v")) {
      int v = r->getParam("v")->value().toInt();
      if (v >= 50 && v <= 100) {
        drainAtLevelPct = v;
        saveAllSettingsToEEPROM();
        r->send(200, "text/plain", "OK");
        return;
      }
    }
    r->send(400, "text/plain", "Invalid");
  });

  server.on("/drain", HTTP_GET, [](AsyncWebServerRequest* r){
    manualDrainActive = true;
    r->send(200, "text/plain", "Drain started");
  });

  server.on("/calibrate", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("pct")) {
      int pct = r->getParam("pct")->value().toInt();
      if (pct == 0) {
        calibZeroCm = distanceCm;
        calibrationDone = true;
        saveAllSettingsToEEPROM();
        r->send(200, "text/plain", "0% calibrated");
      } else if (pct == 100) {
        calibFullCm = distanceCm;
        calibrationDone = true;
        saveAllSettingsToEEPROM();
        r->send(200, "text/plain", "100% calibrated");
      } else {
        r->send(400, "text/plain", "Use 0 or 100");
      }
      return;
    }
    r->send(400, "text/plain", "Missing pct");
  });

  server.on("/preset", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("v")) {
      int v = r->getParam("v")->value().toInt();
      if (v >= 0 && v <= 3) {
        applyPreset(v);
        r->send(200, "text/plain", "Preset applied");
        return;
      }
    }
    r->send(400, "text/plain", "Invalid");
  });


  server.begin();

  lastLogicMs = lastOledMs = lastSseMs = millis();
  // ===== Lecture initiale =====
  readAHT20();
  delay(100);  // Stabilisation capteur
  distanceCm = readUltrasonicCm();
  levelPct = cmToPercent(distanceCm);
  // ===== Premier envoi =====
  pushToGoogleSheet();

}

// ================================================= Loop =====================================================

void loop() {
  unsigned long now = millis();

  if (now - lastLogicMs >= LOGIC_INTERVAL_MS) {
    unsigned long dt = now - lastLogicMs;
    lastLogicMs = now;
    runLogic(dt);
  }

  if (now - lastOledMs >= OLED_INTERVAL_MS) {
    lastOledMs = now;
    drawOLED();
  }

  if (now - lastSseMs >= SSE_INTERVAL_MS) {
    lastSseMs = now;
    events.send(statusJson().c_str(), "message", now);
  }

  if (millis() - lastSheetMs >= SHEET_INTERVAL_MS) {
  lastSheetMs = millis();
  pushToGoogleSheet();
  }
}
