# HTTP-Web-Server

HTTP web server code for an **STM32 development board** with an **ESP8266 WiFi
module** attached over UART, written in C and built with **Arduino libraries via
the Arduino IDE** (official STM32duino core).

The repository is planned to hold two flavours:

| Folder     | Status        | Description                                                  |
|------------|---------------|--------------------------------------------------------------|
| `arduino/` | **available** | Arduino-library version (this document).                     |
| `scratch/` | planned       | From-scratch version that drives the ESP8266 AT link itself. |

## What it does

The STM32 runs a small HTTP/1.1 server that is reachable on your WiFi network
through the ESP8266. Out of the box it serves a control page for the board's
LED and a JSON status endpoint:

| Method     | Path                    | Response                                             |
|------------|-------------------------|------------------------------------------------------|
| GET        | `/`                     | Control page (HTML, lives in flash)                  |
| GET        | `/api/status`           | `{"led":1,"uptime":123,"requests":7}`                |
| GET / POST | `/led?state=on`         | Sets the LED (`on`, `off`, `toggle`); returns status |
| POST       | `/led` + body `state=…` | Same, from an HTML form                              |
| GET        | `/led/on` `/led/off` `/led/toggle` | Sets the LED and redirects to `/`         |
| any        | anything else           | 404 page                                             |

`HEAD` works on every GET route. Malformed requests get a proper 400 / 413 /
414 / 501, and a client that stalls mid-request gets a 408 after 3 s.

## Code layout

```
arduino/stm32_esp8266_webserver/
├── stm32_esp8266_webserver.ino   Arduino glue: WiFi bring-up, client loop, LED handlers
├── http_server.h / http_server.c Portable C HTTP core: parser, router, response builder
├── pages.h                       HTML pages as C string literals (stored in flash)
├── config.h                      Pins, UART, library choice, tunables  <- edit this
└── secrets.h.example             Template for your WiFi credentials
test/
├── test_http_server.c            Host-side unit tests for http_server.c (112 checks)
├── run_tests.sh / run_tests.ps1  Build + run the tests with gcc/clang/tcc
```

The split is deliberate. Everything that understands HTTP is plain C99 with
fixed-size buffers and no Arduino dependency, so it can be unit-tested on a PC
and reused with any other transport. The sketch only moves bytes between the
ESP8266 library and the C core, and implements the handlers that touch
hardware.

## Hardware

Any STM32 board supported by the STM32duino core plus an ESP8266 module
running the **AT-command firmware** (the firmware ESP-01 modules ship with).
Both sides are 3.3 V, so the UART can be wired directly.

| ESP8266 pin | Connect to                                          |
|-------------|-----------------------------------------------------|
| VCC         | 3.3 V, able to supply ~300 mA peaks (see note)      |
| GND         | GND                                                 |
| TX          | STM32 RX pin (`ESP_UART_RX_PIN` in config.h)        |
| RX          | STM32 TX pin (`ESP_UART_TX_PIN` in config.h)        |
| CH_PD / EN  | 3.3 V                                               |
| RST         | leave floating or 3.3 V                             |

Default pins picked so the ESP never shares a USART with the debug port:

| Board family                  | ESP RX pin | ESP TX pin | USART   | Debug `Serial`         |
|-------------------------------|------------|------------|---------|------------------------|
| Nucleo-64 (F103RB, F401RE...) | PA10 (D2)  | PA9 (D8)   | USART1  | USART2 via ST-Link USB |
| Blue Pill / Black Pill        | PA3        | PA2        | USART2  | USART1 (PA9/PA10)      |

**Power note.** The ESP8266 draws short bursts of 200 to 300 mA when
transmitting. Nucleo boards can usually supply this from their 3.3 V pin; a
Blue Pill's tiny regulator often cannot, so use a separate 3.3 V regulator
(an AMS1117 module works) with a 100 µF capacitor close to the ESP. Flaky
resets and "ESP8266 not responding" messages are almost always power.

## Building with the Arduino IDE

1. **Install the STM32 core.** File > Preferences > Additional boards manager
   URLs, add
   `https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json`,
   then Tools > Board > Boards Manager, install **STM32 MCU based boards**.
2. **Install the WiFi library.** Tools > Manage Libraries, install **WiFiEspAT**
   (by Juraj Andrassy). See "Choosing between WiFiEspAT and WiFiEsp" below if
   your module runs old AT firmware.
3. **Credentials.** Copy `secrets.h.example` to `secrets.h` in the sketch
   folder and fill in your SSID and password. `secrets.h` is git-ignored.
4. **Check config.h.** Pins, ESP baud rate (115200 by default), LED polarity.
5. **Select your board.** Tools > Board > STM32 MCU based boards, then
   Tools > Board part number, e.g. "Nucleo F103RB" or "BluePill F103C8".
   On a Blue Pill, also set Tools > USB support to
   "CDC (generic 'Serial' supersede U(S)ART)" if you want debug output over
   USB; otherwise attach a USB-serial adapter to PA9/PA10.
6. Open `arduino/stm32_esp8266_webserver/stm32_esp8266_webserver.ino`, Verify,
   Upload.
7. Open the Serial Monitor at 115200 baud. Once connected the sketch prints
   `Connected. Open http://192.168.x.y/`. Browse to that address from any
   device on the same network.

### Choosing between WiFiEspAT and WiFiEsp

`config.h` has `WIFI_LIBRARY`. The sketch compiles against either library with
no other changes.

