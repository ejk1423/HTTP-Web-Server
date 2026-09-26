/*
 * test_http_server.c - Host-side unit tests for the C HTTP core.
 *
 * Exercises http_server.c exactly as the sketch does (byte-at-a-time feeding)
 * but on a PC, so protocol bugs are caught without flashing a board.
 *
 * Build & run (any C99 compiler):
 *   gcc -std=c99 -Wall -Wextra -I../arduino/stm32_esp8266_webserver \
 *       test_http_server.c ../arduino/stm32_esp8266_webserver/http_server.c \
 *       -o test_http_server && ./test_http_server
 * or use run_tests.sh / run_tests.ps1 in this folder.
 */
#include <stdio.h>
#include <string.h>

#include "http_server.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        failures++; \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_STR(actual, expected) do { \
    checks++; \
    if (strcmp((actual), (expected)) != 0) { \
        failures++; \
        printf("  FAIL %s:%d: expected \"%s\", got \"%s\"\n", __FILE__, __LINE__, (expected), (actual)); \
    } \
} while (0)

/* Feed a string byte by byte, the way the sketch reads from the ESP8266. */
static http_parse_status_t feed(http_parser_t *p, const char *s)
{
    http_parse_status_t st = HTTP_PARSE_INCOMPLETE;
    while (*s) {
        st = http_parser_feed(p, (uint8_t)*s++);
        if (st != HTTP_PARSE_INCOMPLETE) break;
    }
    return st;
}

/* ---- parser -------------------------------------------------------------- */

static void test_simple_get(void)
{
    http_parser_t p; http_request_t r;
    printf("simple GET\n");
    http_parser_init(&p, &r);
    CHECK(feed(&p, "GET / HTTP/1.1\r\nHost: 192.168.1.50\r\nUser-Agent: curl/8.0\r\nAccept: */*\r\n\r\n") == HTTP_PARSE_DONE);
    CHECK(r.method == HTTP_METHOD_GET);
    CHECK_STR(r.path, "/");
    CHECK_STR(r.query, "");
    CHECK(r.version_major == 1 && r.version_minor == 1);
    CHECK(r.keep_alive == true);        /* HTTP/1.1 default */
    CHECK(r.content_length == 0);
    CHECK(r.body_len == 0);
}

static void test_browser_get_with_long_headers(void)
{
    http_parser_t p; http_request_t r;
    printf("browser GET with long headers\n");
    http_parser_init(&p, &r);
    CHECK(feed(&p,
        "GET /api/status HTTP/1.1\r\n"
        "Host: 192.168.1.50\r\n"
        "Connection: keep-alive\r\n"
        "Cache-Control: max-age=0\r\n"
        "Upgrade-Insecure-Requests: 1\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n"
        "Accept-Encoding: gzip, deflate\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Cookie: a=1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef; b=zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\r\n"
        "A-Very-Long-Header-Name-That-Nobody-Would-Ever-Use: value\r\n"
        "\r\n") == HTTP_PARSE_DONE);
    CHECK_STR(r.path, "/api/status");
    CHECK(r.keep_alive == true);
}

static void test_query_string(void)
{
    http_parser_t p; http_request_t r;
    char val[16];
    printf("query string\n");
    http_parser_init(&p, &r);
    CHECK(feed(&p, "GET /led?state=on&name=hello%20world+x HTTP/1.1\r\n\r\n") == HTTP_PARSE_DONE);
    CHECK_STR(r.path, "/led");
    CHECK_STR(r.query, "state=on&name=hello%20world+x");
    CHECK(http_query_get(r.query, "state", val, sizeof(val)));
    CHECK_STR(val, "on");
    CHECK(http_query_get(r.query, "name", val, sizeof(val)));
    CHECK_STR(val, "hello world x");
    CHECK(!http_query_get(r.query, "missing", val, sizeof(val)));
    CHECK_STR(val, "");
    /* key that is a prefix of another must not match */
    CHECK(!http_query_get(r.query, "stat", val, sizeof(val)));
    /* value truncation is safe */
    CHECK(http_query_get("k=abcdefghijklmnopqrstuvwxyz", "k", val, 4));
    CHECK_STR(val, "abc");
    /* key with no '=' */
    CHECK(http_query_get("flag&x=1", "flag", val, sizeof(val)));
    CHECK_STR(val, "");
}

