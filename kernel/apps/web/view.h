#ifndef MW_APPS_WEB_VIEW_H
#define MW_APPS_WEB_VIEW_H

/* apps/web/view.h -- WEB.MWP drawing: window chrome, address bar, page body, scrolling, clicks, form submit.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* Where the page body (text area + scrollbar) sits inside the window. One function so drawing,
 * scrolling and click hit-testing can never disagree about the geometry. */
static void web_body_geom(int *bx, int *by, int *bw, int *bh) {
    *bx = web_win.x + 3;
    *by = web_content_y() + FONT_CELL + 3;
    *bw = web_win.w - 6 - WEB_SB_W;
    *bh = web_win.y + web_win.h - *by - 3;
}
#define WEB_LINE_H (FONT_CELL + 1)             /* the failure screens still draw plain text lines */
#define WEB_SCROLL_STEP (3 * 14)                /* one arrow-key press: three body-text lines, in pixels */
static int web_view_h(void) {
    int bx, by, bw, bh;
    web_body_geom(&bx, &by, &bw, &bh);
    return bh < 1 ? 1 : bh;
}
static void web_scroll_by(int px) {
    int bh = web_view_h();
    int max = web_doc_h > bh ? web_doc_h - bh : 0;
    int sc = web_scroll_px + px;
    if (sc < 0) sc = 0;
    if (sc > max) sc = max;
    web_scroll_px = sc;
}
/* Keeps the layout in step with the window's width (the window can be resized or maximized). */
static void web_ensure_layout(void) {
    int bx, by, bw, bh;
    web_body_geom(&bx, &by, &bw, &bh);
    web_doc_h = rd_relayout(bw, bh, 0);
    web_scroll_by(0);        /* re-clamp */
}

/* ---- explaining a failed page load, in words ---- */
static const char *web_tls_fail_text(void) {
    switch (tls_conn.fail_reason) {
        case TLS_FAIL_TCP:                     return "The connection closed during the secure handshake.";
        case TLS_FAIL_UNEXPECTED_MESSAGE:      return "The server sent something unexpected during the handshake (protocol error).";
        case TLS_FAIL_UNSUPPORTED_CIPHER_SUITE:return "The server picked an encryption method MiniWin does not support.";
        case TLS_FAIL_UNSUPPORTED_CURVE:       return "The server's key-exchange curve is not supported (MiniWin: x25519, P-256, P-384).";
        case TLS_FAIL_CERT_PARSE:              return "The server's certificate could not be read.";
        case TLS_FAIL_CERT_CHAIN:              return "The certificate chain is broken, expired, or uses an unsupported signature.";
        case TLS_FAIL_CERT_UNTRUSTED:          return "The server's certificate is not signed by a root MiniWin trusts.";
        case TLS_FAIL_HOSTNAME_MISMATCH:       return "The certificate is for a different host name than the one requested.";
        case TLS_FAIL_SKE_SIGNATURE:           return "The server's key-exchange signature did not verify.";
        case TLS_FAIL_DECRYPT:                 return "A secure record failed its integrity check (data was corrupted).";
        case TLS_FAIL_SERVER_FINISHED:         return "The server's Finished message did not verify.";
        case TLS_FAIL_BUFFER_OVERFLOW:         return "A handshake message was larger than MiniWin can handle.";
        case TLS_FAIL_PEER_ALERT:              return "The server refused the connection with a TLS alert (see the number below).";
        case TLS_FAIL_CERT_EXTENSION:          return "The certificate has an extension MiniWin cannot safely handle, or its key usage does not allow this use.";
        case TLS_FAIL_BAD_RECORD:              return "The server sent a malformed or oversized secure record.";
        case TLS_FAIL_PROTOCOL_VERSION:        return "The server chose a TLS version MiniWin did not offer (MiniWin speaks TLS 1.2).";
        default:                               return "The secure connection failed.";
    }
}

