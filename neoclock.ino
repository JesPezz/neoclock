/*
 * neoclock.ino - Reloj Neopixel interactivo (ESP32 + anillo WS2812B x16, GPIO23)
 *
 * Librerias (Library Manager):
 *   - Adafruit NeoPixel
 *   - ESPAsyncWebServer  (ESP32Async / me-no-dev)
 *   - AsyncTCP
 *
 * Config persistida en NVS (Preferences). Sin delay(): todo por millis().
 * ponytail: un solo archivo + NVS. Sin SPIFFS ni clases/factorias de un solo uso.
 */

#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>

// ----------------------------- Hardware ------------------------------
#define LED_PIN   23
#define NUM_LEDS  16
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ---------------------------- Timezone -------------------------------
// Monterrey/CDMX: CST6 + DST automatico (2do dom mar -> 1er dom nov)
static const char* TZ_INFO = "CST6CDT,M4.1.0,M10.5.0";
static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.nist.gov";

// ---------------------------- Config ---------------------------------
struct Config {
  uint32_t cHour   = 0xFF0000;  // rojo
  uint32_t cMin    = 0x00FF00;  // verde
  uint32_t cSec    = 0x0000FF;  // azul
  uint32_t cBg     = 0x000000;  // fondo
  uint8_t  bright  = 80;
  uint8_t  mode    = 0;         // 0 reloj, 1 rainbow, 2 breathing, 3 noche
  bool     alarmEn = false;
  uint8_t  aH = 7, aM = 0;
  String   ssid = "";
  String   pass = "";
} cfg;

Preferences prefs;
AsyncWebServer server(80);

// ---------------------------- Estado runtime -------------------------
uint32_t lastRender = 0, lastWifiCheck = 0;
int      lastAlarmMin = -1;
bool     alarmFiring = false;
uint32_t alarmStart = 0;
uint32_t rebootAt = 0;

// Prototipos
uint32_t scale32(uint32_t c, float f);
void checkAlarm(int h, int m);

// ---------------------------- Helpers --------------------------------
inline void addLed(int i, uint32_t c) {
  i %= NUM_LEDS; if (i < 0) i += NUM_LEDS;
  uint32_t p = strip.getPixelColor(i);
  uint8_t r = min(255, (int)((p >> 16) & 0xFF) + (int)((c >> 16) & 0xFF));
  uint8_t g = min(255, (int)((p >> 8)  & 0xFF) + (int)((c >> 8)  & 0xFF));
  uint8_t b = min(255, (int)( p        & 0xFF) + (int)( c        & 0xFF));
  strip.setPixelColor(i, strip.Color(r, g, b));
}

