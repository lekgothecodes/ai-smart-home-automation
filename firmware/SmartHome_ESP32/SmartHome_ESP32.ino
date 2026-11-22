/*
  ==========================================================================
  AI-BASED SMART HOME AUTOMATION SYSTEM  -  ESP32 FIRMWARE (version 2)
  DES371S, CPUT, 2026
  ==========================================================================

  WHAT THIS FIRMWARE DOES
    - Reads DHT22 (temperature + humidity), LDR (light level) and PIR (motion).
    - Switches the REAL 12 V fan and 12 V bulb through the 2-channel relay.
    - Four operating modes, selected from the dashboard (Firebase /control/mode):
        "auto"   -> rule-based control (thresholds + hysteresis + motion timeout)
        "ai"     -> decisions from the trained Decision Tree in model.h
                    (falls back to the rules until a model has been trained)
        "manual" -> follows the dashboard switches (/control/light, /control/fan)
        "off"    -> both loads forced OFF
    - Keeps working WITHOUT Wi-Fi/Firebase: all decisions are made on the ESP32.
    - Logs every reading to Firebase /logs so a machine-learning model can be
      trained on real data (Manual mode = "teaching" data: the user's choices).

  PIN MAP  (must match the wiring diagram)
    GPIO27  DHT22 DATA
    GPIO34  LDR voltage-divider junction (ADC1 - works while Wi-Fi is on)
    GPIO13  PIR OUT (PIR output is 3.3 V logic)
    GPIO26  Relay IN1  -> FAN   (relay channel 1)
    GPIO25  Relay IN2  -> LIGHT (relay channel 2)
    GPIO33  Green LED  -> "System online" indicator (solid = Firebase OK,
                          slow blink = running offline)
    GPIO32  Yellow LED -> "Motion detected" indicator
    NOTE: the old test LED on GPIO26 moves to GPIO33 - GPIO26 now drives
          the fan relay.
    (optional) GPIO21 SDA / GPIO22 SCL -> LCD 1602 I2C
    (optional) GPIO39 MQ-2 AO via 10k/10k divider, GPIO4 buzzer module

  FILES IN THIS SKETCH FOLDER
    SmartHome_ESP32.ino   (this file)
    model.h               (Decision Tree - replaced by ml/train_model.py)
    secrets.h             (your Wi-Fi + Firebase credentials - NOT in Git;
                           copy secrets.example.h and fill it in)

  LIBRARIES (Arduino IDE -> Library Manager)
    FirebaseClient by Mobizt 2.2.13 (already installed)
    DHT sensor library by Adafruit (already installed) + Adafruit Unified Sensor
    LiquidCrystal I2C by Frank de Brabander (ONLY if ENABLE_LCD = 1)
  ==========================================================================
*/

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <DHT.h>
#include <time.h>
#include "model.h"

// ---------------------------------------------------------------------------
// 1. CREDENTIALS  - kept in secrets.h, which is NOT committed to Git.
//    Copy secrets.example.h to secrets.h and fill in your own values.
// ---------------------------------------------------------------------------
#include "secrets.h"

// ---------------------------------------------------------------------------
// 2. OPTIONAL HARDWARE SWITCHES  (0 = not connected, 1 = connected)
// ---------------------------------------------------------------------------
#define ENABLE_LCD  0   // LCD 1602 I2C (needs LiquidCrystal I2C library)
#define LCD_ADDRESS 0x3F // your slides confirm 0x3F; try 0x27 if blank
#define ENABLE_GAS  0   // MQ-2 module + active buzzer module

// ---------------------------------------------------------------------------
// 3. RELAY BOARD SETTINGS  (SONGLE SRD-05VDC-SL-C, JD-VCC type board)
//    These boards are normally ACTIVE-LOW: IN pin LOW = relay ON.
//    Confirm with Test 5 in the wiring guide.
// ---------------------------------------------------------------------------
#define RELAY_ACTIVE_LOW    1  // set 0 only if your board turns ON with HIGH
#define RELAY_OFF_FLOATING  1  // 1 = needed on this board (relay 2 did not release)
                               // (OFF is then done by making the pin an input)

