#include "DHT.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <RTClib.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

// ====== WiFi Configuration ======
const char *ssid =
    "Ruang Dosen Mesin 02_plus"; // Sesuaikan dengan SSID WiFi Yang akan dipaki
const char *password =
    "polmed1234"; // Sesuaikan dengan Password WiFi Yang akan dipakai

// ====== Telegram Bot Configuration ======
const char *TELEGRAM_BOT_TOKEN =
    "8849853286:AAGOijWY83CW4aZUHoFyBf8N-gMCcBItFyo";
const char *TELEGRAM_CHAT_ID = "8909842654";
const char *TELEGRAM_API = "https://api.telegram.org/bot";

// ====== Polmed Research Center Cloud Server Configuration ======
const char *CLOUD_SERVER_URL = "https://polmedresearchcenter.id/api/sics.php";
const unsigned long CLOUD_TELEMETRY_INTERVAL =
    1500; // Kirim telemetri & cek perintah tiap 1.5 detik (sangat responsif & cepat)
unsigned long lastCloudSend = 0;

// ====== Google Sheets Configuration ======
// Panduan Apps Script ada di atas fungsi sendDataToGoogleSheets()
const char *GOOGLE_SHEETS_URL =
    "https://script.google.com/macros/s/"
    "AKfycbztAWFoF7JgaYwx_a1Y6alr41iEyzwG8Er3zu9X008Qzq40uBdQKQ4yB_bIht8_dVsE-"
    "w/exec"; // Letakkan tautan Google Sheets Web App di sini
const unsigned long GOOGLE_SHEETS_INTERVAL =
    60000; // Interval pengiriman ke Google Sheet (milidetik: 60000 = 1 menit)
unsigned long lastGoogleSheetsSend = 0;

WebServer server(80);
Preferences prefs;

// ====== Pin mapping ======
#define X_STEP 18
#define X_DIR 19
#define Y_STEP 17
#define Y_DIR 16
#define EN_PIN 4

#define X_MIN 32
#define X_MAX 33
#define Y_MIN 25
#define Y_MAX 26

#define DHT_PIN 23
#define SOIL_A_PIN 34
#define SOIL_B_PIN 35

#define PUMP_PIN 27
#define LED_YELLOW 14
#define LED_GREEN 13
#define LED_RED 2
#define BUZZER_PIN 15

#define I2C_SDA 21
#define I2C_SCL 22

// ====== Motion timing ======
const uint16_t RELEASE_US = 250;
const uint16_t SEEK_US = 400;
const uint16_t TRAVEL_US = 180;

const uint32_t RELEASE_TIMEOUT = 3000UL;
const uint32_t SEEK_TIMEOUT = 180000UL;
const uint32_t TRAVEL_TIMEOUT = 180000UL;

// ====== Grid ======
const int COLS = 6;
const int ROWS = 8;
const uint32_t DWELL_MS = 2000;

const float X_TOTAL_STEPS = 767.33f;
const float Y_TOTAL_STEPS = 2118.33f;

const float X_SPACING = X_TOTAL_STEPS / (COLS - 1);
const float Y_SPACING = Y_TOTAL_STEPS / (ROWS - 1);

// ====== Soil calibration ======
const int SOIL_DRY_MIN = 2384;
const int SOIL_DRY_MAX = 2535;
const int SOIL_NORMAL_MIN = 1459;
const int SOIL_NORMAL_MAX = 1660;
const int SOIL_WET_MIN = 1014;
const int SOIL_WET_MAX = 1191;

// ====== State machine untuk buzzer, led, pump ======
enum ActuatorState { IDLE, ACTIVE, COOLING_DOWN };

struct ActuatorTimer {
  ActuatorState state = IDLE;
  unsigned long activateTime = 0;
  unsigned long durationMs = 0;
};

ActuatorTimer buzzer_timer, pump_timer;

// ====== LCD Display Mode (rotate setiap 5 detik) ======
enum LCDMode { MODE_DATETIME, MODE_TEMP_HUM, MODE_SOIL_ZONES };
LCDMode currentLCDMode = MODE_DATETIME;
unsigned long lastLCDModeChange = 0;
const unsigned long LCD_MODE_INTERVAL = 5000; // 5 detik

// ====== Devices ======
hd44780_I2Cexp lcd;
RTC_DS3231 rtc;
DHT dht(DHT_PIN, DHT22);

// ====== Global state ======
long currentX = 0;
long currentY = 0;

float lastTempC = NAN;
float lastHum = NAN;
String lastSoilA = "---";
String lastSoilB = "---";
int lastAdcA = 0;
int lastAdcB = 0;

bool emergencyStop = false;
bool isMoving = false;
bool isScanning = false;

// ====== Telegram Bot State ======
unsigned long lastTelegramCheck = 0;
const unsigned long TELEGRAM_CHECK_INTERVAL = 5000; // Check setiap 5 detik (efisien, stabil, tanpa lag)
int lastUpdateId = 0; // Track last update_id untuk avoid duplicate commands

// Forward declarations
void logEvent(String msg);
bool sendTelemetryToCloud();
void systemYield(bool allowCloudSend = true);

// ====== PWM pump ======
const int PUMP_FREQ = 5000;
const int PUMP_RES_BITS = 8;

// ====== Multi-schedule ======
const int MAX_SCHEDULES = 16;
enum ScheduleTargetType {
  TARGET_FULL,
  TARGET_ZONE_A,
  TARGET_ZONE_B,
  TARGET_ROW,
  TARGET_COLUMN
};
struct Schedule {
  int hour;
  int minute;
  ScheduleTargetType targetType;
  int targetIndex;
};
Schedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;
int lastTriggeredDayOfYear[MAX_SCHEDULES]; // dayOfYear that was last triggered
                                           // to avoid repeated triggers the
                                           // same day

// ================= Hardware Utilities ===================

inline bool pressed(uint8_t pin) { return digitalRead(pin) == LOW; }

inline void pulse(uint8_t pin, uint16_t us) {
  digitalWrite(pin, HIGH);
  delayMicroseconds(us);
  digitalWrite(pin, LOW);
  delayMicroseconds(us);
}

void rampPulse(uint8_t stepPin, uint16_t targetDelay) {
  static const uint16_t rampTable[] = {1200, 1000, 800, 650, 500, 400};
  for (size_t i = 0; i < sizeof(rampTable) / sizeof(rampTable[0]); ++i) {
    pulse(stepPin, rampTable[i]);
  }
  pulse(stepPin, targetDelay);
}

// ====== State Machine: Buzzer ======
void buzzer_start(unsigned long durationMs) {
  buzzer_timer.state = ACTIVE;
  buzzer_timer.activateTime = millis();
  buzzer_timer.durationMs = durationMs;
  digitalWrite(BUZZER_PIN, HIGH);
}

void buzzer_update() {
  if (buzzer_timer.state == IDLE)
    return;

  if (buzzer_timer.state == ACTIVE) {
    if (millis() - buzzer_timer.activateTime >= buzzer_timer.durationMs) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzer_timer.state = IDLE;
    }
  }
}

// ====== State Machine: LED Yellow (movement indicator) ======
void led_yellow_on() { digitalWrite(LED_YELLOW, HIGH); }

void led_yellow_off() { digitalWrite(LED_YELLOW, LOW); }

// ====== State Machine: LED Green (pump indicator) ======
void pump_activate(unsigned long durationMs) {
  pump_timer.state = ACTIVE;
  pump_timer.activateTime = millis();
  pump_timer.durationMs = durationMs;
  ledcWrite(PUMP_PIN, 255);
  digitalWrite(LED_GREEN, HIGH);
}

void pump_update() {
  if (pump_timer.state == IDLE)
    return;

  if (pump_timer.state == ACTIVE) {
    if (millis() - pump_timer.activateTime >= pump_timer.durationMs) {
      ledcWrite(PUMP_PIN, 0);
      digitalWrite(LED_GREEN, LOW);
      pump_timer.state = IDLE;
    }
  }
}

// ====== Background System Yield (Anti Disconnect & Keep-Alive) ======
void systemYield(bool allowCloudSend) {
  server.handleClient();
  pump_update();
  buzzer_update();

  if (allowCloudSend && WiFi.status() == WL_CONNECTED && strlen(CLOUD_SERVER_URL) > 0) {
    if (millis() - lastCloudSend >= CLOUD_TELEMETRY_INTERVAL) {
      lastCloudSend = millis();
      sendTelemetryToCloud();
    }
  }
  yield();
}

// ====== Motion functions ======
bool moveUntil(uint8_t stepPin, uint8_t dirPin, bool dir, uint8_t limitPin,
               bool targetPressed, uint16_t delayUs, uint32_t timeout) {
  digitalWrite(EN_PIN, LOW);
  digitalWrite(dirPin, dir);
  delay(20);
  uint32_t t0 = millis();

  while (true) {
    if (emergencyStop) {
      logEvent("EMERGENCY STOP during moveUntil");
      digitalWrite(EN_PIN, HIGH);
      return false;
    }

    if (pressed(limitPin) == targetPressed) {
      digitalWrite(EN_PIN, HIGH);
      return true;
    }
    if (millis() - t0 > timeout) {
      digitalWrite(EN_PIN, HIGH);
      return false;
    }
    rampPulse(stepPin, delayUs);
    pump_update();
    buzzer_update();
  }
}

bool homeX() {
  logEvent("HOME X (seek X-)");
  if (pressed(X_MIN)) {
    logEvent("Release X-");
    if (!moveUntil(X_STEP, X_DIR, HIGH, X_MIN, false, RELEASE_US,
                   RELEASE_TIMEOUT))
      return false;
  }
  logEvent("Seek X-");
  if (!moveUntil(X_STEP, X_DIR, LOW, X_MIN, true, SEEK_US, SEEK_TIMEOUT))
    return false;

  logEvent("HOME X OK");
  currentX = 0;
  return true;
}

bool homeY() {
  logEvent("HOME Y (seek Y-)");
  if (pressed(Y_MIN)) {
    logEvent("Release Y-");
    if (!moveUntil(Y_STEP, Y_DIR, HIGH, Y_MIN, false, RELEASE_US,
                   RELEASE_TIMEOUT))
      return false;
  }
  logEvent("Seek Y-");
  if (!moveUntil(Y_STEP, Y_DIR, LOW, Y_MIN, true, SEEK_US, SEEK_TIMEOUT))
    return false;

  logEvent("HOME Y OK");
  currentY = 0;
  return true;
}

void stepAxis(long steps, uint8_t stepPin, uint8_t dirPin, bool dir,
              uint16_t stepDelayUs) {
  if (steps <= 0)
    return;
  digitalWrite(EN_PIN, LOW);
  digitalWrite(dirPin, dir);
  delay(20);
  led_yellow_on();
  for (long i = 0; i < steps; i++) {
    if (emergencyStop)
      break;
    rampPulse(stepPin, stepDelayUs);
    pump_update();
    buzzer_update();
  }
  led_yellow_off();
  digitalWrite(EN_PIN, HIGH);
}

void moveToStep(long tx, long ty) {
  if (emergencyStop)
    return;

  isMoving = true;
  long dx = tx - currentX;
  long dy = ty - currentY;

  buzzer_start(500);

  if (dx != 0) {
    stepAxis(labs(dx), X_STEP, X_DIR, dx > 0, TRAVEL_US);
    currentX = tx;
  }
  if (dy != 0) {
    stepAxis(labs(dy), Y_STEP, Y_DIR, dy > 0, TRAVEL_US);
    currentY = ty;
  }

  buzzer_start(500);
  isMoving = false;
  systemYield(true);
}

// ====== Grid helpers ======
long targetXForColumn(int colIndex) { return lround(colIndex * X_SPACING); }

long targetYForRow(int rowIndex) {
  return lround((ROWS - 1 - rowIndex) * Y_SPACING);
}

// ====== Helper function: scan dengan list koordinat custom ======
void scanCoordinateList(int rows[], int cols[], int count,
                        const char *zoneName) {
  if (emergencyStop)
    return;

  logEvent(String("SCAN START - ") + zoneName);
  Serial.println(String("MULAI SCAN ") + zoneName);
  isScanning = true;
  systemYield(true);

  for (int i = 0; i < count; i++) {
    if (emergencyStop)
      break;

    int r = rows[i];
    int c = cols[i];

    long tx = targetXForColumn(c);
    long ty = targetYForRow(r);
    moveToStep(tx, ty);

    char loc[4];
    loc[0] = 'A' + r;
    loc[1] = '1' + c;
    loc[2] = 0;
    Serial.print("AT ");
    Serial.println(loc);
    logEvent(String("SCAN POS ") + loc);

    pump_activate(DWELL_MS);

    unsigned long dwell_start = millis();
    while (millis() - dwell_start < DWELL_MS) {
      if (emergencyStop)
        break;
      systemYield(true);
      delay(10);
    }
  }

  Serial.println(String("SELESAI SCAN ") + zoneName);
  logEvent(String("SCAN END - ") + zoneName);
  isScanning = false;
  systemYield(true);
}

// ====== Zona A Scan ======
void scanZonaA() {
  if (emergencyStop)
    return;

  int rows[] = {7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6,
                5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4};
  int cols[] = {0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0,
                0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0};
  int count = sizeof(rows) / sizeof(rows[0]);

  scanCoordinateList(rows, cols, count, "ZONA A");

  logEvent("Homing after ZONA A scan");
  if (!homeX())
    logEvent("HOME X FAILED");
  if (!homeY())
    logEvent("HOME Y FAILED");
}

