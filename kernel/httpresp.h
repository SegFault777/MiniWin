#ifndef HTTPRESP_H
#define HTTPRESP_H
#include "io.h"
#include "memmap.h"

/* ============================================================
 * httpresp.h -- everything about an HTTP RESPONSE that is the same
 * whether the bytes came over plain TCP (http.h) or through TLS
 * (https.h): where they are stored, when the response is complete,
 * how a chunked body is unwrapped, and where a redirect points.
 *
 * WHY THIS EXISTS: http.h and https.h each used to keep a private
 * 4KB response array and simply stopped reading when it filled. The
 * "done" test was "the connection closed AND nothing is left to read",
 * and a full buffer stopped anything being read -- so any page over
 * 4KB never finished at all: the browser sat on "Waiting for response"
 * forever. (DuckDuckGo Lite's results page is tens of KB.) It also
 * knew nothing of Transfer-Encoding: chunked (so a chunked page was
 * shown with hex chunk sizes spliced through it), or of redirects (a
 * 301 from http:// to https:// just displayed the redirect stub).
 *
 * Both clients now feed their bytes here. Storage is a single 256KB
 * buffer in the net arena (memmap.h) -- one request is ever in
 * flight, so one buffer serves both transports.
 * ============================================================ */

#ifndef HR_BUF                       /* (a host-side unit test defines its own array before including this) */
#define HR_BUF ((u8 *)MW_HTTP_BODY_ADDR)
#define HR_BUF_SIZE MW_HTTP_BODY_SIZE
#endif
#define HR_LOCATION_MAX 400
#define HR_CTYPE_MAX 64

typedef struct {
    u32 len;              /* bytes stored in HR_BUF: headers + (still-chunked) body, as received */
    int truncated;        /* the response was bigger than HR_BUF; the rest was dropped */
    int too_big;          /* the headers promise a body that cannot fit HR_BUF (Content-Length), seen before it all arrives */
    int hdr_done;         /* the blank line ending the headers has arrived */
    u32 body_start;       /* offset in HR_BUF where the body begins */
    int status;           /* 200, 301, 404 ... 0 until the status line has been parsed */
    int chunked;          /* Transfer-Encoding: chunked */
    int have_clen;        /* a Content-Length header was present */
    u32 clen;
    int encoded;          /* Content-Encoding other than identity (gzip/br/deflate): can't decode */
    int complete;         /* we know the response ended (Content-Length reached / last chunk seen) */
    u32 chunk_pos;        /* chunk walker: offset of the next chunk-size line */
    int finished;         /* hr_finish() has run: body_start/body_len describe the final decoded body */
    u32 body_len;         /* length of the decoded body, valid after hr_finish() */
    char location[HR_LOCATION_MAX];   /* Location: header, if any */
    char content_type[HR_CTYPE_MAX];  /* Content-Type: header value, if any */
} hr_t;

static hr_t hr;

static inline void hr_reset(void) {
    hr.len = 0; hr.truncated = 0; hr.too_big = 0; hr.hdr_done = 0; hr.body_start = 0; hr.status = 0;
    hr.chunked = 0; hr.have_clen = 0; hr.clen = 0; hr.encoded = 0; hr.complete = 0;
    hr.chunk_pos = 0; hr.finished = 0; hr.body_len = 0;
    hr.location[0] = 0; hr.content_type[0] = 0;
}

/* --- tiny string helpers (freestanding: no libc) --- */
static inline char hr_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Does the header line at `p` (up to `end`) start with `name` followed by ':'? (case-insensitive)
 * Returns a pointer to the first byte of the value (whitespace skipped), or 0. */
static inline const u8 *hr_header_value(const u8 *p, const u8 *end, const char *name) {
    while (*name) {
        if (p >= end || hr_lower((char)*p) != *name) return 0;
        p++; name++;
    }
    if (p >= end || *p != ':') return 0;
    p++;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    return p;
}

static inline void hr_copy_value(char *dst, u32 dst_size, const u8 *v, const u8 *end) {
    u32 n = 0;
    while (v < end && n + 1 < dst_size && *v != '\r' && *v != '\n') dst[n++] = (char)*v++;
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) n--;   /* trailing blanks */
    dst[n] = 0;
}