// ---------------------------------------------------------------------------
// 4. PINS
// ---------------------------------------------------------------------------
#define DHTPIN          27
#define DHTTYPE         DHT22
#define LDR_PIN         34
#define PIR_PIN         13
#define RELAY_FAN_PIN   26   // relay IN1 -> channel 1 -> fan
#define RELAY_LIGHT_PIN 25   // relay IN2 -> channel 2 -> bulb
#define LED_ONLINE_PIN  33   // green indicator (old GPIO26 LED moves here)
#define LED_MOTION_PIN  32   // yellow indicator
#define GAS_PIN         39
#define BUZZER_PIN      4

// ---------------------------------------------------------------------------
// 5. DEFAULT AUTOMATION SETTINGS
//    These can be changed live from the dashboard (Firebase /settings).
// ---------------------------------------------------------------------------
float fanOnTemp      = 27.0;   // fan ON at or above this temperature (C)
float fanOffTemp     = 26.0;   // fan OFF at or below this temperature (C)
int   darkThreshold  = 1500;   // ambient LDR value below this = dark
const int LIGHT_HYSTERESIS = 200;  // must rise above dark+200 to be "bright"
unsigned long motionHoldMs = 10000; // light stays on 10 s after last motion
int   gasThreshold   = 2000;   // MQ-2 raw ADC alarm level (only if ENABLE_GAS)

// AI decision thresholds on the model probability (hysteresis avoids chatter)
const float AI_ON_PROB  = 0.60;
const float AI_OFF_PROB = 0.40;

// ---------------------------------------------------------------------------
// 6. TIMING
// ---------------------------------------------------------------------------
const unsigned long SENSOR_INTERVAL   = 2500;   // DHT22 needs >= 2 s
const unsigned long CONTROL_INTERVAL  = 3000;   // read /control
const unsigned long SETTINGS_INTERVAL = 30000;  // read /settings
const unsigned long SEND_INTERVAL     = 5000;   // write /sensors + /status
const unsigned long LOG_INTERVAL      = 30000;  // push one row to /logs
const unsigned long WIFI_RETRY        = 30000;  // reconnect attempt period
const unsigned long COMM_TIMEOUT      = 120000; // manual -> auto failsafe
const unsigned long LIGHT_SETTLE_MS   = 1500;   // wait after bulb turns off
                                                 // before trusting the LDR

// ---------------------------------------------------------------------------
// OBJECTS
// ---------------------------------------------------------------------------
DHT dht(DHTPIN, DHTTYPE);

WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD, 3000);
FirebaseApp app;
RealtimeDatabase Database;

#if ENABLE_LCD
  #include <Wire.h>
  #include <LiquidCrystal_I2C.h>
  LiquidCrystal_I2C lcd(LCD_ADDRESS, 16, 2);
#endif

// ---------------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------------
String operatingMode = "off";   // safe until the dashboard's mode has been read
bool manualLight = false;
bool manualFan   = false;

float temperature = NAN, humidity = NAN;
unsigned long lastGoodDht = 0;
int  lightRaw = 0;          // LDR now (includes the bulb's own light)
int  ambientLight = 4095;   // LDR measured while the bulb is OFF
bool isDark = false;
bool motionNow = false;
bool motionRecent = false;
unsigned long lastMotionMs = 0;
bool haveMotionEver = false;
int  gasValue = 0;
bool gasAlarm = false;

bool ruleLight = false, ruleFan = false;
bool aiLight = false,   aiFan = false;
float aiLightProb = -1, aiFanProb = -1;
String decisionSource = "rules";

bool lightOn = false, fanOn = false;
unsigned long lightOffSince = 0;

bool firebaseStarted = false;
bool failsafeActive = false;
unsigned long lastControlOk = 0;

unsigned long lastSensorRead = 0, lastControlRead = 0, lastSettingsRead = 0;
unsigned long lastSend = 0, lastLog = 0, lastWifiAttempt = 0, lastBlink = 0;
bool pendingEventLog = false;
unsigned long uploadSeq = 0;   // reading number: same in Serial, Firebase and dashboard
unsigned long lastEventLog = 0;

