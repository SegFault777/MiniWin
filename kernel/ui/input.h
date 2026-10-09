#ifndef MW_UI_INPUT_H
#define MW_UI_INPUT_H

/* ui/input.h -- the desktop's input handling: the mouse (clicks, drags, buttons, menus, icons) and the keyboard routing.
 * (Split out of kmain() in pre-29; still one translation unit -- see the module map in kernel.c.) */

/* ---- input state (these were locals of kmain's loop) ---- */
static int mx = VGA_WIDTH / 2, my = VGA_HEIGHT / 2;
static int awaiting_second_click = 0;   /* 1 = one icon click seen, waiting for a 2nd within the window */
static u32 last_icon_click_tick = 0;
static int awaiting_second_click_setting = 0;   /* same idea, but for the SETTING.MWP icon */
static u32 last_setting_click_tick = 0;
static int awaiting_second_click_web = 0;   /* same idea, but for the WEB.MWP icon */
static u32 last_web_click_tick = 0;
static int awaiting_second_click_term = 0;  /* ...and for the TERMINAL.MWP icon */
static u32 last_term_click_tick = 0;
static int awaiting_second_click_file_slot = -1;   /* which file icon (if any) saw a first click */
static u32 last_file_icon_click_tick = 0;
static u32 tick = 0;
static const u32 double_click_window = 60000; /* tuned for delay(2000) per frame;
                                             * adjust proportionally if delay() changes */

/* Window-drag state: while dragging, we track which window id is
 * being dragged (-1 = none) and the offset from that window's
 * top-left corner to the point the user grabbed, so the window
 * follows the cursor without "snapping" its corner to it. One shared
 * mechanism now covers every Notepad window AND Setting, instead of
 * a separate pair of variables per window. */
static int dragging_id = -1;
static int drag_offset_x = 0, drag_offset_y = 0;

