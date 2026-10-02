# Changelog

Formato basado en [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/).

## [Sin publicar]

### Agregado
- El servidor se reconecta solo a LibreHardwareMonitor cada 30 s, así puede abrirse o reiniciarse sin reiniciar el servidor.
- Advertencia si `PC_MONITOR_TOKEN` es un valor de ejemplo o tiene menos de 16 caracteres.
- Pantalla de GPU con VRAM usada/total y pantalla de RAM con GB usados/totales.
- La descripción del clima se desplaza si no entra en el LCD.

### Cambiado
- El LCD solo reescribe las filas que cambian: sin parpadeo y con menos tráfico I2C.
- Timeout HTTP del ESP32 reducido a 800 ms y reintentos cada 5 s cuando el servidor no responde, para que el botón no se demore.
- CI: `actions/checkout` y `actions/setup-python` actualizadas a v7 (Node 24).

## [Profesionalización inicial] - 2026-10-02

### Corregido
- Temperaturas: se consultaba una clase `Sensor` inexistente en el espacio WMI por defecto y una propiedad `CurrentValue` que no existe; ahora se lee `Value` desde LibreHardwareMonitor/OpenHardwareMonitor sin dividir por 10.
- Zona horaria del ESP32: `ART-3` en POSIX equivale a UTC+3; se reemplazó por `<-03>3` (UTC-3).
- Red en el LCD: el servidor envía kbps pero el ESP32 los dividía por 1000 y casi siempre mostraba `0`; ahora se formatea como `k`/`M`/`G`.
- GPU: con `GPUtil` instalado pero sin NVIDIA nunca se intentaba la alternativa vía WMI.
- Disco: `/` en Windows medía la unidad actual, no `C:\`.
- Cada petición bloqueaba ~0,6 s y consultaba OpenWeather (hasta 60 veces por minuto); ahora se muestrea en segundo plano y el clima se cachea 10 minutos.
- El LCD se borraba y redibujaba 10 veces por segundo (parpadeo); ahora solo se redibuja cuando cambia algo.
- La pantalla de hora mostraba 01/01/1970 antes de sincronizar NTP.
- Los acentos de la descripción del clima se mostraban como caracteres basura en el LCD.
- Los errores de OpenWeather registraban la URL completa, incluida la API key.
- Los valores `NaN` de `GPUtil` (`[N/A]` en `nvidia-smi`) generaban JSON inválido; ahora se envían como `null`.
- Typo `pip in stall` en el README.

### Cambiado
- Configuración del servidor mediante variables de entorno; el token es obligatorio y ya no se imprime en consola.
- Comparación del token en tiempo constante (`hmac.compare_digest`) y servidor HTTP multihilo.
- Credenciales del ESP32 movidas a `config.h` (ignorado por git) a partir de `config.example.h`.
- Sketch renombrado de `sketch_aug12a` a `esp32_monitor`.
- Timeouts HTTP en el ESP32 y aviso "Sin datos del PC" cuando el servidor no responde.

### Agregado
- `LICENSE` (Unlicense, dominio público), `requirements.txt`, `.gitignore`, `.gitattributes`, `.editorconfig`, `ruff.toml`, CI de GitHub Actions y este changelog.