// ---------------------------------------------------------------------------
// FUNCTION DECLARATIONS
// ---------------------------------------------------------------------------
void startWiFi();
void startFirebase();
void readSensors();
void computeDecisions();
void applyOutputs();
void writeRelay(int pin, bool on);
void uploadState();
void pushLog(const char *eventName);
void updateIndicators();
void updateLcd();
int  currentHour();
bool jsonValue(const String &json, const char *key, String &out);
void processData(AsyncResult &result);
void processControlData(AsyncResult &result);
void processSettingsData(AsyncResult &result);

// ===========================================================================
// SETUP
// ===========================================================================
void setup() {
  // Relays OFF before anything else, so the loads never flick on at boot.
  writeRelay(RELAY_FAN_PIN, false);
  writeRelay(RELAY_LIGHT_PIN, false);

  Serial.begin(115200);
  delay(1000);

  pinMode(PIR_PIN, INPUT);
  pinMode(LED_ONLINE_PIN, OUTPUT);
  pinMode(LED_MOTION_PIN, OUTPUT);
  digitalWrite(LED_ONLINE_PIN, LOW);
  digitalWrite(LED_MOTION_PIN, LOW);
  analogSetPinAttenuation(LDR_PIN, ADC_11db);   // full 0-3.3 V range

#if ENABLE_GAS
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  analogSetPinAttenuation(GAS_PIN, ADC_11db);
#endif

#if ENABLE_LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("AI Smart Home");
  lcd.setCursor(0, 1); lcd.print("Starting...");
#endif

  dht.begin();

  Serial.println();
  Serial.println("==================================");
  Serial.println("AI SMART HOME AUTOMATION SYSTEM v2");
  Serial.println("ESP32 + Firebase + Decision Tree");
  Serial.println("==================================");
  Serial.print("AI model loaded: ");
  Serial.println(MODEL_TRAINED ? MODEL_VERSION : "NO (AI mode will use rules)");
  Serial.println("PIR warming up - wait about 60 s before testing motion.");

  startWiFi();

  Serial.print("Connecting to CPUT-USERS...");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 30000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    startFirebase();
  } else {
    Serial.println("Wi-Fi connection failed - running OFFLINE (auto rules).");
    Serial.println("Will keep retrying every 30 s.");
  }
}

// ===========================================================================
// LOOP  (never blocks; local control always runs, even with no Wi-Fi)
// ===========================================================================
void loop() {
  unsigned long now = millis();

  // ---- Wi-Fi / Firebase housekeeping ----
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiAttempt >= WIFI_RETRY) {
      lastWifiAttempt = now;
      Serial.println("Wi-Fi lost - reconnecting...");
      WiFi.disconnect();
      startWiFi();
    }
  } else if (!firebaseStarted) {
    Serial.println("Wi-Fi connected!");
    startFirebase();
  }

  if (firebaseStarted) app.loop();

  bool cloudReady = firebaseStarted && WiFi.status() == WL_CONNECTED && app.ready();

  // ---- PIR is read every loop so short pulses are never missed ----
  bool previousMotion = motionNow;
  motionNow = digitalRead(PIR_PIN) == HIGH;
  if (motionNow) {
    if (!previousMotion) Serial.println(">> PIR: motion detected");
    lastMotionMs = now;
    haveMotionEver = true;
  } else if (previousMotion) {
    Serial.println(">> PIR: output back to LOW");
  }
  motionRecent = haveMotionEver && (now - lastMotionMs < motionHoldMs);

  // ---- slower sensors ----
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    readSensors();
  }

  // ---- failsafe: Manual mode with no dashboard contact for 2 minutes ----
  failsafeActive = (operatingMode == "manual") &&
                   (millis() - lastControlOk > COMM_TIMEOUT);

  computeDecisions();
  applyOutputs();
  updateIndicators();

  // Every 5 s, send the relays their correct state again. This corrects a
  // relay that was left ON by the ESP32's start-up / upload process.
  static unsigned long lastRelayRefresh = 0;
  if (millis() - lastRelayRefresh >= 5000) {
    lastRelayRefresh = millis();
    writeRelay(RELAY_FAN_PIN, fanOn);
    writeRelay(RELAY_LIGHT_PIN, lightOn);
  }

  if (!cloudReady) { delay(5); return; }

  // ---- cloud tasks ----
  if (now - lastControlRead >= CONTROL_INTERVAL) {
    lastControlRead = now;
    Database.get(aClient, "/control", processControlData, false, "controlTask");
  }

  if (lastSettingsRead == 0 || now - lastSettingsRead >= SETTINGS_INTERVAL) {
    lastSettingsRead = now;
    Database.get(aClient, "/settings", processSettingsData, false, "settingsTask");
  }

  if (now - lastSend >= SEND_INTERVAL) {
    lastSend = now;
    uploadState();
  }

  if (now - lastLog >= LOG_INTERVAL) {
    lastLog = now;
    pushLog("periodic");
  }

  // A manual switch change is a valuable training example: log it quickly,
  // but wait 3 s so the relay state has settled.
  if (pendingEventLog && now - lastEventLog >= 3000) {
    pendingEventLog = false;
    lastEventLog = now;
    pushLog("manual_change");
  }

  delay(1);
}

