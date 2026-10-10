#ifndef MW_APPS_WEB_FETCH_H
#define MW_APPS_WEB_FETCH_H

/* apps/web/fetch.h -- starting a fetch: DNS, connect, HTTP/HTTPS transport selection, redirects.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

static void web_connect(u32 ip) {
    if (web_use_https) {
        u8 seed[16];
        web_make_entropy_seed(seed);
        https_get(ip, web_last_port, web_last_host, web_last_path, seed);
    } else {
        http_get(ip, web_last_port, web_last_host, web_last_path);
    }
}

static void web_begin_transport(void) {
    u32 ip;
    if (web_parse_ipv4(web_last_host, &ip)) {
        web_connect(ip);                       /* an IP literal has nothing to resolve */
    } else {
        web_resolving = 1;
        dns_resolve(web_last_host);
    }
}

/* The ONE place a page load starts -- from the URL bar, a bookmark, a clicked link, or a redirect. */
static void web_start_fetch(int https, const char *host, u16 port, const char *path) {
    web_dns_failed = 0;
    web_resolving = 0;
    web_waiting_net = 0;
    web_page_ready = 0;
    http_client.state = HTTP_IDLE;
    https_client.state = HTTPS_IDLE;
    web_use_https = https;
    web_last_port = port ? port : (u16)(https ? 443 : 80);
    kstrcpy(web_last_host, host, sizeof(web_last_host));
    kstrcpy(web_last_path, path, sizeof(web_last_path));
    rd_load_message("", "", "");
    web_title[0] = 0;
    web_scroll_px = 0;
    hr_post_active = 0;             /* a POST is a one-shot: whatever starts next is a plain GET unless the form code re-arms it */

    if (!net_cfg.ready) {
        /* No IP address yet (DHCP hasn't been answered). Don't fire a DNS query from 0.0.0.0 at a
         * server that doesn't exist -- wait for the lease; web_poll() starts the load when it arrives. */
        web_waiting_net = 1;
        return;
    }
    web_begin_transport();
}

/* Follows a link (or anything else carrying an href) from the current page. */
static void web_navigate_ref(const char *ref) {
    int https; u16 port;
    char host[WEB_URLBAR_MAXLEN + 1], path[WEB_PATH_MAX];
    if (!web_resolve_ref(ref, &https, host, sizeof(host), &port, path, sizeof(path))) return;
    {   /* show where we are going in the address bar (what a browser does when you click a link) */
        u32 n = 0; web_urlbar_buf[0] = 0;
        append_str(web_urlbar_buf, &n, sizeof(web_urlbar_buf), https ? "https://" : "http://");
        append_str(web_urlbar_buf, &n, sizeof(web_urlbar_buf), host);
        if (port && port != (https ? 443 : 80)) {
            char d[8]; int nd = 0; u32 v = port;
            do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 6);
            if (n + 1 < sizeof(web_urlbar_buf)) web_urlbar_buf[n++] = ':';
            while (nd && n + 1 < sizeof(web_urlbar_buf)) web_urlbar_buf[n++] = d[--nd];
            web_urlbar_buf[n] = 0;
        }
        append_str(web_urlbar_buf, &n, sizeof(web_urlbar_buf), path);
        web_urlbar_len = n;
    }
    web_current_site = WEB_SOURCE_URLBAR;
    web_redirects_left = 5;
    web_start_fetch(https, host, port, path);
}

static void web_go_url(void) {
    if (web_urlbar_len == 0) return; /* nothing typed, nothing to do */

    web_current_site = WEB_SOURCE_URLBAR;
    web_redirects_left = 5;

    if (web_looks_like_url(web_urlbar_buf)) {
        /* No scheme means HTTPS (nearly every real site is HTTPS-only now); an explicit one is honored. */
        const char *rest = web_urlbar_buf;
        int want_https = 1;
        if (rest[0]=='h'&&rest[1]=='t'&&rest[2]=='t'&&rest[3]=='p'&&rest[4]=='s'&&rest[5]==':'&&rest[6]=='/'&&rest[7]=='/') rest += 8;
        else if (rest[0]=='h'&&rest[1]=='t'&&rest[2]=='t'&&rest[3]=='p'&&rest[4]==':'&&rest[5]=='/'&&rest[6]=='/') { rest += 7; want_https = 0; }

        char host[WEB_URLBAR_MAXLEN + 1], path[WEB_PATH_MAX];
        u16 port = 0;
        /* split at the first '/', '?' or '#' (so "example.com?x=1" works too) */
        u32 hi = 0;
        while (rest[hi] && rest[hi] != '/' && rest[hi] != '?' && rest[hi] != '#' && hi < sizeof(host) - 1) { host[hi] = rest[hi]; hi++; }
        host[hi] = 0;
        web_split_port(host, &port);
        const char *tail = rest + hi;
        if (tail[0] == 0 || tail[0] == '#') kstrcpy(path, "/", sizeof(path));
        else if (tail[0] == '?') { kstrcpy(path, "/", sizeof(path)); u32 pl = 1; append_str(path, &pl, sizeof(path), tail); }
        else kstrcpy(path, tail, sizeof(path));
        web_start_fetch(want_https, host, port, path);
    } else {
        char encoded[WEB_PATH_MAX];
        char path[WEB_PATH_MAX];
        kstrcpy(path, "/lite/?q=", sizeof(path));
        u32 path_len = 9; /* strlen("/lite/?q=") -- append_str() needs the current length */
        /* the encoded text has to fit what is LEFT of the request path, not just its own buffer */
        if (!web_urlencode(web_urlbar_buf, encoded, sizeof(path) - path_len)) {
            status = t(STR_SEARCH_TOO_LONG);       /* never search for a cut-off phrase */
            return;
        }
        append_str(path, &path_len, sizeof(path), encoded);
        web_start_fetch(1, "lite.duckduckgo.com", 443, path);
    }
}

static int web_ct_has(const char *ct, const char *word) {
    u32 wl = 0; while (word[wl]) wl++;
    for (u32 i = 0; ct[i]; i++) {
        u32 k = 0;
        while (k < wl && ct[i + k] && ((ct[i + k] >= 'A' && ct[i + k] <= 'Z') ? ct[i + k] + 32 : ct[i + k]) == word[k]) k++;
        if (k == wl) return 1;
    }
    return 0;
}

#endif
