#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <BH1750.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ================================
//  JOYSTICK
// ================================
#define JOY_Y  36          // eixo Y (ADC1)

// ================================
//  SENSOR PINS
// ================================
#define UV_PIN   32        // ADC1
#define MQ_PIN   33        // ADC1
#define DHTPIN   4
#define DHTTYPE  DHT22

// ================================
//  SERVO CONFIG
// ================================
#define SERVO_X_PIN 14
#define SERVO_Y_PIN 13

Servo servoX;
Servo servoY;

const int SERVO_MIN = 70;
const int SERVO_MAX = 120;

// ================================
//  BUZZER PASSIVO (PWM/LEDC)
// ================================
#define BUZZ_PIN     18
#define BUZZ_CHANNEL 12
#define BUZZ_FREQ    2000
#define BUZZ_RES     10
#define BUZZ_DUTY    512  // ~50%

// ================================
//  DISPLAY
// ================================
Adafruit_SSD1306 display(128, 64, &Wire);

// ================================
//  SENSORS
// ================================
DHT dht(DHTPIN, DHTTYPE);
BH1750 lightMeter;
bool luxAvailable = false;

// ================================
//  WIFI CREDENTIALS
// ================================
const char* ssid     = "Jvudft#5";
const char* password = "Bgcupf@4";

// ================================
//  TELAS (sem menu/enter/back)
// ================================
enum ScreenState {
  SHOW_TEMP,
  SHOW_HUM,
  SHOW_UV,
  SHOW_GAS,
  SHOW_LUX
};
ScreenState state = SHOW_TEMP;

// ================================
//  DATA STRUCT
// ================================
struct SensorData {
  float temperature = 0;
  float humidity    = 0;
  float uv          = 0;
  float co_ppm      = 0;
  float lux         = 0;
} data;

// ================================
//   MQ-9 CO-PPM CONFIG
// ================================
float RL_MQ9 = 10.0;   // kΩ
float R0_MQ9 = 5.0;    // default

float mq9_getPPM_CO(int adcValue) {
  float Vcc = 3.3;
  float Vrl = (adcValue / 4095.0) * Vcc;
  if (Vrl < 0.01) return 0;

  float Rs = (Vcc - Vrl) * (RL_MQ9 / Vrl);
  float ratio = Rs / R0_MQ9;

  float ppm = pow(10, (-1.25 * log10(ratio) + 1.75));
  if (ppm < 0) ppm = 0;
  return ppm;
}

// =======================================================
//   I2C MUTEX (Wire compartilhado entre OLED e BH1750)
// =======================================================
SemaphoreHandle_t i2cMutex;
static inline void i2cLock()   { if (i2cMutex) xSemaphoreTake(i2cMutex, portMAX_DELAY); }
static inline void i2cUnlock() { if (i2cMutex) xSemaphoreGive(i2cMutex); }

// =======================================================
//   ALERTA DE PERIGO (BUZZER)
// =======================================================
const float LUX_DANGER_TH = 1000.0;  // exemplo: lux > 1000 => perigo

void buzzerInit() {
  ledcSetup(BUZZ_CHANNEL, BUZZ_FREQ, BUZZ_RES);
  ledcAttachPin(BUZZ_PIN, BUZZ_CHANNEL);
  ledcWrite(BUZZ_CHANNEL, 0);
}

bool isUnhealthy() {
  if (data.lux > LUX_DANGER_TH) return true;
  return false;
}

void updateBuzzerAlert() {
  static bool buzzerOn = false;
  bool danger = isUnhealthy();

  if (danger && !buzzerOn) {
    ledcWrite(BUZZ_CHANNEL, BUZZ_DUTY);
    buzzerOn = true;
  } else if (!danger && buzzerOn) {
    ledcWrite(BUZZ_CHANNEL, 0);
    buzzerOn = false;
  }
}

// =======================================================
//   SENSOR TASK ON CORE 0
// =======================================================
TaskHandle_t sensorTaskHandle = NULL;

void sensorTask(void *parameter) {
  for (;;) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) data.temperature = t;
    if (!isnan(h)) data.humidity = h;

    int rawUV = analogRead(UV_PIN);
    float voltsUV = rawUV * (3.3 / 4095.0);
    data.uv = voltsUV * 5.0;

    int rawGas = analogRead(MQ_PIN);
    data.co_ppm = mq9_getPPM_CO(rawGas);

    if (luxAvailable) {
      i2cLock();
      float luxValue = lightMeter.readLightLevel();
      i2cUnlock();
      if (luxValue >= 0) data.lux = luxValue;
    }

    vTaskDelay(100 / portTICK_PERIOD_MS); // 10 Hz
  }
}

// =======================================================
//   BOOT SPLASH + BUZZER (1s)
// =======================================================
void bootSplashAndBeep() {
  i2cLock();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.println("Microabençoados 2025");
  display.display();
  i2cUnlock();

  ledcWrite(BUZZ_CHANNEL, BUZZ_DUTY);
  delay(1000);
  ledcWrite(BUZZ_CHANNEL, 0);

  i2cLock();
  display.clearDisplay();
  display.display();
  i2cUnlock();
}