// ====== Zona B Scan ======
void scanZonaB() {
  if (emergencyStop)
    return;

  int rows[] = {3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2,
                1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
  int cols[] = {0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0,
                0, 1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0};
  int count = sizeof(rows) / sizeof(rows[0]);

  scanCoordinateList(rows, cols, count, "ZONA B");

  logEvent("Homing after ZONA B scan");
  if (!homeX())
    logEvent("HOME X FAILED");
  if (!homeY())
    logEvent("HOME Y FAILED");
}

// ====== Full Scan (H1 -> A1) ======
void scanCustom() {
  if (emergencyStop)
    return;

  logEvent("SCAN START - FULL");
  Serial.println("MULAI GRID SCAN (custom serpentine dari H1 ke A1)");
  isScanning = true;
  systemYield(true);

  for (int r = ROWS - 1; r >= 0; --r) {
    int rowFromBottom = (ROWS - 1) - r;
    bool leftToRight = (rowFromBottom % 2 == 0);

    if (leftToRight) {
      for (int c = 0; c < COLS; ++c) {
        if (emergencyStop)
          break;

        long tx = targetXForColumn(c);
        long ty = targetYForRow(r);
        moveToStep(tx, ty);

        char loc[4];
        loc[0] = 'A' + r;
        loc[1] = '1' + c;
        loc[2] = 0;
        Serial.print("AT ");
        Serial.println(loc);
        logEvent(String("SCAN POS ") + loc);

        pump_activate(DWELL_MS);

        unsigned long dwell_start = millis();
        while (millis() - dwell_start < DWELL_MS) {
          if (emergencyStop)
            break;
          systemYield(true);
          delay(10);
        }
      }
    } else {
      for (int c = COLS - 1; c >= 0; --c) {
        if (emergencyStop)
          break;

        long tx = targetXForColumn(c);
        long ty = targetYForRow(r);
        moveToStep(tx, ty);

        char loc[4];
        loc[0] = 'A' + r;
        loc[1] = '1' + c;
        loc[2] = 0;
        Serial.print("AT ");
        Serial.println(loc);
        logEvent(String("SCAN POS ") + loc);

        pump_activate(DWELL_MS);

        unsigned long dwell_start = millis();
        while (millis() - dwell_start < DWELL_MS) {
          if (emergencyStop)
            break;
          systemYield(true);
          delay(10);
        }
      }
    }
    if (emergencyStop)
      break;
  }

  Serial.println("SELESAI GRID SCAN");
  logEvent("SCAN END - FULL");
  isScanning = false;
  systemYield(true);
}

// ====== ROW WATERING ======
void scanRow(int rowIndex) {
  if (emergencyStop || rowIndex < 0 || rowIndex >= ROWS)
    return;

  char rowLetter = 'A' + rowIndex;
  logEvent(String("SCAN START - ROW ") + rowLetter);
  Serial.println(String("MULAI SCAN ROW ") + rowLetter);
  isScanning = true;
  systemYield(true);

  for (int c = 0; c < COLS; ++c) {
    if (emergencyStop)
      break;

    long tx = targetXForColumn(c);
    long ty = targetYForRow(rowIndex);
    moveToStep(tx, ty);

    char loc[4];
    loc[0] = 'A' + rowIndex;
    loc[1] = '1' + c;
    loc[2] = 0;
    Serial.print("AT ");
    Serial.println(loc);
    logEvent(String("SCAN POS ") + loc);

    pump_activate(DWELL_MS);

    unsigned long dwell_start = millis();
    while (millis() - dwell_start < DWELL_MS) {
      if (emergencyStop)
        break;
      systemYield(true);
      delay(10);
    }
  }

  Serial.println(String("SELESAI SCAN ROW ") + rowLetter);
  logEvent(String("SCAN END - ROW ") + rowLetter);
  isScanning = false;
  systemYield(true);
}

// ====== COLUMN WATERING ======
void scanColumn(int colIndex) {
  if (emergencyStop || colIndex < 0 || colIndex >= COLS)
    return;

  logEvent(String("SCAN START - COLUMN ") + (colIndex + 1));
  Serial.println(String("MULAI SCAN COLUMN ") + (colIndex + 1));
  isScanning = true;
  systemYield(true);

  for (int r = ROWS - 1; r >= 0; --r) {
    if (emergencyStop)
      break;

    long tx = targetXForColumn(colIndex);
    long ty = targetYForRow(r);
    moveToStep(tx, ty);

    char loc[4];
    loc[0] = 'A' + r;
    loc[1] = '1' + colIndex;
    loc[2] = 0;
    Serial.print("AT ");
    Serial.println(loc);
    logEvent(String("SCAN POS ") + loc);

    pump_activate(DWELL_MS);

    unsigned long dwell_start = millis();
    while (millis() - dwell_start < DWELL_MS) {
      if (emergencyStop)
        break;
      systemYield(true);
      delay(10);
    }
  }

  Serial.println(String("SELESAI SCAN COLUMN ") + (colIndex + 1));
  logEvent(String("SCAN END - COLUMN ") + (colIndex + 1));
  isScanning = false;
  systemYield(true);
}

// ====== Sensors ======
String soilStatusLabel(int adcValue) {
  if (adcValue >= SOIL_DRY_MIN && adcValue <= SOIL_DRY_MAX)
    return "DRY";
  else if (adcValue >= SOIL_NORMAL_MIN && adcValue <= SOIL_NORMAL_MAX)
    return "NORMAL";
  else if (adcValue >= SOIL_WET_MIN && adcValue <= SOIL_WET_MAX)
    return "WET";
  else if (adcValue > SOIL_DRY_MAX)
    return "DRY";
  else
    return "WET";
}

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t))
    lastTempC = t;
  if (!isnan(h))
    lastHum = h;

  int a = 0, b = 0;
  for (int i = 0; i < 8; i++) {
    a += analogRead(SOIL_A_PIN);
    b += analogRead(SOIL_B_PIN);
    delay(5);
  }
  a /= 8;
  b /= 8;
  lastAdcA = a;
  lastAdcB = b;
  lastSoilA = soilStatusLabel(a);
  lastSoilB = soilStatusLabel(b);
}

// ====== LCD update dengan 3 mode yang berganti setiap 5 detik ======
void updateLCDMode(DateTime now) {
  if (millis() - lastLCDModeChange > LCD_MODE_INTERVAL) {
    lastLCDModeChange = millis();
    currentLCDMode = (LCDMode)((currentLCDMode + 1) % 3);
  }

  lcd.clear();

  if (currentLCDMode == MODE_DATETIME) {
    char row1[17], row2[17];
    sprintf(row1, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
    sprintf(row2, "%02d/%02d/%04d", now.day(), now.month(), now.year());

    lcd.setCursor(0, 0);
    lcd.print("Waktu: ");
    lcd.print(row1);

    lcd.setCursor(0, 1);
    lcd.print("Tanggal:");
    lcd.print(row2);
  } else if (currentLCDMode == MODE_TEMP_HUM) {
    char row1[17], row2[17];
    if (!isnan(lastTempC))
      sprintf(row1, "Suhu : %2.1f C", lastTempC);
    else
      sprintf(row1, "Suhu : --.- C");

    if (!isnan(lastHum))
      sprintf(row2, "Kelembapan:%2.0f%%", lastHum);
    else
      sprintf(row2, "Kelembapan:--%%");

    lcd.setCursor(0, 0);
    lcd.print(row1);

    lcd.setCursor(0, 1);
    lcd.print(row2);
  } else if (currentLCDMode == MODE_SOIL_ZONES) {
    char row1[17], row2[17];
    snprintf(row1, sizeof(row1), "ZONA A:%s", lastSoilA.c_str());
    snprintf(row2, sizeof(row2), "ZONA B:%s", lastSoilB.c_str());

    lcd.setCursor(0, 0);
    lcd.print(row1);

    lcd.setCursor(0, 1);
    lcd.print(row2);
  }
}

// ====== Logging ======
void logEvent(String msg) {
  DateTime now = rtc.now();
  char timebuf[32];
  sprintf(timebuf, "%04d-%02d-%02d %02d:%02d:%02d", now.year(), now.month(),
          now.day(), now.hour(), now.minute(), now.second());
  Serial.print("[");
  Serial.print(timebuf);
  Serial.print("] ");
  Serial.println(msg);
}

void printStatus() {
  Serial.println("===== STATUS =====");
  Serial.print("Current X (steps) = ");
  Serial.println(currentX);
  Serial.print("Current Y (steps) = ");
  Serial.println(currentY);
  Serial.print("X_SPACING = ");
  Serial.println(X_SPACING, 4);
  Serial.print("Y_SPACING = ");
  Serial.println(Y_SPACING, 4);
  Serial.print("Grid: ");
  Serial.print(COLS);
  Serial.print(" x ");
  Serial.println(ROWS);
  DateTime now = rtc.now();
  Serial.print("RTC: ");
  Serial.print(now.timestamp());
  Serial.println();
  Serial.print("Temp: ");
  Serial.print(lastTempC);
  Serial.print("C ");
  Serial.print("Humidity: ");
  Serial.print(lastHum);
  Serial.print("% ");
  Serial.print("SoilA: ");
  Serial.print(lastSoilA);
  Serial.print(" SoilB: ");
  Serial.println(lastSoilB);
  Serial.print("isMoving: ");
  Serial.print(isMoving);
  Serial.print(" isScanning: ");
  Serial.println(isScanning);
  Serial.print("Emergency: ");
  Serial.println(emergencyStop ? "YES" : "NO");
  if (scheduleCount == 0) {
    Serial.println("Scheduled SCANs: (none)");
  } else {
    Serial.print("Scheduled SCANs: ");
    for (int i = 0; i < scheduleCount; i++) {
      char buf[8];
      sprintf(buf, "%02d:%02d", schedules[i].hour, schedules[i].minute);
      Serial.print(buf);
      if (i < scheduleCount - 1)
        Serial.print(", ");
    }
    Serial.println();
  }
  Serial.println("===================");
}

// ====== Manual goto ======
void gotoCell(char rowLetter, int colNum) {
  if (emergencyStop) {
    Serial.println("EMERGENCY STOP ACTIVE - Cannot execute command");
    return;
  }

  int r = toupper(rowLetter) - 'A';
  int c = colNum - 1;
  if (r < 0 || r >= ROWS || c < 0 || c >= COLS) {
    Serial.println("INVALID COORDINATE");
    logEvent("INVALID COORD CMD");
    return;
  }
  long tx = targetXForColumn(c);
  long ty = targetYForRow(r);
  char loc[4];
  loc[0] = 'A' + r;
  loc[1] = '0' + (c + 1);
  loc[2] = 0;
  Serial.print("MOVING TO ");
  Serial.println(loc);
  logEvent(String("MANUAL MOVETO ") + loc);

  isMoving = true;
  moveToStep(tx, ty);
  isMoving = false;

  Serial.println("ARRIVED");
  logEvent(String("ARRIVED ") + loc);

  pump_activate(DWELL_MS);

  unsigned long dwell_start = millis();
  while (millis() - dwell_start < DWELL_MS) {
    if (emergencyStop)
      break;
    systemYield(true);
    delay(10);
  }
  systemYield(true);
}

// ====== Emergency Stop ======
void triggerEmergencyStop() {
  Serial.println("!!! EMERGENCY STOP TRIGGERED !!!");
  logEvent("EMERGENCY STOP");
  emergencyStop = true;
  isMoving = false;
  isScanning = false;

  ledcWrite(PUMP_PIN, 0);
  digitalWrite(LED_GREEN, LOW);
  pump_timer.state = IDLE;

  digitalWrite(EN_PIN, HIGH);
  digitalWrite(LED_RED, HIGH);

  for (int i = 0; i < 3; i++) {
    buzzer_start(200);
    delay(250);
    buzzer_update();
    delay(100);
  }

  Serial.println("Type 'RESET' to resume, or 'HOME' to reset position");
}

void resetFromEmergencyStop() {
  Serial.println("Resetting from emergency stop...");
  logEvent("EMERGENCY STOP RESET");
  emergencyStop = false;
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_GREEN, LOW);
  Serial.println("System ready");
}

// ================= SCHEDULE STORAGE (Preferences) ===================

String schedKeyH(int idx) {
  static char buf[16];
  snprintf(buf, sizeof(buf), "sh%d", idx);
  return String(buf);
}
String schedKeyM(int idx) {
  static char buf[16];
  snprintf(buf, sizeof(buf), "sm%d", idx);
  return String(buf);
}
String schedKeyT(int idx) {
  static char buf[16];
  snprintf(buf, sizeof(buf), "st%d", idx);
  return String(buf);
}
String schedKeyI(int idx) {
  static char buf[16];
  snprintf(buf, sizeof(buf), "si%d", idx);
  return String(buf);
}

void saveSchedulesToPrefs() {
  prefs.begin("sics", false);
  prefs.putInt("sched_count", scheduleCount);
  for (int i = 0; i < scheduleCount && i < MAX_SCHEDULES; i++) {
    prefs.putInt(schedKeyH(i).c_str(), schedules[i].hour);
    prefs.putInt(schedKeyM(i).c_str(), schedules[i].minute);
    prefs.putInt(schedKeyT(i).c_str(), (int)schedules[i].targetType);
    prefs.putInt(schedKeyI(i).c_str(), schedules[i].targetIndex);
  }
  for (int i = scheduleCount; i < MAX_SCHEDULES; i++) {
    prefs.remove(schedKeyH(i).c_str());
    prefs.remove(schedKeyM(i).c_str());
    prefs.remove(schedKeyT(i).c_str());
    prefs.remove(schedKeyI(i).c_str());
  }
  prefs.end();
}