/* Word-wraps `text` into the page area starting at *row; advances *row. */
static void web_draw_wrapped(int *row, const char *text, u32 color) {
    int bx, by, bw, bh;
    web_body_geom(&bx, &by, &bw, &bh);
    u32 cols = (u32)(bw / FONT_CELL);
    if (cols < 10) cols = 10;
    u32 n = 0; while (text[n]) n++;
    u32 pos = 0;
    while (pos < n && (*row + 1) * WEB_LINE_H <= bh) {
        u32 end = pos + cols;
        if (end >= n) end = n;
        else { u32 k = end; while (k > pos && text[k] != ' ') k--; if (k > pos) end = k; }
        char line[160]; u32 ll = 0;
        for (u32 i = pos; i < end && ll + 1 < sizeof(line); i++) line[ll++] = text[i];
        line[ll] = 0;
        font_draw_string(bx, by + *row * WEB_LINE_H, line, color);
        (*row)++;
        pos = end; while (pos < n && text[pos] == ' ') pos++;
    }
}

static void draw_web_failure(void) {
    int row = 0;
    char head[140]; u32 hl = 0;
    if (web_waiting_net) {
        web_draw_wrapped(&row, "This computer has no IP address yet, so the page cannot be requested.", TH_TEXT);
        row++;
        web_draw_wrapped(&row, "MiniWin is asking the network's DHCP server for one and will keep retrying; the page loads by itself as soon as the answer arrives.", TH_TEXT);
        if (dhcp_tries >= 2) {
            row++;
            web_draw_wrapped(&row, "Still no answer. Make sure the virtual machine's network adapter is connected and supported (e1000 or rtl8139), e.g. QEMU: -netdev user,id=n0 -device e1000,netdev=n0", TH_SHADOW);
        }
        return;
    }
    if (web_dns_failed) {
        append_str(head, &hl, sizeof(head), "Could not find \"");
        append_str(head, &hl, sizeof(head), web_last_host);
        append_str(head, &hl, sizeof(head), "\".");
        web_draw_wrapped(&row, head, TH_TEXT);
        row++;
        web_draw_wrapped(&row, dns_client.fail[0] ? dns_client.fail : "The DNS lookup failed.", TH_TEXT);
        row++;
        web_draw_wrapped(&row, "Check the spelling, and that the virtual machine has working network access (DNS comes from the DHCP server).", TH_SHADOW);
        return;
    }
    int https_failed = web_use_https && https_client.state == HTTPS_FAILED;
    int http_failed = !web_use_https && http_client.state == HTTP_FAILED;
    if (!https_failed && !http_failed) return;
    if (hr_size_fatal()) {
        /* Not a connection problem at all: the page arrived (or was announced) but is bigger than the response
         * buffer, and a cut-off page is worse than none. Say so instead of "could not connect". */
        append_str(head, &hl, sizeof(head), "The page from ");
        append_str(head, &hl, sizeof(head), web_last_host);
        append_str(head, &hl, sizeof(head), " is too large to display.");
        web_draw_wrapped(&row, head, TH_TEXT);
        row++;
        web_draw_wrapped(&row, "MiniWin keeps a page in a fixed-size buffer (256 KB). Showing only the first part would change what the page means, so nothing is shown.", TH_TEXT);
        return;
    }
    append_str(head, &hl, sizeof(head), https_failed ? "Could not open a secure connection to " : "Could not connect to ");
    append_str(head, &hl, sizeof(head), web_last_host);
    append_str(head, &hl, sizeof(head), ".");
    web_draw_wrapped(&row, head, TH_TEXT);
    row++;
    if (https_failed) {
        web_draw_wrapped(&row, web_tls_fail_text(), TH_TEXT);
        char code[64]; u32 cl = 0;
        append_str(code, &cl, sizeof(code), "(error ");
        char d[12]; u32 v = (u32)tls_conn.fail_reason; int nd = 0;
        do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 6);
        while (nd) { char one[2] = { d[--nd], 0 }; append_str(code, &cl, sizeof(code), one); }
        if (tls_conn.fail_reason == TLS_FAIL_PEER_ALERT) {
            append_str(code, &cl, sizeof(code), ", alert ");
            v = tls_conn.alert_desc; nd = 0;
            do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 6);
            while (nd) { char one[2] = { d[--nd], 0 }; append_str(code, &cl, sizeof(code), one); }
        }
        append_str(code, &cl, sizeof(code), ")");
        row++;
        web_draw_wrapped(&row, code, TH_SHADOW);
    } else {
        web_draw_wrapped(&row, "The server did not answer, or refused the connection.", TH_TEXT);
    }
}