uint32_t scale32(uint32_t c, float f) {
  uint8_t r = ((c >> 16) & 0xFF) * f;
  uint8_t g = ((c >> 8)  & 0xFF) * f;
  uint8_t b = ( c        & 0xFF) * f;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Anti-aliasing: reparte el brillo de una posicion flotante entre dos LEDs.
// addLed ya hace % NUM_LEDS, asi que base+1 desborda circular sin problema.
// ponytail: gamma 2.2 en vez del lineal (1-frac, frac) porque el lineal se ve "caminando".
void plotLed(float pos, uint32_t color) {
  int   base = (int)pos;
  float frac = pos - base;
  addLed(base,     scale32(color, powf(1.0f - frac, 2.2f)));
  addLed(base + 1, scale32(color, powf(frac, 2.2f)));
}

uint32_t html2rgb(String s) {
  if (s.startsWith("#")) s.remove(0, 1);
  return strtoul(s.c_str(), NULL, 16) & 0xFFFFFF;
}
String rgb2html(uint32_t c) {
  char b[8]; sprintf(b, "#%06X", c & 0xFFFFFF); return String(b);
}

// ---------------------------- Persistencia ---------------------------
void loadCfg() {
  prefs.begin("neoclock", true);
  cfg.cHour   = prefs.getUInt("cHour", cfg.cHour);
  cfg.cMin    = prefs.getUInt("cMin",  cfg.cMin);
  cfg.cSec    = prefs.getUInt("cSec",  cfg.cSec);
  cfg.cBg     = prefs.getUInt("cBg",   cfg.cBg);
  cfg.bright  = prefs.getUChar("bright", cfg.bright);
  cfg.mode    = prefs.getUChar("mode", cfg.mode);
  cfg.alarmEn = prefs.getBool("alEn", false);
  cfg.aH      = prefs.getUChar("aH", 7);
  cfg.aM      = prefs.getUChar("aM", 0);
  cfg.ssid    = prefs.getString("ssid", "");
  cfg.pass    = prefs.getString("pass", "");
  prefs.end();
}

void saveCfg() {
  prefs.begin("neoclock", false);
  prefs.putUInt("cHour", cfg.cHour);
  prefs.putUInt("cMin",  cfg.cMin);
  prefs.putUInt("cSec",  cfg.cSec);
  prefs.putUInt("cBg",   cfg.cBg);
  prefs.putUChar("bright", cfg.bright);
  prefs.putUChar("mode", cfg.mode);
  prefs.putBool("alEn", cfg.alarmEn);
  prefs.putUChar("aH", cfg.aH);
  prefs.putUChar("aM", cfg.aM);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.end();
}

// ---------------------------- WiFi no bloqueante ---------------------
void wifiTick() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiCheck < 10000) return;
  lastWifiCheck = millis();
  if (cfg.ssid.length() == 0) return;
  WiFi.disconnect();
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
}

// ---------------------------- Render ---------------------------------
void renderClock(int h12, int mn, int sc) {
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, cfg.cBg);

  // posiciones flotantes 0.0..15.999 (anti-aliasing sobre la mica difusora)
  float posH = ((h12 + mn / 60.0f) / 12.0f) * NUM_LEDS;  // hora avanza con los minutos
  float posM = (mn / 60.0f) * NUM_LEDS;                  // 0-59 min -> 16 posiciones

  // segundero: interpola dentro del segundo real (evita jitter tm_sec vs millis()%1000)
  static int lastSec = -1; static uint32_t secMark = 0;
  if (sc != lastSec) { lastSec = sc; secMark = millis(); }
  float f = (millis() - secMark) / 1000.0f;
  if (f > 0.999f) f = 0.999f;
  float posS = (sc + f) * NUM_LEDS / 60.0f;

  plotLed(posH, cfg.cHour);
  plotLed(posM, cfg.cMin);
  plotLed(posS, cfg.cSec);
}

void renderRainbow(uint32_t t) {
  for (int i = 0; i < NUM_LEDS; i++) {
    uint16_t hue = (t / 20 + i * (65536 / NUM_LEDS)) & 0xFFFF;
    strip.setPixelColor(i, strip.ColorHSV(hue, 255, 200));
  }
}

void renderBreathing(uint32_t t) {
  float ph = (t % 4000) / 4000.0f;
  float b  = (sin(ph * 2 * PI) + 1) * 0.5f;
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, scale32(cfg.cSec, b));
}

void renderAlarm(uint32_t t) {
  bool on = (t / 250) % 2;
  uint32_t c = on ? cfg.cMin : 0;
  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, c);
}

void checkAlarm(int h, int m) {
  if (!cfg.alarmEn) { lastAlarmMin = -1; return; }
  if (h == cfg.aH && m == cfg.aM) {
    if (lastAlarmMin != h * 60 + m) {         // dispara una vez por minuto
      lastAlarmMin = h * 60 + m;
      alarmFiring = true;
      alarmStart = millis();
    }
  } else {
    lastAlarmMin = -1;
  }
}

void renderTick() {
  uint32_t now = millis();
  if (now - lastRender < 20) return;          // ~50 FPS
  lastRender = now;
  strip.setBrightness(cfg.bright);

  if (alarmFiring && now - alarmStart > 30000) alarmFiring = false;  // dura 30s

  if (alarmFiring) {
    renderAlarm(now);
  } else {
    switch (cfg.mode) {
      case 1: renderRainbow(now); break;
      case 2: renderBreathing(now); break;
      case 3: for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, 0); break;
      default: {
        struct tm ti;
        if (getLocalTime(&ti, 0)) {
          int h12 = ti.tm_hour % 12; if (h12 == 0) h12 = 12;
          renderClock(h12, ti.tm_min, ti.tm_sec);
          checkAlarm(ti.tm_hour, ti.tm_min);
        }
      }
    }
  }
  strip.show();
}

