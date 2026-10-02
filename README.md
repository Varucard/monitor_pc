# 🖥 PC Resource Monitor — Servidor Python + cliente ESP32

[![CI](https://github.com/Varucard/monitor_pc/actions/workflows/ci.yml/badge.svg)](https://github.com/Varucard/monitor_pc/actions/workflows/ci.yml)
[![Licencia: Unlicense](https://img.shields.io/badge/licencia-Unlicense-blue.svg)](LICENSE)

Monitorea en tiempo real el uso de recursos de una PC (CPU, GPU, RAM, disco y red) y lo muestra en una pantalla LCD 16x2 conectada a un ESP32. Opcionalmente muestra la fecha y hora (NTP) y el clima de tu ciudad (OpenWeather).

---

## 📂 Estructura

```
monitor_pc/
├── pc_monitor_server.py          # Servidor de métricas (corre en la PC)
├── requirements.txt              # Dependencias de Python
└── esp32_monitor/
    ├── esp32_monitor.ino         # Cliente ESP32 + LCD
    └── config.example.h          # Plantilla de configuración (copiar a config.h)
```

- **Servidor Python**: muestrea las métricas del sistema cada segundo en segundo plano y las expone vía HTTP en `/metrics`.
- **Cliente ESP32**: consulta el servidor y presenta los datos en el LCD; un botón cambia de pantalla.

---

## 🚀 Características

- **CPU**: uso (%) y temperatura.
- **GPU**: uso (%), temperatura y memoria de video (VRAM).
  - NVIDIA: vía `GPUtil`.
  - AMD / Intel: vía LibreHardwareMonitor (WMI).
- **RAM y disco**: uso (%) y GB de RAM usados/totales.
- **Red**: velocidad de subida y bajada.
- **Clima**: temperatura y descripción (OpenWeather, se consulta cada 10 minutos).
- **Pantallas navegables** con un botón:
  1. CPU (uso + temperatura + red)
  2. GPU (uso + temperatura + VRAM)
  3. RAM y disco
  4. Fecha y hora (NTP)
  5. Clima (si la descripción no entra en 16 caracteres, se desplaza)

La pantalla solo reescribe lo que cambia, sin parpadeo. Si el servidor no responde, muestra "Sin datos del PC" y reintenta cada 5 segundos.

---

## 📦 Requisitos

### Servidor (PC)

- Python 3.8 o superior.
- **Windows** para las temperaturas (en Linux/macOS funciona, pero sin temperaturas de CPU ni GPU no NVIDIA).
- [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor) **ejecutándose** (como administrador) para leer temperaturas de CPU y GPU AMD/Intel. También sirve OpenHardwareMonitor. Windows no publica estas temperaturas por sí solo. Puede abrirse o reiniciarse con el servidor ya en marcha: se reconecta solo cada 30 segundos.
- Clave de API de [OpenWeather](https://openweathermap.org/api) (opcional, para el clima).

```bash
pip install -r requirements.txt
```

> `GPUtil` requiere drivers NVIDIA con `nvidia-smi`. Si no se puede importar (por ejemplo en Python 3.12+, donde ya no existe `distutils`), el servidor sigue funcionando y recurre a LibreHardwareMonitor.

### Cliente (ESP32)

- Placa ESP32 compatible con Arduino (core `esp32` de Espressif).
- Pantalla LCD 16x2 con módulo I2C (PCF8574).
- Pulsador entre GPIO 15 y GND (se usa `INPUT_PULLUP`, no requiere resistencia).
- Librerías (Gestor de librerías del Arduino IDE):
  - **ArduinoJson** (v6 o v7)
  - **LiquidCrystal I2C**

---

## ⚙️ Configuración

### Servidor Python

La configuración se toma de variables de entorno, así no quedan secretos en el código:

| Variable | Descripción | Por defecto |
|---|---|---|
| `PC_MONITOR_TOKEN` | Token que debe enviar el ESP32 en `X-Token`. **Obligatorio.** | — |
| `PC_MONITOR_PORT` | Puerto HTTP. | `5000` |
| `PC_MONITOR_HOST` | Interfaz de escucha. | `0.0.0.0` |
| `PC_MONITOR_DISK` | Unidad a medir. | `C:\` en Windows, `/` en el resto |
| `OPENWEATHER_API_KEY` | Clave de OpenWeather. Sin ella no se muestra el clima. | — |
| `OPENWEATHER_CITY` | Ciudad y país. | `Buenos Aires,AR` |
| `OPENWEATHER_LANG` | Idioma de la descripción del clima. | `es` |
| `PC_MONITOR_LOG_LEVEL` | Nivel de log (`DEBUG` muestra cada petición). | `INFO` |

Para generar un token seguro:

```bash
python -c "import secrets; print(secrets.token_urlsafe(24))"
```

Si el token es un valor de ejemplo o tiene menos de 16 caracteres, el servidor arranca igual pero muestra una advertencia.

Ejemplo en PowerShell:

```powershell
$env:PC_MONITOR_TOKEN = "un-token-largo-y-aleatorio"
$env:OPENWEATHER_API_KEY = "tu_api_key"
python pc_monitor_server.py
```

Ejemplo en Linux/macOS:

```bash
PC_MONITOR_TOKEN="un-token-largo-y-aleatorio" python3 pc_monitor_server.py
```

El servidor queda escuchando en `http://<IP_PC>:5000/metrics`. Si el firewall de Windows pregunta, permití el acceso en redes privadas.

### Cliente ESP32

1. Copiá `esp32_monitor/config.example.h` como `esp32_monitor/config.h` (este archivo está en `.gitignore`).
2. Completá Wi-Fi, IP de la PC, puerto, token (el mismo que `PC_MONITOR_TOKEN`), zona horaria, dirección I2C del LCD y pin del botón.
3. Abrí `esp32_monitor/esp32_monitor.ino` con Arduino IDE (o PlatformIO) y cargalo en el ESP32.

> Conviene reservar una IP fija para la PC en el router (DHCP estático) para que `PC_IP` no cambie.

### Conexiones

| Componente | ESP32 |
|---|---|
| LCD SDA | GPIO 21 |
| LCD SCL | GPIO 22 |
| LCD VCC / GND | 5V (VIN) / GND |
| Botón | GPIO 15 ↔ GND |

---

## 📊 API

`GET /metrics` con el encabezado `X-Token: <token>`.

| Código | Significado |
|---|---|
| `200` | Métricas en JSON. |
| `401` | Token ausente o incorrecto. |
| `404` | Ruta inexistente. |
| `503` | El servidor acaba de arrancar y todavía no tiene la primera muestra. |

```bash
curl -H "X-Token: un-token-largo-y-aleatorio" http://localhost:5000/metrics
```

```json
{
  "host": "NOMBRE_PC",
  "ts": 1691501234.123,
  "cpu_pct": 23.5,
  "cpu_temp_c": 48.0,
  "mem_pct": 67.2,
  "mem_used_mb": 8192,
  "mem_total_mb": 12288,
  "disk_pct": 55.1,
  "net_up_kbps": 123,
  "net_down_kbps": 456,
  "gpu_pct": 40.1,
  "gpu_temp_c": 65.0,
  "vram_used_mb": 2048,
  "vram_total_mb": 4096,
  "weather": { "temp": 25.4, "desc": "cielo claro" }
}
```

Los campos que no se pueden leer en el equipo se envían como `null`.

---

## 🛠 Solución de problemas

| Síntoma | Causa probable |
|---|---|
| El LCD dice "Sin datos del PC" | IP/puerto incorrectos, token distinto, servidor detenido o firewall bloqueando el puerto. |
| Temperatura `--°C` | LibreHardwareMonitor no está ejecutándose como administrador (al abrirlo, el servidor lo detecta en menos de 30 s). |
| GPU `N/A` | No hay GPU NVIDIA con `GPUtil` funcional ni LibreHardwareMonitor. |
| LCD encendido pero sin texto | Ajustar el contraste (potenciómetro del módulo I2C) o probar `LCD_ADDR = 0x3F`. |
| La hora muestra "Sincronizando" | El ESP32 aún no obtuvo la hora por NTP (requiere salida a internet). |

---

## 🔒 Seguridad

- El endpoint `/metrics` exige el token `X-Token`; usá un valor largo y aleatorio.
- El tráfico es HTTP sin cifrar: usalo solo en tu red local o detrás de una VPN.
- No expongas el puerto directamente a internet.

## 🤝 Contribuir

1. Creá una rama desde `dev` (la única rama principal del repositorio).
2. Verificá el estilo del servidor con `ruff check . && ruff format --check .`.
3. Abrí un Pull Request hacia `dev`. La CI compila el sketch y revisa el código Python.

## 📄 Licencia

Software libre de dominio público bajo [The Unlicense](https://unlicense.org): podés usarlo, modificarlo y distribuirlo sin restricciones. Ver [LICENSE](LICENSE).
