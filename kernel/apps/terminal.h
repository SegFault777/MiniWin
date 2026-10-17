/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_TERMINAL_H
#define MW_APPS_TERMINAL_H

/* apps/terminal.h -- TERMINAL.MWP: the command-line app and its commands.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* Forward declarations -- term_run_input() (below) needs to call all
 * three of these for its `re`/`off`/`run` commands, but each is only
 * actually *defined* much further down this file (render_frame() is the
 * main-loop's own redraw entry point; system_restart()/system_shutdown()
 * are SETTING.MWP's power-menu actions, reused here so Terminal's `re`
 * and `off` behave identically instead of duplicating the triple-fault/
 * cli+hlt tricks). Declaring them here instead of moving Terminal's
 * whole section after them keeps this newer app grouped with the rest
 * of the top-level windows (Notepad/Setting/Web/Terminal, in that
 * order) rather than tacked onto the end of the file. */
static void render_frame(int mouse_x, int mouse_y, const char *status_msg);
static void system_restart(void);
static void system_shutdown(void);

/* ============================================================
 * Terminal.mwp -- MiniWin's command-line app. Built into the kernel
 * today for the same historical reason NOTEPAD/SETTING/WEB are (they
 * all predate kernel/mwp.h's loadable-program format) -- see this
 * file's "Project name: MWP" note in README.md. Its actual job,
 * though, is to be the thing that DOES use the loadable-program format:
 * its `run` command is the only place in this OS a person can load and
 * execute an arbitrary .mwp from a program slot (see kernel/mwp.h);
 * every other app is fixed at boot.
 *
 * Commands are deliberately NOT named after their Unix or DOS
 * equivalents (no ls, rm, cat, dir, del, type, ...) -- short original
 * words picked for what they plainly mean, so this reads as MiniWin's
 * own command line rather than a scaled-down clone of somebody else's:
 *   ?     - list every command below, with a one-line description
 *   peek  - list the four document slots (kernel/fs.h's FS_MAX_FILES)
 *   rd    - print one document's contents to the scrollback
 *   del   - delete one document
 *   apps  - list the four program slots (kernel/mwp.h's programs)
 *   run   - load and execute a program by name (kernel/mwp.h's mwp_run())
 *   tick  - print the current time and date (kernel/rtc.h)
 *   fetch - print OS name/version, screen mode, and disk-slot usage
 *   os-inst - install MiniWin on another disk or partition (kernel/os_inst.h); `os-inst ?` explains it
 *   wipe  - clear the scrollback
 *   re    - restart the machine (same triple-fault trick SETTING.MWP's
 *           power menu already uses)
 *   off   - halt the machine (same `cli; hlt` SETTING.MWP already uses)
 * ============================================================ */
#define TERM_COLS        40   /* matches TERM_DEFAULT_W's sizing (see
                               * this file's window_t table above) */
#define TERM_SCROLLBACK_ROWS 200  /* ring buffer -- old lines just get
                                   * overwritten once this fills, no
                                   * "scrollback full" error to handle */
#define TERM_VISIBLE_ROWS 14  /* how many rows actually fit in the
                               * window at TERM_DEFAULT_H -- see
                               * draw_terminal_window()'s geometry */
#define TERM_INPUT_MAXLEN 64

/* One line of scrollback, fixed-width and NUL-terminated -- plain
 * ASCII only (same as the URL bar; see WEB.MWP's own reasoning for why
 * user-typed command-line text doesn't route through the Hangul IME).
 * A ring buffer (term_scroll_head wraps around TERM_SCROLLBACK_ROWS)
 * rather than a growable log, because this kernel has no heap to grow
 * one in -- every buffer here is a fixed static array, same as
 * everywhere else in this codebase. */
static char term_scrollback[TERM_SCROLLBACK_ROWS][TERM_COLS + 1];
static int  term_scroll_count = 0;  /* how many rows have EVER been written
                                     * (caps at TERM_SCROLLBACK_ROWS; used
                                     * to know how far back there's real
                                     * content vs. never-written rows) */