void loadSchedulesFromPrefs() {
  prefs.begin("sics", true);
  scheduleCount = prefs.getInt("sched_count", 0);
  if (scheduleCount < 0)
    scheduleCount = 0;
  if (scheduleCount > MAX_SCHEDULES)
    scheduleCount = MAX_SCHEDULES;
  for (int i = 0; i < scheduleCount; i++) {
    schedules[i].hour = prefs.getInt(schedKeyH(i).c_str(), 0);
    schedules[i].minute = prefs.getInt(schedKeyM(i).c_str(), 0);
    int savedType = prefs.getInt(schedKeyT(i).c_str(), (int)TARGET_FULL);
    if (savedType < TARGET_FULL || savedType > TARGET_COLUMN)
      savedType = TARGET_FULL;
    schedules[i].targetType = (ScheduleTargetType)savedType;
    schedules[i].targetIndex = prefs.getInt(schedKeyI(i).c_str(), -1);
    lastTriggeredDayOfYear[i] = -1;
  }
  for (int i = scheduleCount; i < MAX_SCHEDULES; i++)
    lastTriggeredDayOfYear[i] = -1;
  prefs.end();
  logEvent(String("Loaded schedules: ") + scheduleCount);
  Serial.println(schedulesToString());
}

// parsing time string "HH:MM"
bool parseTimeString(const String &s, int &h, int &m) {
  String t = s;
  t.trim();
  t.replace(" ", "");
  int colon = t.indexOf(':');
  if (colon == -1)
    return false;
  String hs = t.substring(0, colon);
  String ms = t.substring(colon + 1);
  if (hs.length() == 0 || ms.length() == 0)
    return false;
  h = hs.toInt();
  m = ms.toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59)
    return false;
  return true;
}

String scheduleTargetLabel(ScheduleTargetType type, int targetIndex) {
  switch (type) {
  case TARGET_ZONE_A:
    return "ZONA_A";
  case TARGET_ZONE_B:
    return "ZONA_B";
  case TARGET_ROW:
    return String("ROW_") + char('A' + targetIndex);
  case TARGET_COLUMN:
    return String("COL_") + String(targetIndex + 1);
  default:
    return "SCAN";
  }
}

String scheduleTargetLabel(const Schedule &sched) {
  return scheduleTargetLabel(sched.targetType, sched.targetIndex);
}

bool parseScheduleTarget(String token, ScheduleTargetType &type,
                         int &targetIndex) {
  token.trim();
  token.toUpperCase();
  token.replace(" ", "_");
  type = TARGET_FULL;
  targetIndex = -1;
  if (token.length() == 0 || token == "SCAN" || token == "FULL" ||
      token == "FULL_SCAN")
    return true;
  if (token == "ZONA_A" || token == "ZONE_A") {
    type = TARGET_ZONE_A;
    return true;
  }
  if (token == "ZONA_B" || token == "ZONE_B") {
    type = TARGET_ZONE_B;
    return true;
  }
  if (token.startsWith("ROW_")) {
    int rowIndex = token.charAt(4) - 'A';
    if (rowIndex >= 0 && rowIndex < ROWS) {
      type = TARGET_ROW;
      targetIndex = rowIndex;
      return true;
    }
    return false;
  }
  if (token.startsWith("ROW") && token.length() >= 4) {
    int rowIndex = token.charAt(3) - 'A';
    if (rowIndex >= 0 && rowIndex < ROWS) {
      type = TARGET_ROW;
      targetIndex = rowIndex;
      return true;
    }
    return false;
  }
  if (token.startsWith("COL_")) {
    int colIndex = token.substring(4).toInt() - 1;
    if (colIndex >= 0 && colIndex < COLS) {
      type = TARGET_COLUMN;
      targetIndex = colIndex;
      return true;
    }
    return false;
  }
  if (token.startsWith("COLUMN_")) {
    int colIndex = token.substring(7).toInt() - 1;
    if (colIndex >= 0 && colIndex < COLS) {
      type = TARGET_COLUMN;
      targetIndex = colIndex;
      return true;
    }
    return false;
  }
  if (token.startsWith("COL") && token.length() >= 4) {
    int colIndex = token.substring(3).toInt() - 1;
    if (colIndex >= 0 && colIndex < COLS) {
      type = TARGET_COLUMN;
      targetIndex = colIndex;
      return true;
    }
    return false;
  }
  return false;
}

bool parseScheduleArgs(String args, int &h, int &m, ScheduleTargetType &type,
                       int &targetIndex) {
  args.trim();
  int sp = args.indexOf(' ');
  String timePart = sp == -1 ? args : args.substring(0, sp);
  String targetPart = sp == -1 ? "SCAN" : args.substring(sp + 1);
  targetPart.trim();
  if (!parseTimeString(timePart, h, m))
    return false;
  return parseScheduleTarget(targetPart, type, targetIndex);
}

bool scheduleExists(int h, int m, ScheduleTargetType type, int targetIndex) {
  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].hour == h && schedules[i].minute == m &&
        schedules[i].targetType == type &&
        schedules[i].targetIndex == targetIndex)
      return true;
  }
  return false;
}

bool scheduleExists(int h, int m) {
  return scheduleExists(h, m, TARGET_FULL, -1);
}

bool addSchedule(int h, int m, ScheduleTargetType type, int targetIndex) {
  if (scheduleCount >= MAX_SCHEDULES)
    return false;
  if (scheduleExists(h, m, type, targetIndex))
    return false;
  schedules[scheduleCount].hour = h;
  schedules[scheduleCount].minute = m;
  schedules[scheduleCount].targetType = type;
  schedules[scheduleCount].targetIndex = targetIndex;
  lastTriggeredDayOfYear[scheduleCount] = -1;
  scheduleCount++;
  saveSchedulesToPrefs();
  logEvent(String("Added schedule: ") + (h < 10 ? "0" : "") + String(h) + ":" +
           (m < 10 ? "0" : "") + String(m) + " " +
           scheduleTargetLabel(type, targetIndex));
  return true;
}

bool addSchedule(int h, int m) { return addSchedule(h, m, TARGET_FULL, -1); }

bool removeSchedule(int h, int m, ScheduleTargetType type, int targetIndex) {
  int idx = -1;
  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].hour == h && schedules[i].minute == m &&
        schedules[i].targetType == type &&
        schedules[i].targetIndex == targetIndex) {
      idx = i;
      break;
    }
  }
  if (idx == -1)
    return false;
  for (int i = idx; i < scheduleCount - 1; i++) {
    schedules[i] = schedules[i + 1];
    lastTriggeredDayOfYear[i] = lastTriggeredDayOfYear[i + 1];
  }
  scheduleCount--;
  lastTriggeredDayOfYear[scheduleCount] = -1;
  saveSchedulesToPrefs();
  logEvent(String("Removed schedule: ") + (h < 10 ? "0" : "") + String(h) +
           ":" + (m < 10 ? "0" : "") + String(m) + " " +
           scheduleTargetLabel(type, targetIndex));
  return true;
}

bool removeSchedule(int h, int m) {
  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].hour == h && schedules[i].minute == m) {
      return removeSchedule(h, m, schedules[i].targetType,
                            schedules[i].targetIndex);
    }
  }
  return false;
}

void clearAllSchedules() {
  scheduleCount = 0;
  for (int i = 0; i < MAX_SCHEDULES; i++)
    lastTriggeredDayOfYear[i] = -1;
  saveSchedulesToPrefs();
  logEvent("Cleared all schedules");
}

String schedulesToString() {
  if (scheduleCount == 0)
    return String("(none)");
  String s = "";
  for (int i = 0; i < scheduleCount; i++) {
    char buf[8];
    sprintf(buf, "%02d:%02d", schedules[i].hour, schedules[i].minute);
    s += String(buf) + " " + scheduleTargetLabel(schedules[i]);
    if (i < scheduleCount - 1)
      s += ", ";
  }
  return s;
}

void executeScheduleTarget(const Schedule &sched) {
  switch (sched.targetType) {
  case TARGET_ZONE_A:
    scanZonaA();
    break;
  case TARGET_ZONE_B:
    scanZonaB();
    break;
  case TARGET_ROW:
    scanRow(sched.targetIndex);
    break;
  case TARGET_COLUMN:
    scanColumn(sched.targetIndex);
    break;
  default:
    scanCustom();
    break;
  }
}

bool parseCellCommand(String text, char &rowLetter, int &colNum) {
  text.trim();
  text.toUpperCase();
  if (text.startsWith("/"))
    text = text.substring(1);
  if (text.length() != 2)
    return false;
  rowLetter = text.charAt(0);
  colNum = text.substring(1).toInt();
  return rowLetter >= 'A' && rowLetter < ('A' + ROWS) && colNum >= 1 &&
         colNum <= COLS;
}

// ================= TELEGRAM BOT FUNCTIONS ===================
// URL encoding function untuk Telegram messages
String urlEncode(String str) {
  String encodedStr = "";
  char c;
  char code0;
  char code1;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encodedStr += "+";
    } else if (isalnum(c)) {
      encodedStr += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9)
        code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9)
        code0 = c - 10 + 'A';
      encodedStr += '%';
      encodedStr += code0;
      encodedStr += code1;
    }
  }
  return encodedStr;
}

void sendTelegramMessage(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected - Telegram message not sent");
    return;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.setTimeout(3500);

  String url = String(TELEGRAM_API) + TELEGRAM_BOT_TOKEN + "/sendMessage";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(4096);
  doc["chat_id"] = TELEGRAM_CHAT_ID;
  doc["text"] = message;
  doc["parse_mode"] = "Markdown";

  String jsonBody;
  serializeJson(doc, jsonBody);

  Serial.println("Sending Telegram via POST...");
  int httpCode = http.POST(jsonBody);

  if (httpCode == 200) {
    Serial.println("✅ Telegram message sent successfully");
  } else {
    Serial.print("❌ Telegram send failed, HTTP code: ");
    Serial.println(httpCode);
    if (httpCode == 401) {
      Serial.println("⚠️ [TELEGRAM] Token Bot Tidak Valid / Expired (401 "
                     "Unauthorized)! Dapatkan token baru dari @BotFather.");
    }
  }

  http.end();
}

// ================= GOOGLE SHEETS LOGGING ===================
/*
  Panduan Google Apps Script untuk menerima data dari ESP32:
  -------------------------------------------------------------
  function doGet(e) {
    return handleRequest(e);
  }

  function doPost(e) {
    return handleRequest(e);
  }

  function handleRequest(e) {
    var sheet = SpreadsheetApp.getActiveSpreadsheet().getActiveSheet();
    var p = e.parameter;

    var tanggal    = p.tanggal || p.date || "";
    var waktu      = p.waktu || p.time || "";
    var suhu       = p.suhu || p.temp || "";
    var kelembaban = p.kelembaban || p.humidity || "";
    var tanahA     = p.tanah_a || p.soilA || "";
    var tanahB     = p.tanah_b || p.soilB || "";
    var aktuator   = p.aktuator || p.actuator || "";
    var stepper    = p.stepper || "";

    // Simpan baris baru ke Google Sheet
    sheet.appendRow([tanggal, waktu, suhu, kelembaban, tanahA, tanahB, aktuator,
  stepper]);

    return ContentService.createTextOutput("OK")
      .setMimeType(ContentService.MimeType.TEXT);
  }
  -------------------------------------------------------------
*/
bool sendDataToGoogleSheets() {
  if (strlen(GOOGLE_SHEETS_URL) == 0) {
    return false; // URL Google Sheets belum diisi oleh pengguna
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(
        "❌ WiFi belum terhubung - Data Google Sheets tidak dikirim");
    return false;
  }

  readSensors();
  DateTime now = rtc.now();

  char dateStr[16];
  sprintf(dateStr, "%02d/%02d/%04d", now.day(), now.month(), now.year());

  char timeStr[16];
  sprintf(timeStr, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  String suhuStr = isnan(lastTempC) ? "0.0" : String(lastTempC, 1);
  String kelembabanStr = isnan(lastHum) ? "0.0" : String(lastHum, 1);
  String tanahAStr = lastSoilA;
  String tanahBStr = lastSoilB;

  // Status Aktuator (Pompa & Buzzer)
  String pompaStatus = (pump_timer.state == ACTIVE) ? "ON" : "OFF";
  String buzzerStatus = (buzzer_timer.state == ACTIVE) ? "ON" : "OFF";
  String actuatorStr = "Pompa: " + pompaStatus + ", Buzzer: " + buzzerStatus;

  // Status Stepper (Koordinat & Gerakan)
  String motionStatus =
      emergencyStop
          ? "EMERGENCY_STOP"
          : (isScanning ? "SCANNING" : (isMoving ? "MOVING" : "IDLE"));
  String stepperStr = "X: " + String(currentX) + ", Y: " + String(currentY) +
                      " (" + motionStatus + ")";

  // Susun Query Parameter (Mendukung parameter bahasa Indonesia & Inggris)
  String query = "";
  query += "tanggal=" + urlEncode(String(dateStr));
  query += "&waktu=" + urlEncode(String(timeStr));
  query += "&suhu=" + urlEncode(suhuStr);
  query += "&kelembaban=" + urlEncode(kelembabanStr);
  query += "&tanah_a=" + urlEncode(tanahAStr);
  query += "&tanah_b=" + urlEncode(tanahBStr);
  query += "&aktuator=" + urlEncode(actuatorStr);
  query += "&stepper=" + urlEncode(stepperStr);
  query += "&pos_x=" + String(currentX);
  query += "&pos_y=" + String(currentY);
  query += "&pompa=" + urlEncode(pompaStatus);
  query += "&buzzer=" + urlEncode(buzzerStatus);
  query += "&date=" + urlEncode(String(dateStr));
  query += "&time=" + urlEncode(String(timeStr));
  query += "&temp=" + urlEncode(suhuStr);
  query += "&humidity=" + urlEncode(kelembabanStr);
  query += "&soilA=" + urlEncode(tanahAStr);
  query += "&soilB=" + urlEncode(tanahBStr);
  query += "&actuator=" + urlEncode(actuatorStr);

  String fullUrl = String(GOOGLE_SHEETS_URL);
  if (fullUrl.indexOf('?') == -1) {
    fullUrl += "?" + query;
  } else {
    fullUrl += "&" + query;
  }

  Serial.println("📊 Mengirim telemetri ke Google Sheets...");
  Serial.println("URL: " + fullUrl);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);

  bool success = false;
  if (fullUrl.startsWith("https://")) {
    http.begin(client, fullUrl);
  } else {
    http.begin(fullUrl);
  }

  int httpCode = http.GET();

  if (httpCode == 200 || httpCode == 302) {
    Serial.println("✅ Data berhasil dikirim ke Google Sheets! (HTTP " +
                   String(httpCode) + ")");
    success = true;
  } else {
    Serial.print("❌ Gagal mengirim ke Google Sheets, HTTP code: ");
    Serial.println(httpCode);
  }

  http.end();
  return success;
}

