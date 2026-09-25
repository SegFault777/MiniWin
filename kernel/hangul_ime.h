#ifndef HANGUL_IME_H
#define HANGUL_IME_H
#include "io.h"
#include "font_ko.h"

/* This is the part where we stop pretending a keyboard driver is simple.
 * Korean isn't "one key, one character" like English -- you type
 * individual consonants and vowels (자모, jamo) and they glue together
 * into syllable blocks in real time. Get the state machine below wrong
 * and you get garbage instead of "안녕하세요." No pressure.
 *
 * ============================================================
 * Standard 2-beolsik (두벌식) keyboard layout: maps the ASCII letter our
 * keyboard driver already decodes (post-shift) to a jamo index. This is
 * the same layout basically every Korean keyboard/IME uses, so if you
 * know how to type Korean already, your fingers already know this.
 *
 * Index numbering matches Unicode's canonical Hangul jamo order, so these
 * indices plug directly into ko_compose_codepoint() in font_ko.h:
 *   initial (0-18): ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ
 *   medial  (0-20): ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅘㅙㅚㅛㅜㅝㅞㅟㅠㅡㅢㅣ
 *   final   (0-27): (none)ㄱㄲㄳㄴㄵㄶㄷㄹㄺㄻㄼㄽㄾㄿㅀㅁㅂㅄㅅㅆㅇㅈㅊㅋㅌㅍㅎ
 * ============================================================ */

/* Returns the INITIAL-order index (0-18) for a 2-beolsik consonant key,
 * or -1 if `c` isn't one of those keys. */
static inline int ko_consonant_index_for_letter(char c) {
    switch (c) {
        case 'r': return 0;  /* ㄱ */
        case 'R': return 1;  /* ㄲ */
        case 's': return 2;  /* ㄴ */
        case 'e': return 3;  /* ㄷ */
        case 'E': return 4;  /* ㄸ */
        case 'f': return 5;  /* ㄹ */
        case 'a': return 6;  /* ㅁ */
        case 'q': return 7;  /* ㅂ */
        case 'Q': return 8;  /* ㅃ */
        case 't': return 9;  /* ㅅ */
        case 'T': return 10; /* ㅆ */
        case 'd': return 11; /* ㅇ */
        case 'w': return 12; /* ㅈ */
        case 'W': return 13; /* ㅉ */
        case 'c': return 14; /* ㅊ */
        case 'z': return 15; /* ㅋ */
        case 'x': return 16; /* ㅌ */
        case 'v': return 17; /* ㅍ */
        case 'g': return 18; /* ㅎ */
        default:  return -1;
    }
}

/* Returns the MEDIAL-order index (0-20) for a 2-beolsik vowel key, or -1.
 * Only the 14 directly-typeable vowels get their own key; the 7 complex
 * vowels (ㅘㅙㅚㅝㅞㅟㅢ, indices 9,10,11,14,15,16,19) only show up by
 * smashing two of these together via ko_combine_medial() below, exactly
 * like a real keyboard (ㅗ then ㅏ, back to back, becomes ㅘ). */
static inline int ko_vowel_index_for_letter(char c) {
    switch (c) {
        case 'k': return 0;  /* ㅏ */
        case 'o': return 1;  /* ㅐ */
        case 'i': return 2;  /* ㅑ */
        case 'O': return 3;  /* ㅒ */
        case 'j': return 4;  /* ㅓ */
        case 'p': return 5;  /* ㅔ */
        case 'u': return 6;  /* ㅕ */
        case 'P': return 7;  /* ㅖ */
        case 'h': return 8;  /* ㅗ */
        case 'y': return 12; /* ㅛ */
        case 'n': return 13; /* ㅜ */
        case 'b': return 17; /* ㅠ */
        case 'm': return 18; /* ㅡ */
        case 'l': return 20; /* ㅣ */
        default:  return -1;
    }
}

/* Smashes two medial (vowel) indices typed back to back into one complex
 * vowel, e.g. ㅗ(8)+ㅏ(0)->ㅘ(9). Returns -1 if that pair just doesn't go
 * together (most pairs don't -- only these seven combos are real). */
static inline int ko_combine_medial(int base, int add) {
    if (base == 8  && add == 0)  return 9;   /* ㅗ+ㅏ->ㅘ */
    if (base == 8  && add == 1)  return 10;  /* ㅗ+ㅐ->ㅙ */
    if (base == 8  && add == 20) return 11;  /* ㅗ+ㅣ->ㅚ */
    if (base == 13 && add == 4)  return 14;  /* ㅜ+ㅓ->ㅝ */
    if (base == 13 && add == 5)  return 15;  /* ㅜ+ㅔ->ㅞ */
    if (base == 13 && add == 20) return 16;  /* ㅜ+ㅣ->ㅟ */
    if (base == 18 && add == 20) return 19;  /* ㅡ+ㅣ->ㅢ */
    return -1;
}