static int  term_scroll_head = 0;   /* ring-buffer write cursor */
static int  term_scroll_off = 0;    /* how many rows the view is scrolled back from the newest line (Up/Down keys); any
                                     * new output snaps it back to 0 so fresh output is never hidden */

static char term_input_buf[TERM_INPUT_MAXLEN + 1];
static u32  term_input_len = 0;
static int  term_input_focused = 0;

/* Appends one line to the scrollback ring buffer, truncating at
 * TERM_COLS if it's too long to fit a row rather than wrapping it --
 * simpler than word-wrap, and every message this file actually prints
 * was written short enough not to need it. */
static void term_print(const char *s) {
    char *row = term_scrollback[term_scroll_head];
    u32 i = 0;
    while (s[i] && i < TERM_COLS) { row[i] = s[i]; i++; }
    row[i] = 0;
    term_scroll_head = (term_scroll_head + 1) % TERM_SCROLLBACK_ROWS;
    if (term_scroll_count < TERM_SCROLLBACK_ROWS) term_scroll_count++;
    term_scroll_off = 0;
}

/* Up/Down: move the view by `d` rows (positive = back in time), clamped to what exists. */
static void term_scroll_by(int d) {
    int max_off = term_scroll_count - TERM_VISIBLE_ROWS;
    if (max_off < 0) max_off = 0;
    term_scroll_off += d;
    if (term_scroll_off < 0) term_scroll_off = 0;
    if (term_scroll_off > max_off) term_scroll_off = max_off;
}

static void term_print_blank(void) { term_print(""); }

/* Splits the input line into up to 4 whitespace-separated tokens
 * in-place-ish (writes into caller-owned buffers) -- every command
 * here takes at most one argument (a name), so this never needed to
 * be a real argv[]/argc parser. Returns the token count. */
static int term_tokenize(const char *s, char tokens[2][TERM_INPUT_MAXLEN + 1]) {
    int count = 0;
    u32 i = 0;
    while (count < 2) {
        while (s[i] == ' ') i++;
        if (!s[i]) break;
        u32 j = 0;
        while (s[i] && s[i] != ' ' && j < TERM_INPUT_MAXLEN) tokens[count][j++] = s[i++];
        tokens[count][j] = 0;
        count++;
    }
    return count;
}

static int term_streq(const char *a, const char *b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

/* ? -- lists every command with a one-line description. Kept as one
 * function (rather than, say, a table both `?` and a future
 * autocomplete could share) because nothing else in this file needs
 * that table yet, and building it speculatively would just be dead
 * weight -- see this project's general house style on not
 * pre-building things only one caller needs. */
static void term_cmd_help(void) {
    term_print("?        - show this list");
    term_print("peek     - list saved documents");
    term_print("rd <n>   - read document n (1-4)");
    term_print("del <n>  - delete document n (1-4)");
    term_print("apps     - list installed programs");
    term_print("run <nm> - load and run a program");
    term_print("tick     - show the time and date");
    term_print("fetch    - show system information");
    term_print("os-inst  - install MiniWin (os-inst ?)");
    term_print("wipe     - clear this screen");
    term_print("re       - restart the machine");
    term_print("off      - power off the machine");
    term_print("Up/Down keys scroll this screen");
}

/* peek -- lists the four document slots (kernel/fs.h), same slots
 * Notepad's own File > Save As dialog reads/writes. */
static void term_cmd_peek(void) {
    int any = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        u32 len;
        if (!fs_check_slot(i, &len)) continue;
        any = 1;
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  ");
        append_uint(line, &l, sizeof(line), i + 1);
        append_str(line, &l, sizeof(line), ": ");
        append_str(line, &l, sizeof(line), fs_slot_names[i]);
        append_str(line, &l, sizeof(line), " (");
        append_uint(line, &l, sizeof(line), (int)len);
        append_str(line, &l, sizeof(line), " bytes)");
        term_print(line);
    }
    if (!any) term_print("  (no documents saved)");
}

