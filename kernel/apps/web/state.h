#ifndef MW_APPS_WEB_STATE_H
#define MW_APPS_WEB_STATE_H

/* apps/web/state.h -- WEB.MWP state, window geometry, address-bar state, bookmarks.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * WEB.MWP -- MiniWeb, a tiny HTTP client browser.
 *
 * This is the actual "layer above TCP/HTTP" -- an application a person
 * clicks on, not a headless bring-up probe. Two hardcoded target rows
 * (no DNS resolver exists yet, so there's no address bar to type a
 * hostname into -- see kernel/http.h's file header for the same
 * limitation) each fire one HTTP GET through http_get()/http_poll()
 * when clicked, and the response -- or a plain-English reason it
 * failed -- gets drawn as wrapped plain text in the content area below.
 *
 * Reuses the exact same window chrome pattern as SETTING.MWP (own
 * btn_*_x()/hit-test helpers against web_win instead of setting, same
 * ui_titlebar_buttons() calls for the three title bar buttons) rather
 * than trying to generalize that chrome into a shared helper -- two
 * copies of ~15 lines of button-drawing arithmetic is a better trade
 * than a shared abstraction that has to know about both windows'
 * slightly different constant names.
 * ============================================================ */
#define WEB_SITE_ROW_H   16
#define WEB_SITE_COUNT   2
#define WEB_SITE_PYPI    0
#define WEB_SITE_GATEWAY 1

/* Which site row (if any) is the source of whatever's currently in
 * http_client -- purely so the content area can show "Loading
 * PYPI.ORG..." instead of a generic "Loading..." that doesn't say what
 * it's loading. -1 means nothing has been requested yet this boot, -2
 * means the URL bar (not a bookmark row) drove the current fetch. */
static int web_current_site = -1;
#define WEB_SOURCE_URLBAR -2
/* Set while PYPI.ORG's DNS lookup is outstanding -- see web_go()/
 * web_poll() below. Kept separate from http_client.state (rather than
 * overloading it) because a DNS failure and an HTTP failure are
 * genuinely different things to report, and http.h shouldn't need to
 * know MiniWeb pokes at its state from outside for a phase that isn't
 * even HTTP yet. */
static int web_resolving = 0;
static int web_dns_failed = 0;

/* ------------------------------------------------------------
 * URL bar -- a real single-line text field (this kernel's first one;
 * Notepad's edit area is multi-line and SETTING.MWP has no typing at
 * all), sitting above the bookmark rows. Deliberately its own tiny
 * text-editing state rather than reusing notepad_t: a URL bar is a
 * different shape of problem (one line, no newlines, Enter submits
 * instead of inserting, no save/IME-composition ceremony worth
 * borrowing) and bolting it onto notepad_t would mean carrying a whole
 * document buffer + file-slot binding for a field that only ever holds
 * one hostname's worth of text.
 * ------------------------------------------------------------ */
#define WEB_URLBAR_MAXLEN 96
static char web_urlbar_buf[WEB_URLBAR_MAXLEN + 1];
static u32  web_urlbar_len = 0;
static int  web_urlbar_focused = 0;

/* What the URL bar actually asked for, kept around so the status line /
 * "Loading ___..." text can name it instead of just saying "Loading" --
 * same reason web_current_site exists for the two bookmark rows. Also
 * doubles as the Host header string handed to http_get()/https_get(),
 * so it has to outlive the single-line web_urlbar_buf edit field (which
 * the person can keep typing into, or clear, while a fetch from an
 * EARLIER submission is still in flight). */
static char web_last_host[WEB_URLBAR_MAXLEN + 1];
#define WEB_PATH_MAX 320                    /* request path incl. query (a search URL is long) */
static char web_last_path[WEB_PATH_MAX];
static u16  web_last_port = 0;
static int  web_redirects_left = 0;         /* a fetch may follow this many more redirects */
static int  web_page_ready = 0;             /* the finished response has been turned into the on-screen page */
static int  web_waiting_net = 0;             /* a load was requested before DHCP gave us an IP; it starts when the lease arrives */
static int  web_scroll_px = 0;              /* how far the page is scrolled, in pixels */
static int  web_doc_h = 0;                  /* the laid-out page's height, in pixels */
static char web_title[100];                 /* the page's <title>, for the status line */
#define WEB_SB_W 12                         /* scrollbar width, px */
static void web_start_fetch(int https, const char *host, u16 port, const char *path);