// ---------------------------- Web UI ---------------------------------
String page() {
  String p = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>NeoClock</title><style>"
    "body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:16px;max-width:520px;margin:auto}"
    "h1{font-size:1.3rem}label{display:block;margin:12px 0 4px;font-size:.9rem;color:#aaa}"
    "input[type=color]{width:56px;height:36px;border:0;background:none;vertical-align:middle}"
    "input[type=range]{width:100%}select,input[type=time],input[type=text],input[type=password]{width:100%;padding:8px;border-radius:8px;border:1px solid #333;background:#1c1c1c;color:#eee;box-sizing:border-box}"
    "button{margin-top:16px;width:100%;padding:12px;border:0;border-radius:8px;background:#3b82f6;color:#fff;font-size:1rem}"
    "button.gray{background:#333}.row{display:flex;gap:8px;align-items:center}</style></head><body>"
    "<h1>NeoClock</h1>"
    "<form id='f'>"
    "<label>Horas <input type='color' name='cHour'></label>"
    "<label>Minutos <input type='color' name='cMin'></label>"
    "<label>Segundos <input type='color' name='cSec'></label>"
    "<label>Fondo <input type='color' name='cBg'></label>"
    "<label>Brillo <input type='range' name='bright' min='0' max='255'></label>"
    "<label>Modo <select name='mode'>"
    "<option value='0'>Reloj normal</option><option value='1'>Arcoiris</option>"
    "<option value='2'>Respiracion</option><option value='3'>Noche (apagado)</option>"
    "</select></label>"
    "<label>Alarma <input type='time' name='alarmT'></label>"
    "<div class='row'><input type='checkbox' name='alarmEn' id='ae'><label for='ae' style='margin:0'>Activar alarma</label></div>"
    "<button type='submit'>Guardar</button>"
    "</form>"
    "<form id='w'>"
    "<label>WiFi SSID <input type='text' name='ssid'></label>"
    "<label>WiFi Password <input type='password' name='pass'></label>"
    "<button type='submit' class='gray'>Guardar WiFi y reiniciar</button>"
    "</form>"
    "<script>"
    "const f=document.getElementById('f');"
    "fetch('/config').then(r=>r.json()).then(c=>{"
    "f.cHour.value=c.cHour;f.cMin.value=c.cMin;f.cSec.value=c.cSec;f.cBg.value=c.cBg;"
    "f.bright.value=c.bright;f.mode.value=c.mode;f.alarmT.value=c.alarmT;f.alarmEn.checked=c.alarmEn;"
    "document.getElementById('w').ssid.value=c.ssid;});"
    "f.onsubmit=e=>{e.preventDefault();"
    "const d=Object.fromEntries(new FormData(f));"
    "d.alarmEn=f.alarmEn.checked?'1':'0';"
    "fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})"
    ".then(()=>alert('Guardado'));};"
    "function pv(){const d=Object.fromEntries(new FormData(f));d.alarmEn=f.alarmEn.checked?'1':'0';"
    "fetch('/preview',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});}"
    "f.addEventListener('input',pv);f.addEventListener('change',pv);"
    "document.getElementById('w').onsubmit=e=>{e.preventDefault();"
    "fetch('/wifi',{method:'POST',body:new URLSearchParams(new FormData(e.target))})"
    ".then(()=>alert('Guardado. Reiniciando...'));};"
    "</script></body></html>");
  return p;
}