/* rd <n> -- prints one document's raw contents into the scrollback, a
 * line at a time (splitting on the document's own '\n' bytes, same as
 * how Notepad's text buffer stores them). No line-wrap for a document
 * line longer than TERM_COLS -- term_print() already truncates, and a
 * saved .txt is exactly the kind of content that might genuinely be
 * wider than this window; `rd` is for a quick peek, not a full pager. */
static void term_cmd_read(const char *arg) {
    int n = 0;
    for (int i = 0; arg[i]; i++) {
        if (arg[i] < '0' || arg[i] > '9') { term_print("  bad slot number"); return; }
        n = n * 10 + (arg[i] - '0');
    }
    if (n < 1 || n > FS_MAX_FILES) { term_print("  slot must be 1-4"); return; }
    int slot = n - 1;
    u32 len;
    if (!fs_check_slot(slot, &len)) { term_print("  that slot is empty"); return; }

    static u8 buf[FS_MAX_FILE_BYTES];
    u32 got = 0;
    if (!fs_read_slot(slot, (char *)buf, sizeof(buf), &got)) { term_print("  read error: the file is damaged"); return; }
    char line[TERM_COLS + 1]; u32 l = 0;
    for (u32 i = 0; i < got; i++) {
        if (buf[i] == '\n' || l >= TERM_COLS) {
            line[l] = 0;
            term_print(line);
            l = 0;
            if (buf[i] == '\n') continue;
        }
        line[l++] = (char)buf[i];
    }
    if (l > 0) { line[l] = 0; term_print(line); }
}

/* del <n> -- deletes one document slot outright, no confirmation
 * dialog the way Notepad's own close-with-unsaved-changes flow has
 * one: Terminal is an expert tool by nature (typing an exact command
 * is already more deliberate than clicking an X), so this trusts
 * whoever typed it meant it. */
static void term_cmd_del(const char *arg) {
    int n = 0;
    for (int i = 0; arg[i]; i++) {
        if (arg[i] < '0' || arg[i] > '9') { term_print("  bad slot number"); return; }
        n = n * 10 + (arg[i] - '0');
    }
    if (n < 1 || n > FS_MAX_FILES) { term_print("  slot must be 1-4"); return; }
    int slot = n - 1;
    u32 len;
    if (!fs_check_slot(slot, &len)) { term_print("  that slot is empty"); return; }
    fs_delete_slot(slot);
    desktop_file_exists[slot] = 0;
    term_print("  deleted");
}

/* apps -- lists the four loadable-program slots (kernel/mwp.h /
 * kernel/fs.h's PROG_* constants). */
static void term_cmd_apps(void) {
    int any = 0;
    for (int i = 0; i < PROG_MAX_SLOTS; i++) {
        u32 len; char name[PROG_NAME_MAXLEN];
        if (!prog_check_slot(i, &len, 0, name)) continue;
        any = 1;
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  ");
        append_str(line, &l, sizeof(line), name);
        append_str(line, &l, sizeof(line), " (");
        append_uint(line, &l, sizeof(line), (int)len);
        append_str(line, &l, sizeof(line), " bytes)");
        term_print(line);
    }
    if (!any) term_print("  (no programs installed)");
}

/* run <name> -- the whole reason Terminal exists: hands off to
 * kernel/mwp.h's mwp_run(), same loader GREETER.MWP was proven against.
 * Blocks the rest of the OS while the program runs, exactly like
 * mwp_run() always has -- there's no concurrent-programs concept here
 * (see mwp.h's own top-of-file comment), so control simply doesn't come
 * back to Terminal's own event loop until the program's entry function
 * returns on its own. */
static void term_cmd_run(const char *name) {
    if (!name[0]) { term_print("  usage: run <name>"); return; }
    int slot = prog_find_by_name(name);
    if (slot < 0) { term_print("  no program by that name"); return; }
    int ok = mwp_run(slot);
    if (!ok) { term_print("  failed to load or run"); return; }
    /* The program ran and returned control here -- redraw Terminal's
     * own window immediately rather than waiting for the next main-loop
     * tick, since whatever the program drew (and presented) is still
     * sitting in the backbuffer/front buffer over top of everything. */
    render_frame(-1, -1, 0);
}

