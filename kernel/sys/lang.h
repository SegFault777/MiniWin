#ifndef MW_SYS_LANG_H
#define MW_SYS_LANG_H

/* sys/lang.h -- system language + input-method state shared by Notepad, Setting and the keyboard path.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ============================================================
 * System language & IME (SETTING.MWP > SYSTEM)
 *
 * Two genuinely separate settings, even though today they both happen
 * to be a choice of exactly the same two languages:
 *   - sys_language: what language the OS CHROME (menus, dialogs, status
 *     bar) is drawn in. Doesn't affect what you can type.
 *   - ime_enabled[] / current_ime: which input method(s) Right Alt is
 *     allowed to cycle through while typing in Notepad. Doesn't affect
 *     what language the menus are in.
 * Mixing them up would mean you couldn't type English in a Korean-
 * language system or vice versa, which is exactly the annoying
 * limitation real operating systems learned not to have decades ago.
 * ============================================================ */
#define LANG_ENGLISH 0
#define LANG_KOREAN  1
static int sys_language = LANG_ENGLISH;

#define IME_ENGLISH 0
#define IME_KOREAN  1
#define IME_COUNT   2
/* Both on by default -- matches the old F7/Right-Alt behavior, which
 * could always reach either language with no setup required. */
static int ime_enabled[IME_COUNT] = { 1, 1 };
static int current_ime = IME_ENGLISH;

/* Right Alt cycles to the next ENABLED ime, in fixed order, wrapping
 * around. If only one box is checked in SETTING.MWP, the loop below
 * walks all the way around back to the one we started on and quietly
 * changes nothing -- same as real Windows with a single input method
 * installed: the key is still there, it just has nowhere to go.
 * (Defined further down, right after text_buf/text_len exist -- it
 * needs to flush a syllable into that buffer mid-switch.) */
static void ime_cycle_next(void);

/* Called right after SETTING.MWP flips one of the IME checkboxes. If
 * the IME you were actively using just got unchecked, bump over to
 * whichever one is still enabled instead of leaving current_ime
 * pointing at a now-disabled option nobody can reach. */
static void ime_ensure_current_enabled(void) {
    if (ime_enabled[current_ime]) return;
    for (int i = 0; i < IME_COUNT; i++) {
        if (ime_enabled[i]) { current_ime = i; return; }
    }
}

/* ------------------------------------------------------------
 * UI string table. Every bit of chrome text that isn't a literal
 * filename (NOTEPAD.MWP stays NOTEPAD.MWP in any language, same as it
 * would on real Windows) goes through here, so SETTING.MWP's Language
 * switch actually means something. Language *names* themselves
 * ("English", "한국어") are deliberately NOT in this table -- every
 * real language picker shows each language's own name in its own
 * script so you can find yours even if you can't currently read
 * whatever's selected.
 * ------------------------------------------------------------ */
typedef enum {
    STR_SYSTEM, STR_LANGUAGE, STR_IME,
    STR_FILE, STR_EDIT, STR_HELP,
    STR_SAVE_AS, STR_SAVE, STR_NEW,
    STR_YES, STR_NO,
    STR_SAVE_CHANGES, STR_BEFORE_CLOSING, STR_BEFORE_NEW,
    STR_SHUT_DOWN, STR_RESTART, STR_SAFE_TO_TURN_OFF,
    STR_DEFAULT_HINT,
    STR_NOTEPAD_OPENED, STR_SETTING_OPENED, STR_WEB_OPENED, STR_TERMINAL_OPENED,
    STR_NOTEPAD_MINIMIZED, STR_NOTEPAD_MAXIMIZED, STR_NOTEPAD_RESTORED,
    STR_SETTING_MINIMIZED, STR_SETTING_MAXIMIZED, STR_SETTING_RESTORED,
    STR_WEB_MINIMIZED, STR_WEB_MAXIMIZED, STR_WEB_RESTORED,
    STR_TERMINAL_MINIMIZED, STR_TERMINAL_MAXIMIZED, STR_TERMINAL_RESTORED,
    STR_SAVE_AS_COMING_SOON,
    STR_STORAGE_FULL_NOT_SAVED,
    STR_SAVE_FAILED, STR_FILE_DAMAGED,
    STR_SAVED_PREFIX,
    STR_SAVED_AND_CLOSED, STR_STORAGE_FULL_CLOSED_NOT_SAVED,
    STR_NEW_DOCUMENT_NOT_SAVED, STR_CLOSED_NOT_SAVED,
    STR_HANGUL_MODE_ON, STR_ENGLISH_MODE_ON,
    STR_IME_MIN_ONE,
    STR_ALL_NOTEPAD_WINDOWS_OPEN,
    STR_TIMEZONE,
    STR_COUNT
} ui_str_id;