/* One poll of the mouse: moves the pointer, then dispatches presses, drags and releases to whatever is under it. */
static void handle_mouse(void) {
    if (mouse_poll()) {
        mx += mouse_dx;
        my += mouse_dy;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > VGA_WIDTH - 1)  mx = VGA_WIDTH - 1;
        if (my > VGA_HEIGHT - 1) my = VGA_HEIGHT - 1;

        int left_now = mouse_left;
        int clicked = mouse_click_event; /* per-packet edge detection from the driver */

        /* ---- drag in progress: move whichever window is being
         * dragged with the cursor ---- */
        if (dragging_id >= 0) {
            if (left_now) {
                window_t *dw = win_ptr(dragging_id);
                if (!dw->maximized) {
                    int new_x = mx - drag_offset_x;
                    int new_y = my - drag_offset_y;
                    /* keep at least a sliver of the title bar on-screen
                     * so the window can never be dragged somewhere the
                     * user can't grab it again */
                    dw->x = clampi(new_x, -(dw->w - 20), VGA_WIDTH - 20);
                    dw->y = clampi(new_y, 0, VGA_HEIGHT - TASKBAR_H - TITLEBAR_H);
                }
            } else {
                dragging_id = -1; /* button released -> stop dragging */
            }
        }

        /* ---- resize in progress: grow/shrink from whichever
         * edge(s) were grabbed, anchored on the opposite edge(s) so
         * the corner/side you're NOT dragging stays put ---- */
        if (resizing_id >= 0) {
            if (left_now) {
                int dx = mx - resize_start_mx;
                int dy = my - resize_start_my;
                int nx = resize_start_x, ny = resize_start_y;
                int nw = resize_start_w, nh = resize_start_h;

                if (resizing_zone & RESIZE_E) nw = resize_start_w + dx;
                if (resizing_zone & RESIZE_S) nh = resize_start_h + dy;
                if (resizing_zone & RESIZE_W) { nx = resize_start_x + dx; nw = resize_start_w - dx; }
                if (resizing_zone & RESIZE_N) { ny = resize_start_y + dy; nh = resize_start_h - dy; }

                if (nw < MIN_WIN_W) {
                    if (resizing_zone & RESIZE_W) nx -= (MIN_WIN_W - nw);
                    nw = MIN_WIN_W;
                }
                if (nh < MIN_WIN_H) {
                    if (resizing_zone & RESIZE_N) ny -= (MIN_WIN_H - nh);
                    nh = MIN_WIN_H;
                }
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;

                window_t *rw = win_ptr(resizing_id);
                rw->x = nx; rw->y = ny; rw->w = nw; rw->h = nh;
                /* keep restore_* in sync too, so a later maximize+
                 * restore cycle snaps back to this size, not
                 * whatever size the window happened to be before
                 * this resize started */
                rw->restore_x = nx; rw->restore_y = ny; rw->restore_w = nw; rw->restore_h = nh;
            } else {
                resizing_id = -1;
                resizing_zone = RESIZE_NONE;
            }
        }

        if (clicked) {
            /* Recomputed once per click since there are at most
             * WIN_ID_COUNT (5) windows -- cheap enough not to bother
             * caching, and it keeps this in sync with draw_taskbar()
             * by construction (same taskbar_layout() underneath). */
            int taskbar_restore_id = taskbar_glyph_hit(mx, my, 0);
            int taskbar_close_id   = taskbar_glyph_hit(mx, my, 1);

            if (clock_popup_open) {
                /* Purely informational popup, no controls inside it
                 * -- any click (including on the clock itself again)
                 * just closes it. */
                clock_popup_open = 0;
            } else if (power_menu_open) {
                /* Cascaded off the Start Menu's power item. Whatever
                 * this click was for -- an action or a miss -- both
                 * menus close afterward, same as clicking a Start
                 * Menu item normally would. */
                if (power_menu_item_hit(mx, my, 0)) {
                    system_shutdown(); /* does not return */
                } else if (power_menu_item_hit(mx, my, 1)) {
                    system_restart(); /* does not return */
                }
                power_menu_open = 0;
                start_menu_open = 0;
            } else if (start_menu_open) {
                /* Same "pick an item or dismiss" contract as the File
                 * dropdown below -- except the power item, which
                 * cascades into its own flyout instead of resolving
                 * immediately, so it deliberately does NOT close the
                 * Start Menu the way the other two items do. */
                if (start_menu_item_hit(mx, my, 0)) {
                    int slot = find_free_notepad_slot();
                    if (slot < 0) {
                        status = t(STR_ALL_NOTEPAD_WINDOWS_OPEN);
                    } else {
                        active_np = &notepads[slot];
                        active_np->win.open = 1;
                        active_np->win.minimized = 0;
                        active_np->text_len = 0; active_np->text_buf[0] = 0;
                        active_np->bound_slot = -1;
                        active_np->file_menu_open = 0;
                        active_np->confirm_mode = CONFIRM_NONE;
                        ko_ime_reset();
                        win_z_raise(slot);
                        status = t(STR_NOTEPAD_OPENED);
                    }
                    start_menu_open = 0;
                } else if (start_menu_item_hit(mx, my, 1)) {
                    setting.open = 1;
                    win_z_raise(WIN_ID_SETTING);
                    status = t(STR_SETTING_OPENED);
                    start_menu_open = 0;
                } else if (start_menu_item_hit(mx, my, 2)) {
                    web_win.open = 1;
                    win_z_raise(WIN_ID_WEB);
                    status = t(STR_WEB_OPENED);
                    start_menu_open = 0;
                } else if (start_menu_item_hit(mx, my, 3)) {
                    term_win.open = 1;
                    win_z_raise(WIN_ID_TERMINAL);
                    status = t(STR_TERMINAL_OPENED);
                    start_menu_open = 0;
                } else if (start_menu_item_hit(mx, my, STARTMENU_POWER_IDX)) {
                    power_menu_open = 1;
                } else {
                    start_menu_open = 0; /* click outside dismisses everything */
                }
            } else if (start_button_hit(mx, my)) {
                start_menu_open = 1;
            } else if (clock_hit(mx, my)) {
                clock_popup_open = 1;
            } else if (taskbar_restore_id >= 0) {
                win_restore(taskbar_restore_id);
                if (taskbar_restore_id == WIN_ID_SETTING) status = t(STR_SETTING_RESTORED);
                else if (taskbar_restore_id == WIN_ID_WEB) status = t(STR_WEB_RESTORED);
                else if (taskbar_restore_id == WIN_ID_TERMINAL) status = t(STR_TERMINAL_RESTORED);
                else status = t(STR_NOTEPAD_RESTORED);
            } else if (taskbar_close_id >= 0) {
                if (taskbar_close_id == WIN_ID_SETTING) {
                    /* No unsaved-changes concept in Settings, so its
                     * taskbar close just closes -- no warning needed. */
                    setting.open = 0;
                    setting.minimized = 0;
                    win_z_remove(WIN_ID_SETTING);
                } else if (taskbar_close_id == WIN_ID_WEB) {
                    /* Same reasoning as Settings -- MiniWeb has no
                     * unsaved state (a fetched page isn't a
                     * document), so its taskbar close just closes. */
                    web_win.open = 0;
                    web_win.minimized = 0;
                    win_z_remove(WIN_ID_WEB);
                } else if (taskbar_close_id == WIN_ID_TERMINAL) {
                    /* Same reasoning again -- a command scrollback
                     * isn't a document either. */
                    term_win.open = 0;
                    term_win.minimized = 0;
                    win_z_remove(WIN_ID_TERMINAL);
                } else {
                    /* Minimized taskbar close also asks first, for
                     * consistency with the window's own X button. */
                    active_np = &notepads[taskbar_close_id];
                    active_np->confirm_mode = CONFIRM_CLOSE;
                    beep_warning();
                    active_np->win.minimized = 0; /* bring it back on-screen so the dialog is visible */
                    win_z_raise(taskbar_close_id);
                }
            } else {
                /* Topmost open+visible window whose rect (or resize
                 * margin) contains the click, z-order back-to-front
                 * reversed so the FRONT-most window wins when two
                 * happen to overlap. */
                int hit_id;
                int zone = compute_hover_resize_zone(mx, my, &hit_id);

                if (zone != RESIZE_NONE) {
                    /* Grabbed an edge or corner: start resizing
                     * instead of any of the normal window-content
                     * handling below. Brings the window to front
                     * too, same as any other interaction with it. */
                    win_z_raise(hit_id);
                    resizing_id = hit_id;
                    resizing_zone = zone;
                    resize_start_mx = mx;
                    resize_start_my = my;
                    window_t *rw = win_ptr(hit_id);
                    resize_start_x = rw->x;
                    resize_start_y = rw->y;
                    resize_start_w = rw->w;
                    resize_start_h = rw->h;
                } else if (hit_id == WIN_ID_SETTING) {
                    /* Clicking anywhere on a window -- not just a
                     * control that does something -- brings it to
                     * the front, same as any real window manager. */
                    win_z_raise(WIN_ID_SETTING);
                    if (setting_min_hit(mx, my)) {
                        pressed_btn_kind = BTN_MIN;
                        pressed_btn_win = WIN_ID_SETTING;
                    } else if (setting_max_hit(mx, my)) {
                        pressed_btn_kind = BTN_MAX;
                        pressed_btn_win = WIN_ID_SETTING;
                    } else if (setting_close_hit(mx, my)) {
                        pressed_btn_kind = BTN_CLOSE;
                        pressed_btn_win = WIN_ID_SETTING;
                    } else if (setting_nav_hit(mx, my, SETTING_NAV_LANGUAGE)) {
                        setting_page = SETTING_NAV_LANGUAGE;
                    } else if (setting_nav_hit(mx, my, SETTING_NAV_IME)) {
                        setting_page = SETTING_NAV_IME;
                    } else if (setting_nav_hit(mx, my, SETTING_NAV_TIMEZONE)) {
                        setting_page = SETTING_NAV_TIMEZONE;
                    } else if (setting_page == SETTING_NAV_LANGUAGE && setting_row_hit(mx, my, 0)) {
                        sys_language = LANG_ENGLISH;
                        status = t(STR_DEFAULT_HINT);
                    } else if (setting_page == SETTING_NAV_LANGUAGE && setting_row_hit(mx, my, 1)) {
                        sys_language = LANG_KOREAN;
                        status = t(STR_DEFAULT_HINT);
                    } else if (setting_page == SETTING_NAV_IME && setting_row_hit(mx, my, 0)) {
                        /* multi-select checkbox -- refuse to uncheck the
                         * last remaining enabled IME, same as real OSes
                         * never let you remove your only keyboard layout */
                        if (ime_enabled[IME_ENGLISH] && !ime_enabled[IME_KOREAN]) {
                            status = t(STR_IME_MIN_ONE);
                        } else {
                            ime_enabled[IME_ENGLISH] = !ime_enabled[IME_ENGLISH];
                            ime_ensure_current_enabled();
                        }
                    } else if (setting_page == SETTING_NAV_IME && setting_row_hit(mx, my, 1)) {
                        if (ime_enabled[IME_KOREAN] && !ime_enabled[IME_ENGLISH]) {
                            status = t(STR_IME_MIN_ONE);
                        } else {
                            ime_enabled[IME_KOREAN] = !ime_enabled[IME_KOREAN];
                            ime_ensure_current_enabled();
                        }
                    } else if (setting_page == SETTING_NAV_TIMEZONE && tz_minus_hit(mx, my)) {
                        if (tz_offset_hours > -12) tz_offset_hours--;
                    } else if (setting_page == SETTING_NAV_TIMEZONE && tz_plus_hit(mx, my)) {
                        if (tz_offset_hours < 14) tz_offset_hours++;
                    } else if (setting_titlebar_drag_hit(mx, my) && !setting.maximized) {
                        dragging_id = WIN_ID_SETTING;
                        drag_offset_x = mx - setting.x;
                        drag_offset_y = my - setting.y;
                    }
                } else if (hit_id == WIN_ID_WEB) {
                    win_z_raise(WIN_ID_WEB);
                    if (web_min_hit(mx, my)) {
                        pressed_btn_kind = BTN_MIN;
                        pressed_btn_win = WIN_ID_WEB;
                    } else if (web_max_hit(mx, my)) {
                        pressed_btn_kind = BTN_MAX;
                        pressed_btn_win = WIN_ID_WEB;
                    } else if (web_close_hit(mx, my)) {
                        pressed_btn_kind = BTN_CLOSE;
                        pressed_btn_win = WIN_ID_WEB;
                    } else if (web_urlbar_hit(mx, my)) {
                        web_urlbar_focused = 1;
                        rd_focus = 0;
                    } else if (web_go_btn_hit(mx, my)) {
                        pressed_btn_kind = BTN_WEB_GO;
                        pressed_btn_win = WIN_ID_WEB;
                        web_urlbar_focused = 0;
                    } else if (web_site_row_hit(mx, my, WEB_SITE_PYPI)) {
                        web_urlbar_focused = 0;
                        web_go(WEB_SITE_PYPI);
                    } else if (web_site_row_hit(mx, my, WEB_SITE_GATEWAY)) {
                        web_urlbar_focused = 0;
                        web_go(WEB_SITE_GATEWAY);
                    } else if (web_page_click(mx, my)) {
                        web_urlbar_focused = 0;       /* a click on the page: follow a link / use the scrollbar */
                    } else if (web_titlebar_drag_hit(mx, my) && !web_win.maximized) {
                        web_urlbar_focused = 0;
                        dragging_id = WIN_ID_WEB;
                        drag_offset_x = mx - web_win.x;
                        drag_offset_y = my - web_win.y;
                    } else {
                        web_urlbar_focused = 0;
                    }
                } else if (hit_id == WIN_ID_TERMINAL) {
                    win_z_raise(WIN_ID_TERMINAL);
                    if (term_min_hit(mx, my)) {
                        pressed_btn_kind = BTN_MIN;
                        pressed_btn_win = WIN_ID_TERMINAL;
                    } else if (term_max_hit(mx, my)) {
                        pressed_btn_kind = BTN_MAX;
                        pressed_btn_win = WIN_ID_TERMINAL;
                    } else if (term_close_hit(mx, my)) {
                        pressed_btn_kind = BTN_CLOSE;
                        pressed_btn_win = WIN_ID_TERMINAL;
                    } else if (term_input_hit(mx, my)) {
                        term_input_focused = 1;
                    } else if (term_titlebar_drag_hit(mx, my) && !term_win.maximized) {
                        term_input_focused = 0;
                        dragging_id = WIN_ID_TERMINAL;
                        drag_offset_x = mx - term_win.x;
                        drag_offset_y = my - term_win.y;
                    } else {
                        term_input_focused = 0;
                    }
                } else if (hit_id >= 0) {
                    active_np = &notepads[hit_id];
                    win_z_raise(hit_id);

                    if (active_np->confirm_mode != CONFIRM_NONE) {
                        /* Modal to THIS window only -- other windows
                         * remain fully interactive; only clicks that
                         * land inside this one's rect even reach here. */
                        if (confirm_yes_hit(mx, my)) {
                            confirm_yes_action();
                        } else if (confirm_no_hit(mx, my)) {
                            confirm_no_action();
                        } else if (confirm_close_hit(mx, my)) {
                            pressed_btn_kind = BTN_CONFIRM_CLOSE;
                            pressed_btn_win = hit_id;
                        }
                    } else if (active_np->file_menu_open) {
                        if (file_menu_item_hit(mx, my, 0)) {
                            status = t(STR_SAVE_AS_COMING_SOON);
                            active_np->file_menu_open = 0;
                        } else if (file_menu_item_hit(mx, my, 1)) {
                            int saved_slot = save_current_document();
                            status = saved_slot >= 0 ? format_saved_status(saved_slot) : save_error_status(saved_slot);
                            active_np->file_menu_open = 0;
                        } else if (file_menu_item_hit(mx, my, 2)) {
                            active_np->confirm_mode = CONFIRM_NEW;
                            beep_warning();
                            active_np->file_menu_open = 0;
                        } else {
                            active_np->file_menu_open = 0; /* click outside just dismisses it */
                        }
                    } else if (in_rect(mx, my, btn_min_x(), btn_y(), BTN_W, BTN_H)) {
                        pressed_btn_kind = BTN_MIN;
                        pressed_btn_win = hit_id;
                    } else if (in_rect(mx, my, btn_max_x(), btn_y(), BTN_W, BTN_H)) {
                        pressed_btn_kind = BTN_MAX;
                        pressed_btn_win = hit_id;
                    } else if (in_rect(mx, my, btn_close_x(), btn_y(), BTN_W, BTN_H)) {
                        pressed_btn_kind = BTN_CLOSE;
                        pressed_btn_win = hit_id;
                    } else if (file_label_hit(mx, my)) {
                        active_np->file_menu_open = 1;
                    } else if (titlebar_drag_hit(mx, my) && !active_np->win.maximized) {
                        /* start dragging: remember the grab offset so
                         * the window doesn't jump when the drag begins */
                        dragging_id = hit_id;
                        drag_offset_x = mx - active_np->win.x;
                        drag_offset_y = my - active_np->win.y;
                    }
                } else if (in_rect(mx, my, ICON_X, ICON_Y, ICON_W, ICON_H)) {
                    if (awaiting_second_click && (tick - last_icon_click_tick) < double_click_window) {
                        /* 2nd click of a double-click: open a brand
                         * new blank window in the first free slot. */
                        int slot = find_free_notepad_slot();
                        if (slot < 0) {
                            status = t(STR_ALL_NOTEPAD_WINDOWS_OPEN);
                        } else {
                            active_np = &notepads[slot];
                            active_np->win.open = 1;
                            active_np->win.minimized = 0;
                            active_np->text_len = 0; active_np->text_buf[0] = 0;
                            active_np->bound_slot = -1;
                            active_np->file_menu_open = 0;
                            active_np->confirm_mode = CONFIRM_NONE;
                            ko_ime_reset();
                            win_z_raise(slot);
                            status = t(STR_NOTEPAD_OPENED);
                        }
                        awaiting_second_click = 0;
                    } else {
                        awaiting_second_click = 1;
                        last_icon_click_tick = tick;
                    }
                } else if (in_rect(mx, my, ICON2_X, ICON2_Y, ICON2_W, ICON2_H)) {
                    if (awaiting_second_click_setting && (tick - last_setting_click_tick) < double_click_window) {
                        setting.open = 1;
                        win_z_raise(WIN_ID_SETTING);
                        status = t(STR_SETTING_OPENED);
                        awaiting_second_click_setting = 0;
                    } else {
                        awaiting_second_click_setting = 1;
                        last_setting_click_tick = tick;
                    }
                } else if (in_rect(mx, my, ICON3_X, ICON3_Y, ICON3_W, ICON3_H)) {
                    if (awaiting_second_click_web && (tick - last_web_click_tick) < double_click_window) {
                        web_win.open = 1;
                        win_z_raise(WIN_ID_WEB);
                        status = t(STR_WEB_OPENED);
                        awaiting_second_click_web = 0;
                    } else {
                        awaiting_second_click_web = 1;
                        last_web_click_tick = tick;
                    }
                } else if (in_rect(mx, my, ICON4_X, ICON4_Y, ICON4_W, ICON4_H)) {
                    if (awaiting_second_click_term && (tick - last_term_click_tick) < double_click_window) {
                        term_win.open = 1;
                        win_z_raise(WIN_ID_TERMINAL);
                        status = t(STR_TERMINAL_OPENED);
                        awaiting_second_click_term = 0;
                    } else {
                        awaiting_second_click_term = 1;
                        last_term_click_tick = tick;
                    }
                } else {
                    /* Check desktop file icons last (any slot). */
                    for (int slot = 0; slot < FS_MAX_FILES; slot++) {
                        if (!desktop_file_exists[slot]) continue;
                        if (!in_rect(mx, my, fileicon_x(slot), fileicon_y(slot), ICON_W, ICON_H)) continue;

                        if (awaiting_second_click_file_slot == slot &&
                            (tick - last_file_icon_click_tick) < double_click_window) {
                            /* 2nd click: if this file is already open in
                             * some window, just focus that one instead
                             * of loading a second editable copy (which
                             * would race on Save); otherwise open it in
                             * the first free window slot. */
                            int existing = find_notepad_bound_to(slot);
                            if (existing >= 0) {
                                notepads[existing].win.minimized = 0;
                                win_z_raise(existing);
                                status = format_saved_status(slot);
                            } else {
                                int free_slot = find_free_notepad_slot();
                                if (free_slot < 0) {
                                    status = t(STR_ALL_NOTEPAD_WINDOWS_OPEN);
                                } else {
                                    /* Read BEFORE committing to opening the window: a damaged file (CRC mismatch,
                                     * unreadable sector, longer than the buffer) used to open as silently truncated or
                                     * empty text, and the next Save would overwrite the original with that. */
                                    u32 loaded = 0;
                                    if (!fs_read_slot(slot, notepads[free_slot].text_buf, sizeof(notepads[free_slot].text_buf) - 1, &loaded)) {
                                        status = t(STR_FILE_DAMAGED);
                                        awaiting_second_click_file_slot = -1;
                                        break;
                                    }
                                    active_np = &notepads[free_slot];
                                    active_np->text_buf[loaded] = 0;
                                    active_np->text_len = loaded;
                                    active_np->bound_slot = slot;
                                    active_np->win.open = 1;
                                    active_np->win.minimized = 0;
                                    active_np->file_menu_open = 0;
                                    active_np->confirm_mode = CONFIRM_NONE;
                                    ko_ime_reset();
                                    win_z_raise(free_slot);
                                    status = format_saved_status(slot); /* reuse "SAVED: name" wording to show which file opened */
                                }
                            }
                            awaiting_second_click_file_slot = -1;
                        } else {
                            awaiting_second_click_file_slot = slot;
                            last_file_icon_click_tick = tick;
                        }
                        break;
                    }
                }
            }
        }

        /* ---- button release: fire the action IF the cursor is
         * still over the exact button that was pressed, then clear
         * the press state either way. Dragging off before letting
         * go cancels it -- same contract as any real button. ---- */
        if (mouse_release_event && pressed_btn_kind != BTN_NONE) {
            int kind = pressed_btn_kind, win = pressed_btn_win;
            int still_over = 0;

            if (win == WIN_ID_SETTING) {
                if (kind == BTN_MIN) still_over = setting_min_hit(mx, my);
                else if (kind == BTN_MAX) still_over = setting_max_hit(mx, my);
                else if (kind == BTN_CLOSE) still_over = setting_close_hit(mx, my);
            } else if (win == WIN_ID_WEB) {
                if (kind == BTN_MIN) still_over = web_min_hit(mx, my);
                else if (kind == BTN_MAX) still_over = web_max_hit(mx, my);
                else if (kind == BTN_CLOSE) still_over = web_close_hit(mx, my);
                else if (kind == BTN_WEB_GO) still_over = web_go_btn_hit(mx, my);
            } else if (win == WIN_ID_TERMINAL) {
                if (kind == BTN_MIN) still_over = term_min_hit(mx, my);
                else if (kind == BTN_MAX) still_over = term_max_hit(mx, my);
                else if (kind == BTN_CLOSE) still_over = term_close_hit(mx, my);
            } else if (win >= 0 && win < NOTEPAD_MAX) {
                active_np = &notepads[win];
                if (kind == BTN_MIN) still_over = in_rect(mx, my, btn_min_x(), btn_y(), BTN_W, BTN_H);
                else if (kind == BTN_MAX) still_over = in_rect(mx, my, btn_max_x(), btn_y(), BTN_W, BTN_H);
                else if (kind == BTN_CLOSE) still_over = in_rect(mx, my, btn_close_x(), btn_y(), BTN_W, BTN_H);
                else if (kind == BTN_CONFIRM_CLOSE) still_over = confirm_close_hit(mx, my);
            }

            if (still_over) {
                if (win == WIN_ID_SETTING) {
                    if (kind == BTN_MIN) {
                        win_minimize(WIN_ID_SETTING);
                        status = t(STR_SETTING_MINIMIZED);
                    } else if (kind == BTN_MAX) {
                        if (setting.maximized) {
                            unmaximize_window(&setting);
                            status = t(STR_SETTING_RESTORED);
                        } else {
                            maximize_window(&setting);
                            status = t(STR_SETTING_MAXIMIZED);
                        }
                    } else if (kind == BTN_CLOSE) {
                        setting.open = 0;
                        win_z_remove(WIN_ID_SETTING);
                    }
                } else if (win == WIN_ID_WEB) {
                    if (kind == BTN_MIN) {
                        win_minimize(WIN_ID_WEB);
                        status = t(STR_WEB_MINIMIZED);
                    } else if (kind == BTN_MAX) {
                        if (web_win.maximized) {
                            unmaximize_window(&web_win);
                            status = t(STR_WEB_RESTORED);
                        } else {
                            maximize_window(&web_win);
                            status = t(STR_WEB_MAXIMIZED);
                        }
                    } else if (kind == BTN_CLOSE) {
                        /* No unsaved-changes concept here either --
                         * a fetched page isn't a document, so this
                         * closes immediately, same reasoning as
                         * Settings' own close button. */
                        web_win.open = 0;
                        win_z_remove(WIN_ID_WEB);
                    } else if (kind == BTN_WEB_GO) {
                        web_go_url();
                    }
                } else if (win == WIN_ID_TERMINAL) {
                    if (kind == BTN_MIN) {
                        win_minimize(WIN_ID_TERMINAL);
                        status = t(STR_TERMINAL_MINIMIZED);
                    } else if (kind == BTN_MAX) {
                        if (term_win.maximized) {
                            unmaximize_window(&term_win);
                            status = t(STR_TERMINAL_RESTORED);
                        } else {
                            maximize_window(&term_win);
                            status = t(STR_TERMINAL_MAXIMIZED);
                        }
                    } else if (kind == BTN_CLOSE) {
                        /* Same as Setting/Web: no unsaved-changes
                         * concept for a scrollback log, so this
                         * closes immediately. */
                        term_win.open = 0;
                        win_z_remove(WIN_ID_TERMINAL);
                    }
                } else {
                    if (kind == BTN_MIN) {
                        win_minimize(win);
                        status = t(STR_NOTEPAD_MINIMIZED);
                    } else if (kind == BTN_MAX) {
                        if (active_np->win.maximized) {
                            unmaximize_window(&active_np->win);
                            status = t(STR_NOTEPAD_RESTORED);
                        } else {
                            maximize_window(&active_np->win);
                            status = t(STR_NOTEPAD_MAXIMIZED);
                        }
                    } else if (kind == BTN_CLOSE) {
                        /* Ask before closing, same Yes/No pattern as New. */
                        active_np->confirm_mode = CONFIRM_CLOSE;
                        beep_warning();
                    } else if (kind == BTN_CONFIRM_CLOSE) {
                        active_np->confirm_mode = CONFIRM_NONE;
                    }
                }
            }
            pressed_btn_kind = BTN_NONE;
            pressed_btn_win = -1;
        }
    }

}