/* tick -- current time and date, same rtc_read()/rtc_apply_offset()
 * pair the taskbar clock already polls every frame. */
static void term_cmd_tick(void) {
    rtc_time_t now;
    rtc_read(&now);
    rtc_time_t local = rtc_apply_offset(now, tz_offset_hours);
    char date_str[40];
    format_full_date(&local, date_str, sizeof(date_str));
    char line[TERM_COLS + 1]; u32 l = 0;
    append_str(line, &l, sizeof(line), "  ");
    append_str(line, &l, sizeof(line), date_str);
    term_print(line);
}

/* fetch -- OS name/version, screen mode, and disk-slot usage. Named
 * (and shaped, one-fact-per-line) after the neofetch/screenfetch
 * family of "here's what this machine is" tools, which is exactly what
 * this command is for -- just MiniWin-scaled: no CPU/GPU/package-count
 * detection to do when this whole OS IS the software stack. */
static void term_cmd_fetch(void) {
    term_print("  MiniWin 1.1-alpha-3");
    char line[TERM_COLS + 1]; u32 l = 0;
    append_str(line, &l, sizeof(line), "  display: ");
    append_uint(line, &l, sizeof(line), VGA_WIDTH);
    append_str(line, &l, sizeof(line), "x");
    append_uint(line, &l, sizeof(line), VGA_HEIGHT);
    append_str(line, &l, sizeof(line), " truecolor");
    term_print(line);

    int docs = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) { u32 x; if (fs_check_slot(i, &x)) docs++; }
    int progs = 0;
    for (int i = 0; i < PROG_MAX_SLOTS; i++) { u32 x; if (prog_check_slot(i, &x, 0, 0)) progs++; }
    l = 0;
    append_str(line, &l, sizeof(line), "  documents: ");
    append_uint(line, &l, sizeof(line), docs);
    append_str(line, &l, sizeof(line), "/");
    append_uint(line, &l, sizeof(line), FS_MAX_FILES);
    term_print(line);
    l = 0;
    append_str(line, &l, sizeof(line), "  programs: ");
    append_uint(line, &l, sizeof(line), progs);
    append_str(line, &l, sizeof(line), "/");
    append_uint(line, &l, sizeof(line), PROG_MAX_SLOTS);
    term_print(line);
}

static void term_cmd_wipe(void) {
    term_scroll_count = 0;
    term_scroll_head = 0;
    term_scroll_off = 0;
}


/* ============================================================
 * os-inst -- install MiniWin onto another disk / partition.
 *   os-inst ?        explain it
 *   os-inst list     the IDE disks and their partitions, with the id to use
 *   os-inst <id>     vet the target, show what would be erased, ask for YES, then install (kernel/os_inst.h)
 * The install runs as a background job (term_install_poll(), called every frame), so the screen keeps
 * updating and the progress line moves; the scrollback is 40 columns wide, so every message is short.
 * ============================================================ */
static int term_inst_pending = 0;          /* waiting for the user to type YES */
static st_target_t term_inst_target;
static int term_inst_progress_head = -1;   /* scrollback row holding the live "NN%" line, if it is still the last row */
static u32 term_inst_last_pct = 0xFFFFFFFFu;

/* word-wraps `text` into rows of at most TERM_COLS, each indented by `indent` spaces */
static void term_print_wrapped(const char *text, int indent) {
    char row[TERM_COLS + 1];
    u32 i = 0;
    while (text[i]) {
        u32 l = 0;
        for (int k = 0; k < indent; k++) row[l++] = ' ';
        u32 limit = TERM_COLS - (u32)indent, take = 0, last_space = 0;
        while (text[i + take] && take < limit) { if (text[i + take] == ' ') last_space = take; take++; }
        if (text[i + take] && last_space > 0) take = last_space;       /* break at the last space that fits */
        for (u32 k = 0; k < take; k++) row[l++] = text[i + k];
        row[l] = 0;
        term_print(row);
        i += take;
        while (text[i] == ' ') i++;
    }
}

