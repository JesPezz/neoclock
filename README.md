# NeoClock

Reloj interactivo con un anillo NeoPixel (WS2812B) de 16 LEDs controlado por ESP32, con interfaz web para configurar colores, brillo, modos y alarma.

Las agujas (hora, minuto y segundo) se dibujan en posiciones flotantes y se reparten entre los dos LEDs adyacentes (anti-aliasing con gamma 2.2), así el movimiento se ve continuo sobre la mica difusora del anillo en vez de "saltar" de LED en LED.

## Hardware

| Elemento | Valor |
|---|---|
| MCU | ESP32 (probado en ESP32-D0WD-V3) |
| Anillo | WS2812B, 16 LEDs |
| Datos | GPIO23 |
| Chipset | `NEO_GRB + NEO_KHZ800` |

Conecta: 5V del anillo a 5V, GND a GND y DIN a GPIO23. Para 16 LEDs suele bastar con la alimentación de la placa; si usas más, alimenta el anillo aparte.

## Software

- [Arduino IDE](https://www.arduino.cc/en/software) o `arduino-cli`
- Core: **esp32** de Espressif (`esp32:esp32`)
- Librerías (Library Manager):
  - **Adafruit NeoPixel**
  - **ESP Async WebServer** (ESP32Async)
  - **Async TCP** (ESP32Async, 3.x)

> Nota: con ESP Async WebServer 3.x hay que usar el `AsyncTCP` de **ESP32Async** (3.x), no el de me-no-dev (1.1.4), que no compila (`AsyncServer::status() const` descarta qualifiers).

### Compilar y flashear

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload -p COM5 --fqbn esp32:esp32:esp32 .
```

Si tu placa no entra sola en modo descarga, mantén **BOOT**, pulsa y suelta **RESET/EN**, y suelta **BOOT** justo antes del upload.

## Uso

1. Al arrancar con la config vacía, el ESP32 levanta un AP llamado **NeoClock** (sin contraseña).
2. Conéctate y abre `http://192.168.4.1` para ingresar SSID/contraseña de tu WiFi.
3. Guarda y reinicia: ahora entra como cliente y queda en tu red (la IP se imprime por el serial a 115200 baudios).
4. Abre la IP en el navegador para configurar colores, brillo, modo y alarma.

Los cambios de color, brillo y modo se aplican **en vivo** (endpoint `/preview`, solo RAM); el botón **Guardar** los persiste en NVS.

## Endpoints

| Método | Ruta | Descripción |
|---|---|---|
| GET | `/` | Interfaz web |
| GET | `/config` | Config actual en JSON |
| POST | `/save` | Aplica y persiste en NVS |
| POST | `/preview` | Aplica solo en RAM (vista previa en vivo) |
| POST | `/wifi` | Guarda credenciales y reinicia |

## Modos

`0` Reloj · `1` Arcoíris · `2` Respiración · `3` Noche (apagado). La alarma (si está activa) intermitente en verde durante 30 s al llegar a la hora configurada.

## Notas

- Zona horaria por defecto: `CST6CDT,M4.1.0,M10.5.0` (Monterrey/CDMX, con horario de verano). Cambia `TZ_INFO` para otra zona.
- NTP: `pool.ntp.org` y `time.nist.gov`.
- Config persistida en NVS (Preferences, namespace `neoclock`).
- Sin `delay()` en el loop: todo por `millis()` y `vTaskDelay(1)`.