/* One poll of the keyboard: routes the key to the topmost window (page scrolling, text entry, dialogs, shortcuts). */
static void handle_keyboard(void) {
    /* ---- keyboard: routed to whichever window is topmost in
     * z-order, if (and only if) that's a Notepad window -- Setting
     * has no text fields, and if the desktop itself is topmost (or
     * nothing is open at all), keystrokes just go nowhere. This is
     * also why raising a window on click matters beyond visuals:
     * the topmost window IS the keyboard focus. ---- */
    int k = keyboard_poll_key();
    int focused_id = -1;
    for (int zi = z_count - 1; zi >= 0; zi--) {
        int id = z_order[zi];
        if (win_is_open(id) && !win_is_minimized(id)) { focused_id = id; break; }
    }

    /* Scrolling the web page: Up/Down move three lines; with the URL bar NOT focused, Space pages
     * down and B pages up (the same keys every text-mode browser uses). The keys are consumed here
     * so they don't also reach the URL bar's text entry below. */
    if (focused_id == WIN_ID_WEB && k != KEY_NONE) {
        int page = web_view_h() - 28;
        if (page < 14) page = 14;
        if (!web_urlbar_focused && rd_focus && k > 0) {       /* a text field on the page has the keyboard */
            int r = rd_ctl_key(k);
            if (r == 2) { u32 f = rd_find_form(rd_focus); if (f) web_submit_form(f, 0); }
            k = KEY_NONE;
        }
        else if (k == KEY_UP)   { web_scroll_by(-WEB_SCROLL_STEP); k = KEY_NONE; }
        else if (k == KEY_DOWN) { web_scroll_by(WEB_SCROLL_STEP);  k = KEY_NONE; }
        else if (!web_urlbar_focused && k == ' ')                { web_scroll_by(page);  k = KEY_NONE; }
        else if (!web_urlbar_focused && (k == 'b' || k == 'B'))  { web_scroll_by(-page); k = KEY_NONE; }
    }

    if (focused_id >= 0 && focused_id != WIN_ID_SETTING && focused_id != WIN_ID_WEB && focused_id != WIN_ID_TERMINAL) {
        active_np = &notepads[focused_id];

    if (active_np->confirm_mode != CONFIRM_NONE) {
        /* keyboard shortcuts for the confirm dialog, so it can be
         * driven without the mouse too */
        if (k == 'y' || k == 'Y') {
            confirm_yes_action();
        } else if (k == 'n' || k == 'N') {
            confirm_no_action();
        }
    } else if (k == KEY_RALT && active_np->win.open && !active_np->win.minimized && !active_np->file_menu_open) {
        /* Right Alt cycles to the next ENABLED input method (see
         * SETTING.MWP > SYSTEM > IME) -- matches the 한/영 key
         * position on real Korean keyboards. F7 used to do this too,
         * but that's gone now that IME selection lives in Settings. */
        ime_cycle_next();
        status = (current_ime == IME_KOREAN) ? t(STR_HANGUL_MODE_ON) : t(STR_ENGLISH_MODE_ON);
    } else if (k > 0 && active_np->win.open && !active_np->win.minimized && !active_np->file_menu_open) {
        char c = (char)k;
        if (keyboard_ctrl_held()) {
            /* Ctrl+S / Ctrl+N / Ctrl+W accelerators, mirroring the
             * File menu and the title bar's close button */
            if (ko_ime_is_composing()) ko_ime_commit(active_np->text_buf, &active_np->text_len, sizeof(active_np->text_buf));
            if (c == 's' || c == 'S') {
                int saved_slot = save_current_document();
                status = saved_slot >= 0 ? format_saved_status(saved_slot) : save_error_status(saved_slot);
            } else if (c == 'n' || c == 'N') {
                active_np->confirm_mode = CONFIRM_NEW;
                beep_warning();
            } else if (c == 'w' || c == 'W') {
                active_np->confirm_mode = CONFIRM_CLOSE;
                beep_warning();
            }
        } else if (c == '\b') {
            if (current_ime == IME_KOREAN && ko_ime_backspace()) {
                /* consumed by the IME: undid one step of the syllable
                 * currently being composed, nothing else to do */
            } else if (active_np->text_len > 0) {
                int del = ko_utf8_last_char_len(active_np->text_buf, active_np->text_len);
                active_np->text_len -= del;
                active_np->text_buf[active_np->text_len] = 0;
            }
        } else if (current_ime == IME_KOREAN && ko_ime_feed_key(c, active_np->text_buf, &active_np->text_len, sizeof(active_np->text_buf))) {
            /* consumed as a jamo keystroke -- composition state
             * updated (and/or a completed syllable was appended to
             * active_np->text_buf) inside ko_ime_feed_key() itself */
        } else if (c == '\n' || c == ' ' || (c >= 32 && c < 127)) {
            if (current_ime == IME_KOREAN && ko_ime_is_composing()) {
                /* a non-jamo key (space, enter, punctuation) always
                 * flushes an in-progress syllable first, matching how
                 * every real Hangul IME behaves */
                ko_ime_commit(active_np->text_buf, &active_np->text_len, sizeof(active_np->text_buf));
            }
            kstrcpy_append(active_np->text_buf, &active_np->text_len, sizeof(active_np->text_buf), c);
        }
    }
    } else if (focused_id == WIN_ID_WEB && web_urlbar_focused && k > 0) {
        /* URL bar text entry -- deliberately NOT run through the
         * Hangul IME the way Notepad's text is: a URL or search
         * query is typed as plain ASCII here (DuckDuckGo Lite gets
         * UTF-8 Hangul search terms just fine over the wire if
         * someone really wants that, but composing them a jamo at a
         * time into an address bar most people type ASCII hostnames
         * into isn't worth the complexity -- see ko_ime_feed_key()'s
         * use in the Notepad branch above for what that would even
         * involve). Enter submits, exactly like every browser's
         * address bar; Backspace deletes one byte (always safe
         * here since nothing above ever pushes non-ASCII into this
         * particular buffer). */
        char c = (char)k;
        if (c == '\n') {
            web_go_url();
        } else if (c == '\b') {
            if (web_urlbar_len > 0) {
                web_urlbar_len--;
                web_urlbar_buf[web_urlbar_len] = 0;
            }
        } else if (c >= 32 && c < 127) {
            kstrcpy_append(web_urlbar_buf, &web_urlbar_len, sizeof(web_urlbar_buf), c);
        }
    } else if (focused_id == WIN_ID_TERMINAL && term_input_focused && k > 0) {
        /* Command-line text entry -- plain ASCII only, same reasoning
         * as WEB.MWP's URL bar just above: a command word like `peek`
         * or `run` is typed on an ASCII keyboard, so this doesn't
         * route through the Hangul IME either. Enter runs whatever's
         * in term_input_buf (term_run_input() itself clears the
         * buffer once it's done); Backspace deletes one byte. */
        char c = (char)k;
        if (c == '\n') {
            term_run_input();
        } else if (c == '\b') {
            if (term_input_len > 0) {
                term_input_len--;
                term_input_buf[term_input_len] = 0;
            }
        } else if (c >= 32 && c < 127) {
            kstrcpy_append(term_input_buf, &term_input_len, sizeof(term_input_buf), c);
        }
    }

}

#endif