/* "64 MB" / "2.5 GB" */
static void term_fmt_size(char *out, u32 *len, u32 cap, u32 sectors) {
    u32 mb = sectors / 2048;
    if (mb >= 10240) {
        append_uint(out, len, cap, mb / 1024);
        append_str(out, len, cap, ".");
        append_uint(out, len, cap, (mb % 1024) * 10 / 1024);
        append_str(out, len, cap, " GB");
    } else {
        append_uint(out, len, cap, mb);
        append_str(out, len, cap, " MB");
    }
}

static void term_inst_help(void) {
    term_print("os-inst - install MiniWin on a disk");
    term_print("  os-inst ?       this help");
    term_print("  os-inst list    disks and their ids");
    term_print("  os-inst <id>    install onto <id>");
    term_print("id = <type><disk><part>");
    term_print("  type  ide sata nvme scsi usb");
    term_print("  disk  1, 2, 3 ... (which disk)");
    term_print("  part  2 digits: 01 02 03 ...");
    term_print("        00 = the whole disk");
    term_print("  sata103 = sata disk 1, part 3");
    term_print("  ide200  = whole ide disk 2");
    term_print("this build drives ide and sata:");
    term_print("  ide1..4 = primary/secondary");
    term_print("            master/slave");
    term_print("  sata1.. = AHCI disks, in order");
    term_print("It copies the system, not your");
    term_print("files, and asks before writing.");
    term_print("Never the disk it is running from.");
}

static void term_inst_list(void) {
    if (!st_scanned) st_scan();
    char line[TERM_COLS + 1]; u32 l;
    int any = 0;
    for (int d = 0; d < BLK_DEV_MAX; d++) {
        int type = blk_type_of(d);
        u32 num = (u32)(d - blk_first_of(type)) + 1;
        if (!st_devs[d].present && type != ST_IDE) continue;     /* ide positions are always listed; sata disks only when found */
        char id[16]; l = 0;
        st_format_id((st_type_t)type, num, 0, id);
        { int n = 0; while (id[n]) n++; id[n - 2] = 0; }          /* "ide1" / "sata1" -- the disk, without the 2 partition digits */
        append_str(line, &l, sizeof(line), id);
        if (!st_devs[d].present) { append_str(line, &l, sizeof(line), "  (nothing)"); term_print(line); continue; }
        any = 1;
        append_str(line, &l, sizeof(line), "  ");
        term_fmt_size(line, &l, sizeof(line), st_devs[d].sectors);
        append_str(line, &l, sizeof(line), "  ");
        append_str(line, &l, sizeof(line), st_devs[d].model);
        term_print(line);
        if (d == blk_boot_dev) term_print("  <- MiniWin is running from here");
        st_part_t parts[16];
        int n = st_list_partitions(d, parts, 16);
        for (int i = 0; i < n; i++) {
            l = 0;
            st_format_id((st_type_t)type, num, (u32)parts[i].number, id);
            append_str(line, &l, sizeof(line), "  ");
            append_str(line, &l, sizeof(line), id);
            append_str(line, &l, sizeof(line), "  ");
            term_fmt_size(line, &l, sizeof(line), parts[i].sectors);
            if (parts[i].is_gpt) append_str(line, &l, sizeof(line), "  gpt");
            else if (parts[i].extended_container) append_str(line, &l, sizeof(line), "  extended");
            else {
                static const char hx[] = "0123456789ABCDEF";
                append_str(line, &l, sizeof(line), "  type ");
                char t[3] = { hx[parts[i].type >> 4], hx[parts[i].type & 15], 0 };
                append_str(line, &l, sizeof(line), t);
            }
            if (d == blk_boot_dev && st_overlap(parts[i].start, parts[i].sectors, blk_boot_base, ST_INSTALL_SECTORS))
                append_str(line, &l, sizeof(line), " (running)");
            term_print(line);
        }
        if (n == 0) term_print("  no partition table");
        l = 0;
        st_format_id((st_type_t)type, num, 0, id);
        append_str(line, &l, sizeof(line), "  whole disk: ");
        append_str(line, &l, sizeof(line), id);
        term_print(line);
    }
    if (!any) term_print("  no ide or sata disk found");
    term_print("nvme scsi usb: no driver yet");
}