static inline int web_btn_close_x(void) { return win_btn_close_x(&web_win, 2); }
static inline int web_btn_max_x(void)   { return win_btn_max_x(&web_win, 2); }
static inline int web_btn_min_x(void)   { return win_btn_min_x(&web_win, 2); }
static inline int web_btn_y(void)       { return win_btn_y(&web_win, 1); }

static int web_close_hit(int px, int py) { return in_rect(px, py, web_btn_close_x(), web_btn_y(), BTN_W, BTN_H); }
static int web_min_hit(int px, int py)   { return in_rect(px, py, web_btn_min_x(),   web_btn_y(), BTN_W, BTN_H); }
static int web_max_hit(int px, int py)   { return in_rect(px, py, web_btn_max_x(),   web_btn_y(), BTN_W, BTN_H); }

static int web_titlebar_drag_hit(int px, int py) {
    if (!in_rect(px, py, web_win.x + 1, web_win.y + 1, web_win.w - 2, TITLEBAR_H)) return 0;
    if (in_rect(px, py, web_btn_min_x(), web_btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, web_btn_max_x(), web_btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, web_btn_close_x(), web_btn_y(), BTN_W, BTN_H)) return 0;
    return 1;
}

static inline int web_header_y(void) { return web_win.y + TITLEBAR_H + 3; }

/* URL bar row: a sunken text field spanning most of the window's width,
 * with a small "GO" button glued to its right edge -- same sunken/
 * raised bevel language as everywhere else in this UI (white/dark-gray
 * border = sunken, i.e. "you type into this", vs the raised look of the
 * GO button and title bar controls). */
#define WEB_URLBAR_H     18
#define WEB_GO_BTN_W     30
static inline int web_urlbar_x(void) { return web_win.x + 3; }
static inline int web_urlbar_y(void) { return web_header_y(); }
static inline int web_urlbar_w(void) { return web_win.w - 6 - WEB_GO_BTN_W - 3; }
static inline int web_go_btn_x(void) { return web_urlbar_x() + web_urlbar_w() + 3; }
static int web_urlbar_hit(int px, int py) {
    return in_rect(px, py, web_urlbar_x(), web_urlbar_y(), web_urlbar_w(), WEB_URLBAR_H);
}
static int web_go_btn_hit(int px, int py) {
    return in_rect(px, py, web_go_btn_x(), web_urlbar_y(), WEB_GO_BTN_W, WEB_URLBAR_H);
}

/* Bookmark rows now sit below the URL bar instead of right under the
 * title bar -- same WEB_SITE_ROW_H each, just offset by the bar's
 * height plus a divider's worth of breathing room. */
static inline int web_site_row_y(int idx) { return web_urlbar_y() + WEB_URLBAR_H + 4 + idx * WEB_SITE_ROW_H; }
static int web_site_row_hit(int px, int py, int idx) {
    return in_rect(px, py, web_win.x + 3, web_site_row_y(idx), web_win.w - 6, WEB_SITE_ROW_H);
}
static inline int web_content_y(void) { return web_site_row_y(WEB_SITE_COUNT) + 3; }

/* MiniWeb's site rows use two different transports on purpose: PYPI.ORG
 * goes over real HTTPS now that kernel/tls.h exists (the site redirects
 * plain HTTP to HTTPS anyway -- fetching it unencrypted just gets back
 * a 301 with nothing to show), while GATEWAY stays deliberately on
 * plain HTTP port 80 against a target that answers with nothing but a
 * fast TCP RST -- that's still a useful, always-reproducible demo of
 * connection-refused handling, and doesn't need (or benefit from)
 * TLS. */
