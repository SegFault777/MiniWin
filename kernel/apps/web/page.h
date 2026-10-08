#ifndef MW_APPS_WEB_PAGE_H
#define MW_APPS_WEB_PAGE_H

/* apps/web/page.h -- the finished response -> page: content-type handling, redirects, link queries, polling.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* copies an attribute into a C string (empty if absent) */
static void web_attr_copy(u32 node, u32 id, char *out, u32 cap) {
    u32 len; const u8 *v = dom_attr(node, id, &len);
    u32 i = 0;
    if (v) for (; i < len && i + 1 < cap; i++) out[i] = (char)v[i];
    out[i] = 0;
}

#ifdef MW_AUTOTEST_URL
/* ---- little queries over the parsed page (the self-test and link following use them) ---- */
static int web_node_is_link(u32 n) { return RD_NODES[n].kind == RDK_ELEM && RD_NODES[n].tag == TG_A && dom_has_attr(n, AT_HREF); }
static u32 web_link_count(void) {
    u32 c = 0;
    for (u32 n = 1; n < dom_node_count; n++) if (web_node_is_link(n)) c++;
    return c;
}
static const char *web_nth_href(u32 nth) {
    static char buf[WEB_PATH_MAX];
    buf[0] = 0;
    for (u32 n = 1; n < dom_node_count; n++) {
        if (!web_node_is_link(n)) continue;
        if (nth-- == 0) { web_attr_copy(n, AT_HREF, buf, sizeof(buf)); break; }
    }
    return buf;
}
static u32 web_text_bytes(void) {
    rd_relayout(620, 400, 1);                       /* (the real window re-lays out on its next draw) */
    u32 t = 0;
    for (u32 i = 0; i < L.n; i++) if (RD_ITEMS[i].kind == DL_TEXT) t += RD_ITEMS[i].len;
    rd_invalidate();
    return t;
}
#endif /* MW_AUTOTEST_URL */

/* The response has fully arrived: follow a redirect, or turn the body into the page to show. */
static void web_page_complete(void) {
    web_page_ready = 1;
    if (hr_is_redirect() && web_redirects_left > 0) {
        int https; u16 port;
        char host[WEB_URLBAR_MAXLEN + 1], path[WEB_PATH_MAX];
        if (web_resolve_ref(hr.location, &https, host, sizeof(host), &port, path, sizeof(path))) {
            web_redirects_left--;
            serial_puts("[WEB] redirect ("); serial_put_dec((u32)hr.status); serial_puts(") -> ");
            serial_puts(host); serial_puts(path); serial_putc('\n');
            web_start_fetch(https, host, port, path);
            return;
        }
    }
    const u8 *body = HR_BUF + hr.body_start;
    u32 n = hr.body_len;
    if (hr.encoded) {
        rd_load_message("Page not shown", "This page is compressed (Content-Encoding) and MiniWeb cannot decompress it.", "");
    } else if (hr_is_redirect()) {
        rd_load_message("Too many redirects.", "", "");
    } else if (hr.content_type[0] == 0 ? (n > 0 && body[0] == '<') : web_ct_has(hr.content_type, "html")) {
        rd_load_html(body, n);
    } else if (hr.content_type[0] == 0 || web_ct_has(hr.content_type, "text/") ||
               web_ct_has(hr.content_type, "json") || web_ct_has(hr.content_type, "xml")) {
        rd_load_plain(body, n);
    } else {
        rd_load_message("MiniWeb can't display this kind of content.", "", "");
    }
    if (n == 0 && !hr.encoded) {
        char m[48]; u32 ml = 0;
        append_str(m, &ml, sizeof(m), "(empty response, HTTP ");
        char d[8]; u32 st = (u32)hr.status; int nd = 0;
        do { d[nd++] = (char)('0' + st % 10); st /= 10; } while (st && nd < 6);
        while (nd) { char one[2] = { d[--nd], 0 }; append_str(m, &ml, sizeof(m), one); }
        append_str(m, &ml, sizeof(m), ")");
        rd_load_message(m, "", "");
    }
    dom_title(web_title, sizeof(web_title));
    web_scroll_px = 0;
    rd_invalidate();        /* force a fresh layout at the current window width */
}

static void web_poll(void) {
    if (web_waiting_net) {
        if (!net_cfg.ready) return;            /* still no lease; dhcp_poll() keeps retrying in the background */
        web_waiting_net = 0;
        web_begin_transport();
        return;
    }
    if (web_resolving) {
        if (dns_client.state == DNS_RESOLVED) {
            web_resolving = 0;
            web_connect(dns_client.result_ip);
        } else if (dns_client.state == DNS_FAILED) {
            web_resolving = 0;
            web_dns_failed = 1;
        }
        return; /* don't also poll HTTP/HTTPS this same tick -- there's nothing to poll yet */
    }
    if (web_use_https) {
        if (https_client.state != HTTPS_IDLE &&
            https_client.state != HTTPS_DONE && https_client.state != HTTPS_FAILED) {
            https_poll(web_now_packed());
        }
        if (https_client.state == HTTPS_DONE && !web_page_ready) web_page_complete();
    } else {
        if (http_client.state != HTTP_IDLE &&
            http_client.state != HTTP_DONE && http_client.state != HTTP_FAILED) {
            http_poll();
        }
        if (http_client.state == HTTP_DONE && !web_page_ready) web_page_complete();
    }
}

#endif
