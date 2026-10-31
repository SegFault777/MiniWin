/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_APPS_TERMINAL_H
#define MW_APPS_TERMINAL_H

/* apps/terminal.h -- MINISHELL.MWP: the command-line app and its commands.
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
 * MiniShell.mwp -- MiniWin's command-line app. Built into the kernel
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
/* THE SHELL'S GRID (1.1-alpha-12). Until alpha-11 the shell was a fixed 40 x 14 grid painted at the top-left of whatever window it
 * lived in: maximize it and a 40-column strip of text sat in a sea of black; shrink it and the same strip ran off the frame. Now
 * the grid follows the window:
 *   - TERM_COLS is the WIDEST line the scrollback can hold (128 columns; the largest screen MiniWin starts on is under 1300 px
 *     wide, which is 116 cells), and a line is stored at the width it was printed at;
 *   - how many rows and columns are SHOWN comes from the window's size (term_visible_rows / term_vis_cols), so a maximized shell
 *     uses all of its space and its prose (`help`, `cat`, wrapped messages) re-flows to the current width (term_wrap_cols);
 *   - when the window is narrower than the longest line, a HORIZONTAL scroll bar appears under the text (ui/scrollbar.h) and the
 *     view slides across the columns, so nothing is ever drawn past the frame.
 * The scrollback lives on the heap (mm.h) because 256 rows x 129 bytes is 33 KB and .bss has no such room to spare. */
#define TERM_COLS        128
#define TERM_COLS_MIN    40   /* the narrowest width prose is wrapped to (a smaller window scrolls sideways instead) */
#define TERM_SCROLLBACK_ROWS 256  /* ring buffer -- old lines just get
                                   * overwritten once this fills, no
                                   * "scrollback full" error to handle */
#define TERM_INPUT_MAXLEN 120

static int term_visible_rows(void);                  /* geometry: defined with the window's other geometry below */
static int term_vis_cols(void);
static int term_wrap_cols(void);
static void term_print_wrapped(const char *text, int indent);   /* defined further down; wraps to the window's current width */

/* One line of scrollback, fixed-width and NUL-terminated -- plain
 * ASCII only (same as the URL bar; see MINIWEB.MWP's own reasoning for why
 * user-typed command-line text doesn't route through the Hangul IME).
 * A ring buffer (term_scroll_head wraps around TERM_SCROLLBACK_ROWS)
 * rather than a growable log -- the ring itself is one kmalloc'd block, allocated by the first line that is printed. */
static char (*term_sb)[TERM_COLS + 1] = 0;
#define term_scrollback term_sb
static int  term_scroll_count = 0;  /* how many rows have EVER been written
                                     * (caps at TERM_SCROLLBACK_ROWS; used
                                     * to know how far back there's real
                                     * content vs. never-written rows) */
static int  term_scroll_head = 0;   /* ring-buffer write cursor */
static int  term_scroll_off = 0;    /* how many rows the view is scrolled back from the newest line (Up/Down keys); any
                                     * new output snaps it back to 0 so fresh output is never hidden */
static int  term_hscroll = 0;       /* how many columns the view is scrolled to the right (0 = the left edge) */

static int term_sb_ensure(void) {
    if (!term_sb) {
        term_sb = (char (*)[TERM_COLS + 1])kzalloc((u32)TERM_SCROLLBACK_ROWS * (TERM_COLS + 1));
    }
    return term_sb != 0;
}

static char term_input_buf[TERM_INPUT_MAXLEN + 1];
static u32  term_input_len = 0;
static int  term_input_focused = 0;

/* Appends one line to the scrollback ring buffer, truncating at
 * TERM_COLS if it's too long to fit a row rather than wrapping it --
 * simpler than word-wrap, and every message this file actually prints
 * was written short enough not to need it. */
static void term_print(const char *s) {
    if (!term_sb_ensure()) return;
    char *row = term_scrollback[term_scroll_head];
    u32 i = 0;
    while (s[i] && i < TERM_COLS) { row[i] = s[i]; i++; }
    row[i] = 0;
    term_scroll_head = (term_scroll_head + 1) % TERM_SCROLLBACK_ROWS;
    if (term_scroll_count < TERM_SCROLLBACK_ROWS) term_scroll_count++;
    term_scroll_off = 0; term_hscroll = 0;
}

/* The longest line the scrollback holds (what the horizontal bar scrolls across). Scans only the rows that exist; lines are
 * short, so this is a few thousand byte reads -- nothing next to drawing one frame. */
static int term_max_len(void) {
    if (!term_sb) return 0;
    int best = 0;
    for (int r = 0; r < term_scroll_count; r++) { int n = 0; const char *p = term_sb[r]; while (p[n]) n++; if (n > best) best = n; }
    return best;
}

/* Up/Down: move the view by `d` rows (positive = back in time), clamped to what exists. */
static void term_scroll_by(int d) {
    int max_off = term_scroll_count - term_visible_rows();
    if (max_off < 0) max_off = 0;
    term_scroll_off += d;
    if (term_scroll_off < 0) term_scroll_off = 0;
    if (term_scroll_off > max_off) term_scroll_off = max_off;
}

static void term_print_blank(void) { term_print(""); }

/* Splits the input line into up to 5 whitespace-separated tokens (a "double-quoted" token may contain spaces)
 * in-place-ish (writes into caller-owned buffers) -- every command
 * here takes at most one argument (a name), so this never needed to
 * be a real argv[]/argc parser. Returns the token count. */
