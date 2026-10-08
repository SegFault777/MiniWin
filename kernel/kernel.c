/* kernel.c -- MiniWin's kernel: pulls the layers together and runs the main loop.
 *
 * The whole kernel is ONE translation unit (every module is a header of `static` code), so the order of the
 * includes below IS the dependency order: a module may use anything included above it and nothing below.
 * That order is the architecture; docs/ARCHITECTURE.md explains each layer and the rules between them.
 *
 *   hardware & protocols   io vga font keyboard mouse ata fs speaker rtc serial pci  nic rtl8139 e1000
 *   (flat headers in       net arp ip icmp udp dns dhcp tcp tls http https           net_stack mwp
 *    kernel/)              dom css layout render  <- the HTML5 engine used by the web app
 *   design                 ui/theme.h (+ ui/themes/*.h)  ui/widgets.h
 *   system                 sys/util  sys/lang  sys/power  sys/boot
 *   shell                  ui/icons  clock  window  taskbar  startmenu  desktop_files  resize  cursor  compose  input
 *   apps                   apps/notepad/*  apps/setting  apps/web/*  apps/terminal
 *
 * kmain() is only boot + loop: kernel_boot() (sys/boot.h), then forever { handle_mouse(); handle_keyboard();
 * poll the network and the web fetch; render_frame(); }.
 */
#include "io.h"
#include "vga.h"
#include "font.h"
#include "keyboard.h"
#include "mouse.h"
#include "ata.h"
#include "fs.h"
#include "font_ko.h"
#include "hangul_ime.h"
#include "speaker.h"
#include "rtc.h"
#include "serial.h"
#include "pci.h"
#include "nic.h"
#include "rtl8139.h"
#include "e1000.h"
#include "net.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "dns.h"
#include "dhcp.h"
#include "tcp.h"
#include "tls.h"
#include "http.h"
#include "https.h"
#include "render.h"        /* the HTML5 engine: dom.h + css.h + layout.h + painter */
#include "net_stack.h"
#include "mwp.h"

/* ---- design: color roles + metrics, and every reusable piece of chrome ---- */
#include "ui/theme.h"      /* pick a look with  THEME=name ./build.sh  (kernel/ui/themes/<name>.h) */
#include "ui/widgets.h"

#define DESKTOP_COLOR_BG      TH_DESKTOP
#define DESKTOP_COLOR_ICON_BG TH_DESKTOP

/* ---- system ---- */
#include "sys/util.h"
#include "sys/lang.h"

/* ---- shell + apps, in dependency order (Notepad's pieces are interleaved with the shell because the shell
 *      draws Notepad's pills and the Notepad menus draw over the shell) ---- */
#include "ui/icons.h"
#include "ui/clock.h"
#include "ui/taskbar_metrics.h"
#include "ui/window.h"
#include "apps/notepad/windows.h"
#include "apps/notepad/file_menu.h"
#include "apps/notepad/save.h"
#include "ui/taskbar.h"
#include "ui/startmenu.h"
#include "apps/notepad/draw.h"
#include "apps/setting.h"
#include "apps/web/state.h"
#include "apps/web/address.h"
#include "apps/web/fetch.h"
#include "apps/web/page.h"
#include "apps/web/autotest.h"
#include "apps/web/view.h"
#include "apps/terminal.h"
#include "apps/notepad/dialogs.h"
#include "ui/desktop_files.h"
#include "ui/resize.h"
#include "ui/cursor.h"
#include "ui/compose.h"
#include "sys/power.h"
#include "sys/boot.h"
#include "ui/input.h"

void kmain(void) {
    kernel_boot();

    render_frame(mx, my, status);

    while (1) {
        tick++;

        handle_mouse();
        handle_keyboard();

        net_stack_poll(); /* drains and dispatches any received frames: ARP, IP/ICMP/UDP/TCP, DHCP */

        /* Advances whatever MiniWeb (WEB.MWP) currently has outstanding
         * -- a DNS lookup, an HTTP fetch, or nothing at all. The app
         * itself is the network stack's real verification now (see
         * draw_web_window()/web_go() above): it drove a real DNS
         * resolution, a full TCP 3-way handshake, an HTTP/1.1 request,
         * and a genuine multi-header response from pypi.org during this
         * kernel's own development, all reachable by clicking an icon
         * rather than reading a serial log. web_poll() itself no-ops
         * once everything's settled (DONE/FAILED on either the DNS or
         * HTTP side) until the next click on a site row calls
         * web_go() again. */
        web_poll();
#ifdef MW_AUTOTEST_URL
        mw_autotest_step();
#endif

        render_frame(mx, my, status);
        delay(2000); /* lowered further from 8000 -- mouse felt sluggish/
                      * capped at 8000; this loop's real-world speed varies
                      * a lot by CPU, so double_click_window above is
                      * scaled proportionally to keep the same real-time
                      * double-click window. */
    }
}
