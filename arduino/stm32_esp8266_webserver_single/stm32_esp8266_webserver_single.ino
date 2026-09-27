/*
 * stm32_esp8266_webserver_single.ino - compact HTTP server: STM32 + ESP8266 (AT firmware) over UART.
 * Install: Boards Manager "STM32 MCU based boards"; Library Manager "WiFiEspAT" (needs AT firmware 1.7+).
 * Wiring:  ESP TX -> ESP_RX_PIN, ESP RX -> ESP_TX_PIN, 3.3 V (300 mA), GND, CH_PD/EN -> 3.3 V.
 * Serves:  GET /  (LED page)   GET /api/status  (JSON)   GET|POST /led?state=on|off|toggle
 * The full-featured version with unit tests lives in ../stm32_esp8266_webserver.
 */
#include <WiFiEspAT.h>

#define WIFI_SSID  "your-network-name"
#define WIFI_PASS  "your-network-password"
#define ESP_BAUD   115200
#if defined(ARDUINO_BLUEPILL_F103C8) || defined(ARDUINO_BLUEPILL_F103CB) || defined(ARDUINO_BLACKPILL_F411CE)
#define ESP_RX_PIN     PA3   // Blue/Black Pill: ESP on USART2, because USART1 (PA9/PA10) is the debug Serial
#define ESP_TX_PIN     PA2
#define LED_ACTIVE_LOW 1     // on-board LED sits between the pin and 3.3 V
#else
#define ESP_RX_PIN     PA10  // Nucleo-64: ESP on USART1 (D2/D8), because USART2 is the ST-Link Serial
#define ESP_TX_PIN     PA9
#define LED_ACTIVE_LOW 0
#endif

#if defined(ARDUINO_API_VERSION)              // STM32 core 3.x
Uart EspSerial(ESP_RX_PIN, ESP_TX_PIN);
#else                                         // STM32 core 2.x
HardwareSerial EspSerial(ESP_RX_PIN, ESP_TX_PIN);
#endif

WiFiServer server(80);
bool ledOn = false;
unsigned long requests = 0;
struct Request { char method[8], path[64], query[64], body[128]; };

static const char PAGE[] =                    // stored in flash; buttons call /led, page polls /api/status
"<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>STM32 Web Server</title><link rel=\"icon\" href=\"data:,\"><style>body{font-family:sans-serif;text-align:center;"
"background:#111;color:#eee;padding:30px}button{font-size:1.1em;padding:10px 26px;margin:6px;border:0;border-radius:8px;color:#fff}"
".on{background:#2e9e5b}.off{background:#c0392b}.tg{background:#3b5bdb}</style></head><body><h2>STM32 + ESP8266</h2>"
"<p>LED is <b id=\"s\">?</b></p><button class=\"on\" onclick=\"led('on')\">ON</button>"
"<button class=\"off\" onclick=\"led('off')\">OFF</button><button class=\"tg\" onclick=\"led('toggle')\">Toggle</button>"
"<p><small>Uptime <span id=\"u\">-</span> s &middot; Requests <span id=\"r\">-</span></small></p><script>"
"function show(j){s.textContent=j.led?'ON':'OFF';u.textContent=j.uptime;r.textContent=j.requests}"
"function led(v){fetch('/led?state='+v,{method:'POST'}).then(x=>x.json()).then(show).catch(()=>{})}"
"function poll(){fetch('/api/status').then(x=>x.json()).then(show).catch(()=>{})}poll();setInterval(poll,5000)"
"</script></body></html>";

// Read one line (up to LF, CR dropped). False if the client stalls for 3 s or hangs up.
static bool readLine(WiFiClient &c, char *buf, size_t cap) {
  size_t n = 0;
  for (unsigned long t = millis(); millis() - t < 3000; ) {
    if (!c.available()) { if (!c.connected()) return false; continue; }
    char ch = c.read();
    if (ch == '\n') { buf[n] = '\0'; return true; }
    if (ch != '\r' && n < cap - 1) buf[n++] = ch;
  }
  return false;
}

