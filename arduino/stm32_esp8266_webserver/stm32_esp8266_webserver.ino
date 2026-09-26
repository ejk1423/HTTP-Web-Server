/*
 * stm32_esp8266_webserver.ino
 *
 * HTTP web server for an STM32 board with an ESP8266 WiFi module attached
 * over UART (AT-command firmware), built with Arduino libraries on the
 * official STM32duino core.
 *
 * Layout
 *   http_server.h / .c  - HTTP parsing, routing and response building. Pure C,
 *                         no Arduino dependency, unit-tested on a PC.
 *   pages.h             - HTML pages stored in flash.
 *   config.h            - Pins, credentials, tunables. Edit this for your board.
 *   this file           - Arduino glue only: WiFi bring-up, accepting clients,
 *                         moving bytes between the ESP8266 and the C core, and
 *                         the route handlers that touch hardware (the LED).
 *
 * Endpoints
 *   GET  /              control page
 *   GET  /api/status    {"led":1,"uptime":123,"requests":7}
 *   GET|POST /led?state=on|off|toggle   (also accepts a form body "state=on")
 *   GET  /led/on  /led/off  /led/toggle  redirect back to "/" - handy from a
 *                                        browser address bar or curl.
 *
 * Everything else -> 404 page.
 */

#include "config.h"
#include "http_server.h"
#include "pages.h"

#if !defined(ARDUINO_ARCH_STM32)
#error "This sketch targets the official STM32duino core (Tools > Board > STM32 MCU based boards)."
#endif

/* ------------------------------------------------------------------------ */
/* WiFi library selection                                                   */
/* ------------------------------------------------------------------------ */

#if WIFI_LIBRARY == WIFI_LIB_WIFIESPAT
#  include <WiFiEspAT.h>
typedef WiFiServer NetServer;
typedef WiFiClient NetClient;
#else
#  include <WiFiEsp.h>
typedef WiFiEspServer NetServer;
typedef WiFiEspClient NetClient;
#endif

/* ------------------------------------------------------------------------ */
/* Serial ports                                                             */
/* ------------------------------------------------------------------------ */

#if defined(ESP_SERIAL_OBJECT)
#  define EspSerial ESP_SERIAL_OBJECT
#elif defined(ARDUINO_API_VERSION)
/* STM32 core 3.x (ArduinoCore-API based): the concrete UART class is Uart. */
Uart EspSerial(ESP_UART_RX_PIN, ESP_UART_TX_PIN);
#else
/* STM32 core 2.x: HardwareSerial takes the pins directly. */
HardwareSerial EspSerial(ESP_UART_RX_PIN, ESP_UART_TX_PIN);
#endif

#if DEBUG_ENABLED
#  define LOG(x)    DEBUG_SERIAL.print(x)
#  define LOGLN(x)  DEBUG_SERIAL.println(x)
#else
#  define LOG(x)    do {} while (0)
#  define LOGLN(x)  do {} while (0)
#endif

/* ------------------------------------------------------------------------ */
/* Application state                                                        */
/* ------------------------------------------------------------------------ */

static NetServer      server(HTTP_PORT);
static http_server_t  httpServer;

/* Large buffers live here (static) rather than on the stack. */
static http_request_t  request;
static http_response_t response;
static char            headerBuf[HTTP_HEADER_BUF_SIZE];

static bool          ledOn        = false;
static unsigned long requestCount = 0;

static void setLed(bool on)
{
    ledOn = on;
#if LED_ACTIVE_LOW
    digitalWrite(LED_PIN, on ? LOW : HIGH);
#else
    digitalWrite(LED_PIN, on ? HIGH : LOW);
#endif
}

/* Shared by /api/status and /led: the JSON the page polls. */
static void writeStatusJson(http_response_t *res)
{
    http_response_printf(res, 200, HTTP_CT_JSON,
                         "{\"led\":%d,\"uptime\":%lu,\"requests\":%lu}",
                         ledOn ? 1 : 0,
                         (unsigned long)(millis() / 1000UL),
                         requestCount);
    res->no_cache = true;
}

