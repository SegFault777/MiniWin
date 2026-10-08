#ifndef MW_APPS_WEB_ADDRESS_H
#define MW_APPS_WEB_ADDRESS_H

/* apps/web/address.h -- turning typed text, redirects and clicked links into (scheme, host, port, path).
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ---- addresses: turning typed text, redirects and clicked links into (scheme, host, port, path) ---- */

static int web_is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* "host:8080" -> host "host", port 8080 (port left at 0 if absent). Also drops any "user@" prefix. */
static void web_split_port(char *host, u16 *port) {
    for (u32 i = 0; host[i]; i++) {
        if (host[i] == '@') { u32 k = 0; for (u32 m = i + 1; host[m]; m++) host[k++] = host[m]; host[k] = 0; break; }
    }
    int colon = -1;
    for (int i = 0; host[i]; i++) if (host[i] == ':') colon = i;
    if (colon < 0) return;
    u32 v = 0; int nd = 0;
    for (int i = colon + 1; host[i]; i++) {
        if (host[i] < '0' || host[i] > '9') return;          /* not a port: leave it alone */
        v = v * 10 + (u32)(host[i] - '0'); nd++;
        if (v > 65535) return;
    }
    if (nd == 0) return;
    host[colon] = 0;
    *port = (u16)v;
}

/* Resolves `ref` against the page currently shown, the way a browser resolves a link or a Location:
 * header. Handles absolute URLs, scheme-relative ("//host/path" -- exactly what DuckDuckGo Lite's
 * result links look like), absolute paths, ?query-only and plain relative paths. Returns 0 for
 * anything that isn't HTTP(S) (mailto:, javascript:, ...). */
static int web_resolve_ref(const char *ref, int *https, char *host, u32 host_sz, u16 *port, char *path, u32 path_sz) {
    *https = web_use_https; *port = 0;
    kstrcpy(host, web_last_host, host_sz);
    int have_port = 0;
    u16 cur_port = web_last_port;
    const char *rest = ref;
    int authority = 0;

    if (rest[0]=='h'&&rest[1]=='t'&&rest[2]=='t'&&rest[3]=='p'&&rest[4]=='s'&&rest[5]==':'&&rest[6]=='/'&&rest[7]=='/') { rest += 8; *https = 1; authority = 1; }
    else if (rest[0]=='h'&&rest[1]=='t'&&rest[2]=='t'&&rest[3]=='p'&&rest[4]==':'&&rest[5]=='/'&&rest[6]=='/') { rest += 7; *https = 0; authority = 1; }
    else if (rest[0]=='/' && rest[1]=='/') { rest += 2; authority = 1; }
    else {
        for (int i = 0; ref[i] && ref[i] != '/' && ref[i] != '?' && ref[i] != '#'; i++)
            if (ref[i] == ':' && i > 0 && web_is_alpha(ref[0])) {
                /* "scheme:" that isn't http(s) -- but "host:8080/x" also has a colon; a scheme is all letters */
                int all_alpha = 1;
                for (int k = 0; k < i; k++) if (!web_is_alpha(ref[k])) all_alpha = 0;
                if (all_alpha && !(ref[i+1] >= '0' && ref[i+1] <= '9')) return 0;
            }
    }

    char tmp_path[WEB_PATH_MAX];
    tmp_path[0] = 0;
    if (authority) {
        u32 hi = 0;
        while (rest[hi] && rest[hi] != '/' && rest[hi] != '?' && rest[hi] != '#' && hi < host_sz - 1) { host[hi] = rest[hi]; hi++; }
        host[hi] = 0;
        web_split_port(host, port);
        have_port = (*port != 0);
        rest += hi;
        if (rest[0] == 0 || rest[0] == '#') kstrcpy(tmp_path, "/", sizeof(tmp_path));
        else if (rest[0] == '?') { kstrcpy(tmp_path, "/", sizeof(tmp_path)); u32 pl = 1; append_str(tmp_path, &pl, sizeof(tmp_path), rest); }
        else kstrcpy(tmp_path, rest, sizeof(tmp_path));
        if (host[0] == 0) return 0;
    } else if (ref[0] == '/') {
        kstrcpy(tmp_path, ref, sizeof(tmp_path));
    } else if (ref[0] == '?') {
        kstrcpy(tmp_path, web_last_path, sizeof(tmp_path));
        for (u32 i = 0; tmp_path[i]; i++) if (tmp_path[i] == '?') { tmp_path[i] = 0; break; }
        u32 pl = 0; while (tmp_path[pl]) pl++;
        append_str(tmp_path, &pl, sizeof(tmp_path), ref);
    } else if (ref[0] == '#' || ref[0] == 0) {
        kstrcpy(tmp_path, web_last_path, sizeof(tmp_path));
    } else {
        kstrcpy(tmp_path, web_last_path, sizeof(tmp_path));
        int slash = -1;
        for (int i = 0; tmp_path[i] && tmp_path[i] != '?'; i++) if (tmp_path[i] == '/') slash = i;
        tmp_path[slash + 1] = 0;
        u32 pl = (u32)(slash + 1);
        append_str(tmp_path, &pl, sizeof(tmp_path), ref);
    }
    for (u32 i = 0; tmp_path[i]; i++) if (tmp_path[i] == '#') { tmp_path[i] = 0; break; }   /* a fragment never goes on the wire */
    if (tmp_path[0] == 0) kstrcpy(tmp_path, "/", sizeof(tmp_path));
    kstrcpy(path, tmp_path, path_sz);

    if (!have_port) {
        /* the same host over the same scheme keeps the current port; anything else gets its scheme's default */
        int same_host = 1;
        for (u32 i = 0; ; i++) { if (host[i] != web_last_host[i]) { same_host = 0; break; } if (!host[i]) break; }
        *port = (same_host && *https == web_use_https && cur_port) ? cur_port : (u16)(*https ? 443 : 80);
    }
    return 1;
}

#endif
