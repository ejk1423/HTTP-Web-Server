/*
 * config.h - Board wiring, WiFi credentials and tunables for the sketch.
 *
 * Edit this file (and secrets.h, see below) for your hardware. Nothing else
 * in the sketch should need touching for a different STM32 board.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* ------------------------------------------------------------------------ */
/* WiFi credentials                                                         */
/* ------------------------------------------------------------------------ */
/*
 * Put your real credentials in a file called secrets.h next to this one
 * (copy secrets.h.example). secrets.h is git-ignored so passwords never end
 * up in the repository. The placeholders below are used if it is missing.
 */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID  "your-network-name"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS  "your-network-password"
#endif

/* ------------------------------------------------------------------------ */
/* ESP8266 <-> STM32 serial link                                            */
/* ------------------------------------------------------------------------ */
/*
 * Which ESP8266 AT-command library to drive the module with:
 *
 *   WIFI_LIB_WIFIESPAT  "WiFiEspAT" by JAndrassy (default). Needs ESP8266 AT
 *                       firmware 1.7.x or 2.x. Fast, maintained, works with
 *                       STM32 core 2.x and 3.x.
 *   WIFI_LIB_WIFIESP    "WiFiEsp" by bportaluri. Works with any AT firmware
 *                       (0.x / 1.x) and is what older tutorials use, but it
 *                       is unmaintained and only compiles on STM32 core 2.x
 *                       (it includes <avr/pgmspace.h>, which core 3.x
 *                       removed). Pick this only for an ESP module with old
 *                       firmware you can't update, and install core 2.12.0.
 *
 * Check your module's firmware with a serial terminal: send "AT+GMR".
 * Both libraries are installed from Tools > Manage Libraries in the IDE.
 */
#define WIFI_LIB_WIFIESP    1
#define WIFI_LIB_WIFIESPAT  2

#ifndef WIFI_LIBRARY
#define WIFI_LIBRARY  WIFI_LIB_WIFIESPAT
#endif

/*
 * UART pins wired to the ESP8266. ESP TX -> STM32 RX pin, ESP RX -> STM32 TX.
 * Both boards are 3.3 V, so no level shifting is needed.
 *
 * Defaults pick a USART that is NOT already used by the debug "Serial":
 *   Nucleo-64 boards : Serial = USART2 (ST-Link VCP), so ESP goes on USART1
 *                      PA10 = D2 (RX), PA9 = D8 (TX) on the Arduino header.
 *   Blue/Black Pill  : Serial = USART1 (PA9/PA10), so ESP goes on USART2
 *                      PA3 (RX), PA2 (TX).
 * Any other USART works: just give its pins here.
 *
 * To use a port the core already defines (Serial1, Serial2, Serial3...)
 * instead of constructing one from pins, set ESP_SERIAL_OBJECT, e.g.
 *   #define ESP_SERIAL_OBJECT Serial3
 */
#if defined(ARDUINO_BLUEPILL_F103C8) || defined(ARDUINO_BLUEPILL_F103CB) || \
    defined(ARDUINO_BLACKPILL_F401CC) || defined(ARDUINO_BLACKPILL_F411CE) || \
    defined(ARDUINO_BLACKPILL_F401CE)
#  define ESP_UART_RX_PIN  PA3
#  define ESP_UART_TX_PIN  PA2
#else
#  define ESP_UART_RX_PIN  PA10
#  define ESP_UART_TX_PIN  PA9
#endif

/*
 * Baud rate of the ESP8266 AT firmware. Modern firmware ships at 115200.
 * Some old ESP-01 modules default to 9600 or 57600; send
 * "AT+UART_DEF=115200,8,1,0,0" once from a terminal to change it permanently.
 */
#define ESP_BAUD  115200

/* ------------------------------------------------------------------------ */
/* Debug output                                                             */
/* ------------------------------------------------------------------------ */
/*
 * Log to the board's default "Serial" (ST-Link VCP on Nucleo, USB CDC if
 * enabled in Tools > USB support, else USART1). Set to 0 to silence it.
 * NOTE: the WiFiEsp library itself also logs to "Serial" regardless of this,
 * which is why the ESP must never share that USART.
 */
#define DEBUG_ENABLED  1
#define DEBUG_SERIAL   Serial
#define DEBUG_BAUD     115200

/* ------------------------------------------------------------------------ */
/* Application                                                              */
/* ------------------------------------------------------------------------ */

#define HTTP_PORT  80

/* The LED the web page controls. */
#define LED_PIN  LED_BUILTIN

/*
 * Blue/Black Pill LEDs are wired between the pin and 3.3 V, so LOW = on.
 * Nucleo user LEDs are wired to ground, so HIGH = on. Override if needed.
 */
#ifndef LED_ACTIVE_LOW
#  if defined(ARDUINO_BLUEPILL_F103C8) || defined(ARDUINO_BLUEPILL_F103CB) || \
      defined(ARDUINO_BLACKPILL_F401CC) || defined(ARDUINO_BLACKPILL_F411CE) || \
      defined(ARDUINO_BLACKPILL_F401CE)
#    define LED_ACTIVE_LOW  1
#  else
#    define LED_ACTIVE_LOW  0
#  endif
#endif

/* Give up on a client that stops sending mid-request after this long. */
#define HTTP_REQUEST_TIMEOUT_MS  3000

/*
 * Largest single write handed to the WiFi library. The ESP8266 AT command
 * AT+CIPSEND accepts at most 2048 bytes per call; 1024 leaves headroom.
 */
#define NET_TX_CHUNK  1024

/* Size of the buffer that holds one response's status line + headers. */
#define HTTP_HEADER_BUF_SIZE  256

#endif /* CONFIG_H */
