#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>

#define TEMP_WARN       35.0f
#define TEMP_CRIT       42.0f
#define HUMID_WARN      65.0f
#define HUMID_CRIT      80.0f
#define SMOKE_WARN      400
#define SMOKE_CRIT      700
#define VOLT_LOW_WARN   4.0f
#define VOLT_LOW_CRIT   3.5f
#define VOLT_HIGH_WARN  5.2f
#define VOLT_HIGH_CRIT  5.8f

// Health score boundaries
#define SCORE_EXCELLENT  100
#define SCORE_GOOD        75
#define SCORE_WARNING     50
#define SCORE_CRITICAL    25
#define SCORE_EMERGENCY    0

struct SensorData {
  float temperature;
  float humidity;
  int   smokeLevel;
  float voltage;
  bool  doorOpen;
  bool  motionDetected;
  bool  flameDetected;
};

struct HealthReport {
  int   score;          // 0–100
  int   statusCode;      // 0=EMERGENCY 1=CRITICAL 2=WARNING 3=GOOD 4=EXCELLENT
  char  status[16];     // EXCELLENT / WARNING / CRITICAL / EMERGENCY
  char  primaryFault[40];
};

HealthReport computeHealth(const SensorData& d) {
  HealthReport r;
  int deductions = 0;
  char fault[40] = "All systems nominal";

  // Temperature deductions
  if (d.temperature >= TEMP_CRIT) {
    deductions += 40;
    snprintf(fault, sizeof(fault), "TEMP CRITICAL: %.1fC", d.temperature);
  } else if (d.temperature >= TEMP_WARN) {
    deductions += 20;
    snprintf(fault, sizeof(fault), "TEMP WARNING: %.1fC", d.temperature);
  }

  // Humidity deductions
  if (d.humidity >= HUMID_CRIT) {
    deductions += 20;
    if (deductions <= 20)
      snprintf(fault, sizeof(fault), "HUMIDITY CRITICAL: %.0f%%", d.humidity);
  } else if (d.humidity >= HUMID_WARN) {
    deductions += 10;
  }

  // Smoke
  if (d.smokeLevel >= SMOKE_CRIT) {
    deductions += 40;
    snprintf(fault, sizeof(fault), "SMOKE DETECTED: %d ppm", d.smokeLevel);
  } else if (d.smokeLevel >= SMOKE_WARN) {
    deductions += 20;
  }

  // Flame — instant emergency
  if (d.flameDetected) {
    deductions = 100;
    snprintf(fault, sizeof(fault), "FLAME DETECTED - EVACUATE");
  }

  // Power
  if (d.voltage <= VOLT_LOW_CRIT || d.voltage >= VOLT_HIGH_CRIT) {
    deductions += 25;
    if (!d.flameDetected)
      snprintf(fault, sizeof(fault), "POWER ANOMALY: %.2fV", d.voltage);
  } else if (d.voltage <= VOLT_LOW_WARN || d.voltage >= VOLT_HIGH_WARN) {
    deductions += 10;
  }

  // Security
  if (d.motionDetected) { deductions += 10; }
  if (d.doorOpen)        { deductions += 5;  }

  r.score = max(0, SCORE_EXCELLENT - deductions);
  strncpy(r.primaryFault, fault, sizeof(r.primaryFault));

  if      (r.score >= 85) { strncpy(r.status, "EXCELLENT", sizeof(r.status)); r.statusCode = 4; }
  else if (r.score >= 65) { strncpy(r.status, "GOOD",      sizeof(r.status)); r.statusCode = 3; }
  else if (r.score >= 45) { strncpy(r.status, "WARNING",   sizeof(r.status)); r.statusCode = 2; }
  else if (r.score >= 20) { strncpy(r.status, "CRITICAL",  sizeof(r.status)); r.statusCode = 1; }
  else                    { strncpy(r.status, "EMERGENCY", sizeof(r.status)); r.statusCode = 0; }

  return r;
}

// ════════════════════════════════════════════════
//  Risk Prediction Engine — trend analysis, no ML
// ════════════════════════════════════════════════

