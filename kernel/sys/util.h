#ifndef MW_SYS_UTIL_H
#define MW_SYS_UTIL_H

/* sys/util.h -- freestanding helpers (no libc): string/number/clamp utilities every other module uses.
 * (Split out of kernel.c in pre-29; it is still one translation unit, so modules are included by
 * kernel.c in dependency order -- see the module map there.) */

/* ---------- tiny freestanding helpers (no libc available) ---------- */
static void kstrcpy_append(char *buf, u32 *len, u32 maxlen, char c) {
    if (*len < maxlen - 1) {
        buf[*len] = c;
        (*len)++;
        buf[*len] = 0;
    }
}

/* Plain "copy src into dst, truncating at dst_sz - 1, always
 * NUL-terminated" -- the freestanding equivalent of strlcpy(), which
 * this kernel doesn't have because it has no libc at all. Used wherever
 * a fixed-size buffer needs to hold a whole string at once rather than
 * being built up character-by-character via kstrcpy_append() (which
 * wants a running length counter the caller has to keep alive; this
 * doesn't). */
static void kstrcpy(char *dst, const char *src, u32 dst_sz) {
    u32 i = 0;
    while (src[i] && i < dst_sz - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* simple busy-wait delay, calibrated roughly for typical QEMU/CPU speed */
static void delay(volatile u32 loops) {
    while (loops--) { __asm__ volatile ("nop"); }
}

static inline int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

#endif