static const char *ui_strings_en[STR_COUNT] = {
    [STR_SYSTEM] = "SYSTEM",
    [STR_LANGUAGE] = "Language",
    [STR_IME] = "IME",
    [STR_FILE] = "File",
    [STR_EDIT] = "Edit",
    [STR_HELP] = "Help",
    [STR_SAVE_AS] = "Save As",
    [STR_SAVE] = "Save",
    [STR_NEW] = "New",
    [STR_YES] = "Yes",
    [STR_NO] = "No",
    [STR_SAVE_CHANGES] = "Save changes",
    [STR_BEFORE_CLOSING] = "before closing?",
    [STR_BEFORE_NEW] = "before New?",
    [STR_SHUT_DOWN] = "Shut Down",
    [STR_RESTART] = "Restart",
    [STR_SAFE_TO_TURN_OFF] = "It's now safe to turn off your computer.",
    [STR_DEFAULT_HINT] = "MINIWIN 1.0 - DOUBLE-CLICK NOTEPAD.MWP TO OPEN",
    [STR_NOTEPAD_OPENED] = "NOTEPAD.MWP OPENED (RIGHT ALT: SWITCH IME)",
    [STR_SETTING_OPENED] = "SETTING.MWP OPENED",
    [STR_WEB_OPENED] = "WEB.MWP OPENED",
    [STR_TERMINAL_OPENED] = "TERMINAL.MWP OPENED",
    [STR_NOTEPAD_MINIMIZED] = "NOTEPAD.MWP MINIMIZED",
    [STR_NOTEPAD_MAXIMIZED] = "NOTEPAD.MWP MAXIMIZED",
    [STR_NOTEPAD_RESTORED] = "NOTEPAD.MWP RESTORED",
    [STR_SETTING_MINIMIZED] = "SETTING.MWP MINIMIZED",
    [STR_SETTING_MAXIMIZED] = "SETTING.MWP MAXIMIZED",
    [STR_SETTING_RESTORED] = "SETTING.MWP RESTORED",
    [STR_WEB_MINIMIZED] = "WEB.MWP MINIMIZED",
    [STR_WEB_MAXIMIZED] = "WEB.MWP MAXIMIZED",
    [STR_WEB_RESTORED] = "WEB.MWP RESTORED",
    [STR_TERMINAL_MINIMIZED] = "TERMINAL.MWP MINIMIZED",
    [STR_TERMINAL_MAXIMIZED] = "TERMINAL.MWP MAXIMIZED",
    [STR_TERMINAL_RESTORED] = "TERMINAL.MWP RESTORED",
    [STR_SAVE_AS_COMING_SOON] = "SAVE AS - COMING SOON",
    [STR_STORAGE_FULL_NOT_SAVED] = "STORAGE FULL - NOT SAVED",
    [STR_SAVE_FAILED] = "SAVE FAILED - DISK ERROR OR FILE TOO BIG",
    [STR_FILE_DAMAGED] = "FILE DAMAGED - NOT OPENED",
    [STR_SAVED_PREFIX] = "SAVED: ",
    [STR_SAVED_AND_CLOSED] = "SAVED AND CLOSED",
    [STR_STORAGE_FULL_CLOSED_NOT_SAVED] = "STORAGE FULL - CLOSED, NOT SAVED",
    [STR_NEW_DOCUMENT_NOT_SAVED] = "NEW DOCUMENT (NOT SAVED)",
    [STR_CLOSED_NOT_SAVED] = "CLOSED (NOT SAVED)",
    [STR_HANGUL_MODE_ON] = "HANGUL MODE ON (RIGHT ALT TO SWITCH)",
    [STR_ENGLISH_MODE_ON] = "ENGLISH MODE ON (RIGHT ALT TO SWITCH)",
    [STR_IME_MIN_ONE] = "AT LEAST ONE IME MUST STAY ENABLED",
    [STR_ALL_NOTEPAD_WINDOWS_OPEN] = "ALL 4 NOTEPAD WINDOWS ALREADY OPEN",
    [STR_TIMEZONE] = "Time Zone",
};