/* Draws the rendered page into the window body (the engine clips to it), then the scrollbar. */
static void draw_web_page(void) {
    web_ensure_layout();
    int bx, by, bw, bh;
    web_body_geom(&bx, &by, &bw, &bh);
    rd_paint(bx, by, bw, bh, web_scroll_px);

    /* scrollbar: a track, and a thumb sized/placed by how much of the page is in view */
    int sx = web_win.x + web_win.w - 3 - WEB_SB_W;
    int total = web_doc_h > 0 ? web_doc_h : 1;
    int thumb_h = total <= bh ? bh : (bh * bh) / total;
    if (thumb_h < 10) thumb_h = 10;
    int travel = bh - thumb_h;
    int max_scroll = total > bh ? total - bh : 0;
    int thumb_y = by + (max_scroll > 0 ? (travel * web_scroll_px) / max_scroll : 0);
    ui_scrollbar(sx, by, WEB_SB_W, bh, thumb_y, thumb_h);
}

/* A form was submitted (a button clicked, or Enter pressed in a text field): build the query string and
 * GET (or POST) the form's action. */
#define WEB_QUERY_MAX 480
static void web_submit_form(u32 form, u32 submitter) {
    char q[WEB_QUERY_MAX], method[8], action[WEB_PATH_MAX], ref[WEB_PATH_MAX];
    int qlen = rd_form_query(form, submitter, q, sizeof(q));
    web_attr_copy(form, AT_METHOD, method, sizeof(method));
    web_attr_copy(form, AT_ACTION, action, sizeof(action));
    int post = (method[0] == 'p' || method[0] == 'P');
    if (action[0] == 0) kstrcpy(action, web_last_path, sizeof(action));       /* no action="": the page itself */
    for (u32 i = 0; action[i]; i++) if (action[i] == '?' || action[i] == '#') { action[i] = 0; break; }
    /* Every stage below used to cut the data off silently (the encoder at WEB_QUERY_MAX, the request path at
     * WEB_PATH_MAX, the POST body at HR_POST_MAX) and send what was left as if it were the whole form. A form
     * that does not fit is not sent at all, and the user is told. */
    u32 alen = 0; while (action[alen]) alen++;
    if (rd_query_overflow || (post ? (u32)qlen >= HR_POST_MAX : alen + 1 + (u32)qlen + 1 > sizeof(ref))) {
        status = t(STR_FORM_TOO_LONG);
        return;
    }
    u32 rl = 0; ref[0] = 0;
    append_str(ref, &rl, sizeof(ref), action);
    if (!post) { append_str(ref, &rl, sizeof(ref), "?"); append_str(ref, &rl, sizeof(ref), q); }
    web_navigate_ref(ref);
    if (post) hr_set_post(q);                    /* armed AFTER web_start_fetch() disarmed it; the request is built later */
}

/* A mouse click somewhere in the page area: on the scrollbar it scrolls; on the page it follows a link,
 * pokes a form control, or opens/closes a <details>. Returns 1 if the click landed in the body region at all
 * (so the caller doesn't treat it as a click on nothing). */