// ===========================================================================
// WI-FI + FIREBASE
// ===========================================================================
void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(
    WIFI_SSID,
    WPA2_AUTH_PEAP,
    WIFI_USERNAME,
    WIFI_USERNAME,
    WIFI_PASSWORD
  );
}

void startFirebase() {
  // South Africa = UTC+2, no daylight saving. Used for the "hour" AI feature.
  configTime(2 * 3600, 0, "pool.ntp.org", "time.google.com");

  ssl_client.setInsecure();   // testing only: no certificate check
  Serial.println("Connecting to Firebase...");

  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  firebaseStarted = true;
  Serial.println("Firebase setup started.");
}

// ===========================================================================
// SENSORS
// ===========================================================================
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    temperature = t;
    humidity = h;
    lastGoodDht = millis();
  } else {
    Serial.println("DHT22 reading failed (keeping last good value).");
  }

  lightRaw = analogRead(LDR_PIN);

  // Only trust the LDR as "ambient" light while the bulb is OFF and has been
  // off long enough to stop glowing. Otherwise the bulb would light the LDR,
  // the system would think the room is bright and switch the bulb off again
  // (flicker / feedback loop). The same value is the AI's light feature, so
  // the model learns from the room's light, not from the bulb.
  if (!lightOn && millis() - lightOffSince > LIGHT_SETTLE_MS) {
    ambientLight = lightRaw;
  }

  if (isDark && ambientLight > darkThreshold + LIGHT_HYSTERESIS) isDark = false;
  else if (!isDark && ambientLight < darkThreshold) isDark = true;

#if ENABLE_GAS
  gasValue = analogRead(GAS_PIN);
  gasAlarm = gasValue > gasThreshold;
#endif

#if ENABLE_LCD
  updateLcd();
#endif
}

// ===========================================================================
// DECISIONS
// ===========================================================================
void computeDecisions() {
  bool dhtValid = lastGoodDht != 0 && millis() - lastGoodDht < 60000;

  // ---- rule-based (baseline) ----
  ruleLight = motionRecent && isDark;

  if (!dhtValid)                      ruleFan = false;  // fail safe
  else if (temperature >= fanOnTemp)  ruleFan = true;
  else if (temperature <= fanOffTemp) ruleFan = false;
  // between the two thresholds ruleFan keeps its previous value (hysteresis)

  // ---- machine learning (Decision Tree from model.h) ----
  if (MODEL_TRAINED && dhtValid) {
    float hour = (float)currentHour();
    aiLightProb = modelLightProbability(temperature, humidity, (float)ambientLight,
                                        motionRecent ? 1.0f : 0.0f, hour);
    aiFanProb   = modelFanProbability(temperature, humidity, (float)ambientLight,
                                      motionRecent ? 1.0f : 0.0f, hour);
    if (aiLightProb >= AI_ON_PROB) aiLight = true;
    else if (aiLightProb <= AI_OFF_PROB) aiLight = false;
    if (aiFanProb >= AI_ON_PROB) aiFan = true;
    else if (aiFanProb <= AI_OFF_PROB) aiFan = false;
  } else {
    aiLightProb = -1; aiFanProb = -1;
    aiLight = false;  aiFan = false;
  }
}

