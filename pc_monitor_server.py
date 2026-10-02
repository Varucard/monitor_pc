"""Servidor HTTP que expone métricas de la PC para el monitor ESP32.

Las métricas se muestrean en segundo plano a intervalos fijos, de modo que
cada petición a ``/metrics`` responde de inmediato con la última muestra.

Configuración mediante variables de entorno (ver README.md):
    PC_MONITOR_TOKEN      Token requerido en el encabezado X-Token (obligatorio).
    PC_MONITOR_HOST       Interfaz de escucha (por defecto 0.0.0.0).
    PC_MONITOR_PORT       Puerto HTTP (por defecto 5000).
    PC_MONITOR_DISK       Unidad o punto de montaje a medir (por defecto C:\\ o /).
    OPENWEATHER_API_KEY   Clave de OpenWeather (opcional; sin ella no hay clima).
    OPENWEATHER_CITY      Ciudad, por ejemplo "Buenos Aires,AR".
    OPENWEATHER_LANG      Idioma de la descripción del clima (por defecto "es").
"""

from __future__ import annotations

import hmac
import json
import logging
import math
import os
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

import psutil
import requests

try:  # Uso, temperatura y VRAM de GPUs NVIDIA
    import GPUtil
except Exception:  # GPUtil falla al importar en algunas versiones de Python
    GPUtil = None

try:  # Sensores de LibreHardwareMonitor / OpenHardwareMonitor (solo Windows)
    import pythoncom
    import wmi
except ImportError:
    pythoncom = None
    wmi = None

# -------- Configuración ----------
HOST = os.environ.get("PC_MONITOR_HOST", "0.0.0.0")
PORT = int(os.environ.get("PC_MONITOR_PORT", "5000"))
TOKEN = os.environ.get("PC_MONITOR_TOKEN", "")
DISK_PATH = os.environ.get("PC_MONITOR_DISK", "C:\\" if os.name == "nt" else "/")
WEATHER_API_KEY = os.environ.get("OPENWEATHER_API_KEY", "")
CITY_NAME = os.environ.get("OPENWEATHER_CITY", "Buenos Aires,AR")
WEATHER_LANG = os.environ.get("OPENWEATHER_LANG", "es")

SAMPLE_INTERVAL_S = 1.0
WEATHER_TTL_S = 600  # El plan gratuito de OpenWeather limita las consultas
WEATHER_RETRY_S = 60
WEATHER_URL = "https://api.openweathermap.org/data/2.5/weather"

# Espacios de nombres WMI que publican los monitores de hardware
HW_MONITOR_NAMESPACES = ("root\\LibreHardwareMonitor", "root\\OpenHardwareMonitor")
CPU_TEMP_NAMES = ("CPU Package", "Core (Tctl/Tdie)", "Core (Tctl)", "Core (Tdie)", "CPU Core")
GPU_TEMP_NAMES = ("GPU Core",)
GPU_LOAD_NAMES = ("GPU Core",)
VRAM_USED_NAMES = ("GPU Memory Used", "D3D Dedicated Memory Used")
VRAM_TOTAL_NAMES = ("GPU Memory Total", "D3D Dedicated Memory Total")

MB = 1024 * 1024

log = logging.getLogger("pc_monitor")


# -------- Sensores de hardware (WMI) ----------
def connect_hw_monitor():
    """Devuelve una conexión WMI al monitor de hardware, o None si no está disponible.

    Debe llamarse desde el mismo hilo que luego consulta los sensores (COM).
    """
    if wmi is None:
        return None
    for namespace in HW_MONITOR_NAMESPACES:
        try:
            conn = wmi.WMI(namespace=namespace)
            conn.query("SELECT Name FROM Sensor")
            log.info("Sensores de hardware vía WMI: %s", namespace)
            return conn
        except Exception:
            continue
    log.warning("No se encontró LibreHardwareMonitor ni OpenHardwareMonitor; las temperaturas no estarán disponibles.")
    return None