static void term_inst_start(void) {
    inst_begin(&term_inst_target, st_boot_id);
    term_inst_last_pct = 0xFFFFFFFFu;
    term_inst_progress_head = -1;
    term_print("  installing... do not power off");
}

/* os-inst <id> */
static void term_cmd_osinst(const char *arg) {
    if (arg[0] == 0 || term_streq(arg, "?")) { term_inst_help(); return; }
    if (term_streq(arg, "list")) { term_inst_list(); return; }
    st_target_t t;
    int r = st_resolve(arg, &t);
    if (r != ST_OK) { term_print_wrapped(st_error_text(r), 2); return; }

    inst_probe_t pr;
    inst_probe(&t, &pr);
    char line[TERM_COLS + 1]; u32 l = 0;
    append_str(line, &l, sizeof(line), "target: ");
    append_str(line, &l, sizeof(line), t.id);
    append_str(line, &l, sizeof(line), t.whole ? "  (whole disk)" : "  (partition)");
    term_print(line);
    l = 0;
    append_str(line, &l, sizeof(line), "  ");
    append_str(line, &l, sizeof(line), st_devs[t.pos].model);
    append_str(line, &l, sizeof(line), ", ");
    term_fmt_size(line, &l, sizeof(line), t.sectors);
    term_print(line);
    l = 0;
    append_str(line, &l, sizeof(line), "  starts at sector ");
    append_uint(line, &l, sizeof(line), t.base);
    term_print(line);
    if (!pr.readable)          term_print("  warning: cannot read it first");
    else if (pr.miniwin)       term_print("  it already holds a MiniWin");
    else if (pr.non_empty)     term_print("  it holds data (not empty)");
    else                       term_print("  it looks empty");
    if (t.whole && pr.partitions > 0) {
        l = 0;
        append_str(line, &l, sizeof(line), "  destroys the partition table (");
        append_uint(line, &l, sizeof(line), (u32)pr.partitions);
        append_str(line, &l, sizeof(line), ")");
        term_print(line);
    }
    term_print("It writes 1 MB there and erases it.");
    term_print("Type YES to install; else cancels.");
    term_inst_target = t;
    term_inst_pending = 1;
}

/* Called every frame from the main loop: advances a running install and keeps the progress line current. */
static void term_install_poll(void) {
    if (!inst_active()) return;
    int phase = inst_step(24);
    if (phase == INST_PHASE_WRITE || phase == INST_PHASE_VERIFY) {
        u32 pct = inst_percent() / 5 * 5;                         /* update in 5% steps */
        if (pct != term_inst_last_pct) {
            term_inst_last_pct = pct;
            char line[TERM_COLS + 1]; u32 l = 0;
            append_str(line, &l, sizeof(line), phase == INST_PHASE_WRITE ? "  writing  " : "  checking ");
            append_uint(line, &l, sizeof(line), inst_percent());
            append_str(line, &l, sizeof(line), "%");
            int last = (term_scroll_head + TERM_SCROLLBACK_ROWS - 1) % TERM_SCROLLBACK_ROWS;
            if (term_inst_progress_head == last) {                // the progress line is still the newest row: rewrite it
                u32 i = 0; while (line[i] && i < TERM_COLS) { term_scrollback[last][i] = line[i]; i++; } term_scrollback[last][i] = 0;
            } else {
                term_print(line);
                term_inst_progress_head = (term_scroll_head + TERM_SCROLLBACK_ROWS - 1) % TERM_SCROLLBACK_ROWS;
            }
        }
    } else if (phase == INST_PHASE_DONE) {
        { int last = (term_scroll_head + TERM_SCROLLBACK_ROWS - 1) % TERM_SCROLLBACK_ROWS;
          if (term_inst_progress_head == last) { const char *f = "  checking 100%"; u32 i = 0; while (f[i]) { term_scrollback[last][i] = f[i]; i++; } term_scrollback[last][i] = 0; } }
        term_print("  done: written and verified");
        term_print_wrapped(inst_mbr_text(inst.mbr_result), 2);
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  installed on ");
        append_str(line, &l, sizeof(line), inst.t.id);
        term_print(line);
        term_print("  boot that disk to use it");
    } else if (phase == INST_PHASE_FAILED) {
        char line[TERM_COLS + 1]; u32 l = 0;
        term_print("  INSTALL FAILED");
        term_print_wrapped(inst_error_text(inst.err), 2);
        append_str(line, &l, sizeof(line), "  at sector ");
        append_uint(line, &l, sizeof(line), inst.err_lba);
        term_print(line);
        term_print("  the target is not bootable now");
    }
}

