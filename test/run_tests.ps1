# Build and run the host-side unit tests for the C HTTP core on Windows.
# Uses $env:CC if set, otherwise gcc (any C99 compiler works: gcc, clang, tcc).
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$cc = if ($env:CC) { $env:CC } else { "gcc" }
$src = "..\arduino\stm32_esp8266_webserver"
& $cc -std=c99 -Wall -Wextra "-I$src" test_http_server.c "$src\http_server.c" -o test_http_server.exe
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& .\test_http_server.exe
exit $LASTEXITCODE