void applyOutputs() {
  bool wantLight = false, wantFan = false;

  if (operatingMode == "off") {
    decisionSource = "off";
  } else if (operatingMode == "manual" && !failsafeActive) {
    wantLight = manualLight; wantFan = manualFan;
    decisionSource = "manual";
  } else if (operatingMode == "ai" && MODEL_TRAINED && (aiLightProb >= 0 || aiFanProb >= 0)) {
    // Each device uses the model if one was trained for it, otherwise rules.
    wantLight = aiLightProb >= 0 ? aiLight : ruleLight;
    wantFan   = aiFanProb   >= 0 ? aiFan   : ruleFan;
    decisionSource = (aiLightProb >= 0 && aiFanProb >= 0) ? "ai" : "ai+rules";
  } else {
    // "auto", "ai" without a trained model, or manual failsafe
    wantLight = ruleLight; wantFan = ruleFan;
    decisionSource = failsafeActive ? "failsafe" : "rules";
  }

  if (wantLight != lightOn) {
    lightOn = wantLight;
    writeRelay(RELAY_LIGHT_PIN, lightOn);
    if (!lightOn) lightOffSince = millis();
    Serial.printf("LIGHT -> %s  (source: %s, ambient %d, motion %s)\n",
                  lightOn ? "ON" : "OFF", decisionSource.c_str(),
                  ambientLight, motionRecent ? "yes" : "no");
  }
  if (wantFan != fanOn) {
    fanOn = wantFan;
    writeRelay(RELAY_FAN_PIN, fanOn);
    Serial.printf("FAN   -> %s  (source: %s, temp %.1f C)\n",
                  fanOn ? "ON" : "OFF", decisionSource.c_str(), temperature);
  }

#if ENABLE_GAS
  digitalWrite(BUZZER_PIN, gasAlarm ? HIGH : LOW);  // safety: works in every mode
#endif
}

void writeRelay(int pin, bool on) {
#if RELAY_ACTIVE_LOW
  if (on) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
  } else {
    digitalWrite(pin, HIGH);
    pinMode(pin, OUTPUT);       // take the pin over from its start-up state
  #if RELAY_OFF_FLOATING
    pinMode(pin, INPUT);        // then disconnect it so the relay fully releases
  #endif
  }
#else
  pinMode(pin, OUTPUT);
  digitalWrite(pin, on ? HIGH : LOW);
#endif
}

// ===========================================================================
// INDICATORS + LCD
// ===========================================================================
void updateIndicators() {
  bool cloudReady = firebaseStarted && WiFi.status() == WL_CONNECTED && app.ready();
  if (cloudReady) {
    digitalWrite(LED_ONLINE_PIN, HIGH);
  } else if (millis() - lastBlink > 700) {      // slow blink = offline
    lastBlink = millis();
    digitalWrite(LED_ONLINE_PIN, !digitalRead(LED_ONLINE_PIN));
  }
  digitalWrite(LED_MOTION_PIN, motionNow ? HIGH : LOW);
}

#if ENABLE_LCD
void updateLcd() {
  char line1[17], line2[17];
  snprintf(line1, sizeof(line1), "%4.1fC %2.0f%% L%4d",
           isnan(temperature) ? 0.0 : temperature,
           isnan(humidity) ? 0.0 : humidity, lightRaw);
  String m = operatingMode; m.toUpperCase();
  snprintf(line2, sizeof(line2), "%-6s F%s L%s %s", m.c_str(),
           fanOn ? "1" : "0", lightOn ? "1" : "0", motionNow ? "M" : " ");
  lcd.setCursor(0, 0); lcd.print(line1);
  lcd.setCursor(0, 1); lcd.print(line2);
}
#endif

int currentHour() {
  struct tm info;
  if (!getLocalTime(&info, 0)) return -1;   // -1 = clock not synced yet
  return info.tm_hour;
}

// ===========================================================================
// FIREBASE WRITES
// ===========================================================================
static String b(bool v) { return v ? "true" : "false"; }