| Library       | AT firmware it needs | STM32 core     | Notes                                                    |
|---------------|----------------------|----------------|----------------------------------------------------------|
| **WiFiEspAT** | 1.7.x or 2.x         | 2.x and 3.x    | Default. Fast, maintained, buffered writes.              |
| **WiFiEsp**   | any 0.x / 1.x        | 2.x only       | Unmaintained; what older tutorials use. Slow.            |

WiFiEsp includes `<avr/pgmspace.h>`, which STM32 core 3.0.0 (the version the
Boards Manager installs today) no longer provides, so it fails to build there.
Use it only if your ESP8266 runs AT firmware older than 1.7 that you cannot
update, and then install core **2.12.0** from the Boards Manager version
drop-down and set `#define WIFI_LIBRARY WIFI_LIB_WIFIESP`.

Find your firmware version by talking to the module from a serial terminal
and sending `AT+GMR`. Flashing the current AT firmware onto an ESP-01 takes a
USB-serial adapter and Espressif's flash tool, and is worth doing once.

## Verified builds

Compiled with arduino-cli and `--warnings all`; the sketch and the C core
produce no warnings. Flash/RAM figures are what the linker reports.

| STM32 core | Board                  | WiFi library | Flash    | Static RAM |
|------------|------------------------|--------------|----------|------------|
| 3.0.0      | Nucleo F103RB          | WiFiEspAT    | 33.3 KB  | 2.9 KB     |
| 3.0.0      | Blue Pill F103C8       | WiFiEspAT    | 33.2 KB  | 2.9 KB     |
| 3.0.0      | Nucleo F401RE          | WiFiEspAT    | 34.2 KB  | 3.0 KB     |
| 2.12.0     | Nucleo F103RB          | WiFiEsp      | 32.2 KB  | 2.8 KB     |
| 2.12.0     | Blue Pill F103C8       | WiFiEspAT    | 32.4 KB  | 3.0 KB     |

Library versions: WiFiEspAT 1.5.0, WiFiEsp 2.2.2. The request, response and
header buffers (about 1.1 KB) are static and included in the RAM figures, so
serving a request adds little stack on top.

## Testing the HTTP core on your PC

The C core is exercised by 112 checks covering request parsing (byte-at-a-time
and buffered), query/form decoding, error statuses, header building, routing
and an end-to-end request/response. Any C99 compiler works:

```sh
cd test
./run_tests.sh            # Linux / macOS / Git Bash (uses $CC or gcc)
.\run_tests.ps1           # Windows PowerShell
```

Expected last line: `112 checks, 0 failures`.

## Adding your own endpoint

1. Write a handler in the `.ino`:

   ```c
   static void handleTemp(const http_request_t *req, http_response_t *res)
   {
       (void)req;
       http_response_printf(res, 200, HTTP_CT_JSON, "{\"celsius\":%d}", readTemperature());
   }
   ```

2. Add it to the `routes[]` table:

   ```c
   { HTTP_METHOD_GET, "/api/temp", handleTemp },
   ```

Routes match exact paths, or a prefix when the pattern ends in `*`. Read query
parameters with `http_query_get(req->query, "key", buf, sizeof buf)` and form
fields the same way from `req->body`. Point large static content at a `const`
string in `pages.h` with `http_response_set()`; it is streamed from flash.

## Memory footprint and limits

All buffers are fixed-size and configurable at the top of `http_server.h`:

| Item                    | Default | Macro                     |
|-------------------------|---------|---------------------------|
| Request path            | 64 B    | `HTTP_MAX_PATH_LEN`       |
| Query string            | 64 B    | `HTTP_MAX_QUERY_LEN`      |
| Request body            | 256 B   | `HTTP_MAX_BODY_LEN`       |
| Generated response body | 384 B   | `HTTP_DYN_BODY_LEN`       |
| Response header buffer  | 256 B   | `HTTP_HEADER_BUF_SIZE` (config.h) |

Headers the server does not use (User-Agent, Cookie, Accept...) are streamed
past without being stored, so long browser headers never overflow anything.
One request is served per TCP connection (`Connection: close`), which keeps
the AT-command layer simple and reliable.

## Troubleshooting

| Symptom                                      | Likely cause / fix                                                                 |
|----------------------------------------------|------------------------------------------------------------------------------------|
| `ESP8266 not responding` repeating           | Wrong pins (TX/RX swapped), wrong `ESP_BAUD`, CH_PD not high, weak 3.3 V supply.  |
| Garbage on the Serial Monitor                | ESP and debug port share a USART. See the pin table above.                         |
| Connects to WiFi but the page never loads    | Firewall/AP client isolation, or the browser is on a different network.             |
| Page loads slowly                            | Normal for WiFiEsp (each write is an AT round-trip). Switch to WiFiEspAT.          |
| `WiFi.begin` fails with new modules          | ESP-AT 2.x dropped `AT+CWJAP_CUR` used by WiFiEsp. Use WiFiEspAT.                  |
| `WiFi.init` fails with WiFiEspAT             | AT firmware older than 1.7. Update the firmware, or use WiFiEsp on core 2.12.0.    |
| `avr/pgmspace.h: No such file or directory`  | WiFiEsp on STM32 core 3.x. Switch to WiFiEspAT or install core 2.12.0.             |
| `no matching function for call to 'Uart'`    | Wrong pin macros in `ESP_UART_*_PIN` for your variant. Use names like `PA10`.       |
| Random resets when the ESP transmits         | Power. Add a proper 3.3 V regulator and a capacitor at the module.                  |