// Parse "METHOD /path?query HTTP/1.x", skip headers (keeping Content-Length), then read the body.
static bool readRequest(WiFiClient &c, Request &r) {
  char line[160];
  if (!readLine(c, line, sizeof line)) return false;
  char *sp = strchr(line, ' ');
  if (!sp) return false;
  *sp = '\0';
  strncpy(r.method, line, sizeof r.method - 1);
  char *target = sp + 1;
  if ((sp = strchr(target, ' '))) *sp = '\0';
  char *q = strchr(target, '?');
  if (q) *q++ = '\0';
  strncpy(r.path, target, sizeof r.path - 1);
  strncpy(r.query, q ? q : "", sizeof r.query - 1);
  size_t want = 0;
  while (readLine(c, line, sizeof line) && line[0])
    if (strncasecmp(line, "Content-Length:", 15) == 0) want = atol(line + 15);
  size_t n = 0;
  for (unsigned long t = millis(); n < want && n < sizeof r.body - 1 && millis() - t < 3000; )
    if (c.available()) r.body[n++] = c.read();
  r.body[n] = '\0';
  return true;
}

// Value of key in "a=1&state=on" (query string or form body). Empty string if absent.
static void param(const char *src, const char *key, char *out, size_t cap) {
  size_t klen = strlen(key);
  out[0] = '\0';
  for (const char *p = src; *p; p++)
    if ((p == src || p[-1] == '&') && strncmp(p, key, klen) == 0 && p[klen] == '=') {
      size_t n = strcspn(p += klen + 1, "&");
      if (n > cap - 1) n = cap - 1;
      memcpy(out, p, n);
      out[n] = '\0';
      return;
    }
}

// Status line + headers in one write, then the body in chunks the AT firmware accepts (max 2048).
static void send(WiFiClient &c, int status, const char *type, const char *body, size_t len) {
  char hdr[160];
  const char *reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request";
  int n = snprintf(hdr, sizeof hdr, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                   "Connection: close\r\nCache-Control: no-store\r\n\r\n", status, reason, type, (unsigned)len);
  c.write((const uint8_t *)hdr, n);
  for (size_t w; len; body += w, len -= w)
    if (!(w = c.write((const uint8_t *)body, len > 1024 ? 1024 : len))) break;
  c.flush();
}

static void handle(WiFiClient &c, Request &r) {
  char v[12], json[64];
  if (strcmp(r.path, "/") == 0) return send(c, 200, "text/html; charset=utf-8", PAGE, sizeof PAGE - 1);
  if (strcmp(r.path, "/led") == 0) {
    param(r.query, "state", v, sizeof v);
    if (!v[0]) param(r.body, "state", v, sizeof v);
    if      (strcmp(v, "on") == 0)     ledOn = true;
    else if (strcmp(v, "off") == 0)    ledOn = false;
    else if (strcmp(v, "toggle") == 0) ledOn = !ledOn;
    else return send(c, 400, "text/plain", "state must be on, off or toggle\n", 32);
    digitalWrite(LED_BUILTIN, (ledOn != LED_ACTIVE_LOW) ? HIGH : LOW);
  } else if (strcmp(r.path, "/api/status") != 0) {
    return send(c, 404, "text/plain", "404 Not Found\n", 14);
  }
  int n = snprintf(json, sizeof json, "{\"led\":%d,\"uptime\":%lu,\"requests\":%lu}", ledOn, millis() / 1000, requests);
  send(c, 200, "application/json", json, n);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LED_ACTIVE_LOW ? HIGH : LOW);
  Serial.begin(115200);
  EspSerial.begin(ESP_BAUD);
  while (!WiFi.init(EspSerial)) { Serial.println("ESP8266 not responding: check wiring, 3.3 V supply, CH_PD, baud"); delay(3000); }
  while (WiFi.begin(WIFI_SSID, WIFI_PASS) != WL_CONNECTED) { Serial.println("WiFi join failed, retrying"); delay(2000); }
  Serial.print("Connected. Open http://");
  Serial.println(WiFi.localIP());
  server.begin();
}

void loop() {
  WiFiClient client = server.available();
  if (!client) return;
  requests++;
  Request req = {};
  if (readRequest(client, req)) handle(client, req);
  else send(client, 400, "text/plain", "400 Bad Request\n", 16);
  Serial.print(req.method); Serial.print(' '); Serial.println(req.path);
  client.stop();
}
