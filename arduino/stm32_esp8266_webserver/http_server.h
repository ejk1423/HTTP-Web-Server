/*
 * http_server.h - Minimal HTTP/1.1 server core for small microcontrollers.
 *
 * Pure C99, no dynamic allocation, no dependency on Arduino or any network
 * stack. It only understands bytes: you push received bytes into the parser
 * one at a time (or in chunks) and it tells you when a full request has
 * arrived. Routing then produces an http_response_t, and
 * http_response_build_header() turns that into the bytes you send back.
 *
 * The transport (here: an ESP8266 driven over AT commands via an Arduino
 * library) lives entirely in the sketch, so this file can be unit-tested on
 * a PC and reused with any other link (W5500, native lwIP, a UART bridge...).
 *
 * Memory model: every buffer is fixed-size and sized by the HTTP_MAX_* macros
 * below. Override them before including this header (or with -D flags) if a
 * project needs different limits. Defaults fit comfortably in the 20 KB RAM
 * of an STM32F103C8 "Blue Pill".
 */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Tunables                                                                 */
/* ------------------------------------------------------------------------ */

#ifndef HTTP_MAX_PATH_LEN
#define HTTP_MAX_PATH_LEN          64   /* "/api/status" etc.               */
#endif
#ifndef HTTP_MAX_QUERY_LEN
#define HTTP_MAX_QUERY_LEN         64   /* everything after '?'             */
#endif
#ifndef HTTP_MAX_BODY_LEN
#define HTTP_MAX_BODY_LEN         256   /* request body (form posts, JSON)  */
#endif
#ifndef HTTP_MAX_HEADER_NAME_LEN
#define HTTP_MAX_HEADER_NAME_LEN   24   /* longest name we care about       */
#endif
#ifndef HTTP_MAX_HEADER_VALUE_LEN
#define HTTP_MAX_HEADER_VALUE_LEN  48   /* only headers we keep are stored  */
#endif
#ifndef HTTP_DYN_BODY_LEN
#define HTTP_DYN_BODY_LEN         384   /* scratch for generated responses  */
#endif

/* Common Content-Type strings. */
#define HTTP_CT_HTML  "text/html; charset=utf-8"
#define HTTP_CT_TEXT  "text/plain; charset=utf-8"
#define HTTP_CT_JSON  "application/json"
#define HTTP_CT_CSS   "text/css"
#define HTTP_CT_JS    "application/javascript"

/* ------------------------------------------------------------------------ */
/* Requests                                                                 */
/* ------------------------------------------------------------------------ */

typedef enum {
    HTTP_METHOD_UNKNOWN = 0,
    HTTP_METHOD_GET,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_OPTIONS,
    HTTP_METHOD_ANY          /* only meaningful in a route table            */
} http_method_t;

typedef struct {
    http_method_t method;
    uint8_t  version_major;
    uint8_t  version_minor;
    char     path[HTTP_MAX_PATH_LEN + 1];      /* not percent-decoded       */
    char     query[HTTP_MAX_QUERY_LEN + 1];    /* raw, without the '?'      */
    char     content_type[HTTP_MAX_HEADER_VALUE_LEN + 1];
    size_t   content_length;
    bool     keep_alive;     /* what the client asked for; server may ignore */
    char     body[HTTP_MAX_BODY_LEN + 1];      /* always NUL-terminated     */
    size_t   body_len;
} http_request_t;

typedef enum {
    HTTP_PARSE_INCOMPLETE = 0,   /* feed more bytes                         */
    HTTP_PARSE_DONE,             /* request complete, p->req is valid       */
    HTTP_PARSE_ERROR             /* malformed; see http_parser_error_status */
} http_parse_status_t;

typedef struct {
    http_request_t *req;
    uint8_t  state;
    uint8_t  hdr_field;
    uint16_t status_code;        /* suggested HTTP status when in error     */
    size_t   pos;
    char     name[HTTP_MAX_HEADER_NAME_LEN + 1];
    char     value[HTTP_MAX_HEADER_VALUE_LEN + 1];
} http_parser_t;

/* Reset the parser and clear *req. Call once per connection. */
void http_parser_init(http_parser_t *p, http_request_t *req);