/* A header value that is a comma-separated LIST of tokens (Transfer-Encoding, Content-Encoding...) must be read
 * token by token, not searched as text: a substring search finds "chunked" inside "notchunked" or
 * "x-chunked-test" and the body then gets parsed as chunks it never contained. Tokens are trimmed of spaces and
 * tabs, compared case-insensitively and cut at ';' (parameters); empty list members are skipped; the value
 * ends at CR/LF/end. Returns a mask:
 *   HR_TOK_HAS   some token equals `word`
 *   HR_TOK_LAST  the LAST token equals `word` (RFC 7230 3.3.1: chunked must be the final transfer coding)
 *   HR_TOK_OTHER some token equals neither `word` nor `word2` (word2 may be NULL) */
#define HR_TOK_HAS   1
#define HR_TOK_LAST  2
#define HR_TOK_OTHER 4
static inline int hr_tok_eq(const u8 *ts, const u8 *te, const char *word) {
    u32 wl = 0; while (word[wl]) wl++;
    if ((u32)(te - ts) != wl) return 0;
    for (u32 k = 0; k < wl; k++) if (hr_lower((char)ts[k]) != word[k]) return 0;
    return 1;
}
static inline int hr_value_tokens(const u8 *v, const u8 *end, const char *word, const char *word2) {
    int flags = 0, last_is = 0;
    const u8 *p = v;
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        if (p >= end || *p == '\r' || *p == '\n') break;
        const u8 *ts = p;
        while (p < end && *p != ',' && *p != ';' && *p != '\r' && *p != '\n') p++;
        const u8 *te = p;
        while (te > ts && (te[-1] == ' ' || te[-1] == '\t')) te--;
        while (p < end && *p != ',' && *p != '\r' && *p != '\n') p++;     /* skip ;parameters */
        if (te == ts) continue;
        if (hr_tok_eq(ts, te, word)) { flags |= HR_TOK_HAS; last_is = 1; }
        else { last_is = 0; if (!(word2 && hr_tok_eq(ts, te, word2))) flags |= HR_TOK_OTHER; }
    }
    if (last_is) flags |= HR_TOK_LAST;
    return flags;
}

/* Parses the status line and headers once the blank line has been seen. */
static inline void hr_parse_headers(u32 header_end /* offset of the first byte AFTER \r\n\r\n */) {
    const u8 *p = HR_BUF, *end = HR_BUF + header_end;
    /* status line: "HTTP/1.1 200 OK" */
    while (p < end && *p != ' ' && *p != '\n') p++;
    while (p < end && *p == ' ') p++;
    int code = 0;
    while (p < end && *p >= '0' && *p <= '9') { code = code * 10 + (*p - '0'); p++; }
    hr.status = code;
    while (p < end && *p != '\n') p++;
    if (p < end) p++;

    while (p < end) {
        const u8 *line_end = p;
        while (line_end < end && *line_end != '\n') line_end++;
        const u8 *v;
        if ((v = hr_header_value(p, line_end, "content-length"))) {
            u32 n = 0;
            while (v < line_end && *v >= '0' && *v <= '9') { n = n * 10 + (u32)(*v - '0'); v++; }
            hr.have_clen = 1; hr.clen = n;
        } else if ((v = hr_header_value(p, line_end, "transfer-encoding"))) {
            /* chunked only counts when it is the FINAL coding; any coding we cannot undo (gzip, a typo, an
             * unknown token) means the body is not something we can show as text. */
            int fl = hr_value_tokens(v, line_end, "chunked", "identity");
            if ((fl & HR_TOK_HAS) && (fl & HR_TOK_LAST)) hr.chunked = 1;
            if (fl & HR_TOK_OTHER) hr.encoded = 1;
            if ((fl & HR_TOK_HAS) && !(fl & HR_TOK_LAST)) hr.encoded = 1;      /* chunked applied and then something else on top */
        } else if ((v = hr_header_value(p, line_end, "content-encoding"))) {
            if (hr_value_tokens(v, line_end, "identity", 0) & HR_TOK_OTHER) hr.encoded = 1;
        } else if ((v = hr_header_value(p, line_end, "location"))) {
            hr_copy_value(hr.location, sizeof(hr.location), v, line_end);
        } else if ((v = hr_header_value(p, line_end, "content-type"))) {
            hr_copy_value(hr.content_type, sizeof(hr.content_type), v, line_end);
        }
        p = line_end < end ? line_end + 1 : end;
    }
}

