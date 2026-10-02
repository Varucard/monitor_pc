// Copiá este archivo como config.h y completá tus datos.
// config.h está en .gitignore para no publicar credenciales.
#pragma once

// -------- Wi-Fi ----------
constexpr char WIFI_SSID[] = "TU_SSID";
constexpr char WIFI_PASS[] = "TU_PASS";

// -------- Servidor de métricas (pc_monitor_server.py) ----------
constexpr char PC_IP[]  = "192.168.1.50";
constexpr int  PC_PORT  = 5000;
constexpr char TOKEN[]  = "MI_TOKEN_SEGURO";  // Igual a PC_MONITOR_TOKEN del servidor

// -------- Zona horaria (POSIX) ----------
// Buenos Aires sin horario de verano. Otras zonas:
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
constexpr char TZ_INFO[] = "<-03>3";

// -------- Hardware ----------
constexpr uint8_t LCD_ADDR = 0x27;  // Algunos módulos usan 0x3F
constexpr int     BTN_PIN  = 15;    // Botón a GND (se usa INPUT_PULLUP)