String configJson() {
  String j = "{";
  j += "\"cHour\":\"" + rgb2html(cfg.cHour) + "\",";
  j += "\"cMin\":\""  + rgb2html(cfg.cMin)  + "\",";
  j += "\"cSec\":\""  + rgb2html(cfg.cSec)  + "\",";
  j += "\"cBg\":\""   + rgb2html(cfg.cBg)   + "\",";
  j += "\"bright\":"  + String(cfg.bright)  + ",";
  j += "\"mode\":"    + String(cfg.mode)    + ",";
  char t[6]; sprintf(t, "%02d:%02d", cfg.aH, cfg.aM);
  j += "\"alarmT\":\"" + String(t) + "\",";
  j += "\"alarmEn\":" + String(cfg.alarmEn ? "true" : "false") + ",";
  j += "\"ssid\":\""  + cfg.ssid + "\"}";
  return j;
}

// Aplica JSON plano a cfg. persist=false = vista previa en vivo (sin NVS).
void applyConfig(const String& body, bool persist) {
  auto get = [&](const char* k) -> String {
    String key = String("\"") + k + "\":";
    int i = body.indexOf(key);
    if (i < 0) return "";
    i += key.length();
    if (body[i] == '"') { int j = body.indexOf('"', i + 1); return body.substring(i + 1, j); }
    int j = body.indexOf(',', i); if (j < 0) j = body.indexOf('}', i);
    return body.substring(i, j);
  };
  if (body.indexOf("cHour")  >= 0) cfg.cHour  = html2rgb(get("cHour"));
  if (body.indexOf("cMin")   >= 0) cfg.cMin   = html2rgb(get("cMin"));
  if (body.indexOf("cSec")   >= 0) cfg.cSec   = html2rgb(get("cSec"));
  if (body.indexOf("cBg")    >= 0) cfg.cBg    = html2rgb(get("cBg"));
  if (body.indexOf("bright") >= 0) cfg.bright = get("bright").toInt();
  if (body.indexOf("mode")   >= 0) cfg.mode   = get("mode").toInt();
  if (body.indexOf("alarmEn")>= 0) cfg.alarmEn= get("alarmEn") == "1";
  String at = get("alarmT");
  if (at.length() >= 5) { cfg.aH = at.substring(0, 2).toInt(); cfg.aM = at.substring(3, 5).toInt(); }
  if (persist) saveCfg();
}

void setupServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "text/html", page());
  });
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(200, "application/json", configJson());
  });
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest* r) {}, NULL,
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
      applyConfig(String((char*)data, len), true);
      r->send(200, "application/json", "{\"ok\":true}");
    });
  server.on("/preview", HTTP_POST, [](AsyncWebServerRequest* r) {}, NULL,
    [](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t, size_t) {
      applyConfig(String((char*)data, len), false);
      r->send(200, "application/json", "{\"ok\":true}");
    });
  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (r->hasParam("ssid", true)) cfg.ssid = r->getParam("ssid", true)->value();
    if (r->hasParam("pass", true)) cfg.pass = r->getParam("pass", true)->value();
    saveCfg();
    r->send(200, "text/plain", "ok");
    rebootAt = millis() + 1000;        // aplica credenciales nuevas
  });
  server.begin();
}

// ---------------------------- Lifecycle ------------------------------
void setupTime() {
  configTzTime(TZ_INFO, NTP1, NTP2);   // TZ + NTP (core ESP32)
}

void connectWifi(bool blocking) {
  WiFi.mode(WIFI_STA);
  if (cfg.ssid.length()) {
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    uint32_t t = millis();
    while (blocking && WiFi.status() != WL_CONNECTED && millis() - t < 15000) { delay(100); }
  }
  if (WiFi.status() != WL_CONNECTED) {     // sin credenciales o fallo: AP de config
    WiFi.mode(WIFI_AP);
    WiFi.softAP("NeoClock");
  }
}

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.setBrightness(cfg.bright);
  strip.show();

  loadCfg();
  connectWifi(true);                 // espera acotada al arranque (una sola vez)
  setupTime();

  setupServer();
  Serial.print("NeoClock en http://");
  Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP());
}

void loop() {
  if (rebootAt && millis() >= rebootAt) ESP.restart();
  wifiTick();
  renderTick();
  vTaskDelay(pdMS_TO_TICKS(1));      // cede al stack de red (async server corre aparte)
}
