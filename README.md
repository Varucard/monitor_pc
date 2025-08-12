# 🖥 PC Resource Monitor - Servidor y Cliente ESP32
Este proyecto permite monitorear en tiempo real el uso de recursos de una PC (CPU, GPU, RAM, disco, red) y mostrar la información en una pantalla LCD 16x2 conectada a un ESP32.
Opcionalmente, también muestra información meteorológica obtenida desde OpenWeather, hora y clima de la habitación donde se encuentre.

---

## 📂 Estructura del Sistema

- **Servidor Python** (`pc_monitor_server.py`):  
  Recopila métricas del sistema y las expone vía HTTP.

- **Cliente ESP32** (código Arduino):  
  Consume las métricas del servidor y las presenta en la pantalla LCD, con navegación mediante un botón.

---

## 🚀 Características

- **CPU**: uso (%) y temperatura.  
- **GPU**: uso (%), temperatura y memoria de video.  
  - NVIDIA: vía `GPUtil`.
  - Otras: vía WMI (Windows).
- **Memoria RAM y Disco**: uso (%) y valores totales.  
- **Red**: velocidad de subida y bajada (kbps).  
- **Clima**: temperatura y descripción (OpenWeather).  
- **Pantallas navegables** con un botón:
  1. CPU (uso + temperatura + red)
  2. GPU (uso + temperatura)
  3. RAM y Disco
  4. Fecha y Hora (NTP)
  5. Clima

---

## 📦 Requisitos

### Servidor (PC)
- Python 3.8 o superior
- Sistema operativo **Windows** (WMI disponible nativamente)
- Clave de API de **OpenWeather** (opcional para clima)
- Librerías Python:
  ```bash
  pip in stall psutil wmi requests gputil
  ```

### Cliente (ESP32)
- Placa ESP32 compatible con Arduino.
- Pantalla LCD 16x2 con interfaz I2C.
- Botón conectado a GPIO 15 (a GND, con INPUT_PULLUP).
- Librerías Arduino:
  ```bash
  WiFi.h
  HTTPClient.h
  ArduinoJson
  LiquidCrystal_I2C
  Wire.h
  ```

---

## ⚙️ Configuración
- Servidor Python (`pc_monitor_server.py`)
Edita las variables:

  ```python
  PORT = 5000
  TOKEN = "MI_TOKEN_SEGURO"
  WEATHER_API_KEY = "TU_API_KEY_OPENWEATHER"  # opcional
  CITY_NAME = "Buenos Aires,AR"
  ```

- Ejecuta el servidor:

  ```bash
  python pc_monitor_server.py
  ```

- El servidor quedará escuchando en:

  ```arduino
  http://<IP_PC>:5000/metrics
  ```

- con autenticación vía encabezado X-Token.

### Cliente ESP32
- Edita las credenciales y dirección del servidor:

  ```cpp
  const char* WIFI_SSID = "TU_SSID";
  const char* WIFI_PASS = "TU_PASS";
  const char* PC_IP     = "192.168.1.50"; // IP del servidor
  const int   PC_PORT   = 5000;
  const char* TOKEN     = "MI_TOKEN_SEGURO";
  ```

- Carga el código en el ESP32 desde Arduino IDE o PlatformIO.

---

### Conecta el LCD y el botón:

- **LCD I2C** → pines SDA/SCL del ESP32.

- **Botón** → GPIO 15 a GND (con INPUT_PULLUP).

---

## 📊 Estructura de la API
- El servidor expone un endpoint JSON en /metrics con la siguiente estructura:

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
    "weather": {
      "temp": 25.4,
      "desc": "cielo claro"
    }
  }
  ```

---

## 🖥 Ejemplo de Uso
- Inicia el servidor en tu PC.
- Conecta y enciende el ESP32.
- Navega entre pantallas con el botón.
- Observa en tiempo real las métricas y el clima (si está configurado).

---

## 🔒 Seguridad
- El endpoint /metrics requiere un token (X-Token) para evitar accesos no autorizados.
- Se recomienda ejecutar en red local o detrás de una VPN.
- No exponer directamente a internet sin protección adicional.

## 📌 Notas
- La lectura de temperatura de CPU y GPU depende del hardware y drivers instalados.
- GPUtil solo funciona con GPUs NVIDIA y drivers correctos.
- WMI puede devolver valores en décimas de grado o Kelvin; el código asume Celsius.
- Para el clima, se necesita una clave de API válida de OpenWeather.

## 📄 Licencia
Este proyecto se distribuye bajo la licencia MIT.