static const char *ui_strings_ko[STR_COUNT] = {
    [STR_SYSTEM] = "\xec\x8b\x9c\xec\x8a\xa4\xed\x85\x9c",
    [STR_LANGUAGE] = "\xec\x96\xb8\xec\x96\xb4",
    [STR_IME] = "\xec\x9e\x85\xeb\xa0\xa5\xea\xb8\xb0",
    [STR_FILE] = "\xed\x8c\x8c\xec\x9d\xbc",
    [STR_EDIT] = "\xed\x8e\xb8\xec\xa7\x91",
    [STR_HELP] = "\xeb\x8f\x84\xec\x9b\x80\xeb\xa7\x90",
    [STR_SAVE_AS] = "\xeb\x8b\xa4\xeb\xa5\xb8 \xec\x9d\xb4\xeb\xa6\x84\xec\x9c\xbc\xeb\xa1\x9c \xec\xa0\x80\xec\x9e\xa5",
    [STR_SAVE] = "\xec\xa0\x80\xec\x9e\xa5",
    [STR_NEW] = "\xec\x83\x88\xeb\xa1\x9c \xeb\xa7\x8c\xeb\x93\xa4\xea\xb8\xb0",
    [STR_YES] = "\xec\x98\x88",
    [STR_NO] = "\xec\x95\x84\xeb\x8b\x88\xec\x98\xa4",
    [STR_SAVE_CHANGES] = "\xec\xa0\x80\xec\x9e\xa5\xed\x95\x98\xec\x8b\x9c\xea\xb2\xa0\xec\x8a\xb5\xeb\x8b\x88\xea\xb9\x8c,",
    [STR_BEFORE_CLOSING] = "\xeb\x8b\xab\xea\xb8\xb0 \xec\xa0\x84\xec\x97\x90?",
    [STR_BEFORE_NEW] = "\xec\x83\x88\xeb\xa1\x9c \xeb\xa7\x8c\xeb\x93\xa4\xea\xb8\xb0 \xec\xa0\x84\xec\x97\x90?",
    [STR_SHUT_DOWN] = "\xec\x8b\x9c\xec\x8a\xa4\xed\x85\x9c \xec\xa2\x85\xeb\xa3\x8c",
    [STR_RESTART] = "\xeb\x8b\xa4\xec\x8b\x9c \xec\x8b\x9c\xec\x9e\x91",
    [STR_SAFE_TO_TURN_OFF] = "\xec\x9d\xb4\xec\xa0\x9c \xec\xbb\xb4\xed\x93\xa8\xed\x84\xb0\xeb\xa5\xbc \xea\xba\xbc\xeb\x8f\x84 \xeb\x90\xa9\xeb\x8b\x88\xeb\x8b\xa4.",
    [STR_DEFAULT_HINT] = "MINIWIN 1.0 - NOTEPAD.MWP \xeb\x8d\x94\xeb\xb8\x94\xed\x81\xb4\xeb\xa6\xad\xec\x9c\xbc\xeb\xa1\x9c \xec\x8b\xa4\xed\x96\x89",
    [STR_NOTEPAD_OPENED] = "NOTEPAD.MWP \xec\x8b\xa4\xed\x96\x89\xeb\x90\xa8 (RIGHT ALT: \xec\x9e\x85\xeb\xa0\xa5\xea\xb8\xb0 \xec\xa0\x84\xed\x99\x98)",
    [STR_SETTING_OPENED] = "SETTING.MWP \xec\x8b\xa4\xed\x96\x89\xeb\x90\xa8",
    [STR_WEB_OPENED] = "WEB.MWP \xec\x8b\xa4\xed\x96\x89\xeb\x90\xa8",
    [STR_TERMINAL_OPENED] = "TERMINAL.MWP \xec\x8b\xa4\xed\x96\x89\xeb\x90\xa8",
    [STR_NOTEPAD_MINIMIZED] = "NOTEPAD.MWP \xec\xb5\x9c\xec\x86\x8c\xed\x99\x94\xeb\x90\xa8",
    [STR_NOTEPAD_MAXIMIZED] = "NOTEPAD.MWP \xec\xb5\x9c\xeb\x8c\x80\xed\x99\x94\xeb\x90\xa8",
    [STR_NOTEPAD_RESTORED] = "NOTEPAD.MWP \xeb\xb3\xb5\xec\x9b\x90\xeb\x90\xa8",
    [STR_SETTING_MINIMIZED] = "SETTING.MWP \xec\xb5\x9c\xec\x86\x8c\xed\x99\x94\xeb\x90\xa8",
    [STR_SETTING_MAXIMIZED] = "SETTING.MWP \xec\xb5\x9c\xeb\x8c\x80\xed\x99\x94\xeb\x90\xa8",
    [STR_SETTING_RESTORED] = "SETTING.MWP \xeb\xb3\xb5\xec\x9b\x90\xeb\x90\xa8",
    [STR_WEB_MINIMIZED] = "WEB.MWP \xec\xb5\x9c\xec\x86\x8c\xed\x99\x94\xeb\x90\xa8",
    [STR_WEB_MAXIMIZED] = "WEB.MWP \xec\xb5\x9c\xeb\x8c\x80\xed\x99\x94\xeb\x90\xa8",
    [STR_WEB_RESTORED] = "WEB.MWP \xeb\xb3\xb5\xec\x9b\x90\xeb\x90\xa8",
    [STR_TERMINAL_MINIMIZED] = "TERMINAL.MWP \xec\xb5\x9c\xec\x86\x8c\xed\x99\x94\xeb\x90\xa8",
    [STR_TERMINAL_MAXIMIZED] = "TERMINAL.MWP \xec\xb5\x9c\xeb\x8c\x80\xed\x99\x94\xeb\x90\xa8",
    [STR_TERMINAL_RESTORED] = "TERMINAL.MWP \xeb\xb3\xb5\xec\x9b\x90\xeb\x90\xa8",
    [STR_SAVE_AS_COMING_SOON] = "\xeb\x8b\xa4\xeb\xa5\xb8 \xec\x9d\xb4\xeb\xa6\x84\xec\x9c\xbc\xeb\xa1\x9c \xec\xa0\x80\xec\x9e\xa5 - \xec\xa4\x80\xeb\xb9\x84 \xec\xa4\x91",
    [STR_STORAGE_FULL_NOT_SAVED] = "\xec\xa0\x80\xec\x9e\xa5 \xea\xb3\xb5\xea\xb0\x84 \xeb\xb6\x80\xec\xa1\xb1 - \xec\xa0\x80\xec\x9e\xa5 \xec\x95\x88 \xeb\x90\xa8",
    [STR_SAVE_FAILED] = "\xec\xa0\x80\xec\x9e\xa5\x20\xec\x8b\xa4\xed\x8c\xa8\x20\x2d\x20\xeb\x94\x94\xec\x8a\xa4\xed\x81\xac\x20\xec\x98\xa4\xeb\xa5\x98\x20\xeb\x98\x90\xeb\x8a\x94\x20\xed\x8c\x8c\xec\x9d\xbc\x20\xeb\x84\x88\xeb\xac\xb4\x20\xed\x81\xbc",
    [STR_FILE_DAMAGED] = "\xed\x8c\x8c\xec\x9d\xbc\x20\xec\x86\x90\xec\x83\x81\x20\x2d\x20\xec\x97\xb4\x20\xec\x88\x98\x20\xec\x97\x86\xec\x9d\x8c",
    [STR_SAVED_PREFIX] = "\xec\xa0\x80\xec\x9e\xa5\xeb\x90\xa8: ",
    [STR_SAVED_AND_CLOSED] = "\xec\xa0\x80\xec\x9e\xa5 \xed\x9b\x84 \xeb\x8b\xab\xec\x9d\x8c",
    [STR_STORAGE_FULL_CLOSED_NOT_SAVED] = "\xec\xa0\x80\xec\x9e\xa5 \xea\xb3\xb5\xea\xb0\x84 \xeb\xb6\x80\xec\xa1\xb1 - \xec\xa0\x80\xec\x9e\xa5 \xec\x95\x88 \xed\x95\x98\xea\xb3\xa0 \xeb\x8b\xab\xec\x9d\x8c",
    [STR_NEW_DOCUMENT_NOT_SAVED] = "\xec\x83\x88 \xeb\xac\xb8\xec\x84\x9c (\xec\xa0\x80\xec\x9e\xa5 \xec\x95\x88 \xeb\x90\xa8)",
    [STR_CLOSED_NOT_SAVED] = "\xeb\x8b\xab\xec\x9d\x8c (\xec\xa0\x80\xec\x9e\xa5 \xec\x95\x88 \xeb\x90\xa8)",
    [STR_HANGUL_MODE_ON] = "\xed\x95\x9c\xea\xb8\x80 \xeb\xaa\xa8\xeb\x93\x9c \xec\xbc\x9c\xec\xa7\x90 (RIGHT ALT\xeb\xa1\x9c \xec\xa0\x84\xed\x99\x98)",
    [STR_ENGLISH_MODE_ON] = "\xec\x98\x81\xec\x96\xb4 \xeb\xaa\xa8\xeb\x93\x9c \xec\xbc\x9c\xec\xa7\x90 (RIGHT ALT\xeb\xa1\x9c \xec\xa0\x84\xed\x99\x98)",
    [STR_IME_MIN_ONE] = "\xec\xb5\x9c\xec\x86\x8c 1\xea\xb0\x9c\xec\x9d\x98 \xec\x9e\x85\xeb\xa0\xa5\xea\xb8\xb0\xeb\x8a\x94 \xec\xbc\x9c\xec\xa0\xb8 \xec\x9e\x88\xec\x96\xb4\xec\x95\xbc \xed\x95\xa8",
    [STR_ALL_NOTEPAD_WINDOWS_OPEN] = "\xeb\x85\xb8\xed\x8a\xb8\xed\x8c\xa8\xeb\x93\x9c \xec\xb0\xbd 4\xea\xb0\x9c\xea\xb0\x80 \xec\x9d\xb4\xeb\xaf\xb8 \xeb\xaa\xa8\xeb\x91\x90 \xec\x97\xb4\xeb\xa0\xa4 \xec\x9e\x88\xec\x9d\x8c",
    [STR_TIMEZONE] = "\xec\x8b\x9c\xea\xb0\x84\xeb\x8c\x80",
};


static inline const char *t(ui_str_id id) {
    return (sys_language == LANG_KOREAN) ? ui_strings_ko[id] : ui_strings_en[id];
}

#endif
