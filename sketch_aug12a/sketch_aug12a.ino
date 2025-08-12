/*
  ESP32 PC Resource Monitor - LCD 16x2 + botón
  Pantallas (con botón en GPIO 15):
  0) CPU: uso y temp
  1) GPU: uso y temp
  2) RAM y Disco: uso %
  3) Fecha y Hora (NTP)
  4) Clima: temp y desc
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

// -------- Configuración Wi-Fi y servidor ----------
const char* WIFI_SSID = "TU_SSID";
const char* WIFI_PASS = "TU_PASS";
const char* PC_IP     = "192.168.1.50";
const int   PC_PORT   = 5000;
const char* TOKEN     = "MI_TOKEN_SEGURO";

// -------- LCD I2C ----------
LiquidCrystal_I2C lcd(0x27, 16, 2);  // Cambia 0x27 a 0x3F si corresponde

// -------- Botón ----------
const int BTN_PIN = 15;  // Conectar a GND, usar INPUT_PULLUP
volatile bool btnPressed = false;

// -------- NTP / Zona horaria ----------
/*
  Zona horaria (Buenos Aires sin DST):
  TZ string POSIX: "ART-3"
  Ver otras TZ: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
*/
const char* TZ_INFO = "ART-3";

// -------- Variables de UI ----------
uint8_t page = 0;                 // 0..4
unsigned long lastFetch = 0;
const unsigned long fetchIntervalMs = 1000; // 1s
unsigned long lastBtnMs = 0;
const unsigned long debounceMs = 180;

// -------- Datos recibidos ----------
float cpu_pct = 0, cpu_temp = NAN;
float gpu_pct = NAN, gpu_temp = NAN;
float mem_pct = 0, disk_pct = 0;
int net_up = 0, net_down = 0;
float weather_temp = NAN;
String weather_desc = "N/A";

// -------- Iconos personalizados (máx 8) ----------
/* 5x8 píxeles por carácter */
byte ICON_CHIP[8] = {
  B11111,
  B10001,
  B10101,
  B10101,
  B10101,
  B10001,
  B11111,
  B00000
};
byte ICON_THERM[8] = {
  B00100,
  B01010,
  B01010,
  B01010,
  B01010,
  B01110,
  B01110,
  B00100
};
byte ICON_RAM[8] = {
  B11111,
  B10001,
  B10101,
  B10101,
  B10001,
  B11111,
  B00100,
  B00100
};
byte ICON_DISK[8] = {
  B11111,
  B10001,
  B10111,
  B10101,
  B10101,
  B10001,
  B11111,
  B00000
};
byte ICON_CLOUD[8] = {
  B00000,
  B00000,
  B00111,
  B01111,
  B11111,
  B11111,
  B01110,
  B00000
};
byte ICON_CLOCK[8] = {
  B00100,
  B01010,
  B10001,
  B10001,
  B10101,
  B10001,
  B01010,
  B00100
};
byte ICON_UP[8] = {
  B00100,
  B01110,
  B11111,
  B00100,
  B00100,
  B00100,
  B00100,
  B00100
};
byte ICON_DOWN[8] = {
  B00100,
  B00100,
  B00100,
  B00100,
  B11111,
  B01110,
  B00100,
  B00000
};

// Intento de imprimir el símbolo de grado: en la mayoría de HD44780 es (char)223
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
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lcd.setCursor(0, 0); lcd.print("Conectando WiFi");
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    lcd.setCursor(0, 1);
    lcd.print("    ");
    for (int i = 0; i <= (dots % 4); i++) lcd.print(".");
    dots++;
  }
}

void setupNTP() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", TZ_INFO, 1);
  tzset();
}

void setup() {
  pinMode(BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), onBtn, FALLING);

  setupLCD();
  setupWiFi();
  setupNTP();

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Monitor PC listo");
  lcd.setCursor(0,1); lcd.print(WiFi.localIP().toString());
  delay(1200);
  lcd.clear();
}