// ================= CLOUD INTEGRATION (polmedresearchcenter.id)
// ===================

void executeCloudCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0)
    return;
  Serial.print("📡 [Cloud] Menerima Perintah: ");
  Serial.println(cmd);

  String upper = cmd;
  upper.toUpperCase();

  if (upper == "STATUS" || upper == "/STATUS") {
    logEvent("CLOUD CMD STATUS");
    Serial.println(
        "📱 [Telegram] Mengirim update status sistem lengkap ke Bot Telegram...");
    sendTelegramMessage(getSystemStatus());
  } else if (upper == "SCAN" || upper == "/SCAN") {
    if (!emergencyStop) {
      logEvent("CLOUD CMD SCAN");
      sendTelegramMessage("🚀 *[SICS Gantry] Full Scan Dimulai!*\n"
                          "Menyiram seluruh 48 sel tanaman (H1 s/d A1) dengan jeda 2000ms per sel.");
      scanCustom();
      sendTelegramMessage("✅ *[SICS Gantry] Full Scan Selesai!*\n"
                          "Seluruh 48 sel tanaman telah berhasil disiram.\n"
                          "📍 Gantry kembali ke posisi Home (X: 0, Y: 0).");
    } else {
      sendTelegramMessage("❌ *Emergency Stop Aktif!* Perintah SCAN dibatalkan demi keselamatan.");
    }
  } else if (upper == "ZONA A" || upper == "ZONA_A" || upper == "/ZONA_A") {
    if (!emergencyStop) {
      logEvent("CLOUD CMD ZONA A");
      sendTelegramMessage("🚀 *[SICS Gantry] Scan Zona A Dimulai!*\n"
                          "Menyiram 24 sel tanaman Baris H, G, F, E (Sel H1 s/d E6).");
      scanZonaA();
      sendTelegramMessage("✅ *[SICS Gantry] Scan Zona A Selesai!*\n"
                          "Penyiraman 24 sel Zona A tuntas. Gantry kembali ke Home.");
    } else {
      sendTelegramMessage("❌ *Emergency Stop Aktif!* Scan Zona A dibatalkan.");
    }
  } else if (upper == "ZONA B" || upper == "ZONA_B" || upper == "/ZONA_B") {
    if (!emergencyStop) {
      logEvent("CLOUD CMD ZONA B");
      sendTelegramMessage("🚀 *[SICS Gantry] Scan Zona B Dimulai!*\n"
                          "Menyiram 24 sel tanaman Baris D, C, B, A (Sel D1 s/d A6).");
      scanZonaB();
      sendTelegramMessage("✅ *[SICS Gantry] Scan Zona B Selesai!*\n"
                          "Penyiraman 24 sel Zona B tuntas. Gantry kembali ke Home.");
    } else {
      sendTelegramMessage("❌ *Emergency Stop Aktif!* Scan Zona B dibatalkan.");
    }
  } else if (upper.startsWith("ROW_")) {
    char rowLetter = upper.charAt(4);
    int rowIndex = rowLetter - 'A';
    if (rowIndex >= 0 && rowIndex < ROWS && !emergencyStop) {
      logEvent(String("CLOUD CMD ") + upper);
      sendTelegramMessage(String("🚀 *[SICS Gantry] Irigasi Baris ") + rowLetter + " Dimulai!*\n" +
                          "Menyiram 6 sel tanaman pada Baris " + rowLetter + " (Kolom 1 - 6)...");
      scanRow(rowIndex);
      sendTelegramMessage(String("✅ *[SICS Gantry] Irigasi Baris ") + rowLetter + " Selesai!*");
    }
  } else if (upper.startsWith("COL_")) {
    int colNum = upper.substring(4).toInt();
    int colIndex = colNum - 1;
    if (colIndex >= 0 && colIndex < COLS && !emergencyStop) {
      logEvent(String("CLOUD CMD ") + upper);
      sendTelegramMessage(String("🚀 *[SICS Gantry] Irigasi Kolom ") + colNum + " Dimulai!*\n" +
                          "Menyiram 8 sel tanaman pada Kolom " + colNum + " (Baris H - A)...");
      scanColumn(colIndex);
      sendTelegramMessage(String("✅ *[SICS Gantry] Irigasi Kolom ") + colNum + " Selesai!*");
    }
  } else if (upper == "HOME" || upper == "/HOME") {
    if (!emergencyStop) {
      logEvent("CLOUD CMD HOME");
      sendTelegramMessage("🏠 *[SICS Gantry] Kalibrasi Homing Dimulai...*\nMenyelaraskan sumbu X & Y ke limit switch.");
      if (homeX() && homeY()) {
        sendTelegramMessage("✅ *[SICS Gantry] Homing Berhasil!*\nPosisi Toolhead sekarang: X=0.0 steps, Y=0.0 steps (Home).");
      } else {
        sendTelegramMessage("❌ *[SICS Gantry] Homing Gagal!*\nPeriksa saklar limit switch.");
      }
    }
  } else if (upper == "ES" || upper == "STOP" || upper == "/STOP") {
    logEvent("CLOUD CMD EMERGENCY STOP");
    triggerEmergencyStop();
    sendTelegramMessage("⛔ *[SICS Gantry] EMERGENCY STOP AKTIF!*\nSeluruh gerakan motor stepper dan pompa air diputus segera demi keselamatan. Ketik /RESET untuk memulihkan.");
  } else if (upper == "RESET" || upper == "/RESET") {
    logEvent("CLOUD CMD RESET");
    resetFromEmergencyStop();
    sendTelegramMessage("🔄 *[SICS Gantry] Sistem Berhasil Direset!*\nDriver motor diaktifkan kembali. Sistem siap menerima perintah baru.");
  } else if (upper.startsWith("GOTO")) {
    String cell = "";
    int cellIdx = upper.indexOf("CELL=");
    if (cellIdx != -1) {
      cell = upper.substring(cellIdx + 5);
    } else {
      int sp = upper.indexOf(' ');
      if (sp != -1) {
        cell = upper.substring(sp + 1);
      }
    }
    cell.trim();
    if (cell.length() >= 2 && !emergencyStop) {
      char row = cell.charAt(0);
      int col = cell.substring(1).toInt();
      if (col > 0 && col <= COLS) {
        logEvent(String("CLOUD CMD GOTO ") + cell);
        sendTelegramMessage(String("📍 *[SICS Gantry] Menuju Sel ") + cell + "*\nToolhead gantry bergerak ke koordinat target dan menyiram sel...");
        gotoCell(row, col);
        sendTelegramMessage(String("✅ *[SICS Gantry] Sel ") + cell + " Berhasil Disiram!*");
        readSensors();
        DateTime now = rtc.now();
        updateLCDMode(now);
      }
    }
  } else if (upper.startsWith("SCHEDULE ")) {
    String rest = cmd.substring(9);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp != -1) {
      String timestr = rest.substring(0, sp);
      String targetstr = rest.substring(sp + 1);
      int h, m, targetIndex;
      ScheduleTargetType targetType;
      if (parseTimeString(timestr, h, m) &&
          parseScheduleTarget(targetstr, targetType, targetIndex)) {
        addSchedule(h, m, targetType, targetIndex);
        logEvent(String("CLOUD ADD SCHEDULE: ") + timestr + " " + targetstr);
      }
    }
  } else if (upper.startsWith("DELSCHEDULE ")) {
    String rest = cmd.substring(12);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp != -1) {
      String timestr = rest.substring(0, sp);
      String targetstr = rest.substring(sp + 1);
      int h, m, targetIndex;
      ScheduleTargetType targetType;
      if (parseTimeString(timestr, h, m) &&
          parseScheduleTarget(targetstr, targetType, targetIndex)) {
        removeSchedule(h, m, targetType, targetIndex);
        logEvent(String("CLOUD DEL SCHEDULE: ") + timestr + " " + targetstr);
      }
    }
  } else if (upper == "CLEARSCHEDULES") {
    clearAllSchedules();
    logEvent("CLOUD CLEAR ALL SCHEDULES");
  }
}

void printSerialStatusDashboard(int cloudHttpCode) {
  readSensors();
  DateTime now = rtc.now();

  char timeStr[16];
  sprintf(timeStr, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  char dateStr[16];
  sprintf(dateStr, "%02d/%02d/%04d", now.day(), now.month(), now.year());

  Serial.println(
      F("\n========================================================"));
  Serial.println(F("📡 [STATUS JARINGAN & INTERNET]"));

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("  • Status WiFi      : ✅ TERHUBUNG (SSID: "));
    Serial.print(ssid);
    Serial.print(F(" | RSSI: "));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm)"));

    Serial.print(F("  • IP Lokal ESP32   : "));
    Serial.println(WiFi.localIP());

    Serial.print(F("  • Cloud Server     : "));
    if (cloudHttpCode == 200) {
      Serial.println(F("✅ ONLINE (polmedresearchcenter.id - HTTP 200 OK)"));
    } else if (cloudHttpCode > 0) {
      Serial.print(F("⚠️ RESPON ERROR (HTTP "));
      Serial.print(cloudHttpCode);
      Serial.println(F(")"));
    } else if (cloudHttpCode == -1) {
      Serial.println(F("❌ GAGAL (Koneksi Ditolak / Timeout / DNS)"));
    } else if (cloudHttpCode == -5) {
      Serial.println(F("❌ GAGAL (Koneksi Putus / SSL Error)"));
    } else {
      Serial.println(F("⏳ Menghubungi server hosting..."));
    }
  } else {
    Serial.print(
        F("  • Status WiFi      : ❌ TERPUTUS (Mencoba hubungkan ke: "));
    Serial.print(ssid);
    Serial.println(F("...)"));
    Serial.println(F("  • IP Lokal ESP32   : -"));
    Serial.println(F(
        "  • Cloud Server     : ❌ OFFLINE (Tidak ada koneksi WiFi/Internet)"));
  }

  Serial.println(F("--------------------------------------------------------"));
  Serial.println(F("🌿 [STATUS SISTEM & SENSOR]"));
  Serial.print(F("  • Waktu RTC DS3231 : "));
  Serial.print(dateStr);
  Serial.print(F(" "));
  Serial.println(timeStr);

  if (!isnan(lastTempC) && !isnan(lastHum)) {
    Serial.print(F("  • Suhu & Kelembapan: "));
    Serial.print(lastTempC, 1);
    Serial.print(F(" °C | "));
    Serial.print(lastHum, 0);
    Serial.println(F(" % RH"));
  } else {
    Serial.println(
        F("  • Suhu & Kelembapan: --.- °C | -- % RH (Sensor DHT22)"));
  }

  Serial.print(F("  • Tanah Zona A     : "));
  Serial.print(lastSoilA);
  Serial.print(F(" (Nilai ADC: "));
  Serial.print(lastAdcA);
  Serial.println(F(")"));

  Serial.print(F("  • Tanah Zona B     : "));
  Serial.print(lastSoilB);
  Serial.print(F(" (Nilai ADC: "));
  Serial.print(lastAdcB);
  Serial.println(F(")"));

  Serial.print(F("  • Posisi Gantry    : X: "));
  Serial.print((float)currentX, 1);
  Serial.print(F(" | Y: "));
  Serial.print((float)currentY, 1);
  Serial.print(F(" steps ("));
  Serial.print(isMoving ? "BERGERAK" : (isScanning ? "SCANNING" : "IDLE"));
  Serial.println(F(")"));

  Serial.print(F("  • Status Pompa Air : "));
  Serial.println((pump_timer.state == ACTIVE) ? "AKTIF (PWM 100%)" : "OFF");

  Serial.print(F("  • Emergency Stop   : "));
  Serial.println(emergencyStop ? "⛔ AKTIF" : "NORMAL");
  Serial.println(F("========================================================"));
}

