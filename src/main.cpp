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
#include "icons.h"

// ===================== Configuration générale =====================
#define SIMULATION false          // true = simulateur; false = capteurs réels

// ---- WiFi ----
const char* WIFI_SSID = "Nian_nian";
const char* WIFI_PASS = "M@rieK3v";
const char* WIFI_HOSTNAME = "Fontaine";
/* IPAddress ip(192, 168, 1, 40); // Adresse IP de la carte
IPAddress gateway (192,168,1,1);
IPAddress subnet  (255,255,255,0);
IPAddress dns1    (192,168,1,1);     // ou 8.8.8.8 / 1.1.1.1
IPAddress dns2    (1,1,1,1); */

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
const int PIN_VALVE = 18;   // Relais EV out
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

const int LEVEL_TARGET_FILL  = 95; // % à atteindre quand remplissage autorisé
const int PUMP_ON_ABOVE      = 90; // % déclenche pompe au-dessus
const int PUMP_OFF_BELOW     = 40; // % arrêt pompe en redescendant
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
<!doctype html><html lang="fr"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Fontaine</title>
<style>
  :root{font:14px system-ui,Segoe UI,Roboto,Ubuntu,Arial}
  body{margin:0;background:#0b1220;color:#e8eefc}
  header{padding:12px 16px;background:#0f172a;position:sticky;top:0}
  main{padding:16px;max-width:900px;margin:auto}
  .grid{display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(220px,1fr))}
  .card{background:#111827;border:1px solid #1f2937;border-radius:12px;padding:14px}
  .title{opacity:.8;font-size:12px;margin-bottom:6px}
  .big{font-size:36px;margin:4px 0 10px}
  .row{display:flex;gap:8px;align-items:center}
  .pill{padding:3px 8px;border-radius:999px;background:#1f2937;font-size:12px}
  progress{width:100%;height:10px}
  code{background:#0a0f1a;padding:2px 6px;border-radius:6px}
</style>
<header>
  <div class="row"><strong>Fontaine</strong></div>
</header>
<main class="grid">
  <div class="card"><div class="title">Niveau de remplissage du réservoir</div>
    <div class="big"><span id="level">–</span>%</div>
    <progress id="lvlbar" max="100" value="0"></progress>
    <div class="row"><span>Distance mesurée:</span><code id="dist">–</code><span>cm</span></div>
  </div>

  <div class="card"><div class="title">État</div>
    <div class="row">PIR: <strong id="pir">–</strong></div>
    <div class="row">Électrovanne: <strong id="valve">–</strong></div>
    <div class="row">Pompe: <strong id="pump">–</strong></div>
  </div>

  <div class="card"><div class="title">Historique</div>
    <div class="row">Depuis dernière détection PIR: <strong id="sincePir">–:–:–</strong></div>
    <div class="row">Dernier allumage électrovanne: <strong id="lastValveOnAgo">–:–:–</strong></div>
    <div class="row">Dernier allumage pompe: <strong id="lastPumpOnAgo">–:–:–</strong></div>
  </div>
  
  <div class="card"><div class="title">Climat</div>
    <div class="row"><span>Température:</span><code id="temp">–</code><span>°C</span></div>
    <div class="row"><span>Humidité:</span><code id="hum">–</code><span>%</span></div>
  </div>

</main>
<script>
const es = new EventSource('/events');
es.onmessage = e => {
  try{
    const d = JSON.parse(e.data);
    const $ = id => document.getElementById(id);
    
    $('level').textContent = d.level;
    $('lvlbar').value = d.level;
    $('dist').textContent = d.distance.toFixed(1);
    $('temp').textContent = d.temp?.toFixed(1);

    const temp = d.temp ?? 20;
    const tempEl = $('temp');
    if (temp > 30) {
      tempEl.style.background = '#dc2626';  // Rouge >30°C
      tempEl.style.color = '#fff';
      tempEl.style.padding = '2px 6px';
      tempEl.style.borderRadius = '4px';
      tempEl.style.fontWeight = 'bold';
    } else if (temp > 25) {
      tempEl.style.background = '#f59e0b';  // Orange 25-30°C
      tempEl.style.color = '#000';
      tempEl.style.padding = '2px 6px';
      tempEl.style.borderRadius = '4px';
      tempEl.style.fontWeight = 'bold';
    } else if (temp < 15) {
      tempEl.style.background = '#3b82f6';  // Bleu <15°C
      tempEl.style.color = '#fff';
      tempEl.style.padding = '2px 6px';
      tempEl.style.borderRadius = '4px';
      tempEl.style.fontWeight = 'bold';
    } else {
      tempEl.style.background = '';
      tempEl.style.color = '';
      tempEl.style.padding = '';
      tempEl.style.fontWeight = '';
    }
    
    // ===== HUMIDITÉ AVEC ALERTE >65% =====
    const hum = d.hum ?? 0;
    const humEl = $('hum');
    humEl.textContent = hum.toFixed(0);
    if (hum > 65) {
      humEl.style.background = '#dc2626';
      humEl.style.color = '#fff';
      humEl.style.padding = '2px 6px';
      humEl.style.borderRadius = '4px';
      humEl.style.fontWeight = 'bold';
    } else {
      humEl.style.background = '';
      humEl.style.color = '';
      humEl.style.padding = '';
      humEl.style.fontWeight = '';
    }
    
    $('pir').textContent = d.pir ? 'ACTIF' : 'inactif';
    $('valve').textContent = d.valve ? 'ON' : 'OFF';
    $('pump').textContent  = d.pump  ? 'ON' : 'OFF';
    
    // ===== DERNIÈRE DÉTECTION PIR AVEC ALERTE >6H =====
    const sincePir = d.sincePir || '--:--:--';
    const sincePirEl = $('sincePir');
    sincePirEl.textContent = sincePir;
    
    // Convertir HH:MM:SS en heures
    if (sincePir !== '--:--:--') {
      const parts = sincePir.split(':');
      const hours = parseInt(parts[0]) || 0;
      
      if (hours >= 6) {
        sincePirEl.style.background = '#dc2626';
        sincePirEl.style.color = '#fff';
        sincePirEl.style.padding = '2px 6px';
        sincePirEl.style.borderRadius = '4px';
        sincePirEl.style.fontWeight = 'bold';
      } else {
        sincePirEl.style.background = '';
        sincePirEl.style.color = '';
        sincePirEl.style.padding = '';
        sincePirEl.style.fontWeight = '';
      }
    }
    
    $('lastValveOnAgo').textContent = d.lastValveOnAgo || '--:--:--';
    $('lastPumpOnAgo').textContent  = d.lastPumpOnAgo  || '--:--:--';
  }catch(_){}
};
</script>
</html>
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
  if (SIM_FAKE_PIR_BURSTS && now > nextBurstMs) {
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
  float waterHeight = (SENSOR_OFFSET_CM + TANK_HEIGHT_CM) - cm;
  waterHeight = constrain(waterHeight, 0.0f, TANK_HEIGHT_CM);
  return (int)round((waterHeight / TANK_HEIGHT_CM) * 100.0f);
}

void runLogic(unsigned long dtMs) {
  unsigned long now = millis();

  // 1) PIR
  bool pir = readPir();
  pirState = pir;
  if (pir) {
    // étend l'autorisation comme avant
    fillAllowedUntilMs = now + (unsigned long)MOTION_HOLD_SECONDS * 1000UL;
    // mémorise l'instant de la dernière détection
    lastPirDetectMs = now;
  }
  bool fillAuthorized = (long)fillAllowedUntilMs - (long)now > 0;

  // 2) Niveau
  readAHT20();
  distanceCm = readUltrasonicCm();
  int levelNow = cmToPercent(distanceCm);
  if (!SIMULATION) levelPct = levelNow;

  // 3) Décisions relais
  bool prevValve = valveOn;
  bool prevPump  = pumpOn;

  if (levelNow >= PUMP_ON_ABOVE)      pumpOn = true;
  else if (levelNow <= PUMP_OFF_BELOW) pumpOn = false;

  if (fillAuthorized && levelNow < LEVEL_TARGET_FILL) valveOn = true;
  else valveOn = false;
  
  // --- Détection des "allumages" (front montant)
  if (valveOn && !prevValve) {
    lastValveOnMs = now;
    pulseEV1(true);  // OUVRIR EV1
  }
  if (!valveOn && prevValve) {
    pulseEV1(false); // FERMER EV1
  }
  
  if (pumpOn && !prevPump) {
    lastPumpOnMs = now;
  }

  // 4) Appliquer
  setPump(pumpOn);
  setEV_out(VoutOn);
  //setRelay(PIN_PUMP,  pumpOn);
  //setRelay(PIN_VALVE, valveOn);

  // 5) Évolution simulation
  if (SIMULATION) {
    float dt = dtMs / 1000.0f;
    if (valveOn) levelPct += SIM_FILL_RATE_PCT_S  * dt;
    if (pumpOn)  levelPct -= SIM_DRAIN_RATE_PCT_S * dt;
    levelPct -= SIM_LEAK_RATE_PCT_S * dt;
    levelPct = constrain(levelPct, 0.0f, 100.0f);
  }
}

void drawOLED() {
  display.clearDisplay();

  // --- Bandeau Wi-Fi (icône 20x15 + texte 8 px) ---
  const bool connected = WiFi.isConnected();
  const int  rssi = connected ? WiFi.RSSI() : -100;
  const int  qual = rssiToQuality(rssi);
  const unsigned char* icon = wifiIconForRSSI(rssi, connected);

  // Icône 20x15 à (0,0) — respecte la contrainte "≤15 px"
  display.drawBitmap(SCREEN_WIDTH - 20, 0, icon, 20, 15, SSD1306_WHITE);

  // --- Niveau d'eau en gros + barre ---
  display.setTextSize(2);              // 16 px
  display.setCursor(0, 24);
  display.print("Level: ");
  display.print((int)round(levelPct));
  display.print("%");

  int barX=0, barY=50, barW=128, barH=12;
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  int fillW = map((int)round(levelPct), 0, 100, 0, barW-2);
  display.fillRect(barX+1, barY+1, fillW, barH-2, SSD1306_WHITE);

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
  // JSON sans ArduinoJson pour rester léger
  char buf[384];
  bool fillAuth = (long)fillAllowedUntilMs - (long)millis() > 0;
  
  String sincePir = agoFrom(lastPirDetectMs);
  String lastValveOnAgo = agoFrom(lastValveOnMs);
  String lastPumpOnAgo  = agoFrom(lastPumpOnMs);

  snprintf(buf, sizeof(buf),
    "{"
      "\"level\":%d,"
      "\"distance\":%.1f,"
      "\"temp\":%.1f,"
      "\"hum\":%.0f,"
      "\"pir\":%d,"
      "\"valve\":%d,"
      "\"pump\":%d,"
      "\"sincePir\":\"%s\","
      "\"lastValveOnAgo\":\"%s\","
      "\"lastPumpOnAgo\":\"%s\","
      "\"uptime\":\"%s\""
    "}",
    (int)round(levelPct), distanceCm, temperatureC, humidityPct, pirState?1:0, valveOn?1:0, pumpOn?1:0, sincePir.c_str(), lastValveOnAgo.c_str(), lastPumpOnAgo.c_str(), uptimeStr().c_str()
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



// ===================== Setup & Loop =====================
void setup() {
  Serial.begin(115200);

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
  //pinMode(PIN_VALVE, OUTPUT);
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

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request){
    request->send(200, "text/html; charset=utf-8", FPSTR(index_html));
  });

  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* request){
    String js = statusJson();
    request->send(200, "application/json", js);
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