static int web_use_https = 0;

/* Builds a 16-byte seed for tls_connect()'s PRNG from the CMOS RTC plus
 * a call counter, so two fetches in the same clock-second still get
 * different seeds. See tls_conn's own rng_state comment for the honest
 * caveat: this is real hardware-clock entropy, but coarse (one-second
 * granularity) and not remotely a hardened CSPRNG -- adequate for this
 * client's actual uses (a public client_random, and an ephemeral
 * X25519 private key on a hobby OS not defending against a
 * sophisticated adversary), not a general-purpose secure RNG. */
static u32 web_entropy_counter = 0;
static void web_make_entropy_seed(u8 seed[16]) {
    rtc_time_t t;
    rtc_read(&t);
    web_entropy_counter++;
    seed[0] = (u8)(t.year >> 8);   seed[1] = (u8)t.year;
    seed[2] = (u8)t.month;         seed[3] = (u8)t.day;
    seed[4] = (u8)t.hour;          seed[5] = (u8)t.minute;
    seed[6] = (u8)t.second;        seed[7] = (u8)t.weekday;
    seed[8]  = (u8)(web_entropy_counter >> 24);
    seed[9]  = (u8)(web_entropy_counter >> 16);
    seed[10] = (u8)(web_entropy_counter >> 8);
    seed[11] = (u8)web_entropy_counter;
    /* pad the rest with a simple mix rather than leaving it at zero --
     * costs nothing and avoids handing tls_rng_seed() a suspiciously
     * regular tail */
    for (int i = 12; i < 16; i++) seed[i] = (u8)(seed[i - 12] ^ (0x5A + i));
}

/* Packs the current RTC date/time into the YYYYMMDDHHMMSS format
 * kernel/tls.h's certificate validity checks use. */
static u64 web_now_packed(void) {
    rtc_time_t t;
    rtc_read(&t);
    return tls_pack_datetime((u32)t.year, (u32)t.month, (u32)t.day,
                              (u32)t.hour, (u32)t.minute, (u32)t.second);
}

static void web_go(int site) {
    web_current_site = site;
    web_redirects_left = 5;
    if (site == WEB_SITE_PYPI) {
        web_start_fetch(1, "pypi.org", 443, "/");
    } else {
        /* the gateway bookmark: dotted-quad of whatever DHCP said the gateway is */
        char gw[20]; u32 gl = 0; gw[0] = 0;
        for (int sh = 24; sh >= 0; sh -= 8) {
            u32 oct = (net_cfg.gateway_ip >> sh) & 0xFF;
            char d[4]; int nd = 0;
            do { d[nd++] = (char)('0' + oct % 10); oct /= 10; } while (oct);
            while (nd) gw[gl++] = d[--nd];
            if (sh) gw[gl++] = '.';
        }
        gw[gl] = 0;
        web_start_fetch(0, gw, 80, "/");
    }
}

/* Is `s` a bare IPv4 address ("10.0.2.2")? A handful of digits and dots
 * is worth checking for on its own, separately from
 * web_looks_like_url() below, because an IP address should skip DNS
 * entirely and go straight to http_get() with that address -- exactly
 * like the GATEWAY bookmark already does -- rather than getting run
 * through dns_resolve() (which only knows how to look up names, not
 * parse an address someone already handed us) or, worse, getting
 * treated as a search query because it doesn't contain a letter. */
static int web_parse_ipv4(const char *s, u32 *out_ip) {
    u32 octets[4] = {0, 0, 0, 0};
    int oi = 0, digits_in_octet = 0;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            octets[oi] = octets[oi] * 10 + (u32)(c - '0');
            digits_in_octet++;
            if (octets[oi] > 255 || digits_in_octet > 3) return 0;
        } else if (c == '.') {
            if (digits_in_octet == 0) return 0; /* ".." or leading dot */
            oi++;
            digits_in_octet = 0;
            if (oi > 3) return 0; /* more than 4 octets */
        } else {
            return 0; /* any letter, colon, slash, space, etc -- not a bare IPv4 literal */
        }
    }
    if (oi != 3 || digits_in_octet == 0) return 0; /* need exactly 4 octets, last one non-empty */
    *out_ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 1;
}