static int web_page_click(int mx, int my) {
    int bx, by, bw, bh;
    web_body_geom(&bx, &by, &bw, &bh);
    if (!in_rect(mx, my, bx, by, bw + WEB_SB_W, bh)) return 0;
    if (mx >= bx + bw) {                              /* the scrollbar: a click jumps to that proportional spot */
        int max_scroll = web_doc_h > bh ? web_doc_h - bh : 0;
        if (max_scroll > 0) {
            int target = ((my - by) * max_scroll) / (bh > 0 ? bh : 1);
            web_scroll_px = 0; web_scroll_by(target);
        }
        return 1;
    }
    if (!web_page_ready) return 1;
    u32 node;
    int hit = rd_hit(mx - bx, my - by + web_scroll_px, &node);
    rd_focus = 0;
    if (hit == HIT_LINK) {
        char href[WEB_PATH_MAX];
        web_attr_copy(node, AT_HREF, href, sizeof(href));
        if (href[0]) web_navigate_ref(href);
    } else if (hit == HIT_TOGGLE) {
        u32 d = RD_NODES[node].parent;                /* the <details> that owns this <summary> */
        if (d) { rd_toggle_flip(d); rd_invalidate(); }
    } else if (hit == HIT_CTL) {
        rd_node_t *e = &RD_NODES[node];
        if (e->tag == TG_SELECT) { rd_select_next(node); }
        else if (e->tag == TG_TEXTAREA) { rd_ctl_focus(node); }
        else if (e->tag == TG_INPUT || e->tag == TG_BUTTON) {
            int k = e->tag == TG_INPUT ? lay_input_kind(node) : CTL_BUTTON;
            u32 tl; const u8 *ty = dom_attr(node, AT_TYPE, &tl);
            if (k == CTL_TEXT || k == CTL_PASSWORD) rd_ctl_focus(node);
            else if (k == CTL_CHECK) rd_ctl_set_checked(node, !rd_ctl_is_checked(node));
            else if (k == CTL_RADIO) rd_radio_select(node);
            else {                                    /* a button: submit unless it says otherwise */
                int plain = ty && (css_kw(ty, tl, "button") || css_kw(ty, tl, "reset"));
                u32 f = rd_find_form(node);
                if (f && !plain) web_submit_form(f, node);
            }
        }
    }
    return 1;
}