static int term_tokenize(const char *s, char tokens[5][TERM_INPUT_MAXLEN + 1]) {
    int count = 0;
    u32 i = 0;
    while (count < 5) {
        while (s[i] == ' ') i++;
        if (!s[i]) break;
        u32 j = 0;
        if (s[i] == '"') {                                  /* "DOC (1).TXT": a quoted token may hold spaces */
            i++;
            while (s[i] && s[i] != '"' && j < TERM_INPUT_MAXLEN) tokens[count][j++] = s[i++];
            if (s[i] == '"') i++;
        } else {
            while (s[i] && s[i] != ' ' && j < TERM_INPUT_MAXLEN) tokens[count][j++] = s[i++];
        }
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
/* Does the typed word match one of the space-separated names? (Unix name first, then the Windows one, then MiniWin's own.) */
static int term_cmd_is(const char *typed, const char *names) {
    u32 i = 0;
    while (names[i]) {
        char word[24]; u32 w = 0;
        while (names[i] && names[i] != ' ' && w < sizeof(word) - 1) word[w++] = names[i++];
        word[w] = 0;
        while (names[i] == ' ') i++;
        if (term_streq(typed, word)) return 1;
    }
    return 0;
}


/* ============================================================ ACCOUNTS (1.1-alpha-13): root and user
 * The shell now has two accounts. They do not differ in what they may do yet -- that comes with the kernel work that follows (ring 3,
 * processes) -- but every session has one, the prompt shows it ("~#" is root, "~$" is a user) and the plumbing is in place:
 *   - The FIRST time MiniShell is opened on an image that has no root password, it asks for one (twice, to be sure, with the keys
 *     shown as *), stores it, and the session is root.
 *   - Closing the window ends the session. The next time MiniShell is opened it starts as a USER, and the command `root` asks for the
 *     password and makes it root; `user` goes back.
 * The password is kept in /sys/ROOT.PWD as 16 bytes of salt and a SHA-256 hash of salt + password, stretched over 1000 rounds, both in
 * hex (96 characters): the password itself is never written anywhere. Nothing here stops someone who can write the disk image from
 * deleting that file -- no password scheme can, without encrypting the volume. ============================================================ */
#define TERM_PW_FILE "/sys/ROOT.PWD"
enum { TP_NONE, TP_NEW, TP_CONFIRM, TP_LOGIN };           /* what the input line is asking for */
static int  term_pw_mode = TP_NONE;                       /* TP_NONE: an ordinary command line */
static char term_pw_first[TERM_INPUT_MAXLEN + 1];         /* the first entry, while the confirmation is being typed */
static int  term_is_root = 0;                             /* the session's account */
static int  term_session_on = 0;                          /* 1 from the first frame after the window opens until it is closed */

static void term_pw_hex(char *o, const u8 *b, int n) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { o[2 * i] = h[b[i] >> 4]; o[2 * i + 1] = h[b[i] & 15]; }
}
static int term_pw_unhex(const char *s, u8 *b, int n) {
    for (int i = 0; i < n; i++) {
        int v[2];
        for (int k = 0; k < 2; k++) { char c = s[2 * i + k]; v[k] = c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1); if (v[k] < 0) return 0; }
        b[i] = (u8)(v[0] << 4 | v[1]);
    }
    return 1;
}
static void term_pw_hash(const u8 salt[16], const char *pw, u8 out[32]) {
    u32 pl = 0; while (pw[pl]) pl++;
    sha256_ctx_t c;
    sha256_init(&c); sha256_update(&c, salt, 16); sha256_update(&c, (const u8 *)pw, pl); sha256_final(&c, out);
    for (int r = 1; r < 1000; r++) {                      /* stretching: a guess costs 1000 hashes instead of one */
        sha256_init(&c); sha256_update(&c, out, 32); sha256_update(&c, salt, 16); sha256_update(&c, (const u8 *)pw, pl); sha256_final(&c, out);
    }
}
static int term_pw_load(u8 salt[16], u8 hash[32]) {       /* 1 when a well-formed password file exists */
    u8 buf[128]; u32 len = 0;
    if (vfs_read(TERM_PW_FILE, buf, sizeof buf, &len, 0) != MWFS_OK || len < 96) return 0;
    return term_pw_unhex((const char *)buf, salt, 16) && term_pw_unhex((const char *)buf + 32, hash, 32);
}
static int term_pw_is_set(void) { u8 s[16], h[32]; return term_pw_load(s, h); }
static int term_pw_check(const char *pw) {
    u8 salt[16], want[32], got[32];
    if (!term_pw_load(salt, want)) return 0;
    term_pw_hash(salt, pw, got);
    u8 diff = 0; for (int i = 0; i < 32; i++) diff |= (u8)(want[i] ^ got[i]);           /* no early exit */
    return diff == 0;
}
static int term_pw_store(const char *pw) {
    u8 salt[16], h[32]; char file[96];
    tls_rng_mix_hw(); tls_rng_bytes(salt, 16);
    term_pw_hash(salt, pw, h);
    term_pw_hex(file, salt, 16); term_pw_hex(file + 32, h, 32);
    return vfs_write(TERM_PW_FILE, (const u8 *)file, 96, 0) == MWFS_OK;
}