#define HISTORY_SIZE     100
#define PREDICT_MINUTES   10
#define CRITICAL_TEMP     42.0f   // matches TEMP_CRIT above

struct TrendReport {
  float currentTemp;
  float trendPerMin;       // °C per minute (positive = rising)
  float predictedTemp;     // projected temp after PREDICT_MINUTES
  bool  coolingRequired;
  float minutesToCritical; // -1 = not approaching critical, else countdown in minutes
  char  advice[60];
  char  countdown[40];     // human-readable countdown string
};

class Predictor {
private:
  float  _tempHistory[HISTORY_SIZE];
  unsigned long _timeHistory[HISTORY_SIZE];  // millis timestamps
  int    _head   = 0;
  int    _count  = 0;

public:
  void record(float temp) {
    _tempHistory[_head] = temp;
    _timeHistory[_head] = millis();
    _head = (_head + 1) % HISTORY_SIZE;
    if (_count < HISTORY_SIZE) _count++;
  }

  TrendReport analyze(float currentTemp) {
    TrendReport t;
    t.currentTemp = currentTemp;
    t.trendPerMin = 0.0f;
    t.predictedTemp = currentTemp;
    t.coolingRequired = false;
    t.minutesToCritical = -1.0f;
    strncpy(t.advice, "Conditions stable", sizeof(t.advice));
    strncpy(t.countdown, "Not approaching critical", sizeof(t.countdown));

    if (_count < 5) return t;  // need at least 5 points

    // Linear regression over last min of readings
    int samples = min(_count, 20);
    int oldest  = (_head - samples + HISTORY_SIZE) % HISTORY_SIZE;

    float firstTemp = _tempHistory[oldest];
    unsigned long firstTime = _timeHistory[oldest];
    unsigned long lastTime  = _timeHistory[(_head - 1 + HISTORY_SIZE) % HISTORY_SIZE];

    unsigned long elapsed_ms = lastTime - firstTime;
    if (elapsed_ms < 1000) return t;  // too short

    float deltaTemp = currentTemp - firstTemp;
    float elapsed_min = elapsed_ms / 60000.0f;

    t.trendPerMin   = deltaTemp / elapsed_min;
    t.predictedTemp = currentTemp + (t.trendPerMin * PREDICT_MINUTES);

    // ── Time-to-Critical countdown ──
    if (t.trendPerMin > 0.05f && currentTemp < CRITICAL_TEMP) {
      t.minutesToCritical = (CRITICAL_TEMP - currentTemp) / t.trendPerMin;

      if (t.minutesToCritical <= 2.0f) {
        snprintf(t.countdown, sizeof(t.countdown),
          "CRITICAL in %.1f min — ACT NOW", t.minutesToCritical);
      } else if (t.minutesToCritical <= 10.0f) {
        snprintf(t.countdown, sizeof(t.countdown),
          "CRITICAL in %.0f min if trend continues", t.minutesToCritical);
      } else if (t.minutesToCritical <= 60.0f) {
        snprintf(t.countdown, sizeof(t.countdown),
          "CRITICAL in ~%.0f min at current rate", t.minutesToCritical);
      } else {
        t.minutesToCritical = -1.0f;
        strncpy(t.countdown, "Rising slowly — no immediate risk", sizeof(t.countdown));
      }
    } else if (currentTemp >= CRITICAL_TEMP) {
      t.minutesToCritical = 0.0f;
      strncpy(t.countdown, "ALREADY CRITICAL", sizeof(t.countdown));
    } else {
      strncpy(t.countdown, "Not approaching critical", sizeof(t.countdown));
    }

    // Generate human-readable advice
    if (t.trendPerMin > 0.5f) {
      t.coolingRequired = true;
      if (t.predictedTemp >= 45.0f)
        snprintf(t.advice, sizeof(t.advice),
          "CRITICAL: Will reach %.0fC in %d min. Check cooling NOW.",
          t.predictedTemp, PREDICT_MINUTES);
      else if (t.predictedTemp >= 38.0f)
        snprintf(t.advice, sizeof(t.advice),
          "WARNING: Predicted %.0fC in %d min. Inspect cooling.",
          t.predictedTemp, PREDICT_MINUTES);
      else
        snprintf(t.advice, sizeof(t.advice),
          "Rising +%.1fC/min. Monitor closely.", t.trendPerMin);
    } else if (t.trendPerMin < -0.3f) {
      snprintf(t.advice, sizeof(t.advice),
        "Cooling effective. Dropping %.1fC/min.", -t.trendPerMin);
    } else {
      strncpy(t.advice, "Temperature stable.", sizeof(t.advice));
    }

    return t;
  }

