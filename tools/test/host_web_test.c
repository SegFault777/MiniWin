/* host_web_test.c -- unit tests for kernel/httpresp.h (HTTP response handling) and
 * kernel/htmlview.h (HTML -> text). Both are pure logic, so the real kernel headers compile on
 * the build host with their fixed-address buffers swapped for ordinary arrays.
 *   tools/test/run_host_web.sh                                                             */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/io.h"

static unsigned char hr_store[1 << 20];
#define HR_BUF hr_store
#define HR_BUF_SIZE (1u << 18)              /* 256KB, like the kernel */
#include "../../kernel/httpresp.h"

static unsigned char hv_text_store[1 << 20];
static char hv_urls_store[1 << 16];
typedef struct { unsigned tstart, tend, url_off; } hv_link_probe_t;
static unsigned hv_links_store[3 * 1024], hv_lines_store[8192];
#define HV_TEXT_SIZE (1u << 18)
#define HV_URLS_SIZE (1u << 16)
#define HV_MAX_LINKS 1024
#define HV_MAX_LINES 8192
#define HV_TEXT hv_text_store
#define HV_URLS hv_urls_store
#define HV_LINKS ((hv_link_t *)hv_links_store)
#define HV_LINES hv_lines_store
#include "../../kernel/htmlview.h"