static void term_session_end(void) {                      /* the window was closed */
    term_session_on = 0; term_is_root = 0; term_pw_mode = TP_NONE; term_pw_first[0] = 0;
    term_input_buf[0] = 0; term_input_len = 0;
}
static void term_session_begin(void) {                    /* the first frame of a new session */
    term_session_on = 1; term_is_root = 0; term_pw_mode = TP_NONE;
    if (!term_pw_is_set()) {
        term_print_wrapped("First start: there is no root password yet. Choose one for the root account; it is asked twice.", 0);
        term_pw_mode = TP_NEW;
    } else {
        term_print_wrapped("user session: type root for the root account", 0);
    }
}
/* Enter was pressed while the input line was asking for a password. The text is never echoed into the scrollback. */
static void term_pw_submit(void) {
    if (term_pw_mode == TP_NEW) {
        if (term_input_len == 0) { term_print("  a password cannot be empty"); }
        else { for (u32 i = 0; i <= term_input_len; i++) term_pw_first[i] = term_input_buf[i]; term_pw_mode = TP_CONFIRM; }
    } else if (term_pw_mode == TP_CONFIRM) {
        int same = 1; for (u32 i = 0; i <= term_input_len; i++) if (term_pw_first[i] != term_input_buf[i]) same = 0;
        if (!same || term_pw_first[term_input_len] != 0) { term_print("  the two do not match -- start again"); term_pw_mode = TP_NEW; }
        else {
            if (term_pw_store(term_input_buf)) term_print("  root password set. You are root.");
            else term_print_wrapped("The password could not be saved (is the disk full or read-only?). You are root for this session; the next one will ask again.", 2);
            term_is_root = 1; term_pw_mode = TP_NONE;
        }
        for (u32 i = 0; i < sizeof term_pw_first; i++) term_pw_first[i] = 0;
    } else {                                              /* TP_LOGIN */
        if (term_pw_check(term_input_buf)) { term_is_root = 1; term_print("  you are root now"); }
        else term_print("  wrong password");
        term_pw_mode = TP_NONE;
    }
    for (u32 i = 0; i < sizeof term_input_buf; i++) term_input_buf[i] = 0;
    term_input_len = 0;
}

