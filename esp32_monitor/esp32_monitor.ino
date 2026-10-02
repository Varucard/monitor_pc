/*
  ESP32 PC Resource Monitor - LCD 16x2 + botón

  Pantallas (se avanza con el botón):
    0) CPU: uso, temperatura y red
    1) GPU: uso y temperatura
    2) RAM y disco: uso %
    3) Fecha y hora (NTP)
    4) Clima: temperatura y descripción

  La configuración (Wi-Fi, servidor, token) va en config.h,
  que se crea copiando config.example.h.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

#include "config.h"

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// -------- Variables de UI ----------
constexpr uint8_t PAGE_COUNT = 5;
uint8_t page = 0;
bool needsRedraw = true;
int lastShownSecond = -1;

unsigned long lastFetch = 0;
const unsigned long fetchIntervalMs = 1000;
const uint16_t httpTimeoutMs = 1500;

volatile bool btnPressed = false;
volatile unsigned long lastBtnMs = 0;
const unsigned long debounceMs = 180;

// -------- Datos recibidos ----------
bool serverOnline = false;
float cpu_pct = 0, cpu_temp = NAN;
float gpu_pct = NAN, gpu_temp = NAN;
float mem_pct = 0, disk_pct = 0;
long net_up = 0, net_down = 0;  // kbps
float weather_temp = NAN;
String weather_desc = "N/A";

// -------- Iconos personalizados (máx 8, 5x8 píxeles) ----------
byte ICON_CHIP[8]  = { B11111, B10001, B10101, B10101, B10101, B10001, B11111, B00000 };
byte ICON_THERM[8] = { B00100, B01010, B01010, B01010, B01010, B01110, B01110, B00100 };
byte ICON_RAM[8]   = { B11111, B10001, B10101, B10101, B10001, B11111, B00100, B00100 };
byte ICON_DISK[8]  = { B11111, B10001, B10111, B10101, B10101, B10001, B11111, B00000 };
byte ICON_CLOUD[8] = { B00000, B00000, B00111, B01111, B11111, B11111, B01110, B00000 };
byte ICON_CLOCK[8] = { B00100, B01010, B10001, B10001, B10101, B10001, B01010, B00100 };
byte ICON_UP[8]    = { B00100, B01110, B11111, B00100, B00100, B00100, B00100, B00100 };
byte ICON_DOWN[8]  = { B00100, B00100, B00100, B00100, B11111, B01110, B00100, B00000 };

enum Icon : uint8_t { CHIP, THERM, RAM, DISK, CLOUD, CLOCK, UP, DOWN };

// Símbolo de grado en la ROM A00 del HD44780
const char DEG = (char)223;

// -------- ISR de botón ----------
void IRAM_ATTR onBtn() {
  unsigned long now = millis();
  if (now - lastBtnMs > debounceMs) {
    btnPressed = true;
    lastBtnMs = now;
  }
}

void setupLCD() {
  lcd.init();
  lcd.backlight();
  lcd.createChar(CHIP, ICON_CHIP);
  lcd.createChar(THERM, ICON_THERM);
  lcd.createChar(RAM, ICON_RAM);
  lcd.createChar(DISK, ICON_DISK);
  lcd.createChar(CLOUD, ICON_CLOUD);
  lcd.createChar(CLOCK, ICON_CLOCK);
  lcd.createChar(UP, ICON_UP);
  lcd.createChar(DOWN, ICON_DOWN);
  lcd.clear();
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lcd.setCursor(0, 0); lcd.print("Conectando WiFi");
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    lcd.setCursor(0, 1);
    lcd.print("    ");
    lcd.setCursor(0, 1);
    for (int i = 0; i <= (dots % 4); i++) lcd.print(".");
    dots++;
  }
}

void setupNTP() {
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
}

void setup() {
  pinMode(BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), onBtn, FALLING);

  setupLCD();
  setupWiFi();
  setupNTP();

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Monitor PC listo");
  lcd.setCursor(0, 1); lcd.print(WiFi.localIP().toString());
  delay(1200);
}

// Convierte texto UTF-8 al juego de caracteres del LCD (ROM A00):
// quita acentos y mapea ñ/ü a sus glifos; otros caracteres pasan a '?'.
String toLcdText(const String& in) {
  String out;
  out.reserve(in.length());
  for (unsigned int i = 0; i < in.length(); i++) {
    uint8_t c = in[i];
    if (c < 0x80) { out += (char)c; continue; }
    if (c == 0xC3 && i + 1 < in.length()) {
      switch ((uint8_t)in[++i]) {
        case 0xA1: out += 'a'; break;  case 0x81: out += 'A'; break;
        case 0xA9: out += 'e'; break;  case 0x89: out += 'E'; break;
        case 0xAD: out += 'i'; break;  case 0x8D: out += 'I'; break;
        case 0xB3: out += 'o'; break;  case 0x93: out += 'O'; break;
        case 0xBA: out += 'u'; break;  case 0x9A: out += 'U'; break;
        case 0xB1: case 0x91: out += (char)0xEE; break;  // ñ Ñ
        case 0xBC: case 0x9C: out += (char)0xF5; break;  // ü Ü
        default: out += '?';
      }
      continue;
    }
    while (i + 1 < in.length() && ((uint8_t)in[i + 1] & 0xC0) == 0x80) i++;  // Saltea bytes de continuación
    out += '?';
  }
  return out;
}

// Descarga las métricas; devuelve true si hubo cambios que mostrar.
bool fetchMetrics() {
  bool wasOnline = serverOnline;
  serverOnline = false;
  if (WiFi.status() != WL_CONNECTED) return wasOnline;

  HTTPClient http;
  String url = String("http://") + PC_IP + ":" + String(PC_PORT) + "/metrics";
  http.begin(url);
  http.setConnectTimeout(httpTimeoutMs);
  http.setTimeout(httpTimeoutMs);
  http.addHeader("X-Token", TOKEN);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
#if ARDUINOJSON_VERSION_MAJOR >= 7
    JsonDocument doc;
#else
    StaticJsonDocument<1024> doc;
#endif
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      serverOnline = true;
      cpu_pct = doc["cpu_pct"] | 0.0;
      cpu_temp = doc["cpu_temp_c"].isNull() ? NAN : doc["cpu_temp_c"].as<float>();
      mem_pct = doc["mem_pct"] | 0.0;
      disk_pct = doc["disk_pct"] | 0.0;
      net_up = doc["net_up_kbps"] | 0L;
      net_down = doc["net_down_kbps"] | 0L;
      gpu_pct = doc["gpu_pct"].isNull() ? NAN : doc["gpu_pct"].as<float>();
      gpu_temp = doc["gpu_temp_c"].isNull() ? NAN : doc["gpu_temp_c"].as<float>();

      JsonVariant weather = doc["weather"];
      if (weather.isNull()) {
        weather_temp = NAN;
        weather_desc = "N/A";
      } else {
        weather_temp = weather["temp"].isNull() ? NAN : weather["temp"].as<float>();
        weather_desc = weather["desc"].isNull() ? String("N/A") : toLcdText(weather["desc"].as<const char*>());
      }
    }
  }
  http.end();
  return serverOnline || wasOnline;
}

void printPctAt(int col, int row, float value) {
  int pct = (int)round(value);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  char buf[6];
  snprintf(buf, sizeof(buf), "%3d%%", pct);
  lcd.setCursor(col, row);
  lcd.print(buf);
}

void printTemp(float temp) {
  if (isnan(temp)) lcd.print("--");
  else lcd.print((int)round(temp));
  lcd.print(DEG);
  lcd.print("C");
}

// Formatea kbps en hasta 4 caracteres: "999k", "9.9M", "999M", "1.2G"
void formatRate(char* buf, size_t len, long kbps) {
  if (kbps < 1000)         snprintf(buf, len, "%ldk", kbps);
  else if (kbps < 9950)    snprintf(buf, len, "%.1fM", kbps / 1000.0);
  else if (kbps < 999500)  snprintf(buf, len, "%ldM", (kbps + 500) / 1000);
  else                     snprintf(buf, len, "%.1fG", kbps / 1000000.0);
}

void showOffline() {
  lcd.setCursor(0, 0); lcd.print("Sin datos del PC");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.status() == WL_CONNECTED ? "Reintentando..." : "WiFi caido...");
}

void showCPU() {
  lcd.setCursor(0, 0); lcd.write(CHIP); lcd.print("CPU");
  printPctAt(5, 0, cpu_pct);

  // Línea 1: [termómetro]48°C [↑]123k[↓]4.5M  (16 columnas)
  char up[8], down[8], line[12];
  formatRate(up, sizeof(up), net_up);
  formatRate(down, sizeof(down), net_down);
  lcd.setCursor(0, 1); lcd.write(THERM);
  printTemp(cpu_temp);
  lcd.print(" ");
  lcd.write(UP);
  snprintf(line, sizeof(line), "%-4s", up); lcd.print(line);
  lcd.write(DOWN);
  snprintf(line, sizeof(line), "%-4s", down); lcd.print(line);
}

void showGPU() {
  lcd.setCursor(0, 0); lcd.write(CHIP); lcd.print("GPU");
  if (isnan(gpu_pct)) { lcd.setCursor(5, 0); lcd.print(" N/A"); }
  else printPctAt(5, 0, gpu_pct);

  lcd.setCursor(0, 1); lcd.write(THERM); lcd.print(" ");
  printTemp(gpu_temp);
}

void showMemDisk() {
  lcd.setCursor(0, 0); lcd.write(RAM); lcd.print("RAM");
  printPctAt(5, 0, mem_pct);
  lcd.setCursor(0, 1); lcd.write(DISK); lcd.print("DSK");
  printPctAt(5, 1, disk_pct);
}

void showDateTime(const struct tm& t) {
  if (t.tm_year + 1900 < 2020) {  // NTP todavía no sincronizó
    lcd.setCursor(0, 0); lcd.print("Sincronizando");
    lcd.setCursor(0, 1); lcd.print("hora (NTP)...");
    return;
  }
  char buf[17];
  lcd.setCursor(0, 0);
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1, 1900 + t.tm_year);
  lcd.print(buf);

  lcd.setCursor(0, 1); lcd.write(CLOCK); lcd.print(" ");
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  lcd.print(buf);
}

void showWeather() {
  lcd.setCursor(0, 0); lcd.write(CLOUD); lcd.print(" Clima ");
  if (isnan(weather_temp)) lcd.print("N/A");
  else printTemp(weather_temp);

  lcd.setCursor(0, 1);
  lcd.print(weather_desc.substring(0, 16));
}

void render() {
  lcd.clear();
  if (page == 3) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    showDateTime(t);
    return;
  }
  if (!serverOnline) { showOffline(); return; }
  switch (page) {
    case 0: showCPU(); break;
    case 1: showGPU(); break;
    case 2: showMemDisk(); break;
    case 4: showWeather(); break;
  }
}

void loop() {
  if (btnPressed) {
    btnPressed = false;
    page = (page + 1) % PAGE_COUNT;
    needsRedraw = true;
  }

  if (millis() - lastFetch >= fetchIntervalMs) {
    lastFetch = millis();
    if (fetchMetrics() && page != 3) needsRedraw = true;
  }

  // La pantalla de hora se actualiza una vez por segundo
  if (page == 3) {
    time_t now = time(nullptr);
    int sec = now % 60;
    if (sec != lastShownSecond) {
      lastShownSecond = sec;
      needsRedraw = true;
    }
  }

  // Solo se redibuja cuando algo cambió, para evitar parpadeo
  if (needsRedraw) {
    needsRedraw = false;
    render();
  }

  delay(20);
}