static void test_post_form_body(void)
{
    http_parser_t p; http_request_t r;
    char val[16];
    printf("POST with form body\n");
    http_parser_init(&p, &r);
    CHECK(feed(&p,
        "POST /led HTTP/1.1\r\n"
        "Host: x\r\n"
        "content-type: application/x-www-form-urlencoded\r\n"
        "Content-Length:   9  \r\n"
        "Connection: close\r\n"
        "\r\n"
        "state=off") == HTTP_PARSE_DONE);
    CHECK(r.method == HTTP_METHOD_POST);
    CHECK(r.content_length == 9);
    CHECK(r.body_len == 9);
    CHECK_STR(r.body, "state=off");
    CHECK_STR(r.content_type, "application/x-www-form-urlencoded");
    CHECK(r.keep_alive == false);
    CHECK(http_query_get(r.body, "state", val, sizeof(val)));
    CHECK_STR(val, "off");
}

static void test_body_arrives_in_pieces(void)
{
    http_parser_t p; http_request_t r;
    printf("body arriving in pieces\n");
    http_parser_init(&p, &r);
    CHECK(feed(&p, "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nab") == HTTP_PARSE_INCOMPLETE);
    CHECK(feed(&p, "c") == HTTP_PARSE_INCOMPLETE);
    CHECK(feed(&p, "de") == HTTP_PARSE_DONE);
    CHECK_STR(r.body, "abcde");
    /* extra bytes after completion are ignored, state stays DONE */
    CHECK(http_parser_feed(&p, 'Z') == HTTP_PARSE_DONE);
    CHECK_STR(r.body, "abcde");
}

static void test_feed_buf(void)
{
    http_parser_t p; http_request_t r;
    size_t consumed = 0;
    const char *msg = "GET /a HTTP/1.0\r\n\r\nGET /b HTTP/1.0\r\n\r\n";
    printf("feed_buf stops at end of first request\n");
    http_parser_init(&p, &r);
    CHECK(http_parser_feed_buf(&p, (const uint8_t *)msg, strlen(msg), &consumed) == HTTP_PARSE_DONE);
    CHECK(consumed == 19);
    CHECK_STR(r.path, "/a");
    CHECK(r.keep_alive == false);       /* HTTP/1.0 default */
}

static void test_bare_lf_and_leading_blank_lines(void)
{
    http_parser_t p; http_request_t r;
    printf("bare LF line endings and leading CRLF\n");
    http_parser_init(&p, &r);
    CHECK(feed(&p, "\r\n\r\nHEAD /x HTTP/1.1\nHost: y\n\n") == HTTP_PARSE_DONE);
    CHECK(r.method == HTTP_METHOD_HEAD);
    CHECK_STR(r.path, "/x");
}

static void test_http10_keepalive_header(void)
{
    http_parser_t p; http_request_t r;
    printf("HTTP/1.0 with Connection: Keep-Alive\n");
    http_parser_init(&p, &r);
    CHECK(feed(&p, "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n") == HTTP_PARSE_DONE);
    CHECK(r.keep_alive == true);
}

