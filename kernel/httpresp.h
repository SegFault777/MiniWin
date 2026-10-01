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
    hr.len = 0; hr.truncated = 0; hr.hdr_done = 0; hr.body_start = 0; hr.status = 0;
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

static inline int hr_value_has(const u8 *v, const u8 *end, const char *word) {
    /* case-insensitive substring search inside one header value */
    u32 wl = 0; while (word[wl]) wl++;
    for (const u8 *q = v; q + wl <= end && *q != '\r' && *q != '\n'; q++) {
        u32 k = 0;
        while (k < wl && hr_lower((char)q[k]) == word[k]) k++;
        if (k == wl) return 1;
    }
    return 0;
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
            if (hr_value_has(v, line_end, "chunked")) hr.chunked = 1;
        } else if ((v = hr_header_value(p, line_end, "content-encoding"))) {
            if (!hr_value_has(v, line_end, "identity")) hr.encoded = 1;
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

/* Builds the request text into `out` (which must hold at least 1024 bytes); returns its length.
 * Headers a real server expects from a browser -- and some (DuckDuckGo among them) reject a request
 * without a plausible User-Agent. "Accept-Encoding: identity" because we cannot decompress. */
static inline u32 hr_build_request(char *out, const char *host_header, const char *path) {
    u32 pos = 0;
    const char *parts[] = {
        "GET ", path, " HTTP/1.1\r\nHost: ", host_header,
        "\r\nUser-Agent: Mozilla/5.0 (compatible; MiniWin/1.0; +https://github.com/SegFault777/MiniWin)"
        "\r\nAccept: text/html,text/plain;q=0.9,*/*;q=0.5"
        "\r\nAccept-Language: en-US,en;q=0.9,ko;q=0.8"
        "\r\nAccept-Encoding: identity"
        "\r\nConnection: close\r\n\r\n"
    };
    for (unsigned k = 0; k < sizeof(parts) / sizeof(parts[0]); k++) {
        for (u32 i = 0; parts[k][i] && pos < 1000; i++) out[pos++] = parts[k][i];
    }
    out[pos] = 0;
    return pos;
}

#endif