/* Runs whatever's currently in term_input_buf, echoing the command
 * itself into the scrollback first (so the transcript reads like a
 * real session -- prompt, command, output -- rather than just output
 * with no record of what triggered it), then clears the input line. */
static void term_run_input(void) {
    char echoed[TERM_COLS + 1]; u32 el = 0;
    append_str(echoed, &el, sizeof(echoed), "> ");
    append_str(echoed, &el, sizeof(echoed), term_input_buf);
    term_print(echoed);

    /* An install is waiting for its YES, or is running: those two states own the keyboard. */
    if (term_inst_pending) {
        term_inst_pending = 0;
        if (term_streq(term_input_buf, "YES")) term_inst_start();
        else term_print("  cancelled, nothing was written");
        term_input_buf[0] = 0;
        term_input_len = 0;
        return;
    }
    if (inst_active()) {
        term_print("  install running, please wait");
        term_input_buf[0] = 0;
        term_input_len = 0;
        return;
    }

    char tokens[2][TERM_INPUT_MAXLEN + 1];
    int n = term_tokenize(term_input_buf, tokens);

    if (n == 0) {
        /* blank line -- print nothing, same as a real shell */
    } else if (term_streq(tokens[0], "?")) {
        term_cmd_help();
    } else if (term_streq(tokens[0], "peek")) {
        term_cmd_peek();
    } else if (term_streq(tokens[0], "rd")) {
        term_cmd_read(n > 1 ? tokens[1] : "");
    } else if (term_streq(tokens[0], "del")) {
        term_cmd_del(n > 1 ? tokens[1] : "");
    } else if (term_streq(tokens[0], "apps")) {
        term_cmd_apps();
    } else if (term_streq(tokens[0], "run")) {
        term_cmd_run(n > 1 ? tokens[1] : "");
    } else if (term_streq(tokens[0], "tick")) {
        term_cmd_tick();
    } else if (term_streq(tokens[0], "fetch")) {
        term_cmd_fetch();
    } else if (term_streq(tokens[0], "os-inst")) {
        term_cmd_osinst(n > 1 ? tokens[1] : "");
    } else if (term_streq(tokens[0], "wipe")) {
        term_cmd_wipe();
    } else if (term_streq(tokens[0], "re")) {
        system_restart();
    } else if (term_streq(tokens[0], "off")) {
        system_shutdown();
    } else {
        char line[TERM_COLS + 1]; u32 l = 0;
        append_str(line, &l, sizeof(line), "  unknown command: ");
        append_str(line, &l, sizeof(line), tokens[0]);
        term_print(line);
        term_print("  type ? for a list of commands");
    }

    term_input_buf[0] = 0;
    term_input_len = 0;
}

static inline int term_btn_close_x(void) { return win_btn_close_x(&term_win, 2); }
static inline int term_btn_max_x(void)   { return win_btn_max_x(&term_win, 2); }
static inline int term_btn_min_x(void)   { return win_btn_min_x(&term_win, 2); }
static inline int term_btn_y(void)       { return win_btn_y(&term_win, 1); }