/* Push one received byte. Cheap enough to call per byte from client.read(). */
http_parse_status_t http_parser_feed(http_parser_t *p, uint8_t c);

/* Push a buffer. Stops early on DONE/ERROR; *consumed tells how far it got. */
http_parse_status_t http_parser_feed_buf(http_parser_t *p, const uint8_t *buf,
                                         size_t len, size_t *consumed);

/* 400, 413, 414, 501... once http_parser_feed() has returned ERROR. */
uint16_t http_parser_error_status(const http_parser_t *p);

/* ------------------------------------------------------------------------ */
/* Query-string / form helpers                                              */
/* ------------------------------------------------------------------------ */

/*
 * Look up a key in an application/x-www-form-urlencoded string such as
 * req->query or req->body ("led=on&brightness=50"). The value is
 * percent-decoded ('+' becomes space) into out. Returns false (and an empty
 * out) when the key is absent.
 */
bool http_query_get(const char *query, const char *key, char *out, size_t out_cap);

/* Percent-decode in_len bytes into out; returns bytes written (NUL added). */
size_t http_url_decode(const char *in, size_t in_len, char *out, size_t out_cap);

/* ------------------------------------------------------------------------ */
/* Responses                                                                */
/* ------------------------------------------------------------------------ */

typedef struct {
    uint16_t    status;         /* 200, 404, ...                            */
    const char *content_type;   /* one of HTTP_CT_* or your own             */
    const char *body;           /* may point into flash or into dyn         */
    size_t      body_len;
    const char *location;       /* adds "Location:" when non-NULL           */
    const char *extra_headers;  /* raw, each line ending in "\r\n", or NULL */
    bool        no_cache;       /* adds "Cache-Control: no-store"           */
    char        dyn[HTTP_DYN_BODY_LEN]; /* scratch for generated bodies     */
} http_response_t;

void http_response_init(http_response_t *res);

/* Point the response at an existing buffer (typically a const page in flash). */
void http_response_set(http_response_t *res, uint16_t status,
                       const char *content_type, const char *body, size_t body_len);

/* Same as above but body_len = strlen(body). */
void http_response_set_text(http_response_t *res, uint16_t status,
                            const char *content_type, const char *body);

/* printf into res->dyn and use it as the body. Returns like snprintf. */
int http_response_printf(http_response_t *res, uint16_t status,
                         const char *content_type, const char *fmt, ...);

/* 303 See Other with a Location header (post/redirect/get). */
void http_response_redirect(http_response_t *res, const char *location);

/* Plain-text error page: "404 Not Found". */
void http_response_error(http_response_t *res, uint16_t status);

/*
 * Serialise the status line and headers (terminated by the blank line) into
 * out. Returns the number of bytes written, or 0 if cap is too small.
 * The body is NOT copied: send res->body / res->body_len separately, which
 * lets large pages stay in flash and be streamed in chunks.
 * Always emits "Connection: close" - one request per TCP connection keeps
 * the ESP8266 AT layer simple and robust.
 */
size_t http_response_build_header(const http_response_t *res, char *out, size_t cap);

const char *http_status_reason(uint16_t status);
const char *http_method_name(http_method_t m);

/* ------------------------------------------------------------------------ */
/* Routing                                                                  */
/* ------------------------------------------------------------------------ */

typedef void (*http_handler_fn)(const http_request_t *req, http_response_t *res);

typedef struct {
    http_method_t   method;   /* HTTP_METHOD_ANY matches every method       */
    const char     *path;     /* exact match, or prefix match if it ends '*' */
    http_handler_fn handler;
} http_route_t;

typedef struct {
    const http_route_t *routes;
    size_t              count;
    http_handler_fn     not_found;  /* optional custom 404 handler          */
} http_server_t;

void http_server_init(http_server_t *s, const http_route_t *routes, size_t count,
                      http_handler_fn not_found);

/*
 * Pick the first route whose path and method match and run its handler.
 * HEAD is served by GET routes (the transport must simply not send the body).
 * Path matched but method didn't -> 405. Nothing matched -> not_found or 404.
 */
void http_server_handle(const http_server_t *s, const http_request_t *req,
                        http_response_t *res);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SERVER_H */
