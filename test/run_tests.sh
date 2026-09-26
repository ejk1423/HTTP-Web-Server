#!/bin/sh
# Build and run the host-side unit tests for the C HTTP core.
# Uses $CC if set, otherwise gcc (any C99 compiler works: gcc, clang, tcc).
set -e
cd "$(dirname "$0")"
CC="${CC:-gcc}"
SRC=../arduino/stm32_esp8266_webserver
"$CC" -std=c99 -Wall -Wextra -I"$SRC" test_http_server.c "$SRC/http_server.c" -o test_http_server
./test_http_server
