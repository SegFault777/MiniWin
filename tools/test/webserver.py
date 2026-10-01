#!/usr/bin/env python3
"""A small web server for the MiniWeb end-to-end tests: it serves exactly the awkward things a
real site does and a plain `http.server` never will.

  /big            ~150KB HTML page, Content-Length         (the old client stalled forever past 4KB)
  /huge           ~400KB page -> larger than the 256KB buffer (must truncate cleanly, not hang)
  /chunked        Transfer-Encoding: chunked, odd chunk sizes
  /redir          302 -> /redir2 (relative)
  /redir2         301 -> //HOST/chunked (scheme-relative: same scheme, default port)
  /loop           302 -> /loop (must stop after the redirect limit)
  /ddg?q=...      a DuckDuckGo-Lite-shaped results page: table layout, single-quoted attributes,
                  protocol-relative //HOST/l/?uddg=... links, entities, Korean text, script/style
  /l/?uddg=...    the "click tracker": 302 to the decoded target (like duckduckgo.com/l/)
  /target         a plain target page
  /plain          text/plain
  /bin            application/octet-stream
  /gzip           claims Content-Encoding: gzip
  /slow           headers, then the body dribbled out (keep-alive style, closes at the end)
  anything else   files from the served directory (default /tmp/www)

  webserver.py [--port 80] [--tls CERT KEY] [--dir /tmp/www]"""
import argparse, http.server, os, socketserver, ssl, sys, time, urllib.parse

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=80)
ap.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"))
ap.add_argument("--dir", default="/tmp/www")
args = ap.parse_args()

def big(n):
    parts = ["<html><head><title>Big page</title></head><body>"]
    i = 0
    while sum(map(len, parts)) < n:
        parts.append("<p>line %05d of the big page &mdash; padding text to make it long enough to overflow old buffers</p>\n" % i)
        i += 1
    parts.append("<p>THE-END-MARKER</p></body></html>")
    return "".join(parts).encode()

DDG = """<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html xmlns="http://www.w3.org/1999/xhtml"><head><meta http-equiv="content-type" content="text/html; charset=UTF-8" />
<title>%(q)s at DuckDuckGo</title><link rel="stylesheet" href="//%(host)s/dist/lc.css" type="text/css" />
<style type="text/css">body{font-family:sans-serif} .result-link{color:#00f}</style>
<script type="text/javascript">var x = "<a href='/nope'>not a link</a>";</script></head>
<body><form action="/lite/" method="post" class="header"><a href="//%(host)s/lite/" class="logo">DuckDuckGo</a>
<input class="query" type="text" size="40" name="q" autocomplete="off" value="%(q)s" > <input class="submit" type="submit" value="Search" ></form>
<table border="0"><tr><td>Results for <b>%(q)s</b></td></tr></table>
<table border="0">
<tr><td valign="top">1.&nbsp;</td><td><a rel="nofollow" href="//%(host)s/l/?uddg=%(scheme)s%%3A%%2F%%2F%(host)s%%2Ftarget&amp;rut=abc123" class='result-link'>&quot;Hello, World!&quot; program - Wikipedia</a></td></tr>
<tr><td>&nbsp;&nbsp;&nbsp;</td><td class='result-snippet'>A &quot;Hello, World!&quot; program is a computer program that outputs or displays the message &quot;Hello, World!&quot; &ndash; often the first program written by people learning to code.</td></tr>
<tr><td>&nbsp;&nbsp;&nbsp;</td><td><span class='link-text'>en.wikipedia.org/wiki/%%22Hello,_World!%%22_program</span></td></tr>
<tr><td colspan=2>&nbsp;</td></tr>
<tr><td valign="top">2.&nbsp;</td><td><a rel="nofollow" href="//%(host)s/l/?uddg=%(scheme)s%%3A%%2F%%2F%(host)s%%2Ftarget" class='result-link'>안녕하세요 세계 - 한국어 위키백과</a></td></tr>
<tr><td>&nbsp;&nbsp;&nbsp;</td><td class='result-snippet'>위키백과, 우리 모두의 백과사전. &lsquo;안녕&rsquo; 프로그램은 &#xD55C;&#44544; 텍스트를 출력합니다.</td></tr>
<tr><td colspan=2>&nbsp;</td></tr>
</table>
<form action="/lite/" method="post"><input type="submit" class='navbutton' value="Next Page &gt;"><input type="hidden" name="q" value="%(q)s"></form>
</body></html>"""

class H(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def __init__(self, *a, **k): super().__init__(*a, directory=args.dir, **k)
    def log_message(self, fmt, *a): sys.stderr.write("WEB %s\n" % (fmt % a))
    def send_body(self, body, ctype="text/html; charset=utf-8", status=200, extra=()):
        self.send_response(status)
        self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        for k, v in extra: self.send_header(k, v)
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body); self.close_connection = True
    def redirect(self, loc, status=302):
        self.send_response(status); self.send_header("Location", loc); self.send_header("Content-Length", "0")
        self.send_header("Connection", "close"); self.end_headers(); self.close_connection = True
    def do_GET(self):
        u = urllib.parse.urlsplit(self.path); host = self.headers.get("Host", "10.0.2.2")
        if u.path == "/big": return self.send_body(big(150000))
        if u.path == "/huge": return self.send_body(big(400000))
        if u.path == "/chunked":
            body = big(30000); self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8"); self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close"); self.end_headers()
            i, sizes = 0, [1, 7, 100, 1500, 3, 4096, 999]
            k = 0
            while i < len(body):
                n = sizes[k % len(sizes)]; k += 1; c = body[i:i + n]; i += n
                self.wfile.write(b"%x\r\n" % len(c) + c + b"\r\n")
            self.wfile.write(b"0\r\n\r\n"); self.close_connection = True; return
        if u.path == "/redir": return self.redirect("/redir2")
        if u.path == "/redir2": return self.redirect("//%s/chunked" % host, 301)
        if u.path == "/loop": return self.redirect("/loop")
        if u.path == "/ddg":
            q = urllib.parse.parse_qs(u.query).get("q", ["hello world"])[0]
            return self.send_body((DDG % {"q": q, "host": host, "scheme": "https" if args.tls else "http"}).encode())
        if u.path == "/l/":
            tgt = urllib.parse.parse_qs(u.query).get("uddg", ["/target"])[0]
            return self.redirect(tgt)
        if u.path == "/target": return self.send_body(b"<html><head><title>Target</title></head><body><h1>You arrived</h1><p>This is the target page.</p></body></html>")
        if u.path == "/plain": return self.send_body(b"plain line 1\nplain line 2\n", "text/plain")
        if u.path == "/bin": return self.send_body(bytes(range(256)) * 4, "application/octet-stream")
        if u.path == "/gzip": return self.send_body(b"\x1f\x8b\x08\x00garbage", extra=[("Content-Encoding", "gzip")])
        if u.path == "/slow":
            self.send_response(200); self.send_header("Content-Type", "text/plain"); self.send_header("Connection", "close"); self.end_headers()
            for i in range(5): self.wfile.write(b"slow chunk %d\n" % i); self.wfile.flush(); time.sleep(0.4)
            self.close_connection = True; return
        return super().do_GET()
    do_HEAD = do_GET

class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    allow_reuse_address = True; daemon_threads = True
srv = S(("0.0.0.0", args.port), H)
if args.tls:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.tls[0], args.tls[1]); ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384")
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print("webserver on :%d %s" % (args.port, "TLS" if args.tls else "plain"), flush=True)
srv.serve_forever()