static int checks = 0, failures = 0;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Renders `html` and returns the wrapped lines joined with '|' (easy to compare in one string). */
static char out[65536];
static const char *render(const char *html, unsigned cols) {
    hv_render_html((const u8 *)html, (u32)strlen(html));
    hv_layout(cols);
    out[0] = 0; size_t o = 0;
    for (u32 i = 0; i < hv.line_count; i++) {
        u32 e = hv_line_end(i);
        if (i) out[o++] = '|';
        for (u32 p = HV_LINES[i]; p < e; p++) out[o++] = (char)HV_TEXT[p];
    }
    out[o] = 0;
    return out;
}
#define EXPECT(html, cols, want) do { const char *g = render(html, cols); CHECK(strcmp(g, want) == 0, "render(%s)\n   want: [%s]\n   got:  [%s]", #want, want, g); } while (0)

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

    /* ---------- htmlview.h ---------- */
    EXPECT("<html><head><title>T &amp; T</title><style>p{color:red}</style><script>var a='<p>x</p>';</script></head>"
           "<body><p>Hello   <b>brave</b>\n new world</p><p>Second&nbsp;para &lt;3 &#65;&#x42;</p></body></html>", 60,
           "Hello brave new world||Second para <3 AB");
    CHECK(strcmp(hv.title, "T & T") == 0, "title [%s]", hv.title);

    /* the shape of DuckDuckGo Lite's results table */
    const char *ddg =
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\"><html><head><meta http-equiv=\"content-type\" content=\"text/html; charset=UTF-8\" />"
      "<title>hello world at DuckDuckGo</title><link rel=\"stylesheet\" href=\"//duckduckgo.com/x.css\" /></head><body>"
      "<form action=\"/lite/\" method=\"post\"><input class=\"query\" type=\"text\" name=\"q\" value=\"hello world\"><input type=\"submit\" value=\"Search\"></form>"
      "<table border=\"0\">"
      "<tr><td valign=\"top\">1.&nbsp;</td><td><a rel=\"nofollow\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fen.wikipedia.org%2Fwiki%2FHello&amp;rut=abc\" class='result-link'>&quot;Hello, World!&quot; program - Wikipedia</a></td></tr>"
      "<tr><td>&nbsp;&nbsp;&nbsp;</td><td class='result-snippet'>A &quot;Hello, World!&quot; program is a computer program that outputs or displays the message.</td></tr>"
      "<tr><td>&nbsp;&nbsp;&nbsp;</td><td><span class='link-text'>en.wikipedia.org/wiki/Hello</span></td></tr>"
      "<tr><td colspan=2>&nbsp;</td></tr>"
      "<tr><td valign=\"top\">2.&nbsp;</td><td><a rel=\"nofollow\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2F\" class='result-link'>Example</a></td></tr>"
      "</table></body></html>";
    EXPECT(ddg, 44,
           "1. \"Hello, World!\" program - Wikipedia|A \"Hello, World!\" program is a computer|program that outputs or displays the|message.|en.wikipedia.org/wiki/Hello||2. Example");
    CHECK(hv.link_count == 2, "ddg link count %u", hv.link_count);
    CHECK(strcmp(HV_URLS + HV_LINKS[0].url_off, "//duckduckgo.com/l/?uddg=https%3A%2F%2Fen.wikipedia.org%2Fwiki%2FHello&rut=abc") == 0,
          "href entity-decoded [%s]", HV_URLS + HV_LINKS[0].url_off);
    CHECK(HV_LINKS[0].tstart == 3 && memcmp(HV_TEXT + HV_LINKS[0].tstart, "\"Hello, World!\" program - Wikipedia", 35) == 0 &&
          HV_LINKS[0].tend - HV_LINKS[0].tstart == 35, "link 0 text range [%u,%u)", HV_LINKS[0].tstart, HV_LINKS[0].tend);
    { int li = hv_offset_at(0, 10); CHECK(li >= 0 && hv_link_at((u32)li) == 0, "hit-test inside link 0");
      int no = hv_offset_at(0, 1);  CHECK(no >= 0 && hv_link_at((u32)no) == -1, "hit-test on the '1.' numbering is not a link"); }

    EXPECT("<ul><li>one</li><li>two <a href='/x'>link</a></li></ul><div>after</div>", 40, "* one|* two link|after");
    EXPECT("a<br>b<br><br>c<hr>d", 40, "a|b||c|------------------------|d");
    EXPECT("<pre>  indented\n    code  \nline3</pre>text", 40, "  indented|    code  |line3|text");
    EXPECT("<h1>Title</h1>para<h2>Sub</h2>x", 40, "Title||para||Sub||x");
    /* Hangul, raw UTF-8 and as entities; other scripts degrade to '?' */
    EXPECT("<p>\xec\x95\x88\xeb\x85\x95 &#xD55C;&#44544; \xe4\xb8\xad</p>", 40, "\xec\x95\x88\xeb\x85\x95 \xed\x95\x9c\xea\xb8\x80 ?");
    EXPECT("&ldquo;quoted&rdquo; &mdash; it&rsquo;s&hellip; &copy; &nosuchentity; a&amp;b", 60, "\"quoted\" - it's... (c) &nosuchentity; a&b");
    /* wrapping */
    EXPECT("the quick brown fox jumps over the lazy dog", 16, "the quick brown|fox jumps over|the lazy dog");
    EXPECT("supercalifragilisticexpialidocious end", 10, "supercalif|ragilistic|expialidoc|ious end");
    EXPECT("\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94 \xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94", 8, "\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94|\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94");   /* 5 + 1 + 5 cells at width 8: wraps on the space */
    /* malformed input must not derail or overrun */
    EXPECT("a < b and c > d <notatag", 40, "a < b and c > d");
    EXPECT("<div><p>unclosed <b>bold <i>italic", 40, "unclosed bold italic");
    EXPECT("<!-- a comment <p>hidden</p> -->visible<!doctype x>", 40, "visible");
    EXPECT("<a href=\"/a\">one</a> <a href=\"/b\">two</a> <a>nohref</a>", 40, "one two nohref");
    CHECK(hv.link_count == 2, "two hrefs -> two links (%u)", hv.link_count);
    { char big[6000]; strcpy(big, "<a href=\""); for (int i = 0; i < 4000; i++) strcat(big, "x"); strcat(big, "\">t</a> tail");
      const char *g = render(big, 40); CHECK(strcmp(g, "t tail") == 0, "an absurdly long href is bounded, page still renders [%s]", g); }
    /* text that ends in newlines / a long page laid out twice at different widths keeps every character */
    { static char page[100000]; strcpy(page, "<body>");
      for (int i = 0; i < 1500; i++) { char b[64]; sprintf(b, "<p>paragraph %d with some words in it</p>", i); strcat(page, b); }
      render(page, 20); u32 a = hv.line_count; render(page, 90); u32 b = hv.line_count;
      CHECK(a > b && b >= 2900, "layout at two widths: %u vs %u lines", a, b); }
    hv_render_plain((const u8 *)"line1\r\n  line2\ttab\n", 20); hv_layout(40);
    CHECK(hv.line_count == 2 && HV_TEXT[0] == 'l', "plain text keeps its lines (%u)", hv.line_count);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