  int sampleCount() { return _count; }
};

// ════════════════════════════════════════════════
//  Main Sketch
// ════════════════════════════════════════════════

// ── Pin Definitions ──────────────────────────────────
#define DHTPIN        4
#define DHTTYPE       DHT22
#define SMOKE_PIN     34    // Analog (MQ-2)
#define FLAME_PIN     35    // Digital
#define DOOR_PIN      13    // Push button (LOW = door open)
#define PIR_PIN       14    // PIR motion
#define VOLT_PIN      32    // Analog (potentiometer = simulated PSU voltage)
#define LED_RED       25
#define LED_GREEN     26
#define LED_BLUE      27
#define BUZZER_PIN    33

// ── OLED ─────────────────────────────────────────────
#define SCREEN_W 128
#define SCREEN_H 64
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

// ── WiFi (Wokwi guest network) ───────────────────────
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// ── Globals ───────────────────────────────────────────
DHT        dht(DHTPIN, DHTTYPE);
WebServer  server(80);
Predictor  predictor;

SensorData  sensors;
HealthReport health;
TrendReport  trend;

unsigned long lastRead    = 0;
unsigned long lastOLED    = 0;
unsigned long lastAlert   = 0;
int oledPage = 0;

const unsigned long READ_INTERVAL  = 3000;   // sensor poll
const unsigned long OLED_INTERVAL  = 4000;   // page flip
const unsigned long ALERT_COOLDOWN = 60000;  // 1 min between repeated alerts

// ── Voltage mapping (ADC 0-4095 → 0V–6V) ────────────
float readVoltage() {
  int raw = analogRead(VOLT_PIN);
  return (raw / 4095.0f) * 6.0f;
}

// ── RGB LED state ─────────────────────────────────────
void setLED(int statusCode) {
  // statusCode: 0=EMERGENCY 1=CRITICAL 2=WARNING 3=GOOD 4=EXCELLENT
  digitalWrite(LED_RED,   LOW);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_BLUE,  LOW);

  switch (statusCode) {
    case 4: digitalWrite(LED_GREEN, HIGH); break;                              // EXCELLENT
    case 3: digitalWrite(LED_GREEN, HIGH); digitalWrite(LED_BLUE, HIGH); break; // GOOD
    case 2: digitalWrite(LED_RED, HIGH);   digitalWrite(LED_GREEN, HIGH); break;// WARNING
    case 1: digitalWrite(LED_RED, HIGH); break;                                // CRITICAL
    default: digitalWrite(LED_RED, HIGH); break;                              // EMERGENCY
  }
}

// ── Buzzer patterns ───────────────────────────────────
void buzz(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}

// ── OLED display pages ────────────────────────────────
void drawPage0() {   // Environmental
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);  oled.println("=  ENVIRONMENT  =");
  oled.setCursor(0, 12); oled.printf("Temp  : %.1f C\n", sensors.temperature);
  oled.setCursor(0, 22); oled.printf("Humid : %.0f %%\n", sensors.humidity);
  oled.setCursor(0, 32); oled.printf("Smoke : %d\n", sensors.smokeLevel);
  oled.setCursor(0, 42); oled.printf("Flame : %s\n", sensors.flameDetected ? "DETECTED!" : "Clear");
  oled.setCursor(0, 54); oled.printf("Health: %d%%  %s", health.score, health.status);
  oled.display();
}