void uploadState() {
  // /sensors = measurements, /status = what the ESP32 is ACTUALLY doing.
  // The ESP32 never writes /control (that belongs to the dashboard), so the
  // two sides can never overwrite each other.
  uploadSeq++;
  String s = "{";
  s += "\"seq\":" + String(uploadSeq) + ",";
  s += "\"temperature\":" + (isnan(temperature) ? String("null") : String(temperature, 2));
  s += ",\"humidity\":"   + (isnan(humidity) ? String("null") : String(humidity, 2));
  s += ",\"light\":"        + String(lightRaw);
  s += ",\"ambientLight\":" + String(ambientLight);
  s += ",\"motion\":"       + b(motionNow);
  s += ",\"motionRecent\":" + b(motionRecent);
#if ENABLE_GAS
  s += ",\"gas\":"          + String(gasValue);
#endif
  s += "}";
  Database.set<object_t>(aClient, "/sensors", object_t(s), processData, "sensorTask");

  s = "{";
  s += "\"mode\":\"" + operatingMode + "\"";
  s += ",\"source\":\"" + decisionSource + "\"";
  s += ",\"light\":" + b(lightOn);
  s += ",\"fan\":"   + b(fanOn);
  s += ",\"ruleLight\":" + b(ruleLight);
  s += ",\"ruleFan\":"   + b(ruleFan);
  s += ",\"aiLight\":"   + b(aiLight);
  s += ",\"aiFan\":"     + b(aiFan);
  s += ",\"aiLightProb\":" + String(aiLightProb, 2);
  s += ",\"aiFanProb\":"   + String(aiFanProb, 2);
  s += ",\"modelTrained\":" + b(MODEL_TRAINED);
  s += ",\"modelVersion\":\"" + String(MODEL_VERSION) + "\"";
  s += ",\"dark\":"     + b(isDark);
  s += ",\"failsafe\":" + b(failsafeActive);
  s += ",\"gasAlarm\":" + b(gasAlarm);
  s += ",\"hour\":"     + String(currentHour());
  s += ",\"uptime\":"   + String(millis() / 1000);
  s += ",\"seq\":" + String(uploadSeq);
  s += ",\"lastSeen\":{\".sv\":\"timestamp\"}";
  s += "}";
  Database.set<object_t>(aClient, "/status", object_t(s), processData, "statusTask");

  Serial.println();
  Serial.printf("----------- SENSOR DATA  #%lu -----------\n", uploadSeq);
  Serial.printf("Temp %.2f C | Hum %.2f %% | LDR %d (ambient %d, %s)\n",
                temperature, humidity, lightRaw, ambientLight, isDark ? "dark" : "bright");
  Serial.printf("Motion now: %s | recent: %s\n", motionNow ? "YES" : "no", motionRecent ? "YES" : "no");
  Serial.printf("Mode: %s | source: %s | Light %s | Fan %s\n", operatingMode.c_str(),
                decisionSource.c_str(), lightOn ? "ON" : "OFF", fanOn ? "ON" : "OFF");
  Serial.printf("Rules -> light %s fan %s | AI -> light %s (%.2f) fan %s (%.2f)\n",
                ruleLight ? "ON" : "OFF", ruleFan ? "ON" : "OFF",
                aiLight ? "ON" : "OFF", aiLightProb, aiFan ? "ON" : "OFF", aiFanProb);
#if ENABLE_GAS
  Serial.printf("Gas: %d %s\n", gasValue, gasAlarm ? "ALARM" : "ok");
#endif
  Serial.println("-----------------------------------");
}

void pushLog(const char *eventName) {
  if (lastGoodDht == 0) return;   // no valid reading yet

  String s = "{";
  s += "\"ts\":{\".sv\":\"timestamp\"}";
  s += ",\"event\":\"" + String(eventName) + "\"";
  s += ",\"hour\":" + String(currentHour());
  s += ",\"mode\":\"" + operatingMode + "\"";
  s += ",\"source\":\"" + decisionSource + "\"";
  s += ",\"temperature\":" + String(temperature, 2);
  s += ",\"humidity\":" + String(humidity, 2);
  s += ",\"light\":" + String(lightRaw);
  s += ",\"ambientLight\":" + String(ambientLight);
  s += ",\"motion\":" + String(motionRecent ? 1 : 0);
  s += ",\"lightOn\":" + String(lightOn ? 1 : 0);
  s += ",\"fanOn\":" + String(fanOn ? 1 : 0);
  s += ",\"ruleLight\":" + String(ruleLight ? 1 : 0);
  s += ",\"ruleFan\":" + String(ruleFan ? 1 : 0);
  s += ",\"aiLight\":" + String(aiLight ? 1 : 0);
  s += ",\"aiFan\":" + String(aiFan ? 1 : 0);
  s += "}";

  Database.push<object_t>(aClient, "/logs", object_t(s), processData, "logTask");
  Serial.printf("Log row sent (%s, mode %s)\n", eventName, operatingMode.c_str());
}