static void term_cmd_help(void) {
    term_print("?          - show this list");
    term_print("root       - become the root account");
    term_print("user       - go back to the user account");
    term_print("ls [dir]   - list a directory  (dir)");
    term_print("cd <dir>   - change directory (cd = /)");
    term_print("pwd        - show the current directory");
    term_print("mkdir <d>  - make a directory  (md)");
    term_print("touch <f>  - make an empty file");
    term_print("cat <file> - show a text file  (type)");
    term_print("cp <a> <b> - copy a file       (copy)");
    term_print("mv <a> <b> - move or rename    (move)");
    term_print("rm <path>  - delete a file or an empty");
    term_print("             directory (del <path>)");
    term_print("peek       - list saved documents");
    term_print("rd <n>     - read document n (see peek)");
    term_print("del <n>    - delete document n");
    term_print("apps       - list installed programs");
    term_print_wrapped("run <nm> [args] - run a program in a window (-f: full screen)", 0);
    term_print("tick       - show the time and date");
    term_print("fetch      - show system information");
    term_print("mem [test] - memory: RAM, pages, heap");
    term_print("part       - partitions (part ?)");
    term_print("lsusb      - list USB devices  (plug)");
    term_print("os-inst    - install MiniWin (os-inst ?)");
    term_print("clear      - clear this screen (cls)");
    term_print("reboot     - restart the machine");
    term_print("shutdown   - power off the machine");
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
        append_str(line, &l, sizeof(line), fs_doc_name(i));
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
static void term_cmd_cat(const char *arg);                  /* shell_fs.h: shows a file by path */
static void term_cmd_read(const char *arg) {
    for (int i = 0; arg[i]; i++) if (arg[i] < '0' || arg[i] > '9') { term_cmd_cat(arg); return; }     /* not a number: a path */
    int n = 0;
    for (int i = 0; arg[i]; i++) {
        if (arg[i] < '0' || arg[i] > '9') { term_print("  bad slot number"); return; }
        n = n * 10 + (arg[i] - '0');
    }
    if (n < 1 || n > FS_MAX_FILES) { term_print("  no such document number (see peek)"); return; }
    int slot = n - 1;
    u32 len;
    if (!fs_check_slot(slot, &len)) { term_print("  that slot is empty"); return; }

    static u8 buf[FS_MAX_FILE_BYTES];
    u32 got = 0;
    if (!fs_read_slot(slot, (char *)buf, sizeof(buf), &got)) { term_print("  read error: the file is damaged"); return; }
    char line[TERM_COLS + 1]; u32 l = 0;
    for (u32 i = 0; i < got; i++) {
        if (buf[i] == '\n' || l >= (u32)term_wrap_cols()) {
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
static void term_cmd_zap(const char *arg);                    /* shell_fs.h: removes a file or an empty directory by path */
static void term_cmd_del(const char *arg) {
    int n = 0;
    for (int i = 0; arg[i]; i++) {
        if (arg[i] < '0' || arg[i] > '9') { term_cmd_zap(arg); return; }       /* not a number: a path, like `rm` */
        n = n * 10 + (arg[i] - '0');
    }
    if (n < 1 || n > FS_MAX_FILES) { term_print("  no such document number (see peek)"); return; }
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
static void term_cmd_run(const char *a1, const char *a2, const char *a3, const char *a4) {
    int full = (a1[0] == '-' && (a1[1] == 'f' || a1[1] == 'F') && a1[2] == 0);     /* run -f NAME: the old way, the program takes the whole screen */
    const char *name = full ? a2 : a1;
    if (!name[0]) { term_print("  usage: run [-f] <name> [args]"); term_print("  (in a window; -f = full screen)"); return; }
    int slot = prog_find_by_name(name);
    if (slot < 0) { term_print("  no program by that name"); return; }
    char args[64]; u32 al = 0; args[0] = 0;                                         /* what follows the name, as one string for sys.args */
    const char *rest[3] = { full ? a3 : a2, full ? a4 : a3, full ? "" : a4 };
    for (int i = 0; i < 3; i++) {
        if (!rest[i][0]) continue;
        if (al) kstrcpy_append(args, &al, sizeof args, ' ');
        append_str(args, &al, sizeof args, rest[i]);
    }
    if (full) {
        if (pw_running()) { term_print("  close the program in the window first"); return; }
        if (!mwp_run(slot)) { term_print(mwp_load_error ? mwp_load_error : "  failed to load or run"); return; }
        /* The program ran and returned control here -- redraw immediately rather than waiting for the next main-loop tick, since
         * whatever the program drew (and presented) is still sitting in the backbuffer/front buffer over top of everything. */
        render_frame(-1, -1, 0);
        return;
    }
    const char *why = pw_launch_args(slot, prog_name_of(slot), args);
    if (why) { char m[TERM_COLS + 1]; u32 l = 0; append_str(m, &l, sizeof(m), "  "); append_str(m, &l, sizeof(m), why); term_print(m); return; }
    term_print("  started in a window");
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
/* ---- fetch: a fastfetch-style summary. The Terminal is 40 columns wide, so no logo beside the text: key, then value. ---- */
static void term_fetch_kv(const char *key, const char *val) {
    char line[TERM_COLS + 1]; u32 l = 0;
    append_str(line, &l, sizeof(line), "  ");
    append_str(line, &l, sizeof(line), key);
    while (l < 11) line[l++] = ' ';
    line[l] = 0;
    append_str(line, &l, sizeof(line), val);
    term_print(line);
}
/* "a.b" for a size given in KB, in MiB with one decimal ("1.2", "16.0") */
static void term_fetch_mib(char *out, u32 *l, u32 cap, u32 kb) {
    u32 tenths = (kb * 10u + 512u) / 1024u;
    append_uint(out, l, cap, (int)(tenths / 10u));
    append_str(out, l, cap, ".");
    append_uint(out, l, cap, (int)(tenths % 10u));
}

/* mem -- what the memory manager (mm.h) has: the RAM the BIOS reported, the managed pages, the heap, and the call counters.
 * `mem test` allocates 64 blocks of mixed sizes, fills each with its own pattern, frees them in a scrambled order and walks the
 * heap -- a quick way to see on real hardware that the allocator works there too. */
static void term_kb_text(char *out, u32 cap, u32 kb) {
    u32 l = 0; out[0] = 0;
    if (kb >= 10240u) { append_uint(out, &l, cap, (int)(kb / 1024u)); append_str(out, &l, cap, " MB"); }
    else { append_uint(out, &l, cap, (int)kb); append_str(out, &l, cap, " KB"); }
}
static void term_cmd_mem(const char *arg) {
    char v[TERM_COLS + 1], t1[16], t2[16]; u32 l;
    (void)t2;
    mm_stats_t s; mm_stats(&s);
    if (arg[0] && term_streq(arg, "test")) {
        void *p[64]; u32 n[64]; int bad = 0, got = 0;
        for (int i = 0; i < 64; i++) {
            n[i] = 24u + (u32)i * 57u + (u32)((i * i) % 7) * 900u;
            p[i] = kmalloc(n[i]);
            if (!p[i]) continue;
            got++;
            for (u32 k = 0; k < n[i]; k++) ((u8 *)p[i])[k] = (u8)(i + 1);
        }
        for (int step = 0; step < 64; step++) {                 /* a fixed shuffle: 0, 37, 10, 47, ... */
            int i = (step * 37) % 64;
            if (!p[i]) continue;
            for (u32 k = 0; k < n[i]; k++) if (((u8 *)p[i])[k] != (u8)(i + 1)) { bad++; break; }
            if (!kfree(p[i])) bad++;
            p[i] = 0;
        }
        int problems = mm_check();
        l = 0; append_uint(v, &l, sizeof(v), got); append_str(v, &l, sizeof(v), "/64 blocks, ");
        append_uint(v, &l, sizeof(v), bad); append_str(v, &l, sizeof(v), " damaged, heap check: ");
        if (problems) { append_uint(v, &l, sizeof(v), problems); append_str(v, &l, sizeof(v), " problem(s)"); } else append_str(v, &l, sizeof(v), "ok");
        term_print(v);
        return;
    }
    u32 ram_kb = *(volatile u32 *)0x9200;
    term_kb_text(t1, sizeof t1, ram_kb);
    term_fetch_kv("RAM", t1);
    term_kb_text(t1, sizeof t1, s.total_pages * 4u);
    l = 0; append_str(v, &l, sizeof(v), t1); append_str(v, &l, sizeof(v), ", free "); append_uint(v, &l, sizeof(v), (int)(s.free_pages * 4u)); append_str(v, &l, sizeof(v), " KB");
    term_fetch_kv("Pages", v);
    l = 0; append_uint(v, &l, sizeof(v), (int)s.heap_chunks); append_str(v, &l, sizeof(v), " chunk(s), ");
    term_kb_text(t1, sizeof t1, (s.heap_used + 1023u) / 1024u); append_str(v, &l, sizeof(v), t1); append_str(v, &l, sizeof(v), " used");
    term_fetch_kv("Heap", v);
    term_kb_text(t1, sizeof t1, s.heap_largest_free / 1024u);
    l = 0; append_str(v, &l, sizeof(v), "largest free block "); append_str(v, &l, sizeof(v), t1);
    term_fetch_kv("", v);
    l = 0; append_uint(v, &l, sizeof(v), (int)s.allocs); append_str(v, &l, sizeof(v), " allocs, "); append_uint(v, &l, sizeof(v), (int)s.frees);
    append_str(v, &l, sizeof(v), " frees, "); append_uint(v, &l, sizeof(v), (int)(s.bad_frees + s.failed_allocs)); append_str(v, &l, sizeof(v), " refused");
    term_fetch_kv("Calls", v);
    {
        char pgs[64]; u32 pl = 0; pgs[0] = 0;
        if (!pg_enabled) append_str(pgs, &pl, sizeof pgs, "off");
        else {
            append_str(pgs, &pl, sizeof pgs, "on, ");
            append_uint(pgs, &pl, sizeof pgs, (int)pg_ro_pages); append_str(pgs, &pl, sizeof pgs, " RO, "); append_uint(pgs, &pl, sizeof pgs, (int)pg_guards); append_str(pgs, &pl, sizeof pgs, " guards");
        }
        term_fetch_kv("Paging", pgs);
    }
    int problems = mm_check();
    term_fetch_kv("Check", problems ? "PROBLEMS FOUND" : "heap and pages consistent");
}

static void term_cmd_fetch(void) {
    char v[TERM_COLS + 1]; u32 l;
    term_print("  miniwin@terminal");
    term_print("  ----------------");
    term_fetch_kv("OS", "MiniWin " MW_VERSION " i386");

    u32 s = net_wall_elapsed(sysinfo_boot_s);
    l = 0;
    if (s >= 3600u) { append_uint(v, &l, sizeof(v), (int)(s / 3600u)); append_str(v, &l, sizeof(v), " h "); }
    if (s >= 60u) { append_uint(v, &l, sizeof(v), (int)((s / 60u) % 60u)); append_str(v, &l, sizeof(v), " min "); }
    append_uint(v, &l, sizeof(v), (int)(s % 60u)); append_str(v, &l, sizeof(v), " s");
    term_fetch_kv("Uptime", v);

    term_fetch_kv("Shell", "MiniShell.mwp");
    l = 0;
    append_uint(v, &l, sizeof(v), VGA_WIDTH); append_str(v, &l, sizeof(v), "x"); append_uint(v, &l, sizeof(v), VGA_HEIGHT);
    append_str(v, &l, sizeof(v), " truecolor");
    term_fetch_kv("Display", v);
    l = 0;
    append_str(v, &l, sizeof(v), TH_NAME); append_str(v, &l, sizeof(v), ", ");
    append_str(v, &l, sizeof(v), sys_language == LANG_KOREAN ? "Korean" : "English");
    term_fetch_kv("Theme", v);

    char cpu[49];
    sysinfo_cpu_name(cpu);
    {                                                   /* the brand string can be 40+ characters: two rows if needed */
        int n = 0; while (cpu[n]) n++;
        if (n <= 29) term_fetch_kv("CPU", cpu);
        else {
            char first[30]; int cut = 29;
            while (cut > 12 && cpu[cut] != ' ') cut--;
            for (int i = 0; i < cut; i++) first[i] = cpu[i];
            first[cut] = 0;
            term_fetch_kv("CPU", first);
            term_fetch_kv("", cpu + cut + (cpu[cut] == ' ' ? 1 : 0));
        }
    }
    l = 0;
    u32 mhz = sysinfo_cpu_mhz();
    if (mhz) { append_uint(v, &l, sizeof(v), (int)mhz); append_str(v, &l, sizeof(v), " MHz"); }
    else append_str(v, &l, sizeof(v), "speed unknown");
    int load = sysinfo_cpu_load();
    if (load >= 0) { append_str(v, &l, sizeof(v), ", load "); append_uint(v, &l, sizeof(v), load); append_str(v, &l, sizeof(v), "%"); }
    term_fetch_kv("", v);

    char gpu[40];
    sysinfo_gpu_name(gpu);
    term_fetch_kv("GPU", gpu);
    l = 0;
    u32 vram = sysinfo_vram_kb();
    u32 fb_kb = (u32)VGA_WIDTH * (u32)VGA_HEIGHT * 4u / 1024u;
    if (vram) { append_str(v, &l, sizeof(v), "VRAM "); term_fetch_mib(v, &l, sizeof(v), fb_kb); append_str(v, &l, sizeof(v), " / ");
                term_fetch_mib(v, &l, sizeof(v), vram); append_str(v, &l, sizeof(v), " MiB in use"); }
    else { append_str(v, &l, sizeof(v), "VRAM size unknown, "); term_fetch_mib(v, &l, sizeof(v), fb_kb); append_str(v, &l, sizeof(v), " MiB shown"); }
    term_fetch_kv("", v);

    l = 0;
    u32 ram = sysinfo_ram_kb();
    term_fetch_mib(v, &l, sizeof(v), sysinfo_ram_reserved_kb()); append_str(v, &l, sizeof(v), " reserved / ");
    if (ram) { term_fetch_mib(v, &l, sizeof(v), ram); append_str(v, &l, sizeof(v), " MiB"); } else append_str(v, &l, sizeof(v), "? MiB");
    term_fetch_kv("Memory", v);

    if (!st_scanned) st_scan();
    {
        int type = blk_type_of(blk_boot_dev);
        char id[16];
        if (blk_boot_dev == BLK_RAM_DEV) { id[0] = 'r'; id[1] = 'a'; id[2] = 'm'; id[3] = 0; }          /* a live boot: the disc image in RAM */
        else {
            st_format_id((st_type_t)type, (u32)(blk_boot_dev - blk_first_of(type)) + 1, 0, id);
            { int n = 0; while (id[n]) n++; id[n - 2] = 0; }
        }
        l = 0;
        append_str(v, &l, sizeof(v), id); append_str(v, &l, sizeof(v), " ");
        term_fetch_mib(v, &l, sizeof(v), st_devs[blk_boot_dev].sectors / 2u); append_str(v, &l, sizeof(v), " MiB (boot)");
        term_fetch_kv("Disk", v);
    }
    l = 0;
    if (net_cfg.ready) {
        u32 ip = net_cfg.my_ip;
        append_uint(v, &l, sizeof(v), (int)((ip >> 24) & 0xFF)); append_str(v, &l, sizeof(v), ".");
        append_uint(v, &l, sizeof(v), (int)((ip >> 16) & 0xFF)); append_str(v, &l, sizeof(v), ".");
        append_uint(v, &l, sizeof(v), (int)((ip >> 8) & 0xFF));  append_str(v, &l, sizeof(v), ".");
        append_uint(v, &l, sizeof(v), (int)(ip & 0xFF));
    } else append_str(v, &l, sizeof(v), "offline");
    term_fetch_kv("Network", v);

    int docs = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) { u32 x; if (fs_check_slot(i, &x)) docs++; }
    int progs = 0;
    for (int i = 0; i < PROG_MAX_SLOTS; i++) { u32 x; if (prog_check_slot(i, &x, 0, 0)) progs++; }
    l = 0;
    append_uint(v, &l, sizeof(v), docs); append_str(v, &l, sizeof(v), "/"); append_uint(v, &l, sizeof(v), FS_MAX_FILES);
    append_str(v, &l, sizeof(v), " docs, ");
    append_uint(v, &l, sizeof(v), progs); append_str(v, &l, sizeof(v), "/"); append_uint(v, &l, sizeof(v), PROG_MAX_SLOTS);
    append_str(v, &l, sizeof(v), " programs");
    term_fetch_kv("Files", v);
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

/* word-wraps `text` into rows of at most term_wrap_cols() (the window's current width, never under 40), each indented by `indent` spaces */
static void term_print_wrapped(const char *text, int indent) {
    char row[TERM_COLS + 1];
    u32 i = 0;
    while (text[i]) {
        u32 l = 0;
        for (int k = 0; k < indent; k++) row[l++] = ' ';
        u32 limit = (u32)term_wrap_cols() - (u32)indent, take = 0, last_space = 0;
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

#include "shell_fs.h"
#include "shell_part.h"

static void term_inst_help(void) {
    term_print("os-inst - install MiniWin on a disk");
    term_print("  os-inst ?       this help");
    term_print("  os-inst list    disks and their ids");
    term_print("  os-inst <id> <arch>");
    term_print("                  install onto <id>");
    term_print("arch = i386 (32-bit; x86_64 later)");
    term_print("id = <type><disk><part>");
    term_print("  type  ide sata nvme scsi usb");
    term_print("  disk  1, 2, 3 ... (which disk)");
    term_print("  part  2 digits: 01 02 03 ...");
    term_print("        00 = the whole disk");
    term_print("  sata103 = sata disk 1, part 3");
    term_print("  ide200  = whole ide disk 2");
    term_print("this build drives ide sata scsi nvme:");
    term_print("  ide1..4 = primary/secondary");
    term_print("            master/slave");
    term_print("  sata1.. = AHCI disks, in order");
    term_print("  scsi1.. = LSI Logic (MPT) disks");
    term_print("  nvme1.. = NVMe SSDs (namespace 1)");
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
    if (!any) term_print("  no ide/sata/scsi/nvme disk found");
    term_print("usb: keyboards/mice only (lsusb)");
}

static void term_inst_start(void) {
    inst_begin(&term_inst_target, st_boot_id);
    term_inst_last_pct = 0xFFFFFFFFu;
    term_inst_progress_head = -1;
    term_print("  installing... do not power off");
}

/* os-inst <id> */
static void term_cmd_osinst(const char *arg, const char *arch) {
    if (arg[0] == 0 || term_streq(arg, "?")) { term_inst_help(); return; }
    if (term_streq(arg, "list")) { term_inst_list(); return; }
    if (arch[0]) {
        int a = inst_arch_check(arch);
        if (a < 0) { char l2[TERM_COLS + 1]; u32 k = 0; append_str(l2, &k, sizeof(l2), "  unknown architecture: "); append_str(l2, &k, sizeof(l2), arch); term_print(l2); term_print_wrapped("arch: i386  (x86_64: not available yet)", 2); return; }
        if (a == 0) { char l2[TERM_COLS + 1]; u32 k = 0; append_str(l2, &k, sizeof(l2), "  cannot install for "); append_str(l2, &k, sizeof(l2), arch); term_print(l2); term_print_wrapped("MiniWin is a 32-bit (i386) system; there is no 64-bit kernel yet, so only i386 installs can be made.", 2); return; }
    }
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
    term_print("It writes MiniWin + a new empty");
    term_print("filesystem there and erases it.");
    term_print("Type YES to install; else cancels.");
    term_inst_target = t;
    term_inst_pending = 1;
}

/* Called every frame from the main loop: advances a running install and keeps the progress line current. */
static void term_install_poll(void) {
    if (!inst_active() || inst_gui_owns) return;          /* (the INSTALL window of the live disc steps its own job) */
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
    if (term_pw_mode != TP_NONE) { term_pw_submit(); return; }          /* a password is being typed: not a command, and never echoed */
    char prompt[40];
    term_prompt_str(prompt, sizeof prompt);
    append_str(echoed, &el, sizeof(echoed), prompt);
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

    if (part_pending.active) {                                  /* a format/delete/merge is waiting for its YES */
        if (term_streq(term_input_buf, "YES")) part_exec_pending();
        else { part_pending.active = 0; term_print("  cancelled, nothing was changed"); }
        term_input_buf[0] = 0;
        term_input_len = 0;
        return;
    }
    char tokens[5][TERM_INPUT_MAXLEN + 1];
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
        term_cmd_run(n > 1 ? tokens[1] : "", n > 2 ? tokens[2] : "", n > 3 ? tokens[3] : "", n > 4 ? tokens[4] : "");
    } else if (term_streq(tokens[0], "part")) {
        term_cmd_part(n > 1 ? tokens[1] : "", n > 2 ? tokens[2] : "", n > 3 ? tokens[3] : "", n > 4 ? tokens[4] : "");
    } else if (term_cmd_is(tokens[0], "lsusb plug")) {
        term_cmd_usb();
    } else if (term_cmd_is(tokens[0], "pwd where")) {
        term_cmd_where();
    } else if (term_cmd_is(tokens[0], "cd go")) {
        term_cmd_go(n > 1 ? tokens[1] : "");
    } else if (term_cmd_is(tokens[0], "ls dir look")) {
        term_cmd_look(n > 1 ? tokens[1] : "");
    } else if (term_cmd_is(tokens[0], "mkdir md dig")) {
        term_cmd_dig(n > 1 ? tokens[1] : "");
    } else if (term_cmd_is(tokens[0], "rm")) {
        term_cmd_zap(n > 1 ? tokens[1] : "");
    } else if (term_cmd_is(tokens[0], "cat type")) {
        term_cmd_cat(n > 1 ? tokens[1] : "");
    } else if (term_cmd_is(tokens[0], "touch new")) {
        term_cmd_new(n > 1 ? tokens[1] : "");
    } else if (term_cmd_is(tokens[0], "cp copy dup")) {
        term_cmd_dup(n > 1 ? tokens[1] : "", n > 2 ? tokens[2] : "");
    } else if (term_cmd_is(tokens[0], "mv move")) {
        term_cmd_ship(n > 1 ? tokens[1] : "", n > 2 ? tokens[2] : "");
    } else if (term_streq(tokens[0], "root")) {
        if (term_is_root) term_print("  you are root already");
        else { term_pw_mode = term_pw_is_set() ? TP_LOGIN : TP_NEW; if (term_pw_mode == TP_NEW) term_print("  there is no root password yet: choose one"); }
    } else if (term_streq(tokens[0], "user")) {
        if (!term_is_root) term_print("  you are a user already");
        else { term_is_root = 0; term_print("  you are a user now"); }
    } else if (term_streq(tokens[0], "tick")) {
        term_cmd_tick();
    } else if (term_streq(tokens[0], "fetch")) {
        term_cmd_fetch();
    } else if (term_streq(tokens[0], "mem")) {
        term_cmd_mem(n > 1 ? tokens[1] : "");
    } else if (term_streq(tokens[0], "os-inst")) {
        term_cmd_osinst(n > 1 ? tokens[1] : "", n > 2 ? tokens[2] : "");
    } else if (term_cmd_is(tokens[0], "clear cls wipe")) {
        term_cmd_wipe();
    } else if (term_cmd_is(tokens[0], "reboot re")) {
        system_restart();
    } else if (term_cmd_is(tokens[0], "shutdown poweroff off")) {
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
 * same "sunken text field" bevel language as MINIWEB.MWP's URL bar --
 * see that window's own comment for why sunken reads as "typeable." */
#define TERM_INPUT_H 18
static inline int term_input_y(void) { return term_win.y + term_win.h - TERM_INPUT_H - 3; }
static inline int term_input_x(void) { return term_win.x + 3; }
static inline int term_input_w(void) { return term_win.w - 6; }
static int term_input_hit(int px, int py) {
    return in_rect(px, py, term_input_x(), term_input_y(), term_input_w(), TERM_INPUT_H);
}

/* The text area is the black rectangle between the menu bar and the input row. Its left edge is x+4, and the vertical scroll bar takes
 * the last SB_W columns of pixels, so the text itself has (width - 8 - SB_W) pixels: term_vis_cols() cells of FONT_CELL each. */
static inline int term_output_y(void) { return term_win.y + TITLEBAR_H + MB_H + 4; }
static int term_vis_cols(void) { int c = (term_win.w - 8 - SB_W) / FONT_CELL; return c < 1 ? 1 : c; }
static int term_wrap_cols(void) { int c = term_vis_cols(); return c < TERM_COLS_MIN ? TERM_COLS_MIN : (c > TERM_COLS ? TERM_COLS : c); }
/* Is the horizontal bar needed? Yes when the longest stored line is wider than the window shows. It takes SB_W pixels off the
 * bottom of the black area (the text then has one row less -- the rows are derived from the height AFTER this). */
static inline int term_hbar_shown(void) { return term_max_len() > term_vis_cols(); }
static inline int term_output_h(void) { return term_input_y() - term_output_y() - 3 - (term_hbar_shown() ? SB_W : 0); }
static inline int term_hbar_y(void) { return term_output_y() + term_output_h(); }
static int term_visible_rows(void) { int n = (term_output_h() - 2) / (FONT_CELL + 1); return n < 1 ? 1 : n; }


/* ---------------------------------------------------------------- MiniShell's menu bar: the screen, the view, the tools, help */
#define TERM_MENU_N 4
static const mb_menu_t term_menus[TERM_MENU_N] = {
    { { "Shell", "\xec\x85\xb8" }, 3, { { "Clear screen", "\xed\x99\x94\xeb\xa9\xb4 \xec\xa7\x80\xec\x9a\xb0\xea\xb8\xb0" }, { "Command list", "\xeb\xaa\x85\xeb\xa0\xb9 \xeb\xaa\xa9\xeb\xa1\x9d" }, { "Close window", "\xec\xb0\xbd \xeb\x8b\xab\xea\xb8\xb0" } } },
    { { "View",  "\xeb\xb3\xb4\xea\xb8\xb0" }, 3, { { "Scroll up", "\xec\x9c\x84\xeb\xa1\x9c \xec\x8a\xa4\xed\x81\xac\xeb\xa1\xa4" }, { "Scroll down", "\xec\x95\x84\xeb\x9e\x98\xeb\xa1\x9c \xec\x8a\xa4\xed\x81\xac\xeb\xa1\xa4" }, { "Jump to newest", "\xeb\xa7\xa8 \xec\x95\x84\xeb\x9e\x98\xeb\xa1\x9c" } } },
    { { "Tools", "\xeb\x8f\x84\xea\xb5\xac" }, 4, { { "Disks (part list)", "\xeb\x94\x94\xec\x8a\xa4\xed\x81\xac (part list)" }, { "Look here", "\xec\x97\xac\xea\xb8\xb0 \xeb\x91\x98\xeb\x9f\xac\xeb\xb3\xb4\xea\xb8\xb0" }, { "USB devices", "USB \xec\x9e\xa5\xec\xb9\x98" }, { "Time and date", "\xec\x8b\x9c\xea\xb0\x84\xea\xb3\xbc \xeb\x82\xa0\xec\xa7\x9c" } } },
    { { "Help",  "\xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90" }, 2, { { "About MiniShell", "MiniShell \xec\xa0\x95\xeb\xb3\xb4" }, { "Command list", "\xeb\xaa\x85\xeb\xa0\xb9 \xeb\xaa\xa9\xeb\xa1\x9d" } } },
};
static void term_menu_run(const char *cmd) {          /* an item that is just a command: type it for the user, exactly as if they had */
    if (term_pw_mode != TP_NONE) return;                  /* the input line belongs to a password prompt right now */
    u32 i = 0;
    while (cmd[i] && i < TERM_INPUT_MAXLEN) { term_input_buf[i] = cmd[i]; i++; }
    term_input_buf[i] = 0;
    term_input_len = i;
    term_run_input();
}
static void term_menu_action(int menu, int item) {
    if (menu == 0) {
        if (item == 0) term_cmd_wipe();
        else if (item == 1) term_cmd_help();
        else { term_win.open = 0; term_session_end(); win_z_remove(WIN_ID_TERMINAL); }
    } else if (menu == 1) {
        if (item == 0) term_scroll_by(term_visible_rows() - 2);
        else if (item == 1) term_scroll_by(-(term_visible_rows() - 2));
        else term_scroll_off = 0;
    } else if (menu == 2) {
        static const char *const cmds[4] = { "part list", "ls", "lsusb", "tick" };
        if (item >= 0 && item < 4) term_menu_run(cmds[item]);
    } else {
        if (item == 0) {
            term_print("MiniShell - the MiniWin shell");
            term_print("  " MW_VERSION ", " MW_SLOGAN);
            term_print("  ? lists every command");
        } else term_cmd_help();
    }
}

static void draw_terminal_window(void) {
    int wx = term_win.x, wy = term_win.y, ww = term_win.w, wh = term_win.h;

    ui_window_frame_ic(wx, wy, ww, wh, term_win.maximized, "MINISHELL.MWP", IC_TERM);
    ui_titlebar_buttons(term_btn_min_x(), term_btn_max_x(), term_btn_close_x(), term_btn_y(),
                        pressed_btn_kind == BTN_MIN && pressed_btn_win == WIN_ID_TERMINAL,
                        pressed_btn_kind == BTN_MAX && pressed_btn_win == WIN_ID_TERMINAL,
                        pressed_btn_kind == BTN_CLOSE && pressed_btn_win == WIN_ID_TERMINAL);

    mb_draw_bar(WIN_ID_TERMINAL, wx, wy, ww, term_menus, TERM_MENU_N);

    /* Scrollback: black background (classic terminal look, and a clean
     * visual break from every other window's light-gray body), showing
     * as many of the most recent lines as the window is tall and as many
     * columns of them as it is wide (scrolled sideways by term_hscroll). */
    term_sb_ensure();
    if (!term_session_on) term_session_begin();                       /* the first frame after the window was opened */
    int oy = term_output_y();
    int oh = term_output_h();
    int rows = term_visible_rows(), cols = term_vis_cols();
    { int mx = term_max_len() - cols; if (mx < 0) mx = 0; if (term_hscroll > mx) term_hscroll = mx; }     /* the window grew: no empty space past the text */
    { int mo = term_scroll_count - rows; if (mo < 0) mo = 0; if (term_scroll_off > mo) term_scroll_off = mo; }
    bb_fillrect(wx + 2, oy, ww - 4, oh, TH_CONSOLE);

    sb_draw(WIN_ID_TERMINAL);
    if (term_hbar_shown()) {
        bb_fillrect(wx + ww - 3 - SB_W, term_hbar_y(), SB_W, SB_W, TH_FACE);      /* the corner where the two bars meet */
        sb_draw_h(WIN_ID_TERMINAL);
    }
    int visible = term_scroll_count < rows ? term_scroll_count : rows;
    int start = (term_scroll_head - term_scroll_off - visible + 2 * TERM_SCROLLBACK_ROWS) % TERM_SCROLLBACK_ROWS;
    for (int i = 0; i < visible && term_sb; i++) {
        const char *row = term_scrollback[(start + i) % TERM_SCROLLBACK_ROWS];
        int len = 0; while (row[len]) len++;
        if (len <= term_hscroll) continue;                       /* this line ends before the visible columns begin */
        char shown[TERM_COLS + 1]; int k = 0;
        for (int c = term_hscroll; c < len && k < cols; c++) shown[k++] = row[c];
        shown[k] = 0;
        font_draw_string(wx + 4, oy + 2 + i * (FONT_CELL + 1), shown, TH_CONSOLE_TEXT);
    }

    /* Input row: sunken field, "> " prompt drawn as part of the same
     * string. When prompt + input is wider than the field, the line is
     * drawn from its END (the caret stays visible, the beginning slides
     * off the left edge) instead of running past the window frame. */
    int ix = term_input_x(), iy = term_input_y(), iw = term_input_w();
    ui_field(ix, iy, iw, TERM_INPUT_H, TH_CONSOLE, term_input_focused);

    char prompt[40];
    term_prompt_str(prompt, sizeof prompt);
    int plen = 0; while (prompt[plen]) plen++;
    char prompt_line[TERM_INPUT_MAXLEN + 44]; u32 pl = 0;
    append_str(prompt_line, &pl, sizeof(prompt_line), prompt);
    if (term_pw_mode != TP_NONE) { for (u32 i = 0; i < term_input_len; i++) kstrcpy_append(prompt_line, &pl, sizeof(prompt_line), '*'); }   /* a password is shown as stars */
    else append_str(prompt_line, &pl, sizeof(prompt_line), term_input_buf);
    int fit = (iw - 8) / FONT_CELL; if (fit < 1) fit = 1;        /* characters that fit the field (3px margin + the 2px caret) */
    int skip = (int)pl > fit ? (int)pl - fit : 0;
    font_draw_string(ix + 3, iy + 3, prompt_line + skip, TH_CONSOLE_TEXT);
    if (term_input_focused) {
        bb_fillrect(ix + 3 + (plen + (int)term_input_len - skip) * FONT_CELL, iy + 3, 2, FONT_CELL, TH_CONSOLE_TEXT);
    }
    mb_draw_drop(WIN_ID_TERMINAL, wx, wy, term_menus, TERM_MENU_N);
}

#endif
