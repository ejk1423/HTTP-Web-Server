/*
 * http_server.c - see http_server.h for the overview.
 *
 * Compiled as plain C by the Arduino IDE (any .c file in the sketch folder)
 * and by a desktop compiler for the unit tests in ../../test.
 */
#include "http_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Small string helpers (avoid <ctype.h>/<strings.h> locale baggage)        */
/* ------------------------------------------------------------------------ */

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (to_lower(*a) != to_lower(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* RFC 7230 "tchar": characters allowed in a method or header name. */
static bool is_tchar(uint8_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return strchr("!#$%&'*+-.^_`|~", (int)c) != NULL && c != '\0';
}

/* Printable US-ASCII, the only thing that may appear in a request target. */
static bool is_vchar(uint8_t c)
{
    return c >= 0x21 && c <= 0x7e;
}

/* ------------------------------------------------------------------------ */
/* Parser                                                                   */
/* ------------------------------------------------------------------------ */

enum {
    ST_START = 0,     /* tolerate blank lines before the request line       */
    ST_METHOD,
    ST_PATH,
    ST_QUERY,
    ST_VERSION,
    ST_HDR_START,     /* start of a header line, or the blank line          */
    ST_HDR_NAME,
    ST_HDR_VALUE_WS,  /* optional whitespace after ':'                      */
    ST_HDR_VALUE,
    ST_BODY,
    ST_DONE,
    ST_ERROR
};

enum {
    HDR_NONE = 0,     /* header we don't care about: value is skipped       */
    HDR_CONTENT_LENGTH,
    HDR_CONTENT_TYPE,
    HDR_CONNECTION,
    HDR_TRANSFER_ENCODING
};

/* Longest token that ever goes through p->name: header names and methods. */
#define NAME_CAP HTTP_MAX_HEADER_NAME_LEN

static http_parse_status_t fail(http_parser_t *p, uint16_t status)
{
    p->state = ST_ERROR;
    p->status_code = status;
    return HTTP_PARSE_ERROR;
}

static http_method_t method_from_string(const char *s)
{
    if (strcmp(s, "GET") == 0)     return HTTP_METHOD_GET;
    if (strcmp(s, "HEAD") == 0)    return HTTP_METHOD_HEAD;
    if (strcmp(s, "POST") == 0)    return HTTP_METHOD_POST;
    if (strcmp(s, "PUT") == 0)     return HTTP_METHOD_PUT;
    if (strcmp(s, "DELETE") == 0)  return HTTP_METHOD_DELETE;
    if (strcmp(s, "OPTIONS") == 0) return HTTP_METHOD_OPTIONS;
    return HTTP_METHOD_UNKNOWN;
}

static uint8_t classify_header(const char *lower_name)
{
    if (strcmp(lower_name, "content-length") == 0)    return HDR_CONTENT_LENGTH;
    if (strcmp(lower_name, "content-type") == 0)      return HDR_CONTENT_TYPE;
    if (strcmp(lower_name, "connection") == 0)        return HDR_CONNECTION;
    if (strcmp(lower_name, "transfer-encoding") == 0) return HDR_TRANSFER_ENCODING;
    return HDR_NONE;
}

/* "HTTP/1.1" -> major=1, minor=1. Only HTTP/1.x is accepted. */
static bool parse_version(const char *s, http_request_t *req)
{
    if (strncmp(s, "HTTP/", 5) != 0) return false;
    if (s[5] < '0' || s[5] > '9' || s[6] != '.' || s[7] < '0' || s[7] > '9' || s[8] != '\0') {
        return false;
    }
    req->version_major = (uint8_t)(s[5] - '0');
    req->version_minor = (uint8_t)(s[7] - '0');
    if (req->version_major != 1) return false;
    /* HTTP/1.1 defaults to persistent connections, HTTP/1.0 does not. */
    req->keep_alive = (req->version_minor >= 1);
    return true;
}

/* Called with a complete header line in p->name (lowercased) / p->value. */
static http_parse_status_t apply_header(http_parser_t *p)
{
    http_request_t *req = p->req;

    switch (p->hdr_field) {
    case HDR_CONTENT_LENGTH: {
        size_t n = 0;
        const char *s = p->value;
        if (*s == '\0') return fail(p, 400);
        for (; *s; s++) {
            if (*s < '0' || *s > '9') return fail(p, 400);
            if (n > (SIZE_MAX / 10)) return fail(p, 413);
            n = n * 10 + (size_t)(*s - '0');
        }
        if (n > HTTP_MAX_BODY_LEN) return fail(p, 413);
        req->content_length = n;
        break;
    }
    case HDR_CONTENT_TYPE:
        strncpy(req->content_type, p->value, HTTP_MAX_HEADER_VALUE_LEN);
        req->content_type[HTTP_MAX_HEADER_VALUE_LEN] = '\0';
        break;
    case HDR_CONNECTION:
        if (str_ieq(p->value, "close")) {
            req->keep_alive = false;
        } else if (str_ieq(p->value, "keep-alive")) {
            req->keep_alive = true;
        }
        break;
    case HDR_TRANSFER_ENCODING:
        /* Chunked request bodies are not supported on this tiny server. */
        return fail(p, 501);
    default:
        break;
    }
    return HTTP_PARSE_INCOMPLETE;
}

static http_parse_status_t end_of_headers(http_parser_t *p)
{
    http_request_t *req = p->req;

    if (req->method == HTTP_METHOD_UNKNOWN) {
        return fail(p, 501);
    }
    if (req->content_length == 0) {
        p->state = ST_DONE;
        return HTTP_PARSE_DONE;
    }
    p->state = ST_BODY;
    return HTTP_PARSE_INCOMPLETE;
}

void http_parser_init(http_parser_t *p, http_request_t *req)
{
    memset(p, 0, sizeof(*p));
    memset(req, 0, sizeof(*req));
    p->req = req;
    p->state = ST_START;
}

http_parse_status_t http_parser_feed(http_parser_t *p, uint8_t c)
{
    http_request_t *req = p->req;

    switch (p->state) {

    case ST_DONE:
        return HTTP_PARSE_DONE;

    case ST_ERROR:
        return HTTP_PARSE_ERROR;

    case ST_START:
        if (c == '\r' || c == '\n') {
            return HTTP_PARSE_INCOMPLETE;
        }
        p->state = ST_METHOD;
        p->pos = 0;
        /* fall through */

    case ST_METHOD:
        if (c == ' ') {
            if (p->pos == 0) return fail(p, 400);
            p->name[p->pos] = '\0';
            req->method = method_from_string(p->name);
            p->state = ST_PATH;
            p->pos = 0;
            return HTTP_PARSE_INCOMPLETE;
        }
        if (!is_tchar(c)) return fail(p, 400);
        if (p->pos >= NAME_CAP) return fail(p, 501);
        p->name[p->pos++] = (char)c;
        return HTTP_PARSE_INCOMPLETE;

    case ST_PATH:
        if (c == ' ' || c == '?') {
            if (p->pos == 0) return fail(p, 400);
            req->path[p->pos] = '\0';
            p->state = (c == '?') ? ST_QUERY : ST_VERSION;
            p->pos = 0;
            return HTTP_PARSE_INCOMPLETE;
        }
        if (!is_vchar(c)) return fail(p, 400);
        if (p->pos >= HTTP_MAX_PATH_LEN) return fail(p, 414);
        req->path[p->pos++] = (char)c;
        return HTTP_PARSE_INCOMPLETE;

    case ST_QUERY:
        if (c == ' ') {
            req->query[p->pos] = '\0';
            p->state = ST_VERSION;
            p->pos = 0;
            return HTTP_PARSE_INCOMPLETE;
        }
        if (!is_vchar(c)) return fail(p, 400);
        if (p->pos >= HTTP_MAX_QUERY_LEN) return fail(p, 414);
        req->query[p->pos++] = (char)c;
        return HTTP_PARSE_INCOMPLETE;

    case ST_VERSION:
        if (c == '\r') {
            return HTTP_PARSE_INCOMPLETE;      /* CR is optional, LF ends the line */
        }
        if (c == '\n') {
            p->value[p->pos] = '\0';
            if (!parse_version(p->value, req)) return fail(p, 400);
            p->state = ST_HDR_START;
            p->pos = 0;
            return HTTP_PARSE_INCOMPLETE;
        }
        if (p->pos >= 8) return fail(p, 400);  /* "HTTP/1.1" is exactly 8 chars */
        p->value[p->pos++] = (char)c;
        return HTTP_PARSE_INCOMPLETE;

    case ST_HDR_START:
        if (c == '\r') {
            return HTTP_PARSE_INCOMPLETE;
        }
        if (c == '\n') {
            return end_of_headers(p);          /* blank line: headers finished */
        }
        if (c == ' ' || c == '\t') {
            return fail(p, 400);               /* obsolete line folding: reject */
        }
        p->state = ST_HDR_NAME;
        p->pos = 0;
        p->name[0] = '\0';
        /* fall through */

    case ST_HDR_NAME:
        if (c == ':') {
            p->name[p->pos] = '\0';
            p->hdr_field = classify_header(p->name);
            p->state = ST_HDR_VALUE_WS;
            p->pos = 0;
            return HTTP_PARSE_INCOMPLETE;
        }
        if (!is_tchar(c)) return fail(p, 400);
        if (p->pos < NAME_CAP) {
            p->name[p->pos++] = to_lower((char)c);
        } else {
            /* Longer than any name we know: remember it as "uninteresting". */
            p->name[0] = '\0';
            p->pos = NAME_CAP;
        }
        return HTTP_PARSE_INCOMPLETE;

    case ST_HDR_VALUE_WS:
        if (c == ' ' || c == '\t') {
            return HTTP_PARSE_INCOMPLETE;
        }
        p->state = ST_HDR_VALUE;
        p->pos = 0;
        /* fall through */

    case ST_HDR_VALUE:
        if (c == '\r') {
            return HTTP_PARSE_INCOMPLETE;
        }
        if (c == '\n') {
            while (p->pos > 0 && (p->value[p->pos - 1] == ' ' || p->value[p->pos - 1] == '\t')) {
                p->pos--;                      /* trim trailing whitespace */
            }
            p->value[p->pos] = '\0';
            if (apply_header(p) == HTTP_PARSE_ERROR) {
                return HTTP_PARSE_ERROR;
            }
            p->state = ST_HDR_START;
            p->pos = 0;
            return HTTP_PARSE_INCOMPLETE;
        }
        /* Only buffer values of headers we act on; everything else streams past. */
        if (p->hdr_field != HDR_NONE && p->pos < HTTP_MAX_HEADER_VALUE_LEN) {
            p->value[p->pos++] = (char)c;
        }
        return HTTP_PARSE_INCOMPLETE;

    case ST_BODY:
        req->body[req->body_len++] = (char)c;
        if (req->body_len >= req->content_length) {
            req->body[req->body_len] = '\0';
            p->state = ST_DONE;
            return HTTP_PARSE_DONE;
        }
        return HTTP_PARSE_INCOMPLETE;

    default:
        return fail(p, 500);
    }
}

http_parse_status_t http_parser_feed_buf(http_parser_t *p, const uint8_t *buf,
                                         size_t len, size_t *consumed)
{
    http_parse_status_t st = HTTP_PARSE_INCOMPLETE;
    size_t i;

    for (i = 0; i < len; i++) {
        st = http_parser_feed(p, buf[i]);
        if (st != HTTP_PARSE_INCOMPLETE) {
            i++;
            break;
        }
    }
    if (consumed) {
        *consumed = i;
    }
    return st;
}

uint16_t http_parser_error_status(const http_parser_t *p)
{
    return (p->state == ST_ERROR) ? p->status_code : 0;
}

/* ------------------------------------------------------------------------ */
/* Query-string helpers                                                     */
/* ------------------------------------------------------------------------ */

size_t http_url_decode(const char *in, size_t in_len, char *out, size_t out_cap)
{
    size_t o = 0;
    size_t i;

    if (out_cap == 0) {
        return 0;
    }
    for (i = 0; i < in_len && o < out_cap - 1; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < in_len) {
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (char)((hi << 4) | lo);
                i += 2;
            }
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return o;
}

bool http_query_get(const char *query, const char *key, char *out, size_t out_cap)
{
    size_t klen = strlen(key);
    const char *p = query;

    if (out_cap) {
        out[0] = '\0';
    }
    if (query == NULL) {
        return false;
    }

    while (*p) {
        const char *amp = strchr(p, '&');
        size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', pair_len);
        size_t this_klen = eq ? (size_t)(eq - p) : pair_len;

        if (this_klen == klen && memcmp(p, key, klen) == 0) {
            if (eq) {
                http_url_decode(eq + 1, pair_len - this_klen - 1, out, out_cap);
            }
            return true;
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* Responses                                                                */
/* ------------------------------------------------------------------------ */

const char *http_status_reason(uint16_t status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

const char *http_method_name(http_method_t m)
{
    switch (m) {
    case HTTP_METHOD_GET:     return "GET";
    case HTTP_METHOD_HEAD:    return "HEAD";
    case HTTP_METHOD_POST:    return "POST";
    case HTTP_METHOD_PUT:     return "PUT";
    case HTTP_METHOD_DELETE:  return "DELETE";
    case HTTP_METHOD_OPTIONS: return "OPTIONS";
    case HTTP_METHOD_ANY:     return "*";
    default:                  return "UNKNOWN";
    }
}

void http_response_init(http_response_t *res)
{
    res->status = 200;
    res->content_type = HTTP_CT_TEXT;
    res->body = "";
    res->body_len = 0;
    res->location = NULL;
    res->extra_headers = NULL;
    res->no_cache = false;
    res->dyn[0] = '\0';
}

void http_response_set(http_response_t *res, uint16_t status,
                       const char *content_type, const char *body, size_t body_len)
{
    res->status = status;
    res->content_type = content_type;
    res->body = body ? body : "";
    res->body_len = body ? body_len : 0;
}

void http_response_set_text(http_response_t *res, uint16_t status,
                            const char *content_type, const char *body)
{
    http_response_set(res, status, content_type, body, body ? strlen(body) : 0);
}

int http_response_printf(http_response_t *res, uint16_t status,
                         const char *content_type, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(res->dyn, sizeof(res->dyn), fmt, ap);
    va_end(ap);

    if (n < 0) {
        /*
         * C99 says vsnprintf returns the length the output would have had.
         * Pre-C99 runtimes (MSVCRT on Windows hosts) return -1 on truncation
         * and may skip the terminator, so force one and measure what we got.
         */
        res->dyn[sizeof(res->dyn) - 1] = '\0';
        n = (int)strlen(res->dyn);
    }
    res->status = status;
    res->content_type = content_type;
    res->body = res->dyn;
    res->body_len = ((size_t)n < sizeof(res->dyn)) ? (size_t)n : sizeof(res->dyn) - 1;
    return n;
}

void http_response_redirect(http_response_t *res, const char *location)
{
    http_response_set(res, 303, HTTP_CT_TEXT, "", 0);
    res->location = location;
}

void http_response_error(http_response_t *res, uint16_t status)
{
    http_response_printf(res, status, HTTP_CT_TEXT, "%u %s\n",
                         (unsigned)status, http_status_reason(status));
    res->no_cache = true;
}

/* Append helper for the header builder: returns false once out of room. */
static bool append(char *out, size_t cap, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (*len + n >= cap) {
        return false;
    }
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = '\0';
    return true;
}

size_t http_response_build_header(const http_response_t *res, char *out, size_t cap)
{
    char line[48];
    size_t len = 0;

    if (cap == 0) {
        return 0;
    }
    out[0] = '\0';

    snprintf(line, sizeof(line), "HTTP/1.1 %u ", (unsigned)res->status);
    if (!append(out, cap, &len, line)) return 0;
    if (!append(out, cap, &len, http_status_reason(res->status))) return 0;
    if (!append(out, cap, &len, "\r\n")) return 0;

    if (!append(out, cap, &len, "Content-Type: ")) return 0;
    if (!append(out, cap, &len, res->content_type ? res->content_type : HTTP_CT_TEXT)) return 0;
    if (!append(out, cap, &len, "\r\n")) return 0;

    snprintf(line, sizeof(line), "Content-Length: %lu\r\n", (unsigned long)res->body_len);
    if (!append(out, cap, &len, line)) return 0;

    if (!append(out, cap, &len, "Connection: close\r\n")) return 0;

    if (res->no_cache) {
        if (!append(out, cap, &len, "Cache-Control: no-store\r\n")) return 0;
    }
    if (res->location) {
        if (!append(out, cap, &len, "Location: ")) return 0;
        if (!append(out, cap, &len, res->location)) return 0;
        if (!append(out, cap, &len, "\r\n")) return 0;
    }
    if (res->extra_headers) {
        if (!append(out, cap, &len, res->extra_headers)) return 0;
    }
    if (!append(out, cap, &len, "\r\n")) return 0;

    return len;
}

/* ------------------------------------------------------------------------ */
/* Routing                                                                  */
/* ------------------------------------------------------------------------ */

static bool path_matches(const char *pattern, const char *path)
{
    size_t plen = strlen(pattern);

    if (plen > 0 && pattern[plen - 1] == '*') {
        return strncmp(pattern, path, plen - 1) == 0;
    }
    return strcmp(pattern, path) == 0;
}

static bool method_matches(http_method_t route_method, http_method_t req_method)
{
    if (route_method == HTTP_METHOD_ANY) return true;
    if (route_method == req_method) return true;
    /* A GET route also answers HEAD; the transport just omits the body. */
    if (route_method == HTTP_METHOD_GET && req_method == HTTP_METHOD_HEAD) return true;
    return false;
}

void http_server_init(http_server_t *s, const http_route_t *routes, size_t count,
                      http_handler_fn not_found)
{
    s->routes = routes;
    s->count = count;
    s->not_found = not_found;
}

void http_server_handle(const http_server_t *s, const http_request_t *req,
                        http_response_t *res)
{
    bool path_seen = false;
    size_t i;

    for (i = 0; i < s->count; i++) {
        const http_route_t *r = &s->routes[i];
        if (!path_matches(r->path, req->path)) {
            continue;
        }
        path_seen = true;
        if (method_matches(r->method, req->method)) {
            r->handler(req, res);
            return;
        }
    }

    if (path_seen) {
        http_response_error(res, 405);
    } else if (s->not_found) {
        s->not_found(req, res);
    } else {
        http_response_error(res, 404);
    }
}