bool sendTelemetryToCloud() {
  if (strlen(CLOUD_SERVER_URL) == 0)
    return false;
  if (WiFi.status() != WL_CONNECTED) {
    printSerialStatusDashboard(0);
    return false;
  }

  readSensors();
  DateTime now = rtc.now();

  char timeStr[16];
  sprintf(timeStr, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  DynamicJsonDocument doc(2048);
  doc["temp"] = isnan(lastTempC) ? 0.0 : lastTempC;
  doc["humidity"] = isnan(lastHum) ? 0.0 : lastHum;
  doc["soilA"] = lastSoilA;
  doc["soilB"] = lastSoilB;
  doc["adcA"] = lastAdcA;
  doc["adcB"] = lastAdcB;
  doc["rtcTime"] = String(timeStr);
  doc["posX"] = currentX;
  doc["posY"] = currentY;
  doc["isMoving"] = isMoving;
  doc["isScanning"] = isScanning;
  doc["emergencyStop"] = emergencyStop;

  JsonArray arr = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount; i++) {
    char buf[8];
    sprintf(buf, "%02d:%02d", schedules[i].hour, schedules[i].minute);
    JsonObject it = arr.createNestedObject();
    it["time"] = String(buf);
    it["target"] = scheduleTargetLabel(schedules[i]);
    it["type"] = scheduleTargetLabel(schedules[i]);
  }

  String jsonPayload;
  serializeJson(doc, jsonPayload);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(true);
  http.setTimeout(2500);

  bool success = false;
  String urlStr = String(CLOUD_SERVER_URL);

  if (urlStr.startsWith("https://")) {
    http.begin(client, urlStr);
  } else {
    http.begin(urlStr);
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(jsonPayload);

  // Tampilkan status jaringan & sensor di Serial Monitor
  printSerialStatusDashboard(httpCode);

  if (httpCode == 200) {
    String response = http.getString();
    DynamicJsonDocument respDoc(512);
    DeserializationError err = deserializeJson(respDoc, response);
    if (!err && respDoc.containsKey("command")) {
      String cmd = respDoc["command"].as<String>();
      cmd.trim();
      if (cmd.length() > 0) {
        String upperCmd = cmd;
        upperCmd.toUpperCase();
        if (upperCmd == "ES" || upperCmd == "STOP" || upperCmd == "/STOP" ||
            upperCmd == "RESET" || upperCmd == "/RESET" || !isScanning) {
          executeCloudCommand(cmd);
        }
      }
    }
    success = true;
  }

  http.end();
  return success;
}

String getSystemStatus() {
  readSensors();
  DateTime now = rtc.now();

  char timeStr[20];
  sprintf(timeStr, "%02d:%02d:%02d WIB", now.hour(), now.minute(), now.second());
  char dateStr[24];
  sprintf(dateStr, "%02d/%02d/%04d", now.day(), now.month(), now.year());

  String sched = schedulesToString();

  // Estimasi sel gantry saat ini
  int colEst = (int)round(currentX / X_SPACING) + 1;
  int rowEst = (ROWS - 1) - (int)round(currentY / Y_SPACING);
  char cellEst[32];
  if (currentX == 0 && currentY == 0) {
    strcpy(cellEst, "Home (X:0, Y:0)");
  } else if (rowEst >= 0 && rowEst < ROWS && colEst >= 1 && colEst <= COLS) {
    sprintf(cellEst, "Sel %c%d (%s)", 'A' + rowEst, colEst, (rowEst >= 4 ? "Zona A" : "Zona B"));
  } else {
    strcpy(cellEst, "Posisi Kustom");
  }

  // Hitung RSSI WiFi
  int rssi = WiFi.RSSI();
  String wifiQuality;
  if (rssi >= -60) wifiQuality = "Sangat Kuat";
  else if (rssi >= -70) wifiQuality = "Kuat";
  else if (rssi >= -80) wifiQuality = "Sedang";
  else wifiQuality = "Lemah";

  String s = "";
  s.reserve(1600);
  s += "🌱 *SICS GANTRY WATERING V2.0*\n";
  s += "🏢 *Polmed Research Center - Lab EK-6B*\n";
  s += "━━━━━━━━━━━━━━━━━━━━━━\n";
  s += "⏰ *Waktu RTC:* `" + String(timeStr) + "`\n";
  s += "📅 *Tanggal:* `" + String(dateStr) + "`\n\n";

  s += "🌡️ *Suhu Udara:* `" + (isnan(lastTempC) ? String("-") : String(lastTempC, 1)) + " °C` _(Ideal: 24.0 - 32.0°C)_\n";
  s += "💧 *Kelembapan Udara:* `" + (isnan(lastHum) ? String("-") : String((int)round(lastHum))) + "% RH` _(Normal: 60 - 85% RH)_\n\n";

  s += "🌿 *Tanah Zona A (Baris H-E):* *" + lastSoilA + "*\n";
  s += "   └ ADC: `~" + String(lastAdcA) + "` (24 Sel Tanaman - Pin 34)\n";
  s += "🌿 *Tanah Zona B (Baris D-A):* *" + lastSoilB + "*\n";
  s += "   └ ADC: `~" + String(lastAdcB) + "` (24 Sel Tanaman - Pin 35)\n\n";

  s += "📍 *Posisi Gantry:* `X: " + String(currentX) + " | Y: " + String(currentY) + "` steps\n";
  s += "   └ Lokasi: *" + String(cellEst) + "*\n";
  s += "⚡ *Driver Stepper:* " + String(emergencyStop ? "⛔ *Disabled*" : "🟢 *Active*") + "\n";
  s += "🚜 *Status Gerak:* " + String(isMoving ? "🏃 *BERGERAK...*" : (isScanning ? "🚿 *SCANNING (MENYIRAM)...*" : "💤 *IDLE*")) + "\n";
  s += "🚿 *Pompa Air (Pin 27):* " + String(pump_timer.state == ACTIVE ? "🟢 *AKTIF (PWM 100%)*" : "⚪ *OFF*") + "\n";
  s += "⛔ *Emergency Stop:* " + String(emergencyStop ? "⛔ *AKTIF (TERKUNCI)*" : "🟢 *Normal (Aman)*") + "\n\n";

  s += "⏱️ *Jadwal Penyiraman di Memori:* (" + String(scheduleCount) + " aktif)\n";
  s += "   " + sched + "\n\n";

  s += "📶 *WiFi:* `" + String(ssid) + "` (" + String(rssi) + " dBm - " + wifiQuality + ")\n";
  s += "🌐 *IP ESP32:* `" + WiFi.localIP().toString() + "`\n";
  s += "☁️ *Cloud Server:* `polmedresearchcenter.id/api/sics`\n";
  s += "━━━━━━━━━━━━━━━━━━━━━━\n";
  s += "_Status Real-time Seluruh Sistem SICS_";

  return s;
}

// Handle Telegram commands with update_id tracking, support schedule management
void handleTelegramCommands() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.setTimeout(3000);

  String url = String(TELEGRAM_API);
  url += TELEGRAM_BOT_TOKEN;
  url += "/getUpdates?allowed_updates=[\"message\"]&timeout=0";

  if (lastUpdateId > 0) {
    url += "&offset=";
    url += (lastUpdateId + 1);
  }

  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode != 200) {
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.println("JSON Parse Error: " + String(error.c_str()));
    return;
  }

  if (!doc["result"].is<JsonArray>()) {
    return;
  }

  JsonArray result = doc["result"];

  for (JsonObject item : result) {
    int update_id = item["update_id"];
    if (update_id > lastUpdateId) {
      lastUpdateId = update_id;
    }

    if (item["message"].isNull()) {
      continue;
    }

    JsonObject message = item["message"];
    if (message["text"].isNull())
      continue;

    String text = message["text"].as<String>();
    text.trim();
    String original = text;
    text.toUpperCase();

    Serial.println("Telegram Command: " + text);
    char rowLetter = 0;
    int colNum = 0;

    // Status Command
    if (text == "/STATUS" || text == "STATUS") {
      sendTelegramMessage(getSystemStatus());
    }
    // Add Schedule: /SETSCAN HH:MM [SCAN|ZONA_A|ZONA_B|ROW_A|COL_1]
    else if (text.startsWith("/SETSCAN") || text.startsWith("/ADDSCAN") ||
             text.startsWith("SETSCAN") || text.startsWith("ADDSCAN")) {
      int sp = original.indexOf(' ');
      if (sp == -1) {
        sendTelegramMessage("❌ Gunakan: /SETSCAN HH:MM TARGET\nContoh: "
                            "/SETSCAN 07:00 ROW_A atau /SETSCAN 17:30 COL_3");
      } else {
        int h, m, targetIndex;
        ScheduleTargetType targetType;
        if (parseScheduleArgs(original.substring(sp + 1), h, m, targetType,
                              targetIndex)) {
          if (addSchedule(h, m, targetType, targetIndex)) {
            String ip = (WiFi.status() == WL_CONNECTED)
                            ? WiFi.localIP().toString()
                            : String("unknown");
            String msg = String("✅ Jadwal ditambahkan: ") +
                         (h < 10 ? "0" : "") + String(h) + ":" +
                         (m < 10 ? "0" : "") + String(m) + " " +
                         scheduleTargetLabel(targetType, targetIndex) +
                         "\n🔗 Web UI: http://" + ip +
                         "/\n\nJadwal saat ini: " + schedulesToString();
            sendTelegramMessage(msg);
          } else {
            sendTelegramMessage(
                "❌ Gagal menambah jadwal (duplikat atau penuh). Saat ini: " +
                schedulesToString());
          }
        } else {
          sendTelegramMessage(
              "❌ Format salah. Gunakan /SETSCAN HH:MM TARGET. Target: SCAN, "
              "ZONA_A, ZONA_B, ROW_A-ROW_H, COL_1-COL_6");
        }
      }
    }
    // Remove schedule: /UNSETSCAN HH:MM [TARGET] or /DELSCAN HH:MM [TARGET]
    else if (text.startsWith("/UNSETSCAN") || text.startsWith("/DELSCAN") ||
             text.startsWith("UNSETSCAN") || text.startsWith("DELSCAN")) {
      int sp = original.indexOf(' ');
      if (sp == -1) {
        sendTelegramMessage("❌ Gunakan: /UNSETSCAN HH:MM TARGET");
      } else {
        int h, m, targetIndex;
        ScheduleTargetType targetType;
        if (parseScheduleArgs(original.substring(sp + 1), h, m, targetType,
                              targetIndex)) {
          if (removeSchedule(h, m, targetType, targetIndex)) {
            String ip = (WiFi.status() == WL_CONNECTED)
                            ? WiFi.localIP().toString()
                            : String("unknown");
            String msg = String("✅ Jadwal dihapus: ") + (h < 10 ? "0" : "") +
                         String(h) + ":" + (m < 10 ? "0" : "") + String(m) +
                         " " + scheduleTargetLabel(targetType, targetIndex) +
                         "\n🔗 Web UI: http://" + ip +
                         "/\n\nJadwal saat ini: " + schedulesToString();
            sendTelegramMessage(msg);
          } else {
            sendTelegramMessage("❌ Jadwal tidak ditemukan. Saat ini: " +
                                schedulesToString());
          }
        } else {
          sendTelegramMessage(
              "❌ Format salah. Gunakan /UNSETSCAN HH:MM TARGET");
        }
      }
    }
    // List schedules
    else if (text == "/LISTSCAN" || text == "LISTSCAN") {
      String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString()
                                                  : String("unknown");
      String msg = String("📅 Current Scheduled SCANs:\n") +
                   schedulesToString() + "\n\n🔗 Web UI: http://" + ip + "/";
      sendTelegramMessage(msg);
    }
    // Direct coordinate command, example: /G3
    else if (parseCellCommand(text, rowLetter, colNum)) {
      if (!emergencyStop) {
        sendTelegramMessage(String("📍 *[SICS Gantry] Menuju Koordinat ") + rowLetter +
                            colNum + "*\nToolhead gantry bergerak dan mulai menyiram...");
        gotoCell(rowLetter, colNum);
        sendTelegramMessage(String("✅ *[SICS Gantry] Penyiraman Sel ") + rowLetter +
                            colNum + " Selesai!*");
      } else {
        sendTelegramMessage("❌ *Emergency Stop Aktif!* Buka kunci dengan /RESET.");
      }
    }
    // Scan Commands
    else if (text == "/SCAN" || text == "SCAN") {
      if (!emergencyStop) {
        sendTelegramMessage("🚀 *[SICS Gantry] Full Scan Dimulai!*\n"
                            "Menyiram seluruh 48 sel tanaman (H1 s/d A1) dengan jeda 2000ms per sel.");
        scanCustom();
        sendTelegramMessage("✅ *[SICS Gantry] Full Scan Selesai!*\n"
                            "Seluruh 48 sel tanaman telah berhasil disiram.\n"
                            "📍 Gantry kembali ke posisi Home (X: 0, Y: 0).");
      } else {
        sendTelegramMessage("❌ *Emergency Stop Aktif!* Buka kunci dengan /RESET.");
      }
    } else if (text == "/ZONA_A" || text == "ZONA_A") {
      if (!emergencyStop) {
        sendTelegramMessage("🚀 *[SICS Gantry] Scan Zona A Dimulai!*\n"
                            "Menyiram 24 sel tanaman Baris H, G, F, E.");
        scanZonaA();
        sendTelegramMessage("✅ *[SICS Gantry] Scan Zona A Selesai!*\n"
                            "Penyiraman 24 sel Zona A tuntas. Gantry kembali ke Home.");
      } else {
        sendTelegramMessage("❌ *Emergency Stop Aktif!* Buka kunci dengan /RESET.");
      }
    } else if (text == "/ZONA_B" || text == "ZONA_B") {
      if (!emergencyStop) {
        sendTelegramMessage("🚀 *[SICS Gantry] Scan Zona B Dimulai!*\n"
                            "Menyiram 24 sel tanaman Baris D, C, B, A.");
        scanZonaB();
        sendTelegramMessage("✅ *[SICS Gantry] Scan Zona B Selesai!*\n"
                            "Penyiraman 24 sel Zona B tuntas. Gantry kembali ke Home.");
      } else {
        sendTelegramMessage("❌ *Emergency Stop Aktif!* Buka kunci dengan /RESET.");
      }
    }
    // Row Commands (ROW_A sampai ROW_H)
    else if (text.startsWith("/ROW_") || text.startsWith("ROW_")) {
      int startIdx = text.startsWith("/ROW_") ? 5 : 4;
      char rowLetter = text.charAt(startIdx);
      int rowIndex = rowLetter - 'A';

      if (rowIndex >= 0 && rowIndex < ROWS && !emergencyStop) {
        sendTelegramMessage(String("🚀 *[SICS Gantry] Irigasi Baris ") + rowLetter + " Dimulai!*\n" +
                            "Menyiram 6 sel tanaman pada Baris " + rowLetter + " (Kolom 1 - 6)...");
        scanRow(rowIndex);
        sendTelegramMessage(String("✅ *[SICS Gantry] Irigasi Baris ") + rowLetter + " Selesai!*");
      } else {
        sendTelegramMessage("❌ *Invalid Row atau Emergency Stop aktif!*");
      }
    }
    // Column Commands (COL_1 sampai COL_6)
    else if (text.startsWith("/COL_") || text.startsWith("COL_")) {
      int startIdx = text.startsWith("/COL_") ? 5 : 4;
      int colNum = text.substring(startIdx).toInt();
      int colIndex = colNum - 1;

      if (colIndex >= 0 && colIndex < COLS && !emergencyStop) {
        sendTelegramMessage(String("🚀 *[SICS Gantry] Irigasi Kolom ") + colNum + " Dimulai!*\n" +
                            "Menyiram 8 sel tanaman pada Kolom " + colNum + " (Baris H - A)...");
        scanColumn(colIndex);
        sendTelegramMessage(String("✅ *[SICS Gantry] Irigasi Kolom ") + colNum + " Selesai!*");
      } else {
        sendTelegramMessage("❌ *Invalid Column atau Emergency Stop aktif!*");
      }
    }
    // Home Command
    else if (text == "/HOME" || text == "HOME") {
      if (!emergencyStop) {
        sendTelegramMessage("🏠 *[SICS Gantry] Kalibrasi Homing Dimulai...*\nMenyelaraskan sumbu X & Y ke limit switch.");
        if (homeX() && homeY()) {
          sendTelegramMessage("✅ *[SICS Gantry] Homing Berhasil!*\nPosisi Toolhead sekarang: X=0.0 steps, Y=0.0 steps (Home).");
        } else {
          sendTelegramMessage("❌ *[SICS Gantry] Homing Gagal!*\nPeriksa limit switch.");
        }
      } else {
        sendTelegramMessage("❌ *Emergency Stop Aktif!* Buka kunci dengan /RESET.");
      }
    }
    // Emergency Stop
    else if (text == "/STOP" || text == "STOP" || text == "/ES") {
      triggerEmergencyStop();
      sendTelegramMessage("⛔ *[SICS Gantry] EMERGENCY STOP AKTIF!*\nSeluruh gerakan motor stepper dan pompa air diputus segera demi keselamatan. Ketik /RESET untuk memulihkan.");
    }
    // Reset
    else if (text == "/RESET" || text == "RESET") {
      resetFromEmergencyStop();
      sendTelegramMessage("🔄 *[SICS Gantry] Sistem Berhasil Direset!*\nDriver motor diaktifkan kembali. Sistem siap menerima perintah baru.");
    }
    // Google Sheets Command
    else if (text == "/GSHEET" || text == "/SHEET") {
      if (strlen(GOOGLE_SHEETS_URL) == 0) {
        sendTelegramMessage("⚠️ URL Google Sheets belum diatur di firmware.");
      } else {
        sendTelegramMessage("📊 Mengirim data terbaru ke Google Sheets...");
        if (sendDataToGoogleSheets()) {
          sendTelegramMessage("✅ Data berhasil dicatat ke Google Sheets!");
        } else {
          sendTelegramMessage(
              "❌ Gagal mengirim ke Google Sheets. Cek koneksi / URL.");
        }
      }
    }
    // Help Command
    else if (text == "/START" || text == "/HELP" || text == "HELP") {
      String helpText =
          "*🌱 SICS Gantry Bot Commands*\n\n"
          "*Scan Commands:*\n"
          "/SCAN - Full Scan\n"
          "/ZONA_A - Zona A Scan\n"
          "/ZONA_B - Zona B Scan\n\n"
          "*Row Watering:*\n"
          "/ROW_A to /ROW_H\n\n"
          "*Column Watering:*\n"
          "/COL_1 to /COL_6\n\n"
          "*System Control:*\n"
          "/STATUS - Show Status\n"
          "/HOME - Home Position\n"
          "/STOP or /ES - Emergency Stop\n"
          "/RESET - Reset System\n"
          "/GSHEET - Kirim data ke Google Sheets\n\n"
          "*Schedule:*\n"
          "/SETSCAN HH:MM TARGET - Add schedule\n"
          "/UNSETSCAN HH:MM TARGET - Remove schedule\n"
          "/LISTSCAN - List saved schedules\n\nTarget: SCAN, ZONA_A, ZONA_B, "
          "ROW_A-ROW_H, COL_1-COL_6\nContoh koordinat langsung: /G3\n";

      sendTelegramMessage(helpText);
    }
  }
}