def read_hw_sensors(conn):
    """Lee los sensores relevantes como lista de (identificador, nombre, tipo, valor)."""
    if conn is None:
        return []
    try:
        rows = conn.query(
            "SELECT Identifier, Name, SensorType, Value FROM Sensor "
            "WHERE SensorType = 'Temperature' OR SensorType = 'Load' "
            "OR SensorType = 'SmallData'"
        )
        return [(r.Identifier.lower(), r.Name, r.SensorType, r.Value) for r in rows]
    except Exception:
        log.debug("Error leyendo sensores WMI", exc_info=True)
        return []


def find_sensor(sensors, device, sensor_type, preferred_names, fallback_max=False):
    """Busca un sensor del dispositivo indicado ("cpu" o "gpu").

    El identificador tiene la forma "/intelcpu/0/temperature/0" o
    "/gpu-nvidia/0/load/0"; el primer segmento identifica el hardware.
    """
    candidates = [
        (name, value)
        for ident, name, stype, value in sensors
        if stype == sensor_type and _finite(value) is not None and device in ident.strip("/").split("/")[0]
    ]
    for preferred in preferred_names:
        for name, value in candidates:
            if name == preferred:
                return round(float(value), 1)
    if fallback_max and candidates:
        return round(max(float(v) for _, v in candidates), 1)
    return None


# -------- GPU ----------
def _finite(value, ndigits=1):
    """Redondea el valor, o devuelve None si es NaN/infinito (JSON no admite NaN)."""
    return round(value, ndigits) if value is not None and math.isfinite(value) else None


def get_gpu_metrics(sensors):
    """Devuelve (uso %, temperatura °C, VRAM usada MB, VRAM total MB)."""
    if GPUtil is not None:
        try:
            gpus = GPUtil.getGPUs()
            if gpus:
                gpu = max(gpus, key=lambda g: g.load)  # La GPU con mayor carga
                return (
                    _finite(gpu.load * 100),
                    _finite(gpu.temperature),
                    _finite(gpu.memoryUsed),
                    _finite(gpu.memoryTotal),
                )
        except Exception:
            log.debug("GPUtil no pudo leer la GPU", exc_info=True)

    # Sin NVIDIA: se recurre al monitor de hardware (AMD / Intel)
    return (
        find_sensor(sensors, "gpu", "Load", GPU_LOAD_NAMES),
        find_sensor(sensors, "gpu", "Temperature", GPU_TEMP_NAMES, fallback_max=True),
        find_sensor(sensors, "gpu", "SmallData", VRAM_USED_NAMES),
        find_sensor(sensors, "gpu", "SmallData", VRAM_TOTAL_NAMES),
    )


# -------- Clima ----------
class WeatherCache:
    """Consulta OpenWeather como máximo una vez cada WEATHER_TTL_S segundos."""

    def __init__(self):
        self._data = None
        self._next_fetch = 0.0

    def get(self):
        if not WEATHER_API_KEY or not CITY_NAME:
            return None
        now = time.monotonic()
        if now >= self._next_fetch:
            try:
                self._data = self._fetch()
                self._next_fetch = now + WEATHER_TTL_S
            except requests.HTTPError as exc:
                # No se registra la excepción completa: su mensaje incluye la URL con la API key
                log.warning("No se pudo obtener el clima: HTTP %s", exc.response.status_code)
                self._next_fetch = now + WEATHER_RETRY_S
            except Exception as exc:
                log.warning("No se pudo obtener el clima: %s", type(exc).__name__)
                self._next_fetch = now + WEATHER_RETRY_S
        return self._data

    @staticmethod
    def _fetch():
        response = requests.get(
            WEATHER_URL,
            params={"q": CITY_NAME, "appid": WEATHER_API_KEY, "units": "metric", "lang": WEATHER_LANG},
            timeout=5,
        )
        response.raise_for_status()
        data = response.json()
        return {
            "temp": round(data["main"]["temp"], 1),
            "desc": data["weather"][0]["description"],
        }