static void draw_web_window(void) {
    int wx = web_win.x, wy = web_win.y, ww = web_win.w, wh = web_win.h;

    ui_window_frame(wx, wy, ww, wh, web_win.maximized, "WEB.MWP");
    ui_titlebar_buttons(web_btn_min_x(), web_btn_max_x(), web_btn_close_x(), web_btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == WIN_ID_WEB,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == WIN_ID_WEB,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == WIN_ID_WEB);

    /* URL bar: a sunken text field with a GO button, sitting above the
     * bookmark rows -- see web_urlbar_* geometry helpers above. Sunken
     * bevel (dark-gray top/left, white bottom/right) reads as "typeable"
     * the same way Notepad's edit area does; focused state gets a blue
     * outline instead of the default black one so it's obvious which
     * window element keystrokes are about to land in. */
    {
        int ux = web_urlbar_x(), uy = web_urlbar_y(), uw = web_urlbar_w();
        ui_field(ux, uy, uw, WEB_URLBAR_H, TH_FIELD, web_urlbar_focused);
        font_draw_string(ux + 3, uy + 3, web_urlbar_buf, TH_FIELD_TEXT);
        /* text cursor: only while focused, so it doesn't look like the
         * field is mid-edit when nobody's clicked into it */
        if (web_urlbar_focused) {
            bb_fillrect(ux + 3 + web_urlbar_len * FONT_CELL, uy + 3, 2, FONT_CELL, TH_FIELD_TEXT);
        }

        int gx = web_go_btn_x();
        int go_pressed = (pressed_btn_kind == BTN_WEB_GO);
        ui_button(gx, uy, WEB_GO_BTN_W, WEB_URLBAR_H, "GO", 4, 3, go_pressed);
    }

    /* two clickable site rows -- the closest thing this browser has to
     * bookmarks, now that there's also a real address bar to type into */
    const char *site_labels[WEB_SITE_COUNT] = { "> PYPI.ORG (HTTPS)", "> GATEWAY (10.0.2.2)" };
    for (int i = 0; i < WEB_SITE_COUNT; i++) {
        int ry = web_site_row_y(i);
        int active = (web_current_site == i);
        u32 fg = ui_item(wx + 2, ry, ww - 4, WEB_SITE_ROW_H, active);
        font_draw_string(wx + 4, ry + 1, site_labels[i], fg);
    }

    /* divider between the site rows and the response content */
    int div_y = web_content_y() - 2;
    ui_hline(wx + 2, div_y, ww - 4, TH_SHADOW);

    /* status line: what's currently happening, in plain language rather
     * than exposing the raw http_state_t/https_state_t/tls_fail_reason_t
     * enums to whoever's looking. Names whatever's actually being
     * fetched (web_last_host) instead of a hardcoded "pypi.org" now that
     * a fetch might have come from the URL bar or a search instead of
     * either bookmark. */
    const char *status_text;
    if (web_current_site == -1) {
        /* -1: nothing requested yet this boot (the initial value).
         * WEB_SOURCE_URLBAR (-2) and the two WEB_SITE_* bookmark
         * indices (0, 1) all mean "something's been requested" and
         * fall through to the resolving/connecting states below. */
        status_text = "Type a URL or search, then GO.";
    } else if (web_waiting_net) {
        status_text = "Waiting for an IP address (DHCP)...";
    } else if (web_resolving) {
        char buf[128]; u32 blen = 0;
        append_str(buf, &blen, sizeof(buf), "Resolving ");
        append_str(buf, &blen, sizeof(buf), web_last_host);
        append_str(buf, &blen, sizeof(buf), "...");
        ko_draw_mixed_string(wx + 4, web_content_y(), buf, TH_TEXT);
        status_text = 0;
    } else if (web_dns_failed) {
        status_text = "DNS lookup failed.";
    } else if (web_use_https) {
        switch (https_client.state) {
            case HTTPS_CONNECTING:        status_text = "TLS handshake..."; break;
            case HTTPS_SENDING_REQUEST:   status_text = "Sending request..."; break;
            case HTTPS_AWAITING_RESPONSE: status_text = "Waiting for response..."; break;
            case HTTPS_DONE:              status_text = !web_page_ready ? "Rendering..." : rd_page_incomplete() ? "Page too big - only part shown." : "Done. (HTTPS)"; break;
            case HTTPS_FAILED:            status_text = hr_size_fatal() ? "Page too large." : "TLS/HTTPS failed."; break;
            default:                      status_text = ""; break;
        }
    } else {
        switch (http_client.state) {
            case HTTP_CONNECTING:        status_text = "Connecting..."; break;
            case HTTP_SENDING_REQUEST:   status_text = "Sending request..."; break;
            case HTTP_AWAITING_RESPONSE: status_text = "Waiting for response..."; break;
            case HTTP_DONE:              status_text = rd_page_incomplete() ? "Page too big - only part shown." : "Done."; break;
            case HTTP_FAILED:            status_text = hr_size_fatal() ? "Page too large." : "Failed to connect."; break;
            default:                     status_text = ""; break;
        }
    }
    if (status_text) font_draw_string(wx + 3, web_content_y(), status_text, TH_ACCENT);
    if (status_text && web_page_ready && web_title[0]) {
        /* the page's <title>, after the status, clipped to the window's width */
        u32 sl = 0; while (status_text[sl]) sl++;
        int room = (ww - 6) / FONT_CELL - (int)sl - 2;
        if (room > 3) {
            char t[100]; u32 tl = 0;
            while (web_title[tl] && (int)tl < room && tl + 1 < sizeof(t)) { t[tl] = web_title[tl]; tl++; }
            t[tl] = 0;
            font_draw_string(wx + 3 + (int)(sl + 2) * FONT_CELL, web_content_y(), t, TH_TEXT);
        }
    }

    /* response body (or nothing yet, or the failure already explained
     * by the status line above) */
    if (web_page_ready && !web_resolving && !web_dns_failed) draw_web_page();
    else if (!web_resolving) draw_web_failure();   /* a DNS / connection / TLS failure explains itself */
}

#endif
