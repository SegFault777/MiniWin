#ifndef MW_UI_COMPOSE_H
#define MW_UI_COMPOSE_H

/* ui/compose.h -- frame composition: the order everything is drawn in.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * Frame composition
 * ============================================================ */
static void draw_status_line(const char *msg) {
    ko_draw_mixed_string(5, VGA_HEIGHT - TASKBAR_H - FONT_CELL - 2, msg, TH_DESKTOP_TEXT);
}

static void render_frame(int mouse_x, int mouse_y, const char *status_msg) {
    bb_fillrect(0, 0, VGA_WIDTH, VGA_HEIGHT, DESKTOP_COLOR_BG);

    draw_desktop_icon();
    draw_desktop_icon2();
    draw_desktop_icon3();
    draw_desktop_icon4();
    draw_desktop_file_icons();

    if (status_msg) draw_status_line(status_msg);

    /* Bottom-to-top through z_order -- whatever was clicked/opened most
     * recently was raised to the end of this list, so it naturally gets
     * drawn last (i.e. on top) with zero extra bookkeeping here. */
    for (int i = 0; i < z_count; i++) {
        int id = z_order[i];
        if (!win_is_open(id) || win_is_minimized(id)) continue;
        if (id == WIN_ID_SETTING) {
            draw_setting_window();
        } else if (id == WIN_ID_WEB) {
            draw_web_window();
        } else if (id == WIN_ID_TERMINAL) {
            draw_terminal_window();
        } else {
            active_np = &notepads[id];
            draw_window();
            if (active_np->file_menu_open) draw_file_menu(mouse_x, mouse_y);
            if (active_np->confirm_mode != CONFIRM_NONE) draw_confirm_dialog(mouse_x, mouse_y);
        }
    }

    draw_taskbar();
    /* drawn after the taskbar (and after every document window above) so
     * it sits on top of all of them -- popups always win the z-order
     * argument */
    if (start_menu_open) draw_start_menu(mouse_x, mouse_y);
    if (clock_popup_open) draw_date_popup();
    draw_cursor(mouse_x, mouse_y);

    vga_present();
}

#endif