# -------- Muestreo en segundo plano ----------
class MetricsSampler(threading.Thread):
    """Hilo que toma una muestra de métricas cada SAMPLE_INTERVAL_S segundos."""

    def __init__(self):
        super().__init__(name="metrics-sampler", daemon=True)
        self._lock = threading.Lock()
        self._snapshot = None
        self._weather = WeatherCache()
        self._hostname = socket.gethostname()

    def snapshot(self):
        with self._lock:
            return self._snapshot

    def run(self):
        if pythoncom is not None:
            pythoncom.CoInitialize()  # WMI usa COM, que se inicializa por hilo
        hw_conn = connect_hw_monitor()

        psutil.cpu_percent(interval=None)  # La primera lectura siempre es 0
        prev_net = psutil.net_io_counters()
        prev_t = time.monotonic()

        while True:
            time.sleep(SAMPLE_INTERVAL_S)
            try:
                net = psutil.net_io_counters()
                now = time.monotonic()
                data = self._sample(hw_conn, prev_net, net, now - prev_t)
                prev_net, prev_t = net, now
                with self._lock:
                    self._snapshot = data
            except Exception:
                log.exception("Error tomando la muestra de métricas")

    def _sample(self, hw_conn, prev_net, net, elapsed):
        sensors = read_hw_sensors(hw_conn)
        mem = psutil.virtual_memory()
        disk = psutil.disk_usage(DISK_PATH)
        gpu_load, gpu_temp, vram_used, vram_total = get_gpu_metrics(sensors)

        up_bps = max(0, net.bytes_sent - prev_net.bytes_sent) * 8 / elapsed
        down_bps = max(0, net.bytes_recv - prev_net.bytes_recv) * 8 / elapsed

        return {
            "host": self._hostname,
            "ts": time.time(),
            "cpu_pct": psutil.cpu_percent(interval=None),
            "cpu_temp_c": find_sensor(sensors, "cpu", "Temperature", CPU_TEMP_NAMES, fallback_max=True),
            "mem_pct": mem.percent,
            "mem_used_mb": round(mem.used / MB),
            "mem_total_mb": round(mem.total / MB),
            "disk_pct": disk.percent,
            "net_up_kbps": int(up_bps / 1000),
            "net_down_kbps": int(down_bps / 1000),
            "gpu_pct": gpu_load,
            "gpu_temp_c": gpu_temp,
            "vram_used_mb": vram_used,
            "vram_total_mb": vram_total,
            "weather": self._weather.get(),
        }


# -------- Servidor HTTP ----------
class Handler(BaseHTTPRequestHandler):
    sampler: MetricsSampler  # Se asigna en main()

    def do_GET(self):
        if urlsplit(self.path).path != "/metrics":
            self._send_status(404)
            return
        token = self.headers.get("X-Token", "")
        if not hmac.compare_digest(token.encode(), TOKEN.encode()):
            self._send_status(401)
            return

        data = self.sampler.snapshot()
        if data is None:  # Aún no hay una primera muestra
            self._send_status(503)
            return

        body = json.dumps(data, allow_nan=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_status(self, code):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, fmt, *args):
        # El ESP32 consulta cada segundo: solo se registra en modo depuración
        log.debug("%s - %s", self.address_string(), fmt % args)


def main():
    logging.basicConfig(
        level=os.environ.get("PC_MONITOR_LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(message)s",
    )
    if not TOKEN:
        log.error("Definí la variable de entorno PC_MONITOR_TOKEN antes de iniciar el servidor.")
        sys.exit(1)

    sampler = MetricsSampler()
    sampler.start()
    Handler.sampler = sampler

    server = ThreadingHTTPServer((HOST, PORT), Handler)
    log.info("Sirviendo métricas en http://%s:%d/metrics", HOST, PORT)
    if not WEATHER_API_KEY:
        log.info("OPENWEATHER_API_KEY no definida: el clima quedará deshabilitado.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("Servidor detenido.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
