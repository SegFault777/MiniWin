/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_CLIPBOARD_H
#define MW_CLIPBOARD_H

/* clipboard.h -- the one clipboard of the OS (1.1-alpha-11): plain bytes, at most CLIP_MAX. Notepad's Edit menu cuts, copies and
 * pastes through it, and programs reach it with sys.clipboard_get / clipboard_set (sdk/include/mwp_abi.h, ABI 3), so text copied in a
 * program can be pasted into Notepad and the other way round. Text only; a copy that is longer than CLIP_MAX is refused, not cut
 * (a silently shortened paste is worse than none). */
#define CLIP_MAX 4096u
static char clip_buf[CLIP_MAX];
static u32  clip_len = 0;

static int clip_set(const void *data, u32 len) {
    if (len > CLIP_MAX || (len && !data)) return 0;
    for (u32 i = 0; i < len; i++) clip_buf[i] = ((const char *)data)[i];
    clip_len = len;
    return 1;
}
/* Copies up to max bytes into dst; returns the clipboard's full length (so a caller can tell it was cut short). */
static u32 clip_get(void *dst, u32 max) {
    u32 n = clip_len < max ? clip_len : max;
    for (u32 i = 0; i < n; i++) ((char *)dst)[i] = clip_buf[i];
    return clip_len;
}

#endif