// =======================================================
//   NAVEGAÇÃO COM 1 EIXO (JOY_Y)
// =======================================================
int joyCenterY = 2048;
const int JOY_DEADZONE = 400;
const uint32_t NAV_COOLDOWN_MS = 220;
uint32_t lastNavMs = 0;
bool armedUp = true;
bool armedDown = true;

int avgAnalogRead(int pin, int n = 40) {
  long s = 0;
  for (int i = 0; i < n; i++) {
    s += analogRead(pin);
    delay(2);
  }
  return (int)(s / n);
}

void calibrateJoystickCenter() {
  joyCenterY = avgAnalogRead(JOY_Y, 50);
}

void nextScreen() {
  switch (state) {
    case SHOW_TEMP: state = SHOW_HUM; break;
    case SHOW_HUM:  state = SHOW_UV;  break;
    case SHOW_UV:   state = SHOW_GAS; break;
    case SHOW_GAS:  state = SHOW_LUX; break;
    case SHOW_LUX:  state = SHOW_TEMP;break;
  }
}

void prevScreen() {
  switch (state) {
    case SHOW_TEMP: state = SHOW_LUX; break;
    case SHOW_HUM:  state = SHOW_TEMP;break;
    case SHOW_UV:   state = SHOW_HUM; break;
    case SHOW_GAS:  state = SHOW_UV;  break;
    case SHOW_LUX:  state = SHOW_GAS; break;
  }
}

void handleJoystickOneAxis() {
  int y = analogRead(JOY_Y);
  int dy = y - joyCenterY;

  if (millis() - lastNavMs < NAV_COOLDOWN_MS) return;

  if (armedUp && dy < -JOY_DEADZONE) {
    nextScreen();
    lastNavMs = millis();
    armedUp = false;
  }
  if (!armedUp && dy > -(JOY_DEADZONE / 2)) armedUp = true;

  if (armedDown && dy > JOY_DEADZONE) {
    prevScreen();
    lastNavMs = millis();
    armedDown = false;
  }
  if (!armedDown && dy < (JOY_DEADZONE / 2)) armedDown = true;
}

// =======================================================
//   DISPLAY: tela cheia + fonte grande
// =======================================================
uint32_t lastDrawMs = 0;
const uint32_t DRAW_PERIOD_MS = 120;

volatile bool isTracking = false;

void drawBigValue(const char* title, const char* unit, float value, int decimals) {
  char buf[20];
  dtostrf(value, 0, decimals, buf);

  i2cLock();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(title);
  if (isTracking) display.print(" [TRACK]");
  if (isUnhealthy()) display.print(" !");

  display.setTextSize(3);
  display.setCursor(0, 22);
  display.print(buf);

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(unit);

  display.display();
  i2cUnlock();
}

void drawScreenFull() {
  switch (state) {
    case SHOW_TEMP: drawBigValue("Temperatura", "C", data.temperature, 1); break;
    case SHOW_HUM:  drawBigValue("Umidade", "%", data.humidity, 1); break;
    case SHOW_UV:   drawBigValue("UV", "nivel", data.uv, 2); break;
    case SHOW_GAS:  drawBigValue("CO", "ppm", data.co_ppm, 1); break;
    case SHOW_LUX:  drawBigValue("Lux", "lx", data.lux, 0); break;
  }
}

// =======================================================
//   SISTEMA: PRINT SERIAL COMPLETO (no momento do upload)
// =======================================================
void printSystemStatus() {
  Serial.println("====================================");
  Serial.println("ESTACAO - STATUS (no upload)");
  Serial.print("Uptime (ms): ");
  Serial.println(millis());

  Serial.println("\n--- SENSORES ---");
  Serial.print("Temp: ");
  Serial.print(data.temperature, 2);
  Serial.println(" C");

  Serial.print("Hum : ");
  Serial.print(data.humidity, 2);
  Serial.println(" %");

  Serial.print("UV  : ");
  Serial.println(data.uv, 2);

  Serial.print("CO  : ");
  Serial.print(data.co_ppm, 2);
  Serial.println(" ppm");

  Serial.print("Lux : ");
  Serial.print(data.lux, 1);
  Serial.println(" lx");

  Serial.println("\n--- SISTEMA ---");
  Serial.print("WiFi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONECTADO" : "DESCONECTADO");

  Serial.print("Tracking: ");
  Serial.println(isTracking ? "ATIVO" : "INATIVO");

  Serial.print("Alarme: ");
  Serial.println(isUnhealthy() ? "PERIGO" : "NORMAL");

  Serial.println("\n--- SERVOS ---");
  Serial.print("Limites: ");
  Serial.print(SERVO_MIN);
  Serial.print(" -> ");
  Serial.println(SERVO_MAX);

  Serial.print("Servo X (read): ");
  Serial.println(servoX.read());

  Serial.print("Servo Y (read): ");
  Serial.println(servoY.read());

  Serial.println("====================================\n");
}

// =======================================================
//   HTTPS UPLOAD
// =======================================================
unsigned long lastUpload = 0;

