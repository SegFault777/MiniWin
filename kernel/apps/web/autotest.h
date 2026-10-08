#ifndef MW_APPS_WEB_AUTOTEST_H
#define MW_APPS_WEB_AUTOTEST_H

/* apps/web/autotest.h -- the -DMW_AUTOTEST_URL test hook (compiled out of normal builds).
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ---- TEST HOOK (compiled ONLY when built with -DMW_AUTOTEST_URL="..."; a
 * normal build contains none of this) ----
 * Drives MiniWeb without a human: once DHCP has finished and the network
 * has had a moment to settle, it "types" MW_AUTOTEST_URL into the URL bar,
 * presses Enter, then reports the outcome over the serial port and
 * (optionally) powers the VM off, so a shell script can run a whole
 * handshake test headlessly. QEMU's mouse-driven GUI on a 1-core host is
 * too flaky to test a protocol stack through; serial output is not. */
#ifdef MW_AUTOTEST_URL
static int mw_at_state = 0;
static u32 mw_at_settle = 0;
static u32 mw_at_started_tick = 0;

static void mw_autotest_step(void) {
    if (mw_at_state == 0) {
#ifndef MW_AUTOTEST_EARLY
        if (!net_cfg.ready) return;
#endif
        if (++mw_at_settle < 300) return;           /* let ARP/DNS settle for a moment */
        kstrcpy(web_urlbar_buf, MW_AUTOTEST_URL, sizeof(web_urlbar_buf));
        web_urlbar_len = 0;
        while (web_urlbar_buf[web_urlbar_len]) web_urlbar_len++;
#ifdef MW_AUTOTEST_DNS_OVERRIDE
        net_cfg.dns_ip = MW_AUTOTEST_DNS_OVERRIDE;   /* pretend DHCP handed out THIS DNS server (e.g. a dead one) */
#endif
        serial_puts("[AUTOTEST] go " MW_AUTOTEST_URL "\n");
        mw_at_started_tick = net_ticks;
        web_go_url();
        mw_at_state = 1;
        return;
    }
    if (mw_at_state != 1) return;

    /* a heartbeat while waiting, so a stall can be told apart from "just slow" in the serial log */
    {
        static u32 beat_ctr = 0;
        if (++beat_ctr % 100 == 0) {
            serial_puts("[AUTOTEST] .. tcp="); serial_put_dec((u32)tcp_conn.state);
            serial_puts(" hr.len="); serial_put_dec(hr.len);
            serial_puts(" recv_len="); serial_put_dec(tcp_conn.recv_len);
            serial_puts(" fin="); serial_put_dec((u32)tcp_conn.peer_fin_seen);
            serial_puts(" ticks="); serial_put_dec(net_ticks);
            serial_putc('\n');
        }
    }

    /* A redirect restarts the fetch (state goes back to connecting), so "done" really means the FINAL page. */
    if (web_dns_failed) {
        serial_puts("[AUTOTEST] RESULT dns-failed: ");
        serial_puts(dns_client.fail);
        serial_putc('\n');
        mw_at_state = 2;
    } else if (web_use_https) {
        if (https_client.state == HTTPS_DONE && web_page_ready) {
            serial_puts("[AUTOTEST] RESULT https-ok\n");
            mw_at_state = 2;
        } else if (https_client.state == HTTPS_FAILED) {
            serial_puts("[AUTOTEST] RESULT https-failed reason=");
            serial_put_dec((u32)tls_conn.fail_reason);
            serial_puts("\n");
            mw_at_state = 2;
        }
    } else {
        if (http_client.state == HTTP_DONE && web_page_ready) {
            serial_puts("[AUTOTEST] RESULT http-ok\n");
            mw_at_state = 2;
        } else if (http_client.state == HTTP_FAILED) {
            serial_puts("[AUTOTEST] RESULT http-failed\n");
            mw_at_state = 2;
        }
    }
    if (mw_at_state == 1 && net_ticks - mw_at_started_tick > 800000u) {
        serial_puts("[AUTOTEST] RESULT timeout\n");
        mw_at_state = 2;
    }
    if (mw_at_state == 2) {
        serial_puts("[AUTOTEST] status="); serial_put_dec((u32)hr.status);
        serial_puts(" raw-bytes="); serial_put_dec(hr.len);
        serial_puts(" body-bytes="); serial_put_dec(hr.body_len);
        serial_puts(" truncated="); serial_put_dec((u32)hr.truncated);
        serial_puts(" chunked="); serial_put_dec((u32)hr.chunked);
        serial_puts(" host="); serial_puts(web_last_host);
        serial_puts(" path="); serial_puts(web_last_path);
        {   /* a rolling checksum of the decoded body, so a test can prove the bytes are EXACTLY right */
            u32 h = 0;
            for (u32 i = 0; i < hr.body_len; i++) h = h * 31u + HR_BUF[hr.body_start + i];
            serial_puts("\n[AUTOTEST] body-hash=0x"); serial_put_hex32(h);
        }
        serial_puts("\n[AUTOTEST] title=["); serial_puts(web_title);
        serial_puts("] links="); serial_put_dec(web_link_count());
        serial_puts(" text-bytes="); serial_put_dec(web_text_bytes());
        serial_puts(" doc-h="); serial_put_dec((u32)L.doc_h); serial_puts(" items="); serial_put_dec(L.n);
        serial_puts("\n");
#ifdef MW_AUTOTEST_DUMP
        serial_puts("[AUTOTEST] text-begin\n");
        {   /* the page's text runs in reading order, one line per distinct y */
            int last_y = -1; u32 shown = 0;
            for (u32 i = 0; i < L.n && shown < MW_AUTOTEST_DUMP; i++) {
                if (RD_ITEMS[i].kind != DL_TEXT) continue;
                if (RD_ITEMS[i].y != last_y) { serial_putc('\n'); last_y = RD_ITEMS[i].y; shown++; } else { serial_putc(' '); shown++; }
                for (u32 k = 0; k < RD_ITEMS[i].len; k++) {
                    u8 c = RD_POOL[RD_ITEMS[i].off + k];
                    serial_putc((c >= 32 && c < 127) ? (char)c : (dom_is_ws(c) ? ' ' : '.')); shown++;
                }
            }
        }
        serial_puts("\n[AUTOTEST] text-end\n");
#endif
        serial_puts("[AUTOTEST] finished\n");
#ifdef MW_AUTOTEST_FOLLOW
        /* then "click" link number MW_AUTOTEST_FOLLOW on the page, exactly as a mouse click would, and
         * report the page that loads (a link on DuckDuckGo Lite goes through a redirector first) */
        static int followed = 0;
        if (!followed && (int)web_link_count() > MW_AUTOTEST_FOLLOW) {
            followed = 1;
            serial_puts("[AUTOTEST] follow link ");
            serial_put_dec(MW_AUTOTEST_FOLLOW);
            serial_puts(" -> ");
            serial_puts(web_nth_href(MW_AUTOTEST_FOLLOW));
            serial_putc('\n');
            web_navigate_ref(web_nth_href(MW_AUTOTEST_FOLLOW));
            mw_at_state = 1;
            mw_at_started_tick = net_ticks;
        }
#endif
    }
}
#endif

#endif