/* Maps an INITIAL-order consonant index to the SIMPLE (single-jamo)
 * FINAL-order index it becomes as a batchim, or -1 if that consonant
 * flat-out refuses to be a final (ㄸ, ㅃ, ㅉ -- modern Hangul just
 * doesn't let these be batchim, don't ask, that's how it is). */
static const int ko_initial_to_simple_final[19] = {
    1, 2, 4, 7, -1, 8, 16, 17, -1, 19, 20, 21, 22, -1, 23, 24, 25, 26, 27,
};

/* Combines a final already sitting on a syllable with a freshly typed
 * consonant (as an INITIAL-order index) into a complex final, e.g.
 * ㄱ(final 1)+ㅅ(initial 9)->ㄳ(final 3). Returns -1 if they don't want
 * to be friends. */
static inline int ko_combine_final(int base_final, int add_consonant_initial_idx) {
    if (base_final == 1  && add_consonant_initial_idx == 9)  return 3;  /* ㄱ+ㅅ->ㄳ */
    if (base_final == 4  && add_consonant_initial_idx == 12) return 5;  /* ㄴ+ㅈ->ㄵ */
    if (base_final == 4  && add_consonant_initial_idx == 18) return 6;  /* ㄴ+ㅎ->ㄶ */
    if (base_final == 8  && add_consonant_initial_idx == 0)  return 9;  /* ㄹ+ㄱ->ㄺ */
    if (base_final == 8  && add_consonant_initial_idx == 6)  return 10; /* ㄹ+ㅁ->ㄻ */
    if (base_final == 8  && add_consonant_initial_idx == 7)  return 11; /* ㄹ+ㅂ->ㄼ */
    if (base_final == 8  && add_consonant_initial_idx == 9)  return 12; /* ㄹ+ㅅ->ㄽ */
    if (base_final == 8  && add_consonant_initial_idx == 16) return 13; /* ㄹ+ㅌ->ㄾ */
    if (base_final == 8  && add_consonant_initial_idx == 17) return 14; /* ㄹ+ㅍ->ㄿ */
    if (base_final == 8  && add_consonant_initial_idx == 18) return 15; /* ㄹ+ㅎ->ㅀ */
    if (base_final == 17 && add_consonant_initial_idx == 9)  return 18; /* ㅂ+ㅅ->ㅄ */
    return -1;
}

/* This is the fiddly bit that makes "ㄱㅏㄴㅏ" correctly turn into "가나"
 * instead of "간" plus a vowel with nowhere to go. When a vowel shows up
 * right after a syllable already has initial+medial+a tentative final,
 * that final actually belongs to a BRAND NEW syllable as ITS initial --
 * standard 2-beolsik hands it back. For a complex final, only the second
 * jamo gets handed back; the first stays put as the current syllable's
 * (now simple) final. Returns the final-order index that should stay on
 * the CURRENT syllable, and writes the handed-back consonant's
 * INITIAL-order index to *out_new_initial. Screw this up and every
 * multi-syllable word you type comes out mangled, so: don't. */