void drawPage1() {   // Security + Power
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);  oled.println("= SECURITY/POWER =");
  oled.setCursor(0, 12); oled.printf("Door  : %s\n", sensors.doorOpen ? "OPEN !" : "Closed");
  oled.setCursor(0, 22); oled.printf("Motion: %s\n", sensors.motionDetected ? "DETECTED" : "Clear");
  oled.setCursor(0, 32); oled.printf("Volt  : %.2f V\n", sensors.voltage);
  float vdev = sensors.voltage - 5.0f;
  oled.setCursor(0, 42); oled.printf("Vstab : %+.2f V\n", vdev);
  oled.setCursor(0, 54); oled.printf("Score : %d%%  %s", health.score, health.status);
  oled.display();
}

void drawPage2() {   // Prediction
  oled.clearDisplay();
  oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);  oled.println("=  PREDICTION   =");
  oled.setCursor(0, 12); oled.printf("Now   : %.1f C\n", trend.currentTemp);
  oled.setCursor(0, 22); oled.printf("Trend : %+.2f C/min\n", trend.trendPerMin);
  oled.setCursor(0, 32); oled.printf("In 10m: %.1f C\n", trend.predictedTemp);

  // Headline: time-to-critical countdown
  oled.setCursor(0, 44);
  if (trend.minutesToCritical >= 0 && trend.minutesToCritical <= 2.0f) {
    oled.setTextSize(1);
    oled.printf("!! CRIT in %.1fm !!", trend.minutesToCritical);
  } else if (trend.minutesToCritical >= 0) {
    oled.printf("Crit ETA: %.0f min", trend.minutesToCritical);
  } else {
    oled.print("No critical risk");
  }

  oled.setCursor(0, 54);
  oled.printf("Health: %d%%  %s", health.score, health.status);
  oled.display();
}