void fetchMetrics() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String("http://") + PC_IP + ":" + String(PC_PORT) + "/metrics";
  http.begin(url);
  http.addHeader("X-Token", TOKEN);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      cpu_pct = doc["cpu_pct"] | 0.0;
      cpu_temp = doc["cpu_temp_c"].isNull() ? NAN : doc["cpu_temp_c"].as<float>();
      mem_pct = doc["mem_pct"] | 0.0;
      disk_pct = doc["disk_pct"] | 0.0;
      net_up = doc["net_up_kbps"] | 0;
      net_down = doc["net_down_kbps"] | 0;
      gpu_pct = doc["gpu_pct"].isNull() ? NAN : doc["gpu_pct"].as<float>();
      gpu_temp = doc["gpu_temp_c"].isNull() ? NAN : doc["gpu_temp_c"].as<float>();

      if (doc["weather"].isNull()) {
        weather_temp = NAN;
        weather_desc = "N/A";
      } else {
        weather_temp = doc["weather"]["temp"].isNull() ? NAN : doc["weather"]["temp"].as<float>();
        weather_desc = doc["weather"]["desc"].isNull() ? "N/A" : String(doc["weather"]["desc"].as<const char*>());
      }
    }
  }
  http.end();
}

void printPctAt(int col, int row, int pct) {
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  char buf[6];
  snprintf(buf, sizeof(buf), "%3d%%", pct);
  lcd.setCursor(col, row); lcd.print(buf);
}

void showCPU() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.write((uint8_t)0); lcd.print("CPU ");
  printPctAt(5,0, (int)round(cpu_pct));
  lcd.setCursor(0,1); lcd.write((uint8_t)1); // termometro
  lcd.print(" ");
  if (!isnan(cpu_temp)) {
    lcd.print((int)round(cpu_temp)); lcd.print(DEG); lcd.print("C ");
  } else {
    lcd.print("--"); lcd.print(DEG); lcd.print("C ");
  }
  lcd.write((uint8_t)6); lcd.print(net_up/1000);  // up kbps
  lcd.print(" ");
  lcd.write((uint8_t)7); lcd.print(net_down/1000);
}

void showGPU() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.write((uint8_t)0); lcd.print("GPU ");
  if (!isnan(gpu_pct)) printPctAt(5,0,(int)round(gpu_pct));
  else { lcd.setCursor(5,0); lcd.print(" N/A"); }

  lcd.setCursor(0,1); lcd.write((uint8_t)1); lcd.print(" ");
  if (!isnan(gpu_temp)) { lcd.print((int)round(gpu_temp)); lcd.print(DEG); lcd.print("C"); }
  else lcd.print("N/A");
}

void showMemDisk() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.write((uint8_t)2); lcd.print("RAM ");
  printPctAt(5,0,(int)round(mem_pct));
  lcd.setCursor(0,1); lcd.write((uint8_t)3); lcd.print("DSK ");
  printPctAt(5,1,(int)round(disk_pct));
}

void showDateTime() {
  lcd.clear();
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);

  // Línea 0: Fecha DD/MM YYYY
  lcd.setCursor(0,0);
  char buf1[17];
  snprintf(buf1, sizeof(buf1), "%02d/%02d %04d", t.tm_mday, t.tm_mon+1, 1900+t.tm_year);
  lcd.print(buf1);

  // Línea 1: Icono + HH:MM:SS
  lcd.setCursor(0,1); lcd.write((uint8_t)5); lcd.print(" ");
  char buf2[17];
  snprintf(buf2, sizeof(buf2), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  lcd.print(buf2);
}

void showWeather() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.write((uint8_t)4); lcd.print(" Clima ");
  if (!isnan(weather_temp)) {
    lcd.print((int)round(weather_temp)); lcd.print(DEG); lcd.print("C");
  } else {
    lcd.print("N/A");
  }
  lcd.setCursor(0,1);
  String d = weather_desc;
  if (d.length() > 16) d = d.substring(0,16);
  lcd.print(d);
}

void loop() {
  // Botón: cambia de pantalla
  if (btnPressed) {
    btnPressed = false;
    page = (page + 1) % 5; // 0..4
  }

  // Fetch periódico
  if (millis() - lastFetch >= fetchIntervalMs) {
    lastFetch = millis();
    fetchMetrics();
  }

  // Render según página
  switch (page) {
    case 0: showCPU(); break;
    case 1: showGPU(); break;
    case 2: showMemDisk(); break;
    case 3: showDateTime(); break;
    case 4: showWeather(); break;
  }

  delay(100);
}