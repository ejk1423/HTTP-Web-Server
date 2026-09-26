/*
 * pages.h - Static web content served by the sketch.
 *
 * Everything here is a plain C string literal so it lands in flash
 * (.rodata) on the STM32 and costs no RAM. The response builder streams it
 * straight from here in chunks, so pages may be larger than any RAM buffer.
 *
 * Keep the markup self-contained (inline CSS/JS): every extra file is one
 * more TCP connection through the ESP8266, and those are slow.
 */
#ifndef PAGES_H
#define PAGES_H

/* The main control page: LED buttons + live status polled from /api/status. */
static const char PAGE_INDEX[] =
"<!doctype html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>STM32 Web Server</title>\n"
"<link rel=\"icon\" href=\"data:,\">\n"          /* stops browsers requesting /favicon.ico */
"<style>\n"
"body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:#0f1115;color:#e8e8e8;margin:0;padding:24px;text-align:center}\n"
".card{max-width:420px;margin:0 auto;background:#1a1d24;border:1px solid #2a2f3a;border-radius:14px;padding:28px 24px;box-shadow:0 8px 30px rgba(0,0,0,.35)}\n"
"h1{font-size:1.4rem;margin:0 0 4px}\n"
".sub{color:#8a94a6;font-size:.85rem;margin:0 0 20px}\n"
".dot{display:inline-block;width:14px;height:14px;border-radius:50%;background:#444;vertical-align:middle;margin-right:8px;transition:background .2s,box-shadow .2s}\n"
".dot.lit{background:#3ddc84;box-shadow:0 0 14px #3ddc84}\n"
".state{font-size:1.15rem;margin:0 0 18px}\n"
"button{font-size:1rem;font-weight:600;padding:12px 30px;margin:6px;border:0;border-radius:10px;cursor:pointer;color:#fff}\n"
".on{background:#2e9e5b}.off{background:#c0392b}.tg{background:#3b5bdb}\n"
"button:active{transform:translateY(1px)}\n"
".meta{margin-top:22px;color:#8a94a6;font-size:.8rem;line-height:1.7}\n"
".meta b{color:#c7ccd6;font-weight:600}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"card\">\n"
"<h1>STM32 + ESP8266</h1>\n"
"<p class=\"sub\">HTTP server in C, served over an ESP8266 AT link</p>\n"
"<p class=\"state\"><span id=\"dot\" class=\"dot\"></span>LED is <b id=\"state\">&hellip;</b></p>\n"
"<button class=\"on\" onclick=\"led('on')\">ON</button>\n"
"<button class=\"off\" onclick=\"led('off')\">OFF</button>\n"
"<button class=\"tg\" onclick=\"led('toggle')\">Toggle</button>\n"
"<div class=\"meta\">Uptime <b id=\"up\">&ndash;</b> s &middot; Requests <b id=\"req\">&ndash;</b><br>"
"<span id=\"err\"></span></div>\n"
"</div>\n"
"<script>\n"
"var $=function(i){return document.getElementById(i)};\n"
"function show(s){$('state').textContent=s.led?'ON':'OFF';$('dot').className='dot'+(s.led?' lit':'');"
"$('up').textContent=s.uptime;$('req').textContent=s.requests;$('err').textContent='';}\n"
"function fail(){$('err').textContent='(no response - retrying)';}\n"
"function refresh(){fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json()}).then(show).catch(fail);}\n"
"function led(v){fetch('/led?state='+v,{method:'POST'}).then(function(r){return r.json()}).then(show).catch(fail);}\n"
"refresh();setInterval(refresh,5000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* Custom 404 page. */
static const char PAGE_404[] =
"<!doctype html><html><head><meta charset=\"utf-8\"><title>404</title>"
"<style>body{font-family:system-ui,sans-serif;background:#0f1115;color:#e8e8e8;text-align:center;padding:40px}a{color:#7aa2ff}</style>"
"</head><body><h1>404 - Not Found</h1><p>Nothing lives at that address on this STM32.</p>"
"<p><a href=\"/\">Back to the control page</a></p></body></html>";

#endif /* PAGES_H */