// ── Web Dashboard HTML ────────────────────────────────
String buildDashboardHTML() {
  String html = R"rawhtml(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="5">
<title>ServerSense Pro</title>
<style>
  *{margin:0;padding:0;box-sizing:border-box}
  body{font-family:'Segoe UI',sans-serif;background:#0f1117;color:#e0e0e0;padding:20px}
  h1{text-align:center;color:#00d4ff;margin-bottom:4px;font-size:1.6em;letter-spacing:2px}
  .sub{text-align:center;color:#666;font-size:.8em;margin-bottom:20px}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px;margin-bottom:20px}
  .card{background:#1a1d27;border:1px solid #2a2d3a;border-radius:10px;padding:16px}
  .card .label{font-size:.72em;color:#888;text-transform:uppercase;letter-spacing:1px;margin-bottom:6px}
  .card .value{font-size:1.8em;font-weight:600}
  .card .unit{font-size:.85em;color:#888;margin-left:4px}
  .ok{color:#00e676}.warn{color:#ffab00}.crit{color:#ff5252}.em{color:#ff1744}
  .score-bar{background:#1a1d27;border:1px solid #2a2d3a;border-radius:10px;padding:16px;margin-bottom:14px}
  .bar-track{background:#2a2d3a;border-radius:6px;height:18px;overflow:hidden}
  .bar-fill{height:100%;border-radius:6px;transition:width .6s ease}
  .pred-box{background:#1a1d27;border:1px solid #2a2d3a;border-radius:10px;padding:16px;margin-bottom:14px}
  .pred-box h3{color:#00d4ff;font-size:.85em;text-transform:uppercase;letter-spacing:1px;margin-bottom:10px}
  .pred-row{display:flex;justify-content:space-between;font-size:.9em;padding:3px 0;border-bottom:1px solid #2a2d3a}
  .fault{background:#2a1a1a;border:1px solid #ff5252;border-radius:8px;padding:12px;margin-bottom:14px;color:#ff5252;font-size:.9em}
  .footer{text-align:center;font-size:.72em;color:#444;margin-top:10px}
</style></head><body>
<h1>⚡ ServerSense Pro</h1>
<div class="sub">Intelligent Server Room Guardian</div>
)rawhtml";

  // Health score bar
  const char* barColor = "#00e676";
  if (health.score < 45) barColor = "#ff5252";
  else if (health.score < 65) barColor = "#ff5252";
  else if (health.score < 85) barColor = "#ffab00";

  html += "<div class='score-bar'>";
  html += "<div class='label'>Room Health Score</div>";
  html += "<div style='display:flex;align-items:center;gap:12px'>";
  html += "<div class='bar-track' style='flex:1'><div class='bar-fill' style='width:";
  html += String(health.score);
  html += "%;background:"; html += barColor; html += "'></div></div>";
  html += "<span style='font-size:1.4em;font-weight:700;color:"; html += barColor; html += "'>";
  html += String(health.score); html += "%</span>";
  html += "<span style='color:#aaa'>" + String(health.status) + "</span></div></div>";

  // Fault message
  if (health.score < 85) {
    html += "<div class='fault'>⚠ " + String(health.primaryFault) + "</div>";
  }

  html += "<div class='grid'>";

  // Temp
  const char* tc = sensors.temperature >= TEMP_CRIT ? "crit" : sensors.temperature >= TEMP_WARN ? "warn" : "ok";
  html += "<div class='card'><div class='label'>Temperature</div><div class='value " + String(tc) + "'>";
  html += String(sensors.temperature, 1); html += "<span class='unit'>°C</span></div></div>";

  // Humidity
  const char* hc = sensors.humidity >= HUMID_CRIT ? "crit" : sensors.humidity >= HUMID_WARN ? "warn" : "ok";
  html += "<div class='card'><div class='label'>Humidity</div><div class='value " + String(hc) + "'>";
  html += String(sensors.humidity, 0); html += "<span class='unit'>%</span></div></div>";

  // Smoke
  const char* sc = sensors.smokeLevel >= SMOKE_CRIT ? "crit" : sensors.smokeLevel >= SMOKE_WARN ? "warn" : "ok";
  html += "<div class='card'><div class='label'>Smoke (MQ-2)</div><div class='value " + String(sc) + "'>";
  html += String(sensors.smokeLevel); html += "<span class='unit'>raw</span></div></div>";

  // Flame
  html += "<div class='card'><div class='label'>Flame</div><div class='value ";
  html += sensors.flameDetected ? "em'>DETECTED" : "ok'>Clear";
  html += "</div></div>";

  // Door
  html += "<div class='card'><div class='label'>Door</div><div class='value ";
  html += sensors.doorOpen ? "warn'>OPEN" : "ok'>Closed";
  html += "</div></div>";

  // Motion
  html += "<div class='card'><div class='label'>Motion</div><div class='value ";
  html += sensors.motionDetected ? "warn'>DETECTED" : "ok'>Clear";
  html += "</div></div>";

  // Voltage
  const char* vc = (sensors.voltage <= VOLT_LOW_CRIT || sensors.voltage >= VOLT_HIGH_CRIT) ? "crit" :
                   (sensors.voltage <= VOLT_LOW_WARN || sensors.voltage >= VOLT_HIGH_WARN) ? "warn" : "ok";
  html += "<div class='card'><div class='label'>PSU Voltage</div><div class='value " + String(vc) + "'>";
  html += String(sensors.voltage, 2); html += "<span class='unit'>V</span></div></div>";

  html += "</div>";

  // Prediction panel
  html += "<div class='pred-box'><h3>🔮 Risk Prediction Engine</h3>";

  String countdownColor = "#00e676";
  if (trend.minutesToCritical >= 0 && trend.minutesToCritical <= 2.0f) countdownColor = "#ff1744";
  else if (trend.minutesToCritical >= 0 && trend.minutesToCritical <= 10.0f) countdownColor = "#ff5252";
  else if (trend.minutesToCritical >= 0) countdownColor = "#ffab00";

  html += "<div style='background:#0f1117;border:1px solid " + countdownColor + ";border-radius:8px;";
  html += "padding:12px;margin-bottom:12px;text-align:center'>";
  html += "<div style='font-size:.7em;color:#888;text-transform:uppercase;letter-spacing:1px'>Time to Critical</div>";
  html += "<div style='font-size:1.4em;font-weight:700;color:" + countdownColor + ";margin-top:4px'>";
  html += String(trend.countdown);
  html += "</div></div>";

  html += "<div class='pred-row'><span>Current temp</span><span>" + String(trend.currentTemp, 1) + " °C</span></div>";
  html += "<div class='pred-row'><span>Trend</span><span>";
  html += (trend.trendPerMin >= 0 ? "+" : ""); html += String(trend.trendPerMin, 2); html += " °C/min</span></div>";
  html += "<div class='pred-row'><span>Predicted (10 min)</span><span style='font-weight:600;color:";
  html += trend.predictedTemp >= 42 ? "#ff5252" : trend.predictedTemp >= 38 ? "#ffab00" : "#00e676";
  html += "'>" + String(trend.predictedTemp, 1) + " °C</span></div>";
  html += "<div style='margin-top:10px;font-size:.85em;color:#aaa'>" + String(trend.advice) + "</div>";
  html += "</div>";

  html += "<div class='footer'>ServerSense Pro v1.0 · Auto-refresh every 5s</div>";
  html += "</body></html>";
  return html;
}

// ── Alert logic ───────────────────────────────────────
void handleAlerts() {
  unsigned long now = millis();
  if (now - lastAlert < ALERT_COOLDOWN) return;

  if (health.score < 45 || sensors.flameDetected) {
    if (health.statusCode == 0) {
      buzz(5, 100, 80);
    } else {
      buzz(3, 200, 150);
    }
    lastAlert = now;
    Serial.printf("[ALERT] %s | Score: %d | %s\n",
      health.status, health.score, health.primaryFault);
  }
}

// ── Setup ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(FLAME_PIN,  INPUT);
  pinMode(DOOR_PIN,   INPUT_PULLUP);
  pinMode(PIR_PIN,    INPUT_PULLDOWN);
  pinMode(LED_RED,    OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_BLUE,   OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  dht.begin();
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  server.on("/", []() { server.send(200, "text/html", buildDashboardHTML()); });
  server.begin();

  sensors.temperature = 25.0f;
  for (int i = 0; i < 10; i++) { predictor.record(25.0f); delay(10); }
}

// ── Main Loop ─────────────────────────────────────────
void loop() {
  server.handleClient();
  unsigned long now = millis();

  // ── Read sensors every READ_INTERVAL ──
  if (now - lastRead >= READ_INTERVAL) {
    lastRead = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) sensors.temperature = t;
    if (!isnan(h)) sensors.humidity    = h;

    sensors.smokeLevel     = analogRead(SMOKE_PIN);
    sensors.voltage        = readVoltage();
    sensors.flameDetected  = (digitalRead(FLAME_PIN) == HIGH);
    sensors.doorOpen       = (digitalRead(DOOR_PIN)  == LOW);
    sensors.motionDetected = (digitalRead(PIR_PIN)   == HIGH);

    // Feed predictor
    predictor.record(sensors.temperature);

    // Compute health and trend
    health = computeHealth(sensors);
    trend  = predictor.analyze(sensors.temperature);

    // Update LED
    setLED(health.statusCode);

    // Emergency blink override
    if (health.statusCode == 0) {
      for (int i = 0; i < 3; i++) {
        digitalWrite(LED_RED, HIGH); delay(100);
        digitalWrite(LED_RED, LOW);  delay(100);
      }
    }

    // Serial telemetry — one clean line
    Serial.printf("Temp=%.1fC  Health=%d%% [%s]  ETA: %s\n",
      sensors.temperature, health.score, health.status, trend.countdown);

    handleAlerts();
  }

  // ── Rotate OLED pages ──
  if (now - lastOLED >= OLED_INTERVAL) {
    lastOLED = now;
    switch (oledPage % 3) {
      case 0: drawPage0(); break;
      case 1: drawPage1(); break;
      case 2: drawPage2(); break;
    }
    oledPage++;
  }
}