/* Decides whether what the person typed reads as a URL/hostname (goes
 * straight to that site) or as a search query (gets handed to DuckDuckGo
 * Lite instead) -- the same call every real browser's address bar makes,
 * just with a much smaller bag of tricks: no whitespace, and it either
 * contains a '.' (a dot with something on both sides -- "pypi.org",
 * "10.0.2.2") or starts with a scheme this kernel's client could plausibly
 * handle ("http://", "https://"). Anything else -- multiple words, a bare
 * word with no dot ("news"), a trailing-only or leading-only dot -- reads
 * as a search. This deliberately isn't a real URL grammar (no port
 * numbers, no userinfo, no percent-decoding of what's already in the
 * field): MiniWeb only ever calls http_get()/https_get() with a bare
 * host + "/" anyway, so anything this parser can't confidently call a
 * hostname is safer routed to search than guessed at. */
static int web_looks_like_url(const char *s) {
    int len = 0, dot_at = -1;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t') return 0; /* any whitespace at all -> definitely a search query */
        if (c == '.' && dot_at < 0 && i > 0 && s[i + 1] != 0) dot_at = i;
        len++;
    }
    if (len == 0) return 0;
    if (len >= 7 && s[0]=='h' && s[1]=='t' && s[2]=='t' && s[3]=='p') return 1; /* http:// or https:// */
    return dot_at >= 0;
}

/* Splits "host/path" (as typed in the URL bar, scheme already stripped)
 * into separate host and path buffers -- host gets truncated at the
 * first '/', path gets everything from that '/' onward, or just "/" if
 * there wasn't one. Doesn't decode percent-escapes or validate
 * anything; this is purely string-splitting, same "trust the network
 * stack to reject what it can't handle" philosophy as the rest of
 * MiniWeb's URL handling. */
/* Percent-encodes `src` into `out` the minimal amount an HTTP request
 * line actually needs: space -> '+' (the traditional query-string
 * convention, and what DuckDuckGo's own search box sends), and any
 * byte outside the safe printable-ASCII set -> %XX. Everything else
 * (letters, digits, and the handful of punctuation marks that are
 * always safe unescaped in a query value) passes through untouched, so
 * an ordinary search phrase stays readable in status text and server
 * logs instead of turning into a wall of %20-style noise. */
/* Returns 1 if the WHOLE of src was encoded, 0 if it did not fit (what is in `out` is then a cut-off query: the
 * caller must not search with it). */
static int web_urlencode(const char *src, char *out, u32 out_sz) {
    static const char hex[] = "0123456789ABCDEF";
    u32 oi = 0, i = 0;
    int complete = 1;
    for (; src[i] && oi + 1 < out_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (c == ' ') {
            out[oi++] = '+';
        } else if (safe) {
            out[oi++] = (char)c;
        } else if (oi + 3 < out_sz) {
            out[oi++] = '%';
            out[oi++] = hex[c >> 4];
            out[oi++] = hex[c & 0xF];
        } else {
            break; /* not enough room left for a full %XX escape -- stop
                     * cleanly rather than emit a truncated one */
        }
    }
    if (src[i]) complete = 0;
    out[oi] = 0;
    return complete;
}

/* Fires off whatever's currently typed in the URL bar: a URL/hostname
 * goes straight to that site over HTTPS (the sensible default for a
 * typed-in address in 2026, and this kernel's TLS stack can already
 * handle it -- see PYPI.ORG's bookmark for proof); anything else gets
 * handed to DuckDuckGo Lite as a search query instead. Either way this
 * is "web_go(), but the destination came from a text field the person
 * typed into instead of a fixed bookmark index" -- same
 * IDLE-state-clearing, same web_resolving/web_dns_failed bookkeeping,
 * just computing the host/path/transport from web_urlbar_buf first. */

#endif