static int term_close_hit(int px, int py) { return in_rect(px, py, term_btn_close_x(), term_btn_y(), BTN_W, BTN_H); }
static int term_min_hit(int px, int py)   { return in_rect(px, py, term_btn_min_x(),   term_btn_y(), BTN_W, BTN_H); }
static int term_max_hit(int px, int py)   { return in_rect(px, py, term_btn_max_x(),   term_btn_y(), BTN_W, BTN_H); }

static int term_titlebar_drag_hit(int px, int py) {
    if (!in_rect(px, py, term_win.x + 1, term_win.y + 1, term_win.w - 2, TITLEBAR_H)) return 0;
    if (in_rect(px, py, term_btn_min_x(), term_btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, term_btn_max_x(), term_btn_y(), BTN_W, BTN_H)) return 0;
    if (in_rect(px, py, term_btn_close_x(), term_btn_y(), BTN_W, BTN_H)) return 0;
    return 1;
}

/* Input row sits at the bottom of the window (below the scrollback),
 * same "sunken text field" bevel language as WEB.MWP's URL bar --
 * see that window's own comment for why sunken reads as "typeable." */
#define TERM_INPUT_H 18
static inline int term_input_y(void) { return term_win.y + term_win.h - TERM_INPUT_H - 3; }
static inline int term_input_x(void) { return term_win.x + 3; }
static inline int term_input_w(void) { return term_win.w - 6; }
static int term_input_hit(int px, int py) {
    return in_rect(px, py, term_input_x(), term_input_y(), term_input_w(), TERM_INPUT_H);
}

static inline int term_output_y(void) { return term_win.y + TITLEBAR_H + 4; }
static inline int term_output_h(void) { return term_input_y() - term_output_y() - 3; }

static void draw_terminal_window(void) {
    int wx = term_win.x, wy = term_win.y, ww = term_win.w, wh = term_win.h;

    ui_window_frame(wx, wy, ww, wh, term_win.maximized, "TERMINAL.MWP");
    ui_titlebar_buttons(term_btn_min_x(), term_btn_max_x(), term_btn_close_x(), term_btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == WIN_ID_TERMINAL,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == WIN_ID_TERMINAL,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == WIN_ID_TERMINAL);

    /* Scrollback: black background (classic terminal look, and a clean
     * visual break from every other window's light-gray body), showing
     * the most recent TERM_VISIBLE_ROWS lines. */
    int oy = term_output_y();
    int oh = term_output_h();
    bb_fillrect(wx + 2, oy, ww - 4, oh, TH_CONSOLE);

    int visible = term_scroll_count < TERM_VISIBLE_ROWS ? term_scroll_count : TERM_VISIBLE_ROWS;
    int start = (term_scroll_head - term_scroll_off - visible + 2 * TERM_SCROLLBACK_ROWS) % TERM_SCROLLBACK_ROWS;
    for (int i = 0; i < visible; i++) {
        int row = (start + i) % TERM_SCROLLBACK_ROWS;
        font_draw_string(wx + 4, oy + 2 + i * (FONT_CELL + 1), term_scrollback[row], TH_CONSOLE_TEXT);
    }

    /* Input row: sunken field, "> " prompt drawn as part of the same
     * string so it scrolls off the left edge along with long input
     * rather than staying pinned while text runs under it. */
    int ix = term_input_x(), iy = term_input_y(), iw = term_input_w();
    ui_field(ix, iy, iw, TERM_INPUT_H, TH_CONSOLE, term_input_focused);

    char prompt_line[TERM_INPUT_MAXLEN + 3]; u32 pl = 0;
    append_str(prompt_line, &pl, sizeof(prompt_line), "> ");
    append_str(prompt_line, &pl, sizeof(prompt_line), term_input_buf);
    font_draw_string(ix + 3, iy + 3, prompt_line, TH_CONSOLE_TEXT);
    if (term_input_focused) {
        bb_fillrect(ix + 3 + (2 + (int)term_input_len) * FONT_CELL, iy + 3, 2, FONT_CELL, TH_CONSOLE_TEXT);
    }
}

#endif