// ================= WEB SERVER HANDLERS ===================

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SICS Gantry Watering V2.0 - Monitoring Irigasi</title>
  <style>
    :root {
      --bg-page: #f8fafc;
      --bg-card: #ffffff;
      --bg-subtle: #f1f5f9;
      --border-light: #e2e8f0;
      --border-medium: #cbd5e1;
      --text-main: #0f172a;
      --text-body: #334155;
      --text-muted: #64748b;
      --color-primary: #2563eb;
      --color-primary-hover: #1d4ed8;
      --color-success: #16a34a;
      --color-success-bg: #dcfce7;
      --color-success-text: #166534;
      --color-warning: #d97706;
      --color-warning-bg: #fef3c7;
      --color-warning-text: #92400e;
      --color-danger: #dc2626;
      --color-danger-hover: #b91c1c;
      --color-danger-bg: #fee2e2;
      --color-danger-text: #991b1b;
      --color-zone-a: #0284c7;
      --color-zone-b: #7c3aed;
      --font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      --font-mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      --radius-sm: 4px;
      --radius-md: 6px;
      --radius-lg: 8px;
      --radius-pill: 9999px;
      --shadow-sm: 0 1px 2px 0 rgba(0, 0, 0, 0.05);
    }
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: var(--font-family); background-color: var(--bg-page); color: var(--text-body); line-height: 1.5; font-size: 14px; }
    .navbar { background: var(--bg-card); border-bottom: 1px solid var(--border-light); padding: 12px 24px; position: sticky; top: 0; z-index: 50; box-shadow: var(--shadow-sm); }
    .navbar-container { max-width: 1200px; margin: 0 auto; display: flex; align-items: center; justify-content: space-between; gap: 16px; flex-wrap: wrap; }
    .brand-title { font-size: 1.15rem; font-weight: 700; color: var(--text-main); }
    .brand-desc { font-size: 0.75rem; color: var(--text-muted); }
    .author-card { background: var(--bg-subtle); border: 1px solid var(--border-light); padding: 4px 12px; border-radius: var(--radius-md); }
    .author-name { font-size: 0.8rem; font-weight: 600; color: var(--text-main); }
    .author-meta { font-size: 0.7rem; color: var(--text-muted); }
    .navbar-actions { display: flex; align-items: center; gap: 10px; }
    .rtc-clock { background: var(--bg-subtle); border: 1px solid var(--border-light); padding: 4px 10px; border-radius: var(--radius-sm); text-align: right; }
    .rtc-label { display: block; font-size: 0.6rem; color: var(--text-muted); text-transform: uppercase; font-weight: 600; }
    .rtc-time { font-family: var(--font-mono); font-size: 0.9rem; font-weight: 700; color: var(--color-primary); }
    .btn-estop { background: var(--color-danger); color: white; border: 1px solid #b91c1c; padding: 6px 14px; border-radius: var(--radius-md); font-weight: 700; font-size: 0.85rem; cursor: pointer; }
    .btn-estop:hover { background: var(--color-danger-hover); }
    .estop-alert { background: var(--color-danger-bg); border-bottom: 1px solid #fca5a5; color: var(--color-danger-text); padding: 8px 24px; }
    .estop-alert-inner { max-width: 1200px; margin: 0 auto; display: flex; align-items: center; justify-content: space-between; gap: 12px; font-size: 0.85rem; }
    .main-layout { max-width: 1200px; margin: 16px auto; padding: 0 20px 30px; }
    .section-block { margin-bottom: 16px; }
    .section-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px; }
    .section-title { font-size: 0.95rem; font-weight: 600; color: var(--text-main); }
    .card { background: var(--bg-card); border: 1px solid var(--border-light); border-radius: var(--radius-lg); padding: 14px; box-shadow: var(--shadow-sm); }
    .telemetry-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 12px; }
    .card-metric { padding: 12px; display: flex; flex-direction: column; }
    .metric-top { display: flex; align-items: center; justify-content: space-between; margin-bottom: 6px; }
    .metric-label { font-size: 0.75rem; font-weight: 600; color: var(--text-muted); }
    .pin-tag { font-size: 0.68rem; background: var(--bg-subtle); color: var(--text-muted); padding: 1px 5px; border-radius: var(--radius-sm); font-family: var(--font-mono); }
    .metric-main { display: flex; align-items: baseline; gap: 4px; margin-bottom: 6px; }
    .metric-val { font-size: 1.7rem; font-weight: 700; color: var(--text-main); font-family: var(--font-mono); line-height: 1; }
    .metric-unit { font-size: 0.8rem; color: var(--text-muted); font-weight: 500; }
    .metric-bar-bg { width: 100%; height: 4px; background: var(--border-light); border-radius: var(--radius-pill); overflow: hidden; margin-bottom: 6px; }
    .metric-bar-fill { height: 100%; background: var(--color-primary); border-radius: var(--radius-pill); transition: width 0.3s ease; }
    .metric-sub { font-size: 0.7rem; color: var(--text-muted); margin-top: auto; }
    .card-zone-a { border-left: 3px solid var(--color-zone-a); }
    .card-zone-b { border-left: 3px solid var(--color-zone-b); }
    .badge-soil { font-family: var(--font-mono); font-size: 1.05rem; font-weight: 700; padding: 2px 8px; border-radius: var(--radius-sm); letter-spacing: 0.05em; display: inline-block; }
    .badge-normal { background: var(--color-success-bg); color: var(--color-success-text); border: 1px solid #bbf7d0; }
    .badge-dry { background: var(--color-warning-bg); color: var(--color-warning-text); border: 1px solid #fde68a; }
    .badge-wet { background: #e0f2fe; color: #0369a1; border: 1px solid #bae6fd; }
    .card-hardware { border-left: 3px solid var(--border-medium); }
    .actuator-row { display: flex; justify-content: space-between; margin-bottom: 4px; font-size: 0.75rem; }
    .actuator-col { flex: 1; }
    .act-name { color: var(--text-muted); display: block; font-size: 0.68rem; }
    .lower-grid { display: grid; grid-template-columns: 1fr 1.5fr; gap: 14px; margin-top: 14px; }
    @media (max-width: 800px) { .lower-grid { grid-template-columns: 1fr; } }
    .card-header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 10px; padding-bottom: 8px; border-bottom: 1px solid var(--border-light); }
    .card-title { font-size: 0.88rem; font-weight: 600; color: var(--text-main); }
    .actions-grid { display: flex; flex-direction: column; gap: 8px; }
    .btn { display: inline-flex; align-items: center; justify-content: center; padding: 8px 12px; border-radius: var(--radius-sm); font-size: 0.8rem; font-weight: 600; cursor: pointer; border: 1px solid var(--border-medium); background: var(--bg-subtle); color: var(--text-main); text-decoration: none; transition: 0.15s; }
    .btn:hover { background: #e2e8f0; }
    .btn-primary { background: var(--color-primary); color: white; border-color: var(--color-primary); }
    .btn-primary:hover { background: var(--color-primary-hover); }
    .btn-white { background: white; color: var(--color-danger); border-color: white; padding: 4px 10px; font-size: 0.75rem; }
    .terminal-box-view { background: #0f172a; color: #e2e8f0; border-radius: var(--radius-sm); padding: 8px; font-family: var(--font-mono); font-size: 0.72rem; height: 180px; overflow-y: auto; line-height: 1.45; }
    .log-entry { display: flex; gap: 6px; margin-bottom: 2px; }
    .log-time { color: #64748b; flex-shrink: 0; }
    .log-msg.error { color: #f87171; }
    .log-msg.success { color: #4ade80; }
    .mono { font-family: var(--font-mono); font-weight: 600; }
  </style>
</head>
<body>

  <header class="navbar">
    <div class="navbar-container">
      <div>
        <h1 class="brand-title">SICS Gantry Watering V2.0</h1>
        <p class="brand-desc">Monitoring Irigasi Presisi Linear Motion ESP32</p>
      </div>
      <div class="author-card">
        <div class="author-name">Azkia Azraei Shahrefi</div>
        <div class="author-meta">NIM: 2305041030 &bull; Kelas: EK-6B</div>
      </div>
      <div class="navbar-actions">
        <div class="rtc-clock">
          <span class="rtc-label">RTC DS3231</span>
          <span class="rtc-time" id="clockTime">--:--:--</span>
        </div>
        <button class="btn-estop" onclick="confirmEmergency()">E-STOP</button>
      </div>
    </div>
  </header>

  <div class="estop-alert" id="estopAlert" style="display: none;">
    <div class="estop-alert-inner">
      <span><strong>PERINGATAN: EMERGENCY STOP AKTIF!</strong></span>
      <button class="btn btn-white" onclick="doCommand('RESET')">Reset Sistem</button>
    </div>
  </div>

  <main class="main-layout">
    <section class="section-block">
      <div class="section-header">
        <h2 class="section-title">Telemetri Sensor & Status Sistem</h2>
        <span style="font-size:0.75rem;color:var(--text-muted)">Real-Time Polling</span>
      </div>

      <div class="telemetry-grid">
        <div class="card card-metric">
          <div class="metric-top">
            <span class="metric-label">Suhu Udara</span>
            <span class="pin-tag">DHT22 (Pin 23)</span>
          </div>
          <div class="metric-main">
            <span class="metric-val" id="temp">--.-</span>
            <span class="metric-unit">&deg;C</span>
          </div>
          <div class="metric-bar-bg"><div class="metric-bar-fill" id="barTemp" style="width: 50%;"></div></div>
          <div class="metric-sub">Ideal Tanaman: 24.0 - 32.0 &deg;C</div>
        </div>

        <div class="card card-metric">
          <div class="metric-top">
            <span class="metric-label">Kelembapan Udara</span>
            <span class="pin-tag">DHT22 (Pin 23)</span>
          </div>
          <div class="metric-main">
            <span class="metric-val" id="hum">--</span>
            <span class="metric-unit">% RH</span>
          </div>
          <div class="metric-bar-bg"><div class="metric-bar-fill" id="barHum" style="width: 60%;"></div></div>
          <div class="metric-sub">Rentang Normal: 60 - 85% RH</div>
        </div>

        <div class="card card-metric card-zone-a">
          <div class="metric-top">
            <span class="metric-label">Tanah Zona A (Baris H - E)</span>
            <span class="pin-tag">ADC (Pin 34)</span>
          </div>
          <div class="metric-main">
            <span class="badge-soil badge-normal" id="soilA">--</span>
          </div>
          <div class="metric-sub">24 Sel Tanaman (Atas)</div>
        </div>

        <div class="card card-metric card-zone-b">
          <div class="metric-top">
            <span class="metric-label">Tanah Zona B (Baris D - A)</span>
            <span class="pin-tag">ADC (Pin 35)</span>
          </div>
          <div class="metric-main">
            <span class="badge-soil badge-dry" id="soilB">--</span>
          </div>
          <div class="metric-sub">24 Sel Tanaman (Bawah)</div>
        </div>

        <div class="card card-metric card-hardware">
          <div class="metric-top">
            <span class="metric-label">Aktuator & Stepper</span>
            <span class="pin-tag" id="driverStatus">Driver: Active</span>
          </div>
          <div class="actuator-row">
            <div class="actuator-col"><span class="act-name">Pompa:</span><strong id="statusPump">OFF</strong></div>
            <div class="actuator-col"><span class="act-name">Stepper:</span><strong id="statusMotion">IDLE</strong></div>
          </div>
          <div class="actuator-row">
            <div class="actuator-col"><span class="act-name">Pos X:</span><span class="mono" id="posX">0</span> steps</div>
            <div class="actuator-col"><span class="act-name">Pos Y:</span><span class="mono" id="posY">0</span> steps</div>
          </div>
        </div>
      </div>
    </section>

    <div class="lower-grid">
      <section class="card">
        <div class="card-header">
          <h3 class="card-title">Aksi Cepat Sistem</h3>
        </div>
        <div class="actions-grid">
          <button class="btn" onclick="doCommand('STATUS')">🔍 Cek Status</button>
          <button class="btn" onclick="doCommand('HOME')">🏠 Home Kalibrasi</button>
          <button class="btn btn-primary" onclick="doCommand('GSHEET')">📊 Kirim ke Google Sheets</button>
          <button class="btn" onclick="location.reload()">🔄 Refresh Halaman</button>
        </div>
      </section>

      <section class="card">
        <div class="card-header">
          <h3 class="card-title">Log Aktivitas & Serial Monitor</h3>
          <button class="btn" style="padding:2px 8px;font-size:0.7rem;" onclick="clearLog()">Bersihkan</button>
        </div>
        <div class="terminal-box-view" id="terminalLogBox"></div>
      </section>
    </div>
  </main>

  <script>
    function log(msg, type = 'normal') {
      const box = document.getElementById('terminalLogBox');
      if (!box) return;
      const now = new Date();
      const timeStr = now.toTimeString().split(' ')[0];
      const row = document.createElement('div');
      row.className = 'log-entry';
      row.innerHTML = `<span class="log-time">[${timeStr}]</span><span class="log-msg ${type}">${msg}</span>`;
      box.appendChild(row);
      box.scrollTop = box.scrollHeight;
    }

    function clearLog() {
      const box = document.getElementById('terminalLogBox');
      if (box) box.innerHTML = '';
    }

    function updateDashboard() {
      fetch('/api/status')
        .then(r => r.json())
        .then(data => {
          document.getElementById('temp').textContent = (data.temp || 0).toFixed(1);
          document.getElementById('hum').textContent = (data.humidity || 0).toFixed(0);
          const barT = document.getElementById('barTemp');
          const barH = document.getElementById('barHum');
          if (barT) barT.style.width = `${Math.min(100, Math.max(0, ((data.temp || 0) / 50) * 100))}%`;
          if (barH) barH.style.width = `${Math.min(100, Math.max(0, data.humidity || 0))}%`;

          const soilA = document.getElementById('soilA');
          const soilB = document.getElementById('soilB');
          soilA.textContent = data.soilA || '--';
          soilB.textContent = data.soilB || '--';
          soilA.className = `badge-soil badge-${(data.soilA || '').toLowerCase()}`;
          soilB.className = `badge-soil badge-${(data.soilB || '').toLowerCase()}`;

          document.getElementById('clockTime').textContent = data.rtcTime || '--:--:--';
          document.getElementById('posX').textContent = data.posX !== undefined ? data.posX : 0;
          document.getElementById('posY').textContent = data.posY !== undefined ? data.posY : 0;

          document.getElementById('statusMotion').textContent = data.isMoving ? 'Bergerak' : (data.isScanning ? 'Scanning' : 'IDLE');
          document.getElementById('statusPump').textContent = data.isScanning ? 'PUMP ACTIVE' : 'OFF';

          const estop = document.getElementById('estopAlert');
          const driver = document.getElementById('driverStatus');
          if (data.emergencyStop) {
            estop.style.display = 'block';
            driver.textContent = 'Driver: Disabled';
            driver.style.color = '#dc2626';
          } else {
            estop.style.display = 'none';
            driver.textContent = 'Driver: Active';
            driver.style.color = '#64748b';
          }
        })
        .catch(err => console.error(err));
    }

    function doCommand(cmd) {
      if (!confirm('Jalankan perintah: ' + cmd + '?')) return;
      log('CMD: ' + cmd);
      fetch('/api/command?cmd=' + encodeURIComponent(cmd))
        .then(r => r.json())
        .then(j => {
          log('Respon: ' + j.message, j.status === 'ok' ? 'success' : 'error');
          alert(j.message);
          updateDashboard();
        })
        .catch(e => {
          log('Error: ' + e, 'error');
          alert('Gagal menghubungi ESP32');
        });
    }

    function confirmEmergency() {
      if (!confirm('KONFIRMASI: Aktifkan EMERGENCY STOP sekarang?')) return;
      doCommand('ES');
    }

    window.addEventListener('DOMContentLoaded', () => {
      updateDashboard();
      setInterval(updateDashboard, 2500);
      log('SICS Web Monitoring Siap');
    });
  </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleStatus() {
  DateTime now = rtc.now();
  char timeStr[20];
  sprintf(timeStr, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  DynamicJsonDocument doc(2048);
  doc["temp"] = lastTempC;
  doc["humidity"] = lastHum;
  doc["soilA"] = lastSoilA;
  doc["soilB"] = lastSoilB;
  doc["rtcTime"] = String(timeStr);
  doc["posX"] = currentX;
  doc["posY"] = currentY;
  doc["isMoving"] = isMoving;
  doc["isScanning"] = isScanning;
  doc["emergencyStop"] = emergencyStop;
  JsonArray arr = doc.createNestedArray("schedules");
  String schedList = "";
  for (int i = 0; i < scheduleCount; i++) {
    char buf[8];
    sprintf(buf, "%02d:%02d", schedules[i].hour, schedules[i].minute);
    String target = scheduleTargetLabel(schedules[i]);
    JsonObject it = arr.createNestedObject();
    it["time"] = String(buf);
    it["target"] = target;
    it["type"] = target;
    it["index"] = schedules[i].targetIndex;
    if (i > 0)
      schedList += ", ";
    schedList += String(buf) + " " + target;
  }
  doc["scheduledList"] = scheduleCount ? schedList : String("(none)");
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleCommand() {
  String cmd = server.arg("cmd");
  cmd.toUpperCase();

  String message = "Command executed";

  if (cmd == "SCAN") {
    if (!emergencyStop) {
      scanCustom();
      message = "✅ Full Scan dimulai...";
    } else {
      message = "❌ ERROR: Emergency Stop aktif!";
    }
  } else if (cmd == "ZONA A") {
    if (!emergencyStop) {
      scanZonaA();
      message = "✅ Zona A Scan dimulai...";
    } else {
      message = "❌ ERROR: Emergency Stop aktif!";
    }
  } else if (cmd == "ZONA B") {
    if (!emergencyStop) {
      scanZonaB();
      message = "✅ Zona B Scan dimulai...";
    } else {
      message = "❌ ERROR: Emergency Stop aktif!";
    }
  }
  // ROW Commands
  else if (cmd.startsWith("ROW_")) {
    char rowLetter = cmd.charAt(4);
    int rowIndex = rowLetter - 'A';

    if (rowIndex >= 0 && rowIndex < ROWS && !emergencyStop) {
      scanRow(rowIndex);
      message = String("✅ Row ") + rowLetter + " Scan dimulai...";
    } else {
      message = "❌ Invalid Row atau Emergency Stop aktif!";
    }
  }
  // COLUMN Commands
  else if (cmd.startsWith("COL_")) {
    int colNum = cmd.substring(4).toInt();
    int colIndex = colNum - 1;

    if (colIndex >= 0 && colIndex < COLS && !emergencyStop) {
      scanColumn(colIndex);
      message = String("✅ Column ") + colNum + " Scan dimulai...";
    } else {
      message = "❌ Invalid Column atau Emergency Stop aktif!";
    }
  } else if (cmd == "HOME") {
    if (!emergencyStop) {
      if (homeX() && homeY()) {
        message = "✅ Home position OK";
      } else {
        message = "❌ Home failed!";
      }
    } else {
      message = "❌ ERROR: Emergency Stop aktif!";
    }
  } else if (cmd == "STATUS") {
    message = "X=" + String(currentX) + " Y=" + String(currentY) +
              " Temp=" + String(lastTempC, 1) + "C";
  } else if (cmd == "ES") {
    triggerEmergencyStop();
    message = "⛔ EMERGENCY STOP ACTIVATED!";
  } else if (cmd == "RESET") {
    resetFromEmergencyStop();
    message = "✅ System reset OK";
  } else if (cmd == "GOTO") {
    String cell = server.arg("cell");
    if (cell.length() >= 2) {
      char row = cell.charAt(0);
      int col = (int)(cell.charAt(1)) - (int)'0';
      gotoCell(row, col);
      message = "✅ Moving to " + cell;
    } else {
      message = "❌ Invalid cell format";
    }
  } else {
    message = "Unknown command";
  }

  StaticJsonDocument<128> doc;
  doc["status"] = "ok";
  doc["message"] = message;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleAddSchedule() {
  String t = server.hasArg("time") ? server.arg("time") : "";
  int h, m;
  ScheduleTargetType targetType = TARGET_FULL;
  int targetIndex = -1;
  String targetToken = server.hasArg("type") ? server.arg("type") : "SCAN";
  if (targetToken == "ROW" && server.hasArg("target"))
    targetToken = "ROW_" + server.arg("target");
  if (targetToken == "COL" && server.hasArg("target"))
    targetToken = "COL_" + server.arg("target");

  if (parseTimeString(t, h, m) &&
      parseScheduleTarget(targetToken, targetType, targetIndex)) {
    StaticJsonDocument<256> doc;
    if (addSchedule(h, m, targetType, targetIndex)) {
      char buf[16];
      sprintf(buf, "%02d:%02d", h, m);
      doc["status"] = "ok";
      doc["message"] = String("Jadwal ditambahkan: ") + String(buf) + " " +
                       scheduleTargetLabel(targetType, targetIndex);
      String json;
      serializeJson(doc, json);
      server.send(200, "application/json", json);
      return;
    }
    doc["status"] = "error";
    doc["message"] = "Gagal menambah jadwal (duplikat atau penuh)";
    String json;
    serializeJson(doc, json);
    server.send(400, "application/json", json);
    return;
  }
  StaticJsonDocument<128> doc;
  doc["status"] = "error";
  doc["message"] = "Parameter salah. Gunakan time=HH:MM dan type/target.";
  String json;
  serializeJson(doc, json);
  server.send(400, "application/json", json);
}

void handleDelSchedule() {
  if (server.hasArg("time")) {
    String t = server.arg("time");
    String targetToken = server.hasArg("type") ? server.arg("type") : "SCAN";
    int h, m, targetIndex;
    ScheduleTargetType targetType;
    if (parseTimeString(t, h, m) &&
        parseScheduleTarget(targetToken, targetType, targetIndex)) {
      if (removeSchedule(h, m, targetType, targetIndex)) {
        StaticJsonDocument<128> doc;
        doc["status"] = "ok";
        doc["message"] = "Jadwal dihapus: " + t + " " +
                         scheduleTargetLabel(targetType, targetIndex);
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json);
        return;
      } else {
        StaticJsonDocument<128> doc;
        doc["status"] = "error";
        doc["message"] = "Jadwal tidak ditemukan: " + t;
        String json;
        serializeJson(doc, json);
        server.send(404, "application/json", json);
        return;
      }
    }
  }
  StaticJsonDocument<128> doc;
  doc["status"] = "error";
  doc["message"] = "Parameter salah. Gunakan time=HH:MM&type=TARGET";
  String json;
  serializeJson(doc, json);
  server.send(400, "application/json", json);
}

void handleClearSchedules() {
  clearAllSchedules();
  StaticJsonDocument<128> doc;
  doc["status"] = "ok";
  doc["message"] = "All schedules cleared";
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleListSchedules() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.createNestedArray("schedules");
  for (int i = 0; i < scheduleCount; i++) {
    char buf[8];
    sprintf(buf, "%02d:%02d", schedules[i].hour, schedules[i].minute);
    JsonObject it = arr.createNestedObject();
    it["time"] = String(buf);
    it["target"] = scheduleTargetLabel(schedules[i]);
    it["type"] = scheduleTargetLabel(schedules[i]);
    it["index"] = schedules[i].targetIndex;
  }
  doc["count"] = scheduleCount;
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleSendGoogleSheets() {
  bool ok = sendDataToGoogleSheets();
  StaticJsonDocument<128> doc;
  doc["status"] = ok ? "ok" : "error";
  doc["message"] =
      ok ? "Data berhasil dikirim ke Google Sheets"
         : "Gagal mengirim data ke Google Sheets (periksa URL/WiFi)";
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void setupServerRoutes() {
  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/command", handleCommand);
  server.on("/api/schedule", handleAddSchedule);    // add schedule
  server.on("/api/delschedule", handleDelSchedule); // delete one schedule
  server.on("/api/clearschedules", handleClearSchedules); // clear all
  server.on("/api/schedules", handleListSchedules);       // list schedules
  server.on("/api/gsheet",
            handleSendGoogleSheets); // trigger log Google Sheets
  server.begin();
  Serial.println("WebServer started!");
}

void setup() {
  Serial.begin(115200);

  // 1. KONEKSI ULTRA CEPAT (FAST BOOT):
  // Menyalakan WiFi langsung di baris pertama agar proses handshake & DHCP
  // berjalan paralel di background FreeRTOS saat peripheral hardware lain diinisialisasi.
  // WiFi.setSleep(false) mematikan modem-sleep agar latensi 0ms dan transmisi data instan.
  WiFi.persistent(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  pinMode(X_STEP, OUTPUT);
  pinMode(X_DIR, OUTPUT);
  pinMode(Y_STEP, OUTPUT);
  pinMode(Y_DIR, OUTPUT);
  pinMode(EN_PIN, OUTPUT);

  pinMode(X_MIN, INPUT_PULLUP);
  pinMode(X_MAX, INPUT_PULLUP);
  pinMode(Y_MIN, INPUT_PULLUP);
  pinMode(Y_MAX, INPUT_PULLUP);

  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(EN_PIN, HIGH);

  ledcAttach(PUMP_PIN, PUMP_FREQ, PUMP_RES_BITS);
  ledcWrite(PUMP_PIN, 0);

  Wire.begin(I2C_SDA, I2C_SCL);

  int status = lcd.begin(16, 2);
  if (status) {
    Serial.print("LCD failed: ");
    Serial.println(status);
    digitalWrite(LED_RED, HIGH);
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("SICS V2.0");
    lcd.setCursor(0, 1);
    lcd.print("Koneksi Cepat...");
  }

  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    digitalWrite(LED_RED, HIGH);
  } else {
    DateTime checkTime = rtc.now();
    if (rtc.lostPower() || checkTime.year() < 2024 || checkTime.year() > 2035 ||
        checkTime.month() > 12) {
      Serial.println(
          "⚠️ RTC time corrupted/lost power, resetting to compile time!");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }

  dht.begin();
  loadSchedulesFromPrefs();

  Serial.println("=== SICS Gantry Watering V2.0 (Fast Connect & Non-Stop Live) ===");
  DateTime now = rtc.now();
  logEvent("SYSTEM BOOT");

  // 2. Cek koneksi WiFi - karena sudah bernegosiasi paralel di background,
  // waktu tunggu di sini menjadi sangat singkat (< 1-2 detik)
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 3500) {
    delay(40);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi terhubung sangat cepat!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    setupServerRoutes();

    if (!status) {
      lcd.setCursor(0, 1);
      lcd.print("Online & Ready! ");
    }

    // KIRIM TELEMETRI PERTAMA KE CLOUD DETIK ITU JUGA!
    // Web Dashboard LANGSUNG TERHUBUNG SEKETIKA (1-2 detik setelah power switch ON)!
    readSensors();
    sendTelemetryToCloud();
    lastCloudSend = millis();
  } else {
    Serial.println("\n⚠️ WiFi belum tersambung, background reconnect aktif.");
    if (!status) {
      lcd.setCursor(0, 1);
      lcd.print("WiFi: Syncing.. ");
    }
  }

  // 3. Sekarang jalankan Homing Gantry (Web Dashboard sudah online dan menampilkan status secara live!)
  if (!homeX()) {
    logEvent("HOME X FAILED");
    digitalWrite(LED_RED, HIGH);
  }
  if (!homeY()) {
    logEvent("HOME Y FAILED");
    digitalWrite(LED_RED, HIGH);
  }

  readSensors();
  lastLCDModeChange = millis();
  updateLCDMode(now);

  // Update status final setelah homing selesai
  if (WiFi.status() == WL_CONNECTED) {
    sendTelemetryToCloud();
    lastCloudSend = millis();
  }

  Serial.println("\n===== READY =====");
  Serial.println("WebServer: http://ESP32_IP");
  Serial.println("Telegram Bot Active!");
  Serial.println("Cloud Sync: Active & Non-Stop!");
  Serial.println("==================");
}

// Helper: compute day of year from DateTime (RTClib has no dayOfYear())
int getDayOfYear(const DateTime &dt) {
  // month days for non-leap
  int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int y = dt.year();
  bool leap = ((y % 4 == 0) && ((y % 100 != 0) || (y % 400 == 0)));
  if (leap)
    mdays[1] = 29;
  int doy = dt.day();
  int mon = dt.month();
  for (int i = 0; i < mon - 1; i++)
    doy += mdays[i];
  return doy;
}

void loop() {
  server.handleClient();

  pump_update();
  buzzer_update();

  static unsigned long lastSensorUpdate = 0;
  if (millis() - lastSensorUpdate > 5000) {
    lastSensorUpdate = millis();
    readSensors();
    DateTime now = rtc.now();
    updateLCDMode(now);
  } else {
    DateTime now = rtc.now();
    updateLCDMode(now);
  }

  // Telegram Bot Check - NON-BLOCKING
  if (millis() - lastTelegramCheck > TELEGRAM_CHECK_INTERVAL) {
    lastTelegramCheck = millis();
    handleTelegramCommands();
  }

  // Auto-reconnect WiFi cepat jika terputus
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWiFiRetry = 0;
    if (millis() - lastWiFiRetry > 4000) {
      lastWiFiRetry = millis();
      Serial.println("⚠️ [WiFi] Terputus dari jaringan, mencoba rekoneksi cepat...");
      WiFi.reconnect();
    }
  }

  // Polmed Research Center Cloud Server Auto-Send & Serial Monitor Status
  // Display
  if (millis() - lastCloudSend >= CLOUD_TELEMETRY_INTERVAL) {
    lastCloudSend = millis();
    if (WiFi.status() == WL_CONNECTED && strlen(CLOUD_SERVER_URL) > 0) {
      sendTelemetryToCloud();
    } else {
      printSerialStatusDashboard(0);
    }
  }

  // Google Sheets Auto-Send - NON-BLOCKING
  if (strlen(GOOGLE_SHEETS_URL) > 0 &&
      (millis() - lastGoogleSheetsSend >= GOOGLE_SHEETS_INTERVAL)) {
    lastGoogleSheetsSend = millis();
    sendDataToGoogleSheets();
  }

  // Scheduled scans check (once per minute)
  static int lastCheckedMinute = -1;
  DateTime now = rtc.now();
  int curMinute = now.minute();
  int dayOfYear = getDayOfYear(now); // <-- fixed: use helper
  if (curMinute != lastCheckedMinute) {
    lastCheckedMinute = curMinute;
    // iterate schedules
    for (int i = 0; i < scheduleCount; i++) {
      if (emergencyStop)
        break;
      if (now.hour() == schedules[i].hour &&
          now.minute() == schedules[i].minute) {
        if (lastTriggeredDayOfYear[i] != dayOfYear) {
          logEvent(String("AUTO WATERING TRIGGERED BY SCHEDULE ") +
                   (schedules[i].hour < 10 ? "0" : "") +
                   String(schedules[i].hour) + ":" +
                   (schedules[i].minute < 10 ? "0" : "") +
                   String(schedules[i].minute) + " " +
                   scheduleTargetLabel(schedules[i]));
          executeScheduleTarget(schedules[i]);
          lastTriggeredDayOfYear[i] = dayOfYear;
        }
      } else {
        // nothing: will be reset when next day arrives automatically by
        // dayOfYear comparison
      }
    }
  }

  if (Serial.available()) {
    String s = Serial.readStringUntil('\n');
    s.trim();
    String raw = s;
    s.toUpperCase();
    if (s.length() == 0)
      return;

    if (s.startsWith("SETSCAN") || s.startsWith("ADDSCAN")) {
      int sp = raw.indexOf(' ');
      if (sp == -1) {
        Serial.println("Usage: SETSCAN HH:MM");
      } else {
        String timestr = raw.substring(sp + 1);
        int h, m;
        ScheduleTargetType targetType;
        int targetIndex;
        if (parseScheduleArgs(timestr, h, m, targetType, targetIndex)) {
          if (addSchedule(h, m, targetType, targetIndex)) {
            Serial.print("Scheduled watering added: ");
            Serial.print(timestr);
            Serial.print(" ");
            Serial.println(scheduleTargetLabel(targetType, targetIndex));
          } else {
            Serial.println("Failed to add schedule (duplicate or max reached)");
          }
        } else {
          Serial.println("Invalid time format. Use HH:MM");
        }
      }
    } else if (s.startsWith("UNSETSCAN") || s.startsWith("DELSCAN")) {
      int sp = raw.indexOf(' ');
      if (sp == -1) {
        Serial.println("Usage: UNSETSCAN HH:MM");
      } else {
        String timestr = raw.substring(sp + 1);
        int h, m;
        ScheduleTargetType targetType;
        int targetIndex;
        if (parseScheduleArgs(timestr, h, m, targetType, targetIndex)) {
          if (removeSchedule(h, m, targetType, targetIndex)) {
            Serial.print("Scheduled watering removed: ");
            Serial.print(timestr);
            Serial.print(" ");
            Serial.println(scheduleTargetLabel(targetType, targetIndex));
          } else {
            Serial.println("Schedule not found: ");
            Serial.println(timestr);
          }
        } else {
          Serial.println("Invalid time format. Use HH:MM");
        }
      }
    } else if (s == "CLEARSCHEDULES" || s == "UNSETALL") {
      clearAllSchedules();
      Serial.println("All schedules cleared");
    } else if (s == "SCAN") {
      if (emergencyStop) {
        Serial.println("EMERGENCY STOP ACTIVE - Cannot execute");
      } else {
        logEvent("CMD SCAN FULL");
        scanCustom();
      }
    } else if (s == "ZONA A") {
      if (emergencyStop) {
        Serial.println("EMERGENCY STOP ACTIVE - Cannot execute");
      } else {
        logEvent("CMD ZONA A");
        scanZonaA();
      }
    } else if (s == "ZONA B") {
      if (emergencyStop) {
        Serial.println("EMERGENCY STOP ACTIVE - Cannot execute");
      } else {
        logEvent("CMD ZONA B");
        scanZonaB();
      }
    } else if (s == "HOME") {
      if (emergencyStop) {
        Serial.println("EMERGENCY STOP ACTIVE - Cannot execute");
      } else {
        logEvent("CMD HOME");
        if (!homeX()) {
          logEvent("HOME X FAILED");
          digitalWrite(LED_RED, HIGH);
        }
        if (!homeY()) {
          logEvent("HOME Y FAILED");
          digitalWrite(LED_RED, HIGH);
        }
      }
    } else if (s == "STATUS") {
      printStatus();
    } else if (s == "ES") {
      triggerEmergencyStop();
    } else if (s == "RESET") {
      resetFromEmergencyStop();
    } else if (s == "GSHEET" || s == "SENDSHEET") {
      Serial.println("Mengirim data ke Google Sheets...");
      sendDataToGoogleSheets();
    } else {
      if (emergencyStop) {
        Serial.println("EMERGENCY STOP ACTIVE - Cannot execute");
      } else {
        char row = s.charAt(0);
        String colStr = s.substring(1);
        int col = colStr.toInt();
        if (col <= 0) {
          Serial.println("Invalid command");
          logEvent("INVALID CMD");
        } else {
          logEvent(String("CMD GOTO ") + s);
          gotoCell(row, col);
          readSensors();
          DateTime now = rtc.now();
          updateLCDMode(now);
        }
      }
    }
  }

  delay(10);
}