# pc_monitor_server.py
import json, time, socket
from http.server import BaseHTTPRequestHandler, HTTPServer
import psutil
import wmi # Para temperaturas de CPU y GPU (si no es NVIDIA)
try:
  import GPUtil # Para uso y temperatura de GPU NVIDIA
except ImportError:
  GPUtil = None
import requests # Para obtener el clima (opcional)

PORT = 5000
TOKEN = "MI_TOKEN_SEGURO"
WEATHER_API_KEY = "TU_API_KEY_OPENWEATHER" # Reemplaza con tu clave de API
CITY_NAME = "Buenos Aires,AR" # Reemplaza con tu ciudad y código de país

# Inicializar WMI
c = wmi.WMI()

def get_cpu_temp():
  try:
    # Busca sensores de temperatura de CPU
    # Esto puede variar según el hardware y los drivers
    temperature_data = c.Sensor().query("SELECT CurrentValue FROM Sensor WHERE SensorType = 'Temperature' AND Name LIKE '%CPU%'")
    if temperature_data:
      # WMI a veces devuelve en décimas de grado Celsius, o en Kelvin.
      # Asumimos Celsius aquí, ajusta si es necesario.
      return round(temperature_data[0].CurrentValue / 10.0, 1) # Si viene en décimas
    return None
  except Exception:
    return None

def get_gpu_metrics():
  gpu_load = None
  gpu_temp = None
  vram_used = None
  vram_total = None

  if GPUtil:
    try:
      gpus = GPUtil.getGPUs()
      if gpus:
        # Asume la GPU con mayor carga o la primera
        main_gpu = max(gpus, key=lambda g: g.load) if gpus else gpus[0]
        gpu_load = round(main_gpu.load * 100, 1)
        gpu_temp = round(main_gpu.temperature, 1)
        vram_used = round(main_gpu.memoryUsed, 1)
        vram_total = round(main_gpu.memoryTotal, 1)
    except Exception:
      pass # GPUtil puede fallar si no hay GPU NVIDIA o drivers
  else:
    # Intenta con WMI para otras GPUs (AMD/Intel)
    try:
      # Esto es más genérico y puede no funcionar para todas las GPUs
      # Busca sensores de temperatura de GPU
      gpu_temp_data = c.Sensor().query("SELECT CurrentValue FROM Sensor WHERE SensorType = 'Temperature' AND Name LIKE '%GPU%'")
      if gpu_temp_data:
        gpu_temp = round(gpu_temp_data[0].CurrentValue / 10.0, 1) # Si viene en décimas
      
      # Uso de GPU con WMI es más complejo y menos fiable que GPUtil
      # Puedes intentar con 'Win32_PerfFormattedData_GPUPerformance_GPUEngine'
      # Pero es más lento y menos directo. Para simplicidad, lo omitimos aquí.
    except Exception:
      pass

  return gpu_load, gpu_temp, vram_used, vram_total

def get_weather_data():
  if not WEATHER_API_KEY or not CITY_NAME:
    return None

  try:
    url = f"http://api.openweathermap.org/data/2.5/weather?q={CITY_NAME}&appid={WEATHER_API_KEY}&units=metric"
    response = requests.get(url, timeout=5)
    response.raise_for_status() # Lanza excepción para errores HTTP
    data = response.json()
    
    temp = round(data['main']['temp'], 1)
    description = data['weather'][0]['description']
    return {"temp": temp, "desc": description}
  except Exception:
    return None

def get_metrics():
  # CPU
  cpu_percent = psutil.cpu_percent(interval=0.3)
  cpu_temp = get_cpu_temp()

  # Memoria
  mem = psutil.virtual_memory()

  # Disco (solo C: por simplicidad)
  disk = psutil.disk_usage('/')

  # Red (bytes por segundo)
  net1 = psutil.net_io_counters()
  time.sleep(0.3) # Pequeña pausa para calcular la diferencia
  net2 = psutil.net_io_counters()
  net_up_bps = (net2.bytes_sent - net1.bytes_sent) * 8 / 0.3
  net_down_bps = (net2.bytes_recv - net1.bytes_recv) * 8 / 0.3

  # GPU
  gpu_load, gpu_temp, vram_used, vram_total = get_gpu_metrics()

  # Clima
  weather = get_weather_data()

  return {
    "host": socket.gethostname(),
    "ts": time.time(),
    "cpu_pct": cpu_percent,
    "cpu_temp_c": cpu_temp,
    "mem_pct": mem.percent,
    "mem_used_mb": round(mem.used / (1024*1024)),
    "mem_total_mb": round(mem.total / (1024*1024)),
    "disk_pct": disk.percent,
    "net_up_kbps": int(net_up_bps / 1000), # Convertir a kbps
    "net_down_kbps": int(net_down_bps / 1000), # Convertir a kbps
    "gpu_pct": gpu_load,
    "gpu_temp_c": gpu_temp,
    "vram_used_mb": vram_used,
    "vram_total_mb": vram_total,
    "weather": weather
  }

class Handler(BaseHTTPRequestHandler):
  def do_GET(self):
    if self.path != "/metrics":
      self.send_response(404); self.end_headers(); return
    if self.headers.get("X-Token") != TOKEN:
      self.send_response(401); self.end_headers(); return
    
    data = get_metrics()
    body = json.dumps(data).encode("utf-8")
    
    self.send_response(200)
    self.send_header("Content-Type", "application/json")
    self.send_header("Access-Control-Allow-Origin", "*") # Permite CORS
    self.send_header("Content-Length", str(len(body)))
    self.end_headers()
    self.wfile.write(body)

if __name__ == "__main__":
  server = HTTPServer(("0.0.0.0", PORT), Handler)
  print(f"Sirviendo métricas en http://0.0.0.0:{PORT}/metrics")
  print(f"Token requerido: {TOKEN}")
  server.serve_forever()