/*
  ESP32 PC Resource Monitor - LCD 16x2 + botón

  Pantallas (se avanza con el botón):
    0) CPU: uso, temperatura y red
    1) GPU: uso, temperatura y VRAM
    2) RAM y disco: uso % (y GB de RAM)
    3) Fecha y hora (NTP)
    4) Clima: temperatura y descripción (con desplazamiento si no entra)

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

constexpr uint8_t LCD_COLS = 16;
constexpr uint8_t LCD_ROWS = 2;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// -------- Variables de UI ----------
constexpr uint8_t PAGE_COUNT = 5;
uint8_t page = 0;
unsigned long pageShownMs = 0;
char shownRows[LCD_ROWS][LCD_COLS + 1];  // Lo que hay en pantalla, para escribir solo lo que cambia

const unsigned long fetchIntervalMs = 1000;
const unsigned long offlineRetryMs = 5000;  // Con el PC apagado se reintenta con menos frecuencia
const uint16_t httpTimeoutMs = 800;
unsigned long lastFetch = 0;

const unsigned long scrollIntervalMs = 400;
const unsigned long scrollPauseSteps = 4;  // Pausa al inicio antes de desplazar el texto

volatile bool btnPressed = false;
volatile unsigned long lastBtnMs = 0;
const unsigned long debounceMs = 180;

// -------- Datos recibidos ----------
bool serverOnline = false;
float cpu_pct = 0, cpu_temp = NAN;
float gpu_pct = NAN, gpu_temp = NAN;
float vram_used = NAN, vram_total = NAN;  // MB
float mem_pct = 0, disk_pct = 0;
float mem_used = NAN, mem_total = NAN;    // MB
long net_up = 0, net_down = 0;            // kbps
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

// Los caracteres personalizados 0-7 también responden en los códigos 8-15,
// lo que permite usarlos dentro de cadenas (el 0 cortaría la cadena).
// Se definen como literales separados para que "\x08" no absorba el texto siguiente.
#define I_CHIP  "\x08"
#define I_THERM "\x09"
#define I_RAM   "\x0A"
#define I_DISK  "\x0B"
#define I_CLOUD "\x0C"
#define I_CLOCK "\x0D"
#define I_UP    "\x0E"
#define I_DOWN  "\x0F"
#define S_DEG   "\xDF"  // Símbolo de grado en la ROM A00 del HD44780

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
  lcd.createChar(0, ICON_CHIP);
  lcd.createChar(1, ICON_THERM);
  lcd.createChar(2, ICON_RAM);
  lcd.createChar(3, ICON_DISK);
  lcd.createChar(4, ICON_CLOUD);
  lcd.createChar(5, ICON_CLOCK);
  lcd.createChar(6, ICON_UP);
  lcd.createChar(7, ICON_DOWN);
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

  lastFetch = millis() - offlineRetryMs;  // Primera consulta inmediata
  pageShownMs = millis();
}

// Convierte texto UTF-8 al juego de caracteres del LCD (ROM A00):
// quita acentos y mapea ñ/ü a sus glifos; otros caracteres pasan a '?'.
String toLcdText(const String& in) {
  String out;
  out.reserve(in.length());
  for (unsigned int i = 0; i < in.length(); i++) {
    uint8_t c = in[i];
    if (c < 0x80) { out += (c < 0x20) ? ' ' : (char)c; continue; }
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

float jsonFloat(JsonVariantConst v) {
  return v.isNull() ? NAN : v.as<float>();
}

// Descarga las métricas del servidor y actualiza serverOnline.
void fetchMetrics() {
  serverOnline = false;
  if (WiFi.status() != WL_CONNECTED) return;

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
      cpu_temp = jsonFloat(doc["cpu_temp_c"]);
      mem_pct = doc["mem_pct"] | 0.0;
      mem_used = jsonFloat(doc["mem_used_mb"]);
      mem_total = jsonFloat(doc["mem_total_mb"]);
      disk_pct = doc["disk_pct"] | 0.0;
      net_up = doc["net_up_kbps"] | 0L;
      net_down = doc["net_down_kbps"] | 0L;
      gpu_pct = jsonFloat(doc["gpu_pct"]);
      gpu_temp = jsonFloat(doc["gpu_temp_c"]);
      vram_used = jsonFloat(doc["vram_used_mb"]);
      vram_total = jsonFloat(doc["vram_total_mb"]);

      JsonVariant weather = doc["weather"];
      if (weather.isNull()) {
        weather_temp = NAN;
        weather_desc = "N/A";
      } else {
        weather_temp = jsonFloat(weather["temp"]);
        weather_desc = weather["desc"].isNull() ? String("N/A") : toLcdText(weather["desc"].as<const char*>());
      }
    }
  }
  http.end();
}

// -------- Formato de valores ----------
// "  7%", " 42%", "100%" o " N/A"
void formatPct(char* buf, size_t len, float value) {
  if (isnan(value)) { snprintf(buf, len, " N/A"); return; }
  int pct = constrain((int)round(value), 0, 100);
  snprintf(buf, len, "%3d%%", pct);
}

// "48°C" o "--°C"
void formatTemp(char* buf, size_t len, float temp) {
  if (isnan(temp)) snprintf(buf, len, "--" S_DEG "C");
  else snprintf(buf, len, "%d" S_DEG "C", (int)round(temp));
}

// kbps en hasta 4 caracteres: "999k", "9.9M", "999M", "1.2G"
void formatRate(char* buf, size_t len, long kbps) {
  if (kbps < 1000)         snprintf(buf, len, "%ldk", kbps);
  else if (kbps < 9950)    snprintf(buf, len, "%.1fM", kbps / 1000.0);
  else if (kbps < 999500)  snprintf(buf, len, "%ldM", (kbps + 500) / 1000);
  else                     snprintf(buf, len, "%.1fG", kbps / 1000000.0);
}

// "usado/total" en GB: "2.5/8G"; sin decimal si no entra en maxChars. Vacío si no hay datos.
void formatGb(char* buf, size_t len, float usedMb, float totalMb, size_t maxChars) {
  if (isnan(usedMb) || isnan(totalMb) || totalMb <= 0) { buf[0] = '\0'; return; }
  float used = usedMb / 1024.0, total = totalMb / 1024.0;
  snprintf(buf, len, "%.1f/%.0fG", used, total);
  if (strlen(buf) > maxChars) snprintf(buf, len, "%.0f/%.0fG", used, total);
}

// -------- Pantallas: cada una arma sus dos líneas de texto ----------
typedef char Row[LCD_COLS + 1];

void pageOffline(Row l0, Row l1) {
  snprintf(l0, sizeof(Row), "Sin datos del PC");
  snprintf(l1, sizeof(Row), "%s", WiFi.status() == WL_CONNECTED ? "Reintentando..." : "WiFi caido...");
}

void pageCPU(Row l0, Row l1) {
  char pct[6], temp[8], up[8], down[8];
  formatPct(pct, sizeof(pct), cpu_pct);
  formatTemp(temp, sizeof(temp), cpu_temp);
  formatRate(up, sizeof(up), net_up);
  formatRate(down, sizeof(down), net_down);
  snprintf(l0, sizeof(Row), I_CHIP "CPU%s", pct);                              // ■CPU 23%
  snprintf(l1, sizeof(Row), I_THERM "%s " I_UP "%-4s" I_DOWN "%-4s", temp, up, down);  // ▯48°C ↑123k↓4.5M
}

void pageGPU(Row l0, Row l1) {
  char pct[6], temp[8], vram[12];
  formatPct(pct, sizeof(pct), gpu_pct);
  formatTemp(temp, sizeof(temp), gpu_temp);
  formatGb(vram, sizeof(vram), vram_used, vram_total, 10);
  snprintf(l0, sizeof(Row), I_CHIP "GPU%s", pct);       // ■GPU 40%
  snprintf(l1, sizeof(Row), I_THERM "%s %s", temp, vram);  // ▯65°C 2.0/4G
}

void pageMemDisk(Row l0, Row l1) {
  char mem[6], disk[6], gb[12];
  formatPct(mem, sizeof(mem), mem_pct);
  formatPct(disk, sizeof(disk), disk_pct);
  formatGb(gb, sizeof(gb), mem_used, mem_total, 7);
  snprintf(l0, sizeof(Row), I_RAM "RAM%s %s", mem, gb);  // ▣RAM 67% 8.0/16G
  snprintf(l1, sizeof(Row), I_DISK "DSK%s", disk);       // ▤DSK 55%
}

void pageDateTime(Row l0, Row l1) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  if (t.tm_year + 1900 < 2020) {  // NTP todavía no sincronizó
    snprintf(l0, sizeof(Row), "Sincronizando");
    snprintf(l1, sizeof(Row), "hora (NTP)...");
    return;
  }
  snprintf(l0, sizeof(Row), "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1, 1900 + t.tm_year);
  snprintf(l1, sizeof(Row), I_CLOCK " %02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
}

void pageWeather(Row l0, Row l1) {
  char temp[8];
  if (isnan(weather_temp)) snprintf(temp, sizeof(temp), "N/A");
  else formatTemp(temp, sizeof(temp), weather_temp);
  snprintf(l0, sizeof(Row), I_CLOUD " Clima %s", temp);

  // Si la descripción no entra, se desplaza en bucle separada por espacios
  unsigned int n = weather_desc.length();
  if (n <= LCD_COLS) {
    snprintf(l1, sizeof(Row), "%s", weather_desc.c_str());
    return;
  }
  String text = weather_desc + "   ";
  unsigned long step = (millis() - pageShownMs) / scrollIntervalMs;
  unsigned int start = step < scrollPauseSteps ? 0 : (step - scrollPauseSteps) % text.length();
  for (uint8_t i = 0; i < LCD_COLS; i++) l1[i] = text[(start + i) % text.length()];
  l1[LCD_COLS] = '\0';
}

// Escribe la fila completando con espacios, solo si cambió (evita parpadeo).
void drawRow(uint8_t row, const char* text) {
  Row padded;
  snprintf(padded, sizeof(padded), "%-16s", text);
  if (strcmp(padded, shownRows[row]) == 0) return;
  lcd.setCursor(0, row);
  lcd.print(padded);
  strcpy(shownRows[row], padded);
}

void render() {
  Row l0 = "", l1 = "";
  if (page == 3) pageDateTime(l0, l1);  // La hora no depende del servidor
  else if (!serverOnline) pageOffline(l0, l1);
  else {
    switch (page) {
      case 0: pageCPU(l0, l1); break;
      case 1: pageGPU(l0, l1); break;
      case 2: pageMemDisk(l0, l1); break;
      case 4: pageWeather(l0, l1); break;
    }
  }
  drawRow(0, l0);
  drawRow(1, l1);
}

void loop() {
  if (btnPressed) {
    btnPressed = false;
    page = (page + 1) % PAGE_COUNT;
    pageShownMs = millis();
  }

  unsigned long interval = serverOnline ? fetchIntervalMs : offlineRetryMs;
  if (millis() - lastFetch >= interval) {
    lastFetch = millis();
    fetchMetrics();
  }

  render();
  delay(20);
}
