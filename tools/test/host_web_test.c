/* host_web_test.c -- unit tests for kernel/httpresp.h  (HTTP response handling).
 * (the HTML engine has its own host_engine_test.c). Pure logic: the real kernel header compiles on
 * the build host with its fixed-address buffer swapped for an ordinary array.
 *   tools/test/run_host_web.sh                                                             */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/io.h"

static unsigned char hr_store[1 << 20];
#define HR_BUF hr_store
#define HR_BUF_SIZE (1u << 18)              /* 256KB, like the kernel */
#include "../../kernel/httpresp.h"

static int checks = 0, failures = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void feed_all(const char *resp, unsigned piece) {
    hr_reset();
    size_t n = strlen(resp);
    for (size_t i = 0; i < n; i += piece) hr_feed((const u8 *)resp + i, (u32)(n - i < piece ? n - i : piece));
}

int main(void) {
    /* ---------- httpresp.h ---------- */
    const char *plain = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: 11\r\nX-Foo: bar\r\n\r\nhello world";
    for (unsigned piece = 1; piece <= 200; piece += (piece < 10 ? 1 : 37)) {
        feed_all(plain, piece);
        CHECK(hr.hdr_done && hr.status == 200, "status parse (piece %u): %d", piece, hr.status);
        CHECK(hr.have_clen && hr.clen == 11 && hr.complete, "content-length complete (piece %u)", piece);
        CHECK(strcmp(hr.content_type, "text/html; charset=UTF-8") == 0, "content-type [%s]", hr.content_type);
        hr_finish();
        CHECK(hr.body_len == 11 && memcmp(HR_BUF + hr.body_start, "hello world", 11) == 0, "body (piece %u)", piece);
    }

    const char *chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                          "5\r\nhello\r\n7;ext=1\r\n, world\r\n1B\r\n and more text after that!!\r\n0\r\n\r\n";
    for (unsigned piece = 1; piece <= 90; piece += (piece < 8 ? 1 : 13)) {
        feed_all(chunked, piece);
        CHECK(hr.chunked && hr.complete, "chunked completes (piece %u)", piece);
        hr_finish();
        const char *want = "hello, world and more text after that!!";
        CHECK(hr.body_len == strlen(want) && memcmp(HR_BUF + hr.body_start, want, hr.body_len) == 0,
              "chunked decode (piece %u): got %u bytes", piece, hr.body_len);
    }
    /* a chunked response cut off mid-chunk still yields the bytes that did arrive, and doesn't claim completion */
    feed_all("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n10\r\nabc", 4);
    CHECK(!hr.complete, "truncated chunked is not complete");
    hr_finish();
    CHECK(hr.body_len == 8 && memcmp(HR_BUF + hr.body_start, "helloabc", 8) == 0, "truncated chunked keeps what arrived (%u)", hr.body_len);

    feed_all("HTTP/1.1 301 Moved Permanently\r\nLocation: https://www.example.com/a?b=1&c=2\r\nContent-Length: 0\r\n\r\n", 7);
    CHECK(hr.status == 301 && hr_is_redirect() && strcmp(hr.location, "https://www.example.com/a?b=1&c=2") == 0, "redirect location [%s]", hr.location);
    feed_all("HTTP/1.1 200 OK\r\nlocation: /nope\r\n\r\nbody", 5);       /* not a redirect status */
    CHECK(!hr_is_redirect(), "Location on a 200 is not a redirect");
    feed_all("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\nxx", 9);
    CHECK(hr.encoded, "gzip flagged as unsupported");
    feed_all("HTTP/1.1 204 No Content\r\n\r\n", 3);
    CHECK(hr.complete, "204 completes without a body");
    feed_all("HTTP/1.0 200 OK\r\n\r\nno length, closes to end", 6);          /* delimited by connection close */
    CHECK(!hr.complete, "no length and no chunking: not complete until close");
    hr_finish();
    CHECK(hr.body_len == strlen("no length, closes to end"), "close-delimited body (%u)", hr.body_len);
    /* oversized responses: bounded, flagged, never overrun */
    hr_reset();
    static unsigned char blob[70000]; memset(blob, 'x', sizeof blob);
    hr_feed((const u8 *)"HTTP/1.1 200 OK\r\n\r\n", 19);
    for (int k = 0; k < 6; k++) hr_feed(blob, sizeof blob);
    CHECK(hr.truncated && hr.len == HR_BUF_SIZE, "oversize is truncated at the buffer size (%u)", hr.len);
    { char req[1100]; u32 n = hr_build_request(req, "example.com:8080", "/p?q=1");
      CHECK(n == strlen(req) && strstr(req, "GET /p?q=1 HTTP/1.1\r\nHost: example.com:8080\r\n") == req &&
            strstr(req, "Accept-Encoding: identity") && strstr(req, "User-Agent:") && strstr(req, "\r\n\r\n") == req + n - 4, "request text"); }


    /* ---------- rc-5: oversized responses must be an explicit failure (G-03) ---------- */
    feed_all("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\nhello world", 7);
    CHECK(!hr.too_big && !hr.truncated && !hr_size_fatal(), "a normal response is not 'too big'");
    { char hdr[128]; snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n", HR_BUF_SIZE + 1000);
      hr_reset(); hr_feed((const u8 *)hdr, (u32)strlen(hdr));
      CHECK(hr.too_big && hr_size_fatal(), "Content-Length above the buffer is flagged as soon as the headers are parsed (before any body arrives)"); }
    { char hdr[128]; snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Length: 4294967295\r\n\r\n");
      hr_reset(); hr_feed((const u8 *)hdr, (u32)strlen(hdr));
      CHECK(hr.too_big && hr_size_fatal(), "a 4 GB Content-Length does not wrap the size check"); }
    { char hdr[128];
      snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n", 262000u);   /* same digit count as the values below */
      hr_reset(); hr_feed((const u8 *)hdr, (u32)strlen(hdr));
      u32 room = HR_BUF_SIZE - hr.body_start;
      CHECK(!hr.too_big, "a 262000-byte body fits (room left after the headers: %u)", room);
      snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n", room);
      hr_reset(); hr_feed((const u8 *)hdr, (u32)strlen(hdr));
      CHECK(!hr.too_big, "a body that fits EXACTLY is fine");
      snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n", room + 1);
      hr_reset(); hr_feed((const u8 *)hdr, (u32)strlen(hdr));
      CHECK(hr.too_big, "one byte over is too big (the headers count against the buffer too)"); }
    hr_reset(); hr_feed((const u8 *)"HTTP/1.1 200 OK\r\n\r\n", 19);
    for (int k = 0; k < 6; k++) hr_feed(blob, sizeof blob);
    CHECK(hr.truncated && hr_size_fatal(), "no Content-Length, body overflowed the buffer => fatal (this was reported as success)");
    { const char *h = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
      hr_reset(); hr_feed((const u8 *)h, (u32)strlen(h));
      for (int k = 0; k < 4; k++) { char ch[32]; snprintf(ch, sizeof ch, "%x\r\n", (unsigned)sizeof blob); hr_feed((const u8 *)ch, (u32)strlen(ch)); hr_feed(blob, sizeof blob); hr_feed((const u8 *)"\r\n", 2); }
      CHECK(hr.truncated && hr_size_fatal(), "an oversized chunked response is fatal too"); }
    { const char *h = "HTTP/1.1 302 Found\r\nLocation: /elsewhere\r\nContent-Length: 999999999\r\n\r\n";
      hr_reset(); hr_feed((const u8 *)h, (u32)strlen(h));
      CHECK(hr.too_big && hr_is_redirect() && !hr_size_fatal(), "a redirect with a huge body is exempt: only Location is used"); }
    { const char *h = "HTTP/1.1 302 Found\r\nContent-Length: 999999999\r\n\r\n";
      hr_reset(); hr_feed((const u8 *)h, (u32)strlen(h));
      CHECK(hr_size_fatal(), "a 302 WITHOUT Location is not a usable redirect, so the size still matters"); }

    /* ---------- rc-5: Transfer-Encoding / Content-Encoding are token lists (M-01) ---------- */
    const char *te_chunked_cases[] = { "chunked", "Chunked", "CHUNKED", " chunked ", "\tchunked", "chunked;q=1", "identity, chunked", "gzip, chunked", "chunked , " };
    for (unsigned k = 0; k < sizeof te_chunked_cases / sizeof te_chunked_cases[0]; k++) {
        char h[200]; snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: %s\r\n\r\n", te_chunked_cases[k]);
        hr_reset(); hr_feed((const u8 *)h, (u32)strlen(h));
        CHECK(hr.chunked, "Transfer-Encoding: [%s] is chunked", te_chunked_cases[k]);
    }
    const char *te_not_chunked[] = { "notchunked", "x-chunked-test", "chunkedx", "chunked-ish", "unchunked, identity", "gzip" };
    for (unsigned k = 0; k < sizeof te_not_chunked / sizeof te_not_chunked[0]; k++) {
        char h[200]; snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nTransfer-Encoding: %s\r\n\r\n", te_not_chunked[k]);
        hr_reset(); hr_feed((const u8 *)h, (u32)strlen(h));
        CHECK(!hr.chunked, "Transfer-Encoding: [%s] is NOT chunked (substring search used to say it was)", te_not_chunked[k]);
    }
    hr_reset(); hr_feed((const u8 *)"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n", 54);
    CHECK(!hr.chunked && hr.encoded, "chunked that is not the FINAL coding is not treated as chunked (and the gzip layer makes it undecodable)");
    hr_reset(); hr_feed((const u8 *)"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 54);
    CHECK(hr.chunked && hr.encoded, "gzip + chunked: de-chunkable, but the body stays encoded");
    hr_reset(); hr_feed((const u8 *)"HTTP/1.1 200 OK\r\nTransfer-Encoding: identity, chunked\r\n\r\n", 58);
    CHECK(hr.chunked && !hr.encoded, "identity + chunked is plain chunked");
    hr_reset(); hr_feed((const u8 *)"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\n\r\n", 47);
    CHECK(!hr.encoded, "Content-Encoding: identity is not an encoding");
    hr_reset(); hr_feed((const u8 *)"HTTP/1.1 200 OK\r\nContent-Encoding: notidentity\r\n\r\n", 50);
    CHECK(hr.encoded, "Content-Encoding: notidentity IS an encoding (substring search said identity)");
    hr_reset(); hr_feed((const u8 *)"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n", 43);
    CHECK(hr.encoded, "Content-Encoding: gzip is still flagged");
    hr_reset(); hr_feed((const u8 *)"HTTP/1.1 200 OK\r\nContent-Encoding: identity, gzip\r\n\r\n", 54);
    CHECK(hr.encoded, "a list that includes gzip is encoded");
    /* a notchunked body must be read as a plain close-delimited body, not parsed as chunks */
    hr_reset(); { const char *r = "HTTP/1.1 200 OK\r\nTransfer-Encoding: notchunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"; hr_feed((const u8 *)r, (u32)strlen(r)); hr_finish(); }
    CHECK(hr.body_len == strlen("5\r\nhello\r\n0\r\n\r\n"), "an unknown coding is read until close, not mis-parsed as chunks (%u)", hr.body_len);

    /* ---------- POST (a submitted form) ---------- */
    { char req[2100]; hr_set_post("q=a+b&k=%E4%B8%80"); u32 n = hr_build_request(req, "example.com", "/form");
      CHECK(n == strlen(req) && strstr(req, "POST /form HTTP/1.1\r\nHost: example.com\r\n") == req &&
            strstr(req, "Content-Type: application/x-www-form-urlencoded") && strstr(req, "Content-Length: 17\r\n") &&
            strcmp(req + n - 17, "q=a+b&k=%E4%B8%80") == 0, "POST request text");
      CHECK(strstr(req, "Connection: close") != 0, "POST keeps Connection: close");
      hr_post_active = 0;
      n = hr_build_request(req, "example.com", "/form");
      CHECK(strncmp(req, "GET /form", 9) == 0 && !strstr(req, "Content-Length"), "disarmed: back to plain GET"); }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