void uploadToCloud() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[UPLOAD] WiFi desconectado — envio cancelado");
    return;
  }

  // ✅ imprime status sempre que for upar
  printSystemStatus();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, "https://projeto-micro.onrender.com/update")) {
    Serial.println("[UPLOAD] begin() failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"temperature\":" + String(data.temperature) + ",";
  json += "\"humidity\":"   + String(data.humidity) + ",";
  json += "\"uv\":"         + String(data.uv) + ",";
  json += "\"gas\":"        + String(data.co_ppm) + ",";
  json += "\"lux\":"        + String(data.lux);
  json += "}";

  int code = http.POST(json);
  Serial.print("[UPLOAD] HTTP code: ");
  Serial.println(code);

  http.end();
}

// =======================================================
//   LIGHT TRACKING TASK (zig-zag + matriz + retorna ao max)
// =======================================================
TaskHandle_t trackerTaskHandle = NULL;

const int GRID_X = 11;
const int GRID_Y = 11;
const int STEP_X = (SERVO_MAX - SERVO_MIN) / (GRID_X - 1);
const int STEP_Y = (SERVO_MAX - SERVO_MIN) / (GRID_Y - 1);

const uint32_t SETTLE_MS = 120;
const uint32_t TRACK_EVERY_MS = 5UL * 60UL * 1000UL;

float luxGrid[GRID_Y][GRID_X];
volatile bool requestTracking = false;
uint32_t lastTrackRequestMs = 0;

static inline int clampAngle(int a) {
  if (a < SERVO_MIN) return SERVO_MIN;
  if (a > SERVO_MAX) return SERVO_MAX;
  return a;
}

float readLuxSafe() {
  if (!luxAvailable) return -1;
  i2cLock();
  float v = lightMeter.readLightLevel();
  i2cUnlock();
  return v;
}

void moveServos(int ax, int ay) {
  ax = clampAngle(ax);
  ay = clampAngle(ay);
  servoX.write(ax);
  servoY.write(ay);
}

void trackerTask(void *parameter) {
  for (;;) {
    if (!requestTracking) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }
    requestTracking = false;

    if (!luxAvailable) {
      vTaskDelay(200 / portTICK_PERIOD_MS);
      continue;
    }

    isTracking = true;

    float bestLux = -1;
    int bestAx = SERVO_MIN;
    int bestAy = SERVO_MIN;

    for (int iy = 0; iy < GRID_Y; iy++) {
      int ay = clampAngle(SERVO_MIN + iy * STEP_Y);
      bool forward = (iy % 2 == 0);

      for (int ix = 0; ix < GRID_X; ix++) {
        int xIndex = forward ? ix : (GRID_X - 1 - ix);
        int ax = clampAngle(SERVO_MIN + xIndex * STEP_X);

        moveServos(ax, ay);
        vTaskDelay(SETTLE_MS / portTICK_PERIOD_MS);

        float lux = readLuxSafe();
        if (lux < 0) lux = 0;

        luxGrid[iy][xIndex] = lux;

        if (lux > bestLux) {
          bestLux = lux;
          bestAx = ax;
          bestAy = ay;
        }
      }
    }

    moveServos(bestAx, bestAy);
    vTaskDelay(200 / portTICK_PERIOD_MS);

    float finalLux = readLuxSafe();
    if (finalLux >= 0) data.lux = finalLux;

    isTracking = false;

    Serial.print("[TRACK] Best lux=");
    Serial.print(bestLux, 1);
    Serial.print(" at X=");
    Serial.print(bestAx);
    Serial.print(" Y=");
    Serial.println(bestAy);
  }
}

// =======================================================
//                    SETUP
// =======================================================
void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  i2cMutex = xSemaphoreCreateMutex();

  dht.begin();
  Wire.begin(21, 22);

  i2cLock();
  luxAvailable = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  i2cUnlock();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    while (1);
  }

  // buzzer + splash + beep
  buzzerInit();
  bootSplashAndBeep();

  calibrateJoystickCenter();

  servoX.attach(SERVO_X_PIN);
  servoY.attach(SERVO_Y_PIN);
  moveServos(95, 95);

  xTaskCreatePinnedToCore(
      sensorTask,
      "SensorTask",
      4096,
      NULL,
      1,
      &sensorTaskHandle,
      0
  );

  xTaskCreatePinnedToCore(
      trackerTask,
      "TrackerTask",
      8192,
      NULL,
      1,
      &trackerTaskHandle,
      0
  );

  WiFi.begin(ssid, password);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 5000) {
    delay(200);
  }

  // scan logo no boot
  lastTrackRequestMs = millis();
  requestTracking = true;
}

// =======================================================
//                 MAIN LOOP (CORE 1)
// =======================================================
void loop() {
  handleJoystickOneAxis();

  if (millis() - lastDrawMs > DRAW_PERIOD_MS) {
    drawScreenFull();
    lastDrawMs = millis();
  }

  if (millis() - lastUpload > 3000) {
    uploadToCloud();
    lastUpload = millis();
  }

  if (millis() - lastTrackRequestMs > TRACK_EVERY_MS) {
    lastTrackRequestMs = millis();
    requestTracking = true;
  }

  // buzzer alerta
  updateBuzzerAlert();
}