static inline int ko_split_final_for_handback(int final_idx, int *out_new_initial) {
    switch (final_idx) {
        case 1:  *out_new_initial = 0;  return 0;  /* ㄱ -> new initial ㄱ */
        case 2:  *out_new_initial = 1;  return 0;  /* ㄲ -> new initial ㄲ */
        case 3:  *out_new_initial = 9;  return 1;  /* ㄳ -> keep ㄱ, new initial ㅅ */
        case 4:  *out_new_initial = 2;  return 0;  /* ㄴ -> new initial ㄴ */
        case 5:  *out_new_initial = 12; return 4;  /* ㄵ -> keep ㄴ, new initial ㅈ */
        case 6:  *out_new_initial = 18; return 4;  /* ㄶ -> keep ㄴ, new initial ㅎ */
        case 7:  *out_new_initial = 3;  return 0;  /* ㄷ -> new initial ㄷ */
        case 8:  *out_new_initial = 5;  return 0;  /* ㄹ -> new initial ㄹ */
        case 9:  *out_new_initial = 0;  return 8;  /* ㄺ -> keep ㄹ, new initial ㄱ */
        case 10: *out_new_initial = 6;  return 8;  /* ㄻ -> keep ㄹ, new initial ㅁ */
        case 11: *out_new_initial = 7;  return 8;  /* ㄼ -> keep ㄹ, new initial ㅂ */
        case 12: *out_new_initial = 9;  return 8;  /* ㄽ -> keep ㄹ, new initial ㅅ */
        case 13: *out_new_initial = 16; return 8;  /* ㄾ -> keep ㄹ, new initial ㅌ */
        case 14: *out_new_initial = 17; return 8;  /* ㄿ -> keep ㄹ, new initial ㅍ */
        case 15: *out_new_initial = 18; return 8;  /* ㅀ -> keep ㄹ, new initial ㅎ */
        case 16: *out_new_initial = 6;  return 0;  /* ㅁ -> new initial ㅁ */
        case 17: *out_new_initial = 7;  return 0;  /* ㅂ -> new initial ㅂ */
        case 18: *out_new_initial = 9;  return 17; /* ㅄ -> keep ㅂ, new initial ㅅ */
        case 19: *out_new_initial = 9;  return 0;  /* ㅅ -> new initial ㅅ */
        case 20: *out_new_initial = 10; return 0;  /* ㅆ -> new initial ㅆ */
        case 21: *out_new_initial = 11; return 0;  /* ㅇ -> new initial ㅇ */
        case 22: *out_new_initial = 12; return 0;  /* ㅈ -> new initial ㅈ */
        case 23: *out_new_initial = 14; return 0;  /* ㅊ -> new initial ㅊ */
        case 24: *out_new_initial = 15; return 0;  /* ㅋ -> new initial ㅋ */
        case 25: *out_new_initial = 16; return 0;  /* ㅌ -> new initial ㅌ */
        case 26: *out_new_initial = 17; return 0;  /* ㅍ -> new initial ㅍ */
        case 27: *out_new_initial = 18; return 0;  /* ㅎ -> new initial ㅎ */
        default: *out_new_initial = -1; return 0;
    }
}

#define KO_INITIAL_IEUNG 11 /* the silent "filler" initial we sneak in
                              * when a vowel gets typed with nothing in
                              * front of it (type just "ㅏ" and you should
                              * see "아", not a lone floating vowel) --
                              * exactly what every real Hangul IME does. */

/* ============================================================
 * Composition state -- whatever syllable is currently half-built.
 * kernel.c only ever talks to this through ko_ime_feed_key() and
 * ko_ime_backspace() below; it doesn't (and shouldn't) poke these fields
 * directly. Keep it that way.
 *
 * Whether Hangul composition is ACTIVE right now (as opposed to just
 * available) is a separate question, decided by kernel.c's IME system
 * (current_ime / ime_enabled[], driven by Right Alt and SETTING.MWP's
 * IME picker) -- this file doesn't know or care which language is
 * "selected," it just knows how to build a syllable once asked to. */
static int ko_cur_initial = -1;
static int ko_cur_medial  = -1;
static int ko_cur_final   = -1;     /* -1 = nothing attached yet. 0 WOULD mean "definitely no final," but we never actually store 0 mid-composition -- see the commit function below for why */

static inline int ko_ime_is_composing(void) {
    return ko_cur_initial != -1;
}

/* Whatever codepoint should be shown RIGHT NOW for the syllable in
 * progress (or just a lone jamo if that's all we've got so far), or -1
 * if there's nothing going on. */
static inline int ko_ime_preview_codepoint(void) {
    if (ko_cur_initial == -1) return -1;
    if (ko_cur_medial == -1) return ko_jamo_cp_for_initial[ko_cur_initial];
    int f = (ko_cur_final == -1) ? 0 : ko_cur_final;
    return ko_compose_codepoint(ko_cur_initial, ko_cur_medial, f);
}

static inline void ko_ime_reset(void) {
    ko_cur_initial = -1;
    ko_cur_medial = -1;
    ko_cur_final = -1;
}

/* Bolts a finished syllable's UTF-8 bytes (3 of them) onto the end of
 * buf, staying under maxlen like a responsible adult. Updates *len.
 * Returns 1 if something actually got written, 0 if there was nothing to
 * write or no room left. */