static inline int hr_hexval(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Advances the chunk walker as far as the bytes received so far allow. Sets hr.complete when the
 * terminating zero-size chunk has fully arrived (its size line is enough -- trailers, if any,
 * follow it but we've closed the connection by then, so we don't wait for them). */
static inline void hr_walk_chunks(void) {
    for (;;) {
        u32 p = hr.chunk_pos;
        u32 line_end = p;
        while (line_end + 1 < hr.len && !(HR_BUF[line_end] == '\r' && HR_BUF[line_end + 1] == '\n')) line_end++;
        if (line_end + 1 >= hr.len) return;                      /* size line not fully here yet */
        u32 size = 0; int any = 0;
        for (u32 i = p; i < line_end; i++) {
            int h = hr_hexval(HR_BUF[i]);
            if (h < 0) break;                                    /* ';' starts a chunk extension: ignore it */
            size = (size << 4) | (u32)h; any = 1;
        }
        if (!any) { hr.complete = 1; return; }                   /* garbage: stop waiting, decode what we have */
        if (size == 0) { hr.complete = 1; return; }
        u32 next = line_end + 2 + size + 2;                      /* size line + data + trailing CRLF */
        if (next > hr.len) return;                               /* this chunk's data not fully here yet */
        hr.chunk_pos = next;
    }
}

/* Appends received bytes. Returns how many were actually stored (less than `n` only when the
 * buffer is full -- the caller should keep DRAINING its transport anyway, or the connection
 * never finishes; see the `truncated` note above). Never fails, never overruns. */
static inline u32 hr_feed(const u8 *data, u32 n) {
    u32 room = HR_BUF_SIZE - hr.len;
    u32 take = n < room ? n : room;
    if (take < n) hr.truncated = 1;
    for (u32 i = 0; i < take; i++) HR_BUF[hr.len + i] = data[i];
    u32 old = hr.len;
    hr.len += take;

    if (!hr.hdr_done) {
        u32 from = old >= 3 ? old - 3 : 0;
        for (u32 i = from; i + 3 < hr.len; i++) {
            if (HR_BUF[i] == '\r' && HR_BUF[i+1] == '\n' && HR_BUF[i+2] == '\r' && HR_BUF[i+3] == '\n') {
                hr.hdr_done = 1;
                hr.body_start = i + 4;
                hr_parse_headers(hr.body_start);
                hr.chunk_pos = hr.body_start;
                /* A Content-Length that cannot fit the buffer is known NOW; don't download megabytes to
                 * find out (compared as "clen > room" so a huge value can't wrap the addition). */
                if (hr.have_clen && !hr.chunked && hr.clen > HR_BUF_SIZE - hr.body_start) hr.too_big = 1;
                break;
            }
        }
    }
    if (hr.hdr_done && !hr.complete) {
        if (hr.chunked) hr_walk_chunks();
        else if (hr.have_clen && hr.len - hr.body_start >= hr.clen) hr.complete = 1;
        /* a HEAD-less 1xx/204/304 has no body at all */
        if (hr.status == 204 || hr.status == 304 || (hr.status >= 100 && hr.status < 200)) hr.complete = 1;
    }
    return take;
}

/* Called once, when the response has ended (transport closed, or hr.complete). Unwraps a chunked
 * body IN PLACE (the write cursor can never overtake the read cursor, so a forward copy is safe)
 * and fixes body_start/body_len. After this, HR_BUF + hr.body_start is the page. */
static inline void hr_finish(void) {
    if (hr.finished) return;
    if (!hr.hdr_done) {                       /* no complete header block: show whatever arrived as-is */
        hr.body_start = 0; hr.body_len = hr.len; hr.finished = 1; return;
    }
    if (hr.chunked) {
        u32 r = hr.body_start, w = hr.body_start;
        while (r < hr.len) {
            u32 line_end = r;
            while (line_end + 1 < hr.len && !(HR_BUF[line_end] == '\r' && HR_BUF[line_end + 1] == '\n')) line_end++;
            if (line_end + 1 >= hr.len) break;
            u32 size = 0; int any = 0;
            for (u32 i = r; i < line_end; i++) {
                int h = hr_hexval(HR_BUF[i]);
                if (h < 0) break;
                size = (size << 4) | (u32)h; any = 1;
            }
            if (!any || size == 0) break;
            u32 data = line_end + 2;
            u32 avail = hr.len - data;
            u32 take = size < avail ? size : avail;    /* a truncated final chunk still yields its bytes */
            for (u32 i = 0; i < take; i++) HR_BUF[w + i] = HR_BUF[data + i];
            w += take;
            r = data + size + 2;
        }
        hr.body_len = w - hr.body_start;
    } else {
        hr.body_len = hr.len - hr.body_start;
        if (hr.have_clen && hr.clen < hr.body_len) hr.body_len = hr.clen;   /* ignore any garbage after the declared length */
    }
    hr.finished = 1;
}

static inline int hr_is_redirect(void) {
    return (hr.status == 301 || hr.status == 302 || hr.status == 303 || hr.status == 307 || hr.status == 308)
           && hr.location[0] != 0;
}

/* True when this response is too big to hold and the part that did not fit matters. Showing the first 256KB of
 * a bigger page as if it were the page changes its meaning (and used to be reported as success); the clients
 * turn this into an explicit failure. A redirect is exempt: its body is never shown, only Location is used. */
static inline int hr_size_fatal(void) {
    return (hr.truncated || hr.too_big) && !hr_is_redirect();
}

/* A form submitted with method="post": the next request built is a POST carrying this urlencoded body. The
 * browser UI sets it right after starting the fetch and web_start_fetch() clears it, so a redirect (303 -> GET)
 * or any later navigation goes back to plain GETs by itself. */
#define HR_POST_MAX 400
static char hr_post_body[HR_POST_MAX + 1];
static u32  hr_post_len;
static int  hr_post_active;

static inline void hr_set_post(const char *body) {
    u32 n = 0;
    while (body[n] && n < HR_POST_MAX) { hr_post_body[n] = body[n]; n++; }
    hr_post_body[n] = 0; hr_post_len = n; hr_post_active = 1;
}

/* Builds the request text into `out` (which must hold at least 2048 bytes); returns its length.
 * Headers a real server expects from a browser -- and some (DuckDuckGo among them) reject a request
 * without a plausible User-Agent. "Accept-Encoding: identity" because we cannot decompress. */
static inline u32 hr_build_request(char *out, const char *host_header, const char *path) {
    u32 pos = 0;
    char clen[12]; u32 cl = 0;
    if (hr_post_active) { char d[12]; int nd = 0; u32 v = hr_post_len; do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 10); while (nd) clen[cl++] = d[--nd]; }
    clen[cl] = 0;
    const char *parts[] = {
        hr_post_active ? "POST " : "GET ", path, " HTTP/1.1\r\nHost: ", host_header,
        "\r\nUser-Agent: Mozilla/5.0 (compatible; MiniWin/1.0; +https://github.com/SegFault777/MiniWin)"
        "\r\nAccept: text/html,text/plain;q=0.9,*/*;q=0.5"
        "\r\nAccept-Language: en-US,en;q=0.9,ko;q=0.8"
        "\r\nAccept-Encoding: identity",
        hr_post_active ? "\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " : "",
        hr_post_active ? clen : "",
        "\r\nConnection: close\r\n\r\n",
        hr_post_active ? hr_post_body : ""
    };
    for (unsigned k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
        for (u32 i = 0; parts[k][i] && pos < 1900; i++) out[pos++] = parts[k][i];
    }
    out[pos] = 0;
    return pos;
}

#endif