// ===========================================================================
// FIREBASE READS
// ===========================================================================
// Finds "key": value in a flat JSON string. Returns the value text without
// quotes. Firebase returns compact JSON such as
// {"fan":false,"light":true,"mode":"manual"}
bool jsonValue(const String &json, const char *key, String &out) {
  String k = String("\"") + key + "\"";
  int i = json.indexOf(k);
  if (i < 0) return false;
  int c = json.indexOf(':', i + k.length());
  if (c < 0) return false;
  int s = c + 1;
  while (s < (int)json.length() && json[s] == ' ') s++;
  if (s >= (int)json.length()) return false;
  if (json[s] == '"') {
    int e = json.indexOf('"', s + 1);
    if (e < 0) return false;
    out = json.substring(s + 1, e);
    return true;
  }
  int e = s;
  while (e < (int)json.length() && json[e] != ',' && json[e] != '}') e++;
  out = json.substring(s, e);
  out.trim();
  return true;
}

void processControlData(AsyncResult &result) {
  if (!result.isResult()) return;
  if (result.isError()) {
    Serial.print("Control read error: ");
    Serial.println(result.error().message());
    return;
  }
  if (!result.available()) return;

  String data = result.c_str();
  data.trim();
  if (data == "null" || data.length() == 0) return;

  lastControlOk = millis();

  String v;
  String newMode = operatingMode;
  bool newLight = manualLight, newFan = manualFan;

  if (jsonValue(data, "mode", v)) {
    v.toLowerCase();
    if (v == "auto" || v == "ai" || v == "manual" || v == "off") newMode = v;
  }
  if (jsonValue(data, "light", v)) newLight = (v == "true");
  if (jsonValue(data, "fan", v))   newFan   = (v == "true");

  bool modeChanged = newMode != operatingMode;
  bool manualChanged = newLight != manualLight || newFan != manualFan;

  if (modeChanged || manualChanged) {
    Serial.print("Firebase control response: ");
    Serial.println(data);
  }
  if (modeChanged) {
    Serial.print("Selected mode: ");
    Serial.println(newMode);
  }
  if (manualChanged && newMode == "manual") {
    Serial.printf("Manual command: light %s, fan %s\n",
                  newLight ? "ON" : "OFF", newFan ? "ON" : "OFF");
    pendingEventLog = true;   // user taught the system something -> log it
  }

  operatingMode = newMode;
  manualLight = newLight;
  manualFan = newFan;
}

void processSettingsData(AsyncResult &result) {
  if (!result.isResult()) return;
  if (result.isError()) {
    Serial.print("Settings read error: ");
    Serial.println(result.error().message());
    return;
  }
  if (!result.available()) return;

  String data = result.c_str();
  data.trim();
  if (data == "null" || data.length() == 0) return;   // keep defaults

  String v;
  float on = fanOnTemp, off = fanOffTemp;
  if (jsonValue(data, "fanOnTemp", v))  on  = v.toFloat();
  if (jsonValue(data, "fanOffTemp", v)) off = v.toFloat();
  if (on > off && on > 0 && on < 60) { fanOnTemp = on; fanOffTemp = off; }

  if (jsonValue(data, "darkThreshold", v)) {
    int d = v.toInt();
    if (d > 0 && d < 4095) darkThreshold = d;
  }
  if (jsonValue(data, "motionHoldSec", v)) {
    int sec = v.toInt();
    if (sec >= 2 && sec <= 3600) motionHoldMs = (unsigned long)sec * 1000UL;
  }
  if (jsonValue(data, "gasThreshold", v)) {
    int g = v.toInt();
    if (g > 0 && g < 4095) gasThreshold = g;
  }
}

void processData(AsyncResult &result) {
  if (!result.isResult()) return;

  if (result.isEvent()) {
    Serial.print("Firebase event: ");
    Serial.println(result.eventLog().message());
  }
  if (result.isError()) {
    Serial.print("Firebase error (");
    Serial.print(result.uid());
    Serial.print("): ");
    Serial.println(result.error().message());
  }
  // Successful writes are not printed every time, to keep the monitor readable.
}