static void test_errors(void)
{
    http_parser_t p; http_request_t r;
    char big[HTTP_MAX_PATH_LEN + 16];
    printf("malformed requests\n");

    http_parser_init(&p, &r);
    CHECK(feed(&p, "GARBAGE\r\n\r\n") == HTTP_PARSE_ERROR);
    CHECK(http_parser_error_status(&p) == 400);
    CHECK(http_parser_feed(&p, 'x') == HTTP_PARSE_ERROR);   /* sticky */

    http_parser_init(&p, &r);
    CHECK(feed(&p, "GET / HTTP/2.0\r\n\r\n") == HTTP_PARSE_ERROR);
    CHECK(http_parser_error_status(&p) == 400);

    http_parser_init(&p, &r);
    CHECK(feed(&p, "GET / HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n") == HTTP_PARSE_ERROR);
    CHECK(http_parser_error_status(&p) == 400);

    http_parser_init(&p, &r);
    CHECK(feed(&p, "POST / HTTP/1.1\r\nContent-Length: 99999\r\n\r\n") == HTTP_PARSE_ERROR);
    CHECK(http_parser_error_status(&p) == 413);

    http_parser_init(&p, &r);
    CHECK(feed(&p, "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == HTTP_PARSE_ERROR);
    CHECK(http_parser_error_status(&p) == 501);

    http_parser_init(&p, &r);
    CHECK(feed(&p, "BREW / HTTP/1.1\r\n\r\n") == HTTP_PARSE_ERROR);
    CHECK(http_parser_error_status(&p) == 501);

    http_parser_init(&p, &r);
    CHECK(feed(&p, "GET / HTTP/1.1\r\n continued\r\n\r\n") == HTTP_PARSE_ERROR);   /* obs-fold */
    CHECK(http_parser_error_status(&p) == 400);

    http_parser_init(&p, &r);
    memset(big, 'a', sizeof(big));
    big[0] = '/';
    big[sizeof(big) - 1] = '\0';
    CHECK(feed(&p, "GET ") == HTTP_PARSE_INCOMPLETE);
    CHECK(feed(&p, big) == HTTP_PARSE_ERROR);
    CHECK(http_parser_error_status(&p) == 414);

    http_parser_init(&p, &r);
    CHECK(feed(&p, "GET /\x01 HTTP/1.1\r\n\r\n") == HTTP_PARSE_ERROR);
    CHECK(http_parser_error_status(&p) == 400);

    /* no error reported while healthy */
    http_parser_init(&p, &r);
    CHECK(http_parser_error_status(&p) == 0);
}

/* ---- url decode ---------------------------------------------------------- */

static void test_url_decode(void)
{
    char out[32];
    printf("url decode\n");
    CHECK(http_url_decode("a%20b+c%2Fd", 11, out, sizeof(out)) == 7);
    CHECK_STR(out, "a b c/d");
    CHECK(http_url_decode("100%", 4, out, sizeof(out)) == 4);   /* dangling % kept */
    CHECK_STR(out, "100%");
    CHECK(http_url_decode("%zz", 3, out, sizeof(out)) == 3);    /* bad hex kept */
    CHECK_STR(out, "%zz");
    CHECK(http_url_decode("abcdef", 6, out, 3) == 2);           /* truncation */
    CHECK_STR(out, "ab");
    CHECK(http_url_decode("abc", 3, out, 0) == 0);              /* zero cap safe */
}

/* ---- responses ----------------------------------------------------------- */

static void test_response_header(void)
{
    http_response_t res;
    char hdr[256];
    size_t n;
    printf("response header building\n");

    http_response_init(&res);
    http_response_set_text(&res, 200, HTTP_CT_HTML, "<p>hi</p>");
    n = http_response_build_header(&res, hdr, sizeof(hdr));
    CHECK(n > 0 && n == strlen(hdr));
    CHECK_STR(hdr,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: 9\r\n"
        "Connection: close\r\n"
        "\r\n");

    http_response_init(&res);
    http_response_redirect(&res, "/");
    res.no_cache = true;
    res.extra_headers = "X-Powered-By: STM32\r\n";
    n = http_response_build_header(&res, hdr, sizeof(hdr));
    CHECK(n > 0);
    CHECK_STR(hdr,
        "HTTP/1.1 303 See Other\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "Location: /\r\n"
        "X-Powered-By: STM32\r\n"
        "\r\n");

    /* too-small buffer reports 0 instead of overflowing */
    http_response_init(&res);
    n = http_response_build_header(&res, hdr, 40);
    CHECK(n == 0);
    n = http_response_build_header(&res, hdr, 0);
    CHECK(n == 0);
}

static void test_response_printf_and_error(void)
{
    http_response_t res;
    char huge[HTTP_DYN_BODY_LEN * 2];
    int n;
    printf("response printf / error\n");

    http_response_init(&res);
    n = http_response_printf(&res, 200, HTTP_CT_JSON, "{\"led\":%d,\"uptime\":%lu}", 1, 42UL);
    CHECK(n == (int)strlen("{\"led\":1,\"uptime\":42}"));
    CHECK(res.body == res.dyn);
    CHECK(res.body_len == (size_t)n);
    CHECK_STR(res.body, "{\"led\":1,\"uptime\":42}");
    CHECK(res.status == 200);
    CHECK_STR(res.content_type, HTTP_CT_JSON);

    /* oversize output is truncated, not overflowed */
    memset(huge, 'x', sizeof(huge));
    huge[sizeof(huge) - 1] = '\0';
    http_response_init(&res);
    n = http_response_printf(&res, 200, HTTP_CT_TEXT, "%s", huge);
    /* C99 runtimes report the full length; MSVCRT-based hosts report what fit. */
    CHECK(n >= HTTP_DYN_BODY_LEN - 1);
    CHECK(res.body_len == HTTP_DYN_BODY_LEN - 1);
    CHECK(strlen(res.dyn) == HTTP_DYN_BODY_LEN - 1);

    http_response_init(&res);
    http_response_error(&res, 404);
    CHECK(res.status == 404);
    CHECK_STR(res.body, "404 Not Found\n");
    CHECK(res.no_cache);

    CHECK_STR(http_status_reason(999), "Unknown");
    CHECK_STR(http_method_name(HTTP_METHOD_POST), "POST");
}

/* ---- routing ------------------------------------------------------------- */

static int hits_index, hits_led, hits_prefix, hits_404;

static void h_index(const http_request_t *req, http_response_t *res)
{ (void)req; hits_index++; http_response_set_text(res, 200, HTTP_CT_HTML, "index"); }
static void h_led(const http_request_t *req, http_response_t *res)
{ (void)req; hits_led++; http_response_set_text(res, 200, HTTP_CT_JSON, "{}"); }
static void h_prefix(const http_request_t *req, http_response_t *res)
{ (void)req; hits_prefix++; http_response_redirect(res, "/"); }
static void h_404(const http_request_t *req, http_response_t *res)
{ (void)req; hits_404++; http_response_set_text(res, 404, HTTP_CT_HTML, "custom 404"); }

static void run(const http_server_t *s, const char *raw, http_response_t *res)
{
    http_parser_t p; http_request_t r;
    http_parser_init(&p, &r);
    http_response_init(res);
    if (feed(&p, raw) != HTTP_PARSE_DONE) {
        printf("  (test bug: request did not parse)\n");
        failures++;
        return;
    }
    http_server_handle(s, &r, res);
}

static void test_routing(void)
{
    static const http_route_t routes[] = {
        { HTTP_METHOD_GET,  "/",      h_index  },
        { HTTP_METHOD_POST, "/led",   h_led    },
        { HTTP_METHOD_GET,  "/led/*", h_prefix },
    };
    http_server_t s;
    http_response_t res;
    printf("routing\n");

    http_server_init(&s, routes, 3, h_404);

    run(&s, "GET / HTTP/1.1\r\n\r\n", &res);
    CHECK(hits_index == 1 && res.status == 200);

    run(&s, "HEAD / HTTP/1.1\r\n\r\n", &res);           /* HEAD served by GET */
    CHECK(hits_index == 2 && res.status == 200);

    run(&s, "POST /led HTTP/1.1\r\n\r\n", &res);
    CHECK(hits_led == 1 && res.status == 200);

    run(&s, "GET /led HTTP/1.1\r\n\r\n", &res);         /* wrong method */
    CHECK(hits_led == 1 && res.status == 405);

    run(&s, "GET /led/on HTTP/1.1\r\n\r\n", &res);      /* prefix route */
    CHECK(hits_prefix == 1 && res.status == 303);
    CHECK_STR(res.location, "/");

    run(&s, "GET /ledx HTTP/1.1\r\n\r\n", &res);        /* "/led/*" must not match "/ledx" */
    CHECK(hits_prefix == 1 && hits_404 == 1 && res.status == 404);
    CHECK_STR(res.body, "custom 404");

    http_server_init(&s, routes, 3, NULL);              /* default 404 */
    run(&s, "GET /nope HTTP/1.1\r\n\r\n", &res);
    CHECK(res.status == 404);
    CHECK_STR(res.body, "404 Not Found\n");
}

/* ---- end-to-end: request bytes in, response bytes out --------------------- */

static void test_end_to_end(void)
{
    static const http_route_t routes[] = { { HTTP_METHOD_GET, "/", h_index } };
    http_server_t s;
    http_parser_t p; http_request_t r; http_response_t res;
    char hdr[256];
    char wire[512];
    size_t hlen;
    printf("end to end\n");

    http_server_init(&s, routes, 1, NULL);
    http_parser_init(&p, &r);
    http_response_init(&res);
    CHECK(feed(&p, "GET / HTTP/1.1\r\nHost: stm32\r\n\r\n") == HTTP_PARSE_DONE);
    http_server_handle(&s, &r, &res);
    hlen = http_response_build_header(&res, hdr, sizeof(hdr));
    CHECK(hlen > 0);
    memcpy(wire, hdr, hlen);
    memcpy(wire + hlen, res.body, res.body_len);
    wire[hlen + res.body_len] = '\0';
    CHECK_STR(wire,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: 5\r\n"
        "Connection: close\r\n"
        "\r\n"
        "index");
}

int main(void)
{
    test_simple_get();
    test_browser_get_with_long_headers();
    test_query_string();
    test_post_form_body();
    test_body_arrives_in_pieces();
    test_feed_buf();
    test_bare_lf_and_leading_blank_lines();
    test_http10_keepalive_header();
    test_errors();
    test_url_decode();
    test_response_header();
    test_response_printf_and_error();
    test_routing();
    test_end_to_end();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