static inline int ko_emit_utf8(char *buf, u32 *len, u32 maxlen, int cp) {
    if (cp < 0) return 0;
    if (*len + 3 > maxlen) return 0;
    buf[(*len)++] = (char)(0xE0 | (cp >> 12));
    buf[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[(*len)++] = (char)(0x80 | (cp & 0x3F));
    return 1;
}

static inline void ko_ime_commit(char *buf, u32 *len, u32 maxlen) {
    if (ko_cur_initial == -1) return;
    int f = (ko_cur_final == -1) ? 0 : ko_cur_final;
    if (ko_cur_medial == -1) {
        /* just a bare consonant with nothing else -- spit out the
         * standalone jamo instead of trying to force it into a syllable
         * that doesn't exist */
        ko_emit_utf8(buf, len, maxlen, ko_jamo_cp_for_initial[ko_cur_initial]);
    } else {
        ko_emit_utf8(buf, len, maxlen, ko_compose_codepoint(ko_cur_initial, ko_cur_medial, f));
    }
    ko_ime_reset();
}

/* Feeds one already-decoded ASCII key (post-shift, straight from
 * keyboard_poll_key()) into the composition state machine. If the key
 * isn't on the 2-beolsik layout at all, whatever was mid-composition
 * gets flushed first and this returns 0 so the caller deals with the key
 * like normal (space, backspace, punctuation, someone typing English
 * without remembering to hit F7 first, etc). If it IS a jamo key, it's
 * consumed right here and this returns 1. `buf`/`len`/`maxlen` are the
 * document's text buffer, used whenever a syllable needs to get flushed. */
static inline int ko_ime_feed_key(char c, char *buf, u32 *len, u32 maxlen) {
    int cons = ko_consonant_index_for_letter(c);
    int vow  = ko_vowel_index_for_letter(c);

    if (cons < 0 && vow < 0) {
        /* not a jamo key at all -- flush whatever we had and let the
         * caller take it from here, we're done being involved */
        ko_ime_commit(buf, len, maxlen);
        return 0;
    }

    if (cons >= 0) {
        if (ko_cur_initial == -1) {
            ko_cur_initial = cons;
            ko_cur_medial = -1;
            ko_cur_final = -1;
        } else if (ko_cur_medial == -1) {
            /* two consonants back to back with no vowel between them --
             * we don't have a clean way to stack two bare jamo into one
             * displayed unit, so commit the first as its own standalone
             * jamo and start over with this one. Documented shortcut:
             * only consonant+vowel[+consonant] is fully supported, which
             * covers, y'know, basically all real typing. */
            ko_ime_commit(buf, len, maxlen);
            ko_cur_initial = cons;
        } else if (ko_cur_final == -1) {
            int simple = ko_initial_to_simple_final[cons];
            if (simple < 0) {
                /* ㄸ/ㅃ/ㅉ showed up trying to be a batchim -- not happening,
                 * finish this syllable with no final and start fresh */
                ko_ime_commit(buf, len, maxlen);
                ko_cur_initial = cons;
                ko_cur_medial = -1;
                ko_cur_final = -1;
            } else {
                ko_cur_final = simple; /* tentatively bolt it on, might get handed back later */
            }
        } else {
            int combined = ko_combine_final(ko_cur_final, cons);
            if (combined >= 0) {
                ko_cur_final = combined;
            } else {
                ko_ime_commit(buf, len, maxlen);
                ko_cur_initial = cons;
                ko_cur_medial = -1;
                ko_cur_final = -1;
            }
        }
        return 1;
    }

    /* vow >= 0 */
    if (ko_cur_initial == -1) {
        /* a vowel showed up with nothing before it -- slap the silent ㅇ
         * in front, same as every real Hangul IME does */
        ko_cur_initial = KO_INITIAL_IEUNG;
        ko_cur_medial = vow;
        ko_cur_final = -1;
    } else if (ko_cur_medial == -1) {
        ko_cur_medial = vow;
    } else if (ko_cur_final == -1) {
        int combined = ko_combine_medial(ko_cur_medial, vow);
        if (combined >= 0) {
            ko_cur_medial = combined;
        } else {
            ko_ime_commit(buf, len, maxlen);
            ko_cur_initial = KO_INITIAL_IEUNG;
            ko_cur_medial = vow;
            ko_cur_final = -1;
        }
    } else {
        /* the hand-back move: that tentative final was lying to us, it
         * actually belongs to a brand new syllable as its initial */
        int new_initial;
        int kept_final = ko_split_final_for_handback(ko_cur_final, &new_initial);
        ko_cur_final = (kept_final == 0) ? -1 : kept_final;
        ko_ime_commit(buf, len, maxlen);
        ko_cur_initial = new_initial;
        ko_cur_medial = vow;
        ko_cur_final = -1;
    }
    return 1;
}

/* Undoes one step of whatever's being composed (final first, then
 * medial, then initial), same as any real IME's backspace. Returns 1 if
 * it ate the backspace (something WAS being composed), 0 if there was
 * nothing going on and the caller should just do a plain buffer
 * backspace instead. */
static inline int ko_ime_backspace(void) {
    if (ko_cur_final != -1) {
        ko_cur_final = -1;
        return 1;
    }
    if (ko_cur_medial != -1) {
        ko_cur_medial = -1;
        return 1;
    }
    if (ko_cur_initial != -1) {
        ko_cur_initial = -1;
        return 1;
    }
    return 0;
}

#endif
