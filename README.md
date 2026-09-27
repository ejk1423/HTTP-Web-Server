# HTTP-Web-Server
HTTP web server code, with code made from scratch and code made using Arduino libraries via Arduino IDE.

## Arduino libraries and tools to install

| What | Kind | Where to get it | Needed for | Version verified |
|------|------|-----------------|------------|------------------|
| **STM32 MCU based boards** (STMicroelectronics) | Board package | Tools > Board > Boards Manager, after adding the URL below under File > Preferences > Additional boards manager URLs | Both sketches (required) | 3.0.0 (and 2.12.0) |
| **WiFiEspAT** (Juraj Andrassy) | Library | Tools > Manage Libraries | Both sketches (required). Drives the ESP8266 over AT commands; needs AT firmware 1.7 or newer | 1.5.0 |
| **WiFiEsp** (bportaluri) | Library | Tools > Manage Libraries | Multi-file sketch only, and only if you set `WIFI_LIBRARY` to WiFiEsp for an ESP8266 with old AT firmware. Requires board package 2.12.0 | 2.2.2 |
| **STM32CubeProgrammer** (ST) | PC tool | st.com (free account) | Uploading. The board package calls it for ST-Link, DFU and serial uploads; without it Verify works but Upload fails | any current |

Boards Manager URL:

```
https://github.com/stm32duino/BoardManagerFiles/raw/main/package_stmicroelectronics_index.json
```