/* Apply "on" / "off" / "toggle". Returns false for anything else. */
static bool applyLedCommand(const char *cmd)
{
    if (strcmp(cmd, "on") == 0)          { setLed(true);   return true; }
    if (strcmp(cmd, "off") == 0)         { setLed(false);  return true; }
    if (strcmp(cmd, "toggle") == 0)      { setLed(!ledOn); return true; }
    if (strcmp(cmd, "1") == 0)           { setLed(true);   return true; }
    if (strcmp(cmd, "0") == 0)           { setLed(false);  return true; }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Route handlers                                                           */
/* ------------------------------------------------------------------------ */

static void handleIndex(const http_request_t *req, http_response_t *res)
{
    (void)req;
    http_response_set(res, 200, HTTP_CT_HTML, PAGE_INDEX, sizeof(PAGE_INDEX) - 1);
}

static void handleStatus(const http_request_t *req, http_response_t *res)
{
    (void)req;
    writeStatusJson(res);
}

/* /led?state=on   or   POST /led with body "state=on" */
static void handleLed(const http_request_t *req, http_response_t *res)
{
    char state[12];

    if (!http_query_get(req->query, "state", state, sizeof(state)) &&
        !http_query_get(req->body,  "state", state, sizeof(state))) {
        http_response_set_text(res, 400, HTTP_CT_TEXT,
                               "missing state=on|off|toggle\n");
        return;
    }
    if (!applyLedCommand(state)) {
        http_response_set_text(res, 400, HTTP_CT_TEXT,
                               "state must be on, off or toggle\n");
        return;
    }
    writeStatusJson(res);
}

/* /led/on, /led/off, /led/toggle -> act, then bounce back to the page. */
static void handleLedPath(const http_request_t *req, http_response_t *res)
{
    const char *cmd = req->path + strlen("/led/");

    if (!applyLedCommand(cmd)) {
        http_response_error(res, 404);
        return;
    }
    http_response_redirect(res, "/");
}

static void handleNotFound(const http_request_t *req, http_response_t *res)
{
    (void)req;
    http_response_set(res, 404, HTTP_CT_HTML, PAGE_404, sizeof(PAGE_404) - 1);
    res->no_cache = true;
}

static const http_route_t routes[] = {
    { HTTP_METHOD_GET, "/",           handleIndex   },
    { HTTP_METHOD_GET, "/api/status", handleStatus  },
    { HTTP_METHOD_ANY, "/led",        handleLed     },
    { HTTP_METHOD_GET, "/led/*",      handleLedPath },
};

/* ------------------------------------------------------------------------ */
/* Transport: ESP8266 <-> HTTP core                                         */
/* ------------------------------------------------------------------------ */

/*
 * Write a buffer to the client in chunks the AT firmware accepts. Returns
 * false if the library reported a failed write (connection dropped).
 */
static bool netWrite(NetClient &client, const char *data, size_t len)
{
    while (len > 0) {
        size_t n = (len > NET_TX_CHUNK) ? NET_TX_CHUNK : len;
        size_t w = client.write((const uint8_t *)data, n);
        if (w == 0) {
            return false;
        }
        data += w;
        len  -= w;
    }
    return true;
}

/* Read one request from the client. Returns the parser's final state. */
static http_parse_status_t readRequest(NetClient &client, http_parser_t *parser)
{
    http_parse_status_t st = HTTP_PARSE_INCOMPLETE;
    unsigned long lastByteAt = millis();

    while (st == HTTP_PARSE_INCOMPLETE) {
        if (client.available()) {
            int c = client.read();
            if (c < 0) {
                continue;
            }
            st = http_parser_feed(parser, (uint8_t)c);
            lastByteAt = millis();
        } else if (millis() - lastByteAt > HTTP_REQUEST_TIMEOUT_MS) {
            break;                                   /* client went quiet */
        } else if (!client.connected()) {
            break;                                   /* client hung up    */
        }
    }
    return st;
}

static void sendResponse(NetClient &client, const http_request_t *req,
                         const http_response_t *res)
{
    size_t hlen = http_response_build_header(res, headerBuf, sizeof(headerBuf));
    if (hlen == 0) {
        /* Header didn't fit HTTP_HEADER_BUF_SIZE: fall back to a bare 500. */
        static const char fallback[] =
            "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        netWrite(client, fallback, sizeof(fallback) - 1);
        return;
    }
    if (!netWrite(client, headerBuf, hlen)) {
        return;
    }
    if (req->method != HTTP_METHOD_HEAD && res->body_len > 0) {
        netWrite(client, res->body, res->body_len);
    }
    client.flush();   /* WiFiEspAT: push buffered TX. WiFiEsp: harmless. */
}

static void serviceClient(NetClient &client)
{
    http_parser_t parser;
    http_parse_status_t st;

    requestCount++;
    http_parser_init(&parser, &request);
    http_response_init(&response);

    st = readRequest(client, &parser);

    if (st == HTTP_PARSE_DONE) {
        http_server_handle(&httpServer, &request, &response);
    } else if (st == HTTP_PARSE_INCOMPLETE) {
        http_response_error(&response, 408);
    } else {
        http_response_error(&response, http_parser_error_status(&parser));
    }

    LOG(http_method_name(request.method));
    LOG(' ');
    LOG(request.path);
    if (request.query[0]) { LOG('?'); LOG(request.query); }
    LOG(" -> ");
    LOGLN(response.status);

    sendResponse(client, &request, &response);
    client.stop();
}

/* ------------------------------------------------------------------------ */
/* WiFi bring-up                                                            */
/* ------------------------------------------------------------------------ */

static bool initWifiModule()
{
#if WIFI_LIBRARY == WIFI_LIB_WIFIESPAT
    return WiFi.init(EspSerial);
#else
    WiFi.init(&EspSerial);
    return WiFi.status() != WL_NO_SHIELD;
#endif
}

static void connectWifi()
{
    int attempt = 0;

    while (WiFi.status() != WL_CONNECTED) {
        attempt++;
        LOG("Connecting to \"");
        LOG(WIFI_SSID);
        LOG("\" (attempt ");
        LOG(attempt);
        LOGLN(")");
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        if (WiFi.status() != WL_CONNECTED) {
            delay(2000);
        }
    }
}

void setup()
{
    pinMode(LED_PIN, OUTPUT);
    setLed(false);

#if DEBUG_ENABLED
    DEBUG_SERIAL.begin(DEBUG_BAUD);
    delay(200);
#endif
    LOGLN();
    LOGLN("STM32 + ESP8266 HTTP server");

    EspSerial.begin(ESP_BAUD);

    while (!initWifiModule()) {
        LOGLN("ESP8266 not responding. Check wiring, 3.3 V supply, CH_PD/EN high, and ESP_BAUD.");
        delay(3000);
    }
    LOGLN("ESP8266 ready");

    connectWifi();
    LOG("Connected. Open http://");
    LOG(WiFi.localIP());
    LOGLN("/");

    http_server_init(&httpServer, routes, sizeof(routes) / sizeof(routes[0]), handleNotFound);
    server.begin();
    LOGLN("HTTP server listening");
}

void loop()
{
    NetClient client = server.available();
    if (client) {
        serviceClient(client);
    }

    /* If the access point drops us, reconnect rather than serving nothing. */
    static unsigned long lastLinkCheck = 0;
    if (millis() - lastLinkCheck > 10000UL) {
        lastLinkCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            LOGLN("WiFi link lost, reconnecting");
            connectWifi();
            LOG("Reconnected. IP: ");
            LOGLN(WiFi.localIP());
        }
    }
}
