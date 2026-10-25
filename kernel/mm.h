/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef MW_MM_H
#define MW_MM_H

/* mm.h -- the kernel's memory manager (1.1-alpha-10): a page-frame allocator and a heap on top of it.
 *
 * WHAT CHANGED. Until alpha-9 there was no allocator at all: every buffer sat at a fixed, named address (memmap.h) and
 * everything above the last fixed region (0x900000 and up -- on the usual 128 MB machine, 119 MB) was simply unused. That
 * suited a kernel whose every buffer has a known worst-case size, and it still does: the big fixed regions STAY fixed (DMA
 * buffers must not move, the HTML engine's arenas are sized once). But anything that needs "some memory, a size known only at
 * run time" -- a program asking for a work buffer, a file manager listing a large folder, a decompressor -- had nowhere to
 * ask. Now it does.
 *
 *   mm_page_alloc / mm_pages_alloc(n) / mm_pages_free   whole 4 KB pages (n contiguous), zero-filled on request
 *   kmalloc / kzalloc / krealloc / kfree                 any size, 16-byte aligned, from a heap built out of those pages
 *   mm_stats / mm_check                                  numbers for the `mem` command, and a full integrity walk
 *
 * HOW IT IS BUILT.
 *  - The managed range is [0x900000, top of RAM) clamped to 1 GB (E801h's total, saved by stage 2 at 0x9200). Its first pages
 *    hold the allocation bitmap (1 bit per page), so the manager costs no .bss.
 *  - mm_pages_alloc() is first-fit over that bitmap, starting from a rolling hint so freed low pages are found again on the
 *    next wrap. Freeing a page that is not allocated is counted (mm_stat_bad_frees) and ignored, never trusted.
 *  - The heap is a list of up to 16 chunks, each a run of pages (64 KB minimum), divided into blocks that each start with a
 *    16-byte header: {size, size of the block before it, magic, state}. Blocks are laid out back to back, so the next block is
 *    at +size and the previous at -prev_size: freeing merges with both neighbours in O(1), and a chunk that becomes ONE free
 *    block goes back to the page allocator (except the first, so a small allocation never pays for a page-run request).
 *  - The header magic differs for used and free blocks. kfree() of a pointer that is not at a used block's payload, or of a
 *    block already freed, or of a block whose header was overwritten, is refused and counted (mm_stat_bad_frees); mm_check()
 *    walks every block and verifies the sizes chain exactly to the end of each chunk.
 *
 * WHAT IT IS NOT. There is still no paging: a pointer is a physical address, a stray write can hit anything, and a freed block
 * is not unmapped, only reusable. The allocator cannot make ring-0 code safe; it can only make a double free or a header smash
 * LOUD instead of silent. (Paging with per-program address spaces is the next step of this line of work.)
 *
 * Host-testable: tools/test/host_mm_test.c compiles this header against an ordinary array (mm_init_region).
 * Needs: io.h (u8/u16/u32), nothing else. Not re-entrant (the kernel has one thread and no interrupt-time allocations). */

#define MM_PAGE          4096u
#define MM_CHUNK_MAX     16
#define MM_CHUNK_MIN_PG  16u                    /* 64 KB: a heap chunk is never smaller */
#define MM_HDR           16u
#define MM_MAGIC_USED    0xA110C8EDu
#define MM_MAGIC_FREE    0xF4EEB10Cu
#define MM_MAX_TOP       0x40000000u            /* manage at most 1 GB: the bitmap stays 32 KB and the LFB/PCI hole is never reached */

typedef struct { u32 size, prev_size, magic, pad; } mm_hdr_t;     /* size/prev_size include the header; 16 bytes keeps payloads 16-aligned */
typedef struct { u8 *base; u32 bytes; } mm_chunk_t;

typedef struct {
    u32 total_pages, free_pages, used_pages;      /* the managed range, in 4 KB pages */
    u32 heap_chunks, heap_bytes, heap_used, heap_free, heap_largest_free;   /* heap_bytes = chunk bytes; used/free count payload+header */
    u32 allocs, frees, bad_frees, failed_allocs;
} mm_stats_t;

static u8  *mm_base = 0;                        /* first managed page (also where the bitmap starts) */
static u32  mm_npages = 0, mm_bitmap_pages = 0, mm_hint = 0, mm_free_pg = 0;
static u8  *mm_bitmap = 0;
static mm_chunk_t mm_chunks[MM_CHUNK_MAX];
static u32  mm_stat_allocs = 0, mm_stat_frees = 0, mm_stat_bad_frees = 0, mm_stat_failed = 0;

static inline int  mm_bit(u32 p)       { return (mm_bitmap[p >> 3] >> (p & 7)) & 1; }
static inline void mm_bit_set(u32 p)   { mm_bitmap[p >> 3] |= (u8)(1u << (p & 7)); }
static inline void mm_bit_clr(u32 p)   { mm_bitmap[p >> 3] &= (u8)~(1u << (p & 7)); }

/* Takes [base, base+bytes) (page aligned) as the managed range. Its first pages become the bitmap. 1 = ok. */
static int mm_init_region(void *base, u32 bytes) {
    u32 n = bytes / MM_PAGE;
    if (!base || n < 8 || ((unsigned long)base & (MM_PAGE - 1))) return 0;
    mm_base = (u8 *)base; mm_npages = n; mm_bitmap = mm_base;
    mm_bitmap_pages = (n / 8 + MM_PAGE - 1) / MM_PAGE;
    for (u32 i = 0; i < mm_bitmap_pages * MM_PAGE; i++) mm_bitmap[i] = 0;
    for (u32 p = 0; p < mm_bitmap_pages; p++) mm_bit_set(p);               /* the bitmap pages are never handed out */
    mm_free_pg = n - mm_bitmap_pages; mm_hint = mm_bitmap_pages;
    for (int i = 0; i < MM_CHUNK_MAX; i++) { mm_chunks[i].base = 0; mm_chunks[i].bytes = 0; }
    mm_stat_allocs = mm_stat_frees = mm_stat_bad_frees = mm_stat_failed = 0;
    return 1;
}

/* n contiguous pages (n >= 1), or 0. The pages are NOT cleared unless zero is set. */
static void *mm_pages_alloc_z(u32 n, int zero) {
    if (!mm_base || n == 0 || n > mm_free_pg) { mm_stat_failed++; return 0; }
    for (u32 pass = 0; pass < 2; pass++) {
        u32 start = pass == 0 ? mm_hint : mm_bitmap_pages, end = pass == 0 ? mm_npages : mm_hint + n;
        if (end > mm_npages) end = mm_npages;
        u32 run = 0;
        for (u32 p = start; p < end; p++) {
            if (mm_bit(p)) { run = 0; continue; }
            if (++run == n) {
                u32 first = p + 1 - n;
                for (u32 q = first; q <= p; q++) mm_bit_set(q);
                mm_free_pg -= n; mm_hint = p + 1 < mm_npages ? p + 1 : mm_bitmap_pages;
                u8 *r = mm_base + (unsigned long)first * MM_PAGE;
                if (zero) for (u32 i = 0; i < n * MM_PAGE; i++) r[i] = 0;
                return r;
            }
        }
    }
    mm_stat_failed++;
    return 0;
}
static void *mm_pages_alloc(u32 n)  { return mm_pages_alloc_z(n, 0); }
static void *mm_page_alloc(void)    { return mm_pages_alloc_z(1, 1); }
/* Gives n pages back. A pointer that is not a page boundary inside the range, or pages that are not allocated, is counted and refused. */
static int mm_pages_free(void *p, u32 n) {
    unsigned long off = (unsigned long)((u8 *)p - mm_base);
    if (!mm_base || (u8 *)p < mm_base || (off & (MM_PAGE - 1)) || n == 0) { mm_stat_bad_frees++; return 0; }
    u32 first = (u32)(off / MM_PAGE);
    if (first < mm_bitmap_pages || first + n > mm_npages) { mm_stat_bad_frees++; return 0; }
    for (u32 q = first; q < first + n; q++) if (!mm_bit(q)) { mm_stat_bad_frees++; return 0; }      /* all or nothing */
    for (u32 q = first; q < first + n; q++) mm_bit_clr(q);
    mm_free_pg += n;
    if (first < mm_hint) mm_hint = first;
    return 1;
}

/* ---------------------------------------------------------------- the heap */
static inline mm_hdr_t *mm_next(const mm_chunk_t *c, mm_hdr_t *h) { u8 *n = (u8 *)h + h->size; return n >= c->base + c->bytes ? 0 : (mm_hdr_t *)n; }
static inline mm_hdr_t *mm_prev(const mm_chunk_t *c, mm_hdr_t *h) { return (u8 *)h == c->base ? 0 : (mm_hdr_t *)((u8 *)h - h->prev_size); }
static inline u32 mm_round(u32 n) { return (n + 15u) & ~15u; }

static mm_chunk_t *mm_chunk_of(const void *p) {
    for (int i = 0; i < MM_CHUNK_MAX; i++) if (mm_chunks[i].base && (const u8 *)p >= mm_chunks[i].base + MM_HDR && (const u8 *)p < mm_chunks[i].base + mm_chunks[i].bytes) return &mm_chunks[i];
    return 0;
}
static mm_chunk_t *mm_chunk_add(u32 need_block) {
    for (int i = 0; i < MM_CHUNK_MAX; i++) {
        if (mm_chunks[i].base) continue;
        u32 pages = (need_block + MM_PAGE - 1) / MM_PAGE;
        if (pages < MM_CHUNK_MIN_PG) pages = MM_CHUNK_MIN_PG;
        u8 *b = (u8 *)mm_pages_alloc_z(pages, 0);
        if (!b) return 0;
        mm_chunks[i].base = b; mm_chunks[i].bytes = pages * MM_PAGE;
        mm_hdr_t *h = (mm_hdr_t *)b;
        h->size = mm_chunks[i].bytes; h->prev_size = 0; h->magic = MM_MAGIC_FREE; h->pad = 0;
        return &mm_chunks[i];
    }
    return 0;
}
/* Carves `need` bytes (header included, a multiple of 16) out of free block h; the rest, if worth a block, stays free. */
static void *mm_carve(mm_chunk_t *c, mm_hdr_t *h, u32 need) {
    if (h->size >= need + MM_HDR + 16u) {
        mm_hdr_t *r = (mm_hdr_t *)((u8 *)h + need);
        r->size = h->size - need; r->prev_size = need; r->magic = MM_MAGIC_FREE; r->pad = 0;
        mm_hdr_t *after = mm_next(c, r);
        if (after) after->prev_size = r->size;
        h->size = need;
    }
    h->magic = MM_MAGIC_USED;
    return (u8 *)h + MM_HDR;
}
static void *kmalloc(u32 n) {
    if (n == 0 || n > MM_MAX_TOP / 2) { mm_stat_failed++; return 0; }
    u32 need = mm_round(n) + MM_HDR;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < MM_CHUNK_MAX; i++) {
            mm_chunk_t *c = &mm_chunks[i];
            if (!c->base) continue;
            for (mm_hdr_t *h = (mm_hdr_t *)c->base; h; h = mm_next(c, h)) {
                if (h->magic != MM_MAGIC_FREE && h->magic != MM_MAGIC_USED) { mm_stat_bad_frees++; return 0; }    /* a smashed header: stop walking, report */
                if (h->magic == MM_MAGIC_FREE && h->size >= need) { mm_stat_allocs++; return mm_carve(c, h, need); }
                if (h->size < MM_HDR) break;
            }
        }
        if (pass == 0) {                                   /* nothing fits: grow the heap by one chunk big enough for this */
            mm_chunk_t *c = mm_chunk_add(need);
            if (!c) { mm_stat_failed++; return 0; }
        }
    }
    mm_stat_failed++;
    return 0;
}
static void *kzalloc(u32 n) {
    u8 *p = (u8 *)kmalloc(n);
    if (p) { u32 m = mm_round(n); for (u32 i = 0; i < m; i++) p[i] = 0; }
    return p;
}
static u32 mm_usable(const void *p) {                       /* payload bytes of a live block, 0 for anything else */
    mm_chunk_t *c = mm_chunk_of(p);
    if (!c || ((unsigned long)((const u8 *)p - c->base) & 15u)) return 0;
    const mm_hdr_t *h = (const mm_hdr_t *)((const u8 *)p - MM_HDR);
    return h->magic == MM_MAGIC_USED ? h->size - MM_HDR : 0;
}
/* 1 = freed, 0 = refused (not ours, already free, or its header is damaged). */
static int kfree(void *p) {
    if (!p) return 1;
    mm_chunk_t *c = mm_chunk_of(p);
    if (!c || ((unsigned long)((u8 *)p - c->base) & 15u)) { mm_stat_bad_frees++; return 0; }
    mm_hdr_t *h = (mm_hdr_t *)((u8 *)p - MM_HDR);
    if (h->magic != MM_MAGIC_USED || h->size < MM_HDR + 16u || (u8 *)h + h->size > c->base + c->bytes) { mm_stat_bad_frees++; return 0; }
    h->magic = MM_MAGIC_FREE;
    mm_stat_frees++;
    mm_hdr_t *n = mm_next(c, h);
    if (n && n->magic == MM_MAGIC_FREE) { h->size += n->size; n->magic = 0; n = mm_next(c, h); if (n) n->prev_size = h->size; }       /* merge forward */
    mm_hdr_t *pr = mm_prev(c, h);
    if (pr && pr->magic == MM_MAGIC_FREE) { pr->size += h->size; h->magic = 0; h = pr; n = mm_next(c, h); if (n) n->prev_size = h->size; }   /* merge backward */
    if (h->size == c->bytes && c != &mm_chunks[0]) {                    /* the whole chunk is free again: back to the page allocator */
        u8 *b = c->base; u32 pages = c->bytes / MM_PAGE;
        c->base = 0; c->bytes = 0;
        mm_pages_free(b, pages);
    }
    return 1;
}
static void *krealloc(void *p, u32 n) {
    if (!p) return kmalloc(n);
    if (n == 0) { kfree(p); return 0; }
    u32 have = mm_usable(p);
    if (!have) { mm_stat_bad_frees++; return 0; }
    if (n <= have) return p;                                  /* shrinking or fitting: the block stays (no split, keeps this simple) */
    u8 *q = (u8 *)kmalloc(n);
    if (!q) return 0;                                         /* the old block is left alone, as C says */
    for (u32 i = 0; i < have; i++) q[i] = ((u8 *)p)[i];
    kfree(p);
    return q;
}

/* ---------------------------------------------------------------- numbers and checks */
static void mm_stats(mm_stats_t *s) {
    s->total_pages = mm_npages - mm_bitmap_pages; s->free_pages = mm_free_pg; s->used_pages = s->total_pages - mm_free_pg;
    s->heap_chunks = s->heap_bytes = s->heap_used = s->heap_free = s->heap_largest_free = 0;
    for (int i = 0; i < MM_CHUNK_MAX; i++) {
        mm_chunk_t *c = &mm_chunks[i];
        if (!c->base) continue;
        s->heap_chunks++; s->heap_bytes += c->bytes;
        for (mm_hdr_t *h = (mm_hdr_t *)c->base; h; h = mm_next(c, h)) {
            if (h->size < MM_HDR) break;
            if (h->magic == MM_MAGIC_USED) s->heap_used += h->size;
            else if (h->magic == MM_MAGIC_FREE) { s->heap_free += h->size; if (h->size - MM_HDR > s->heap_largest_free) s->heap_largest_free = h->size - MM_HDR; }
            else break;
        }
    }
    s->allocs = mm_stat_allocs; s->frees = mm_stat_frees; s->bad_frees = mm_stat_bad_frees; s->failed_allocs = mm_stat_failed;
}
/* Walks every heap block: headers must carry a valid magic, sizes must chain exactly to the end of the chunk, each block's
 * prev_size must equal the size of the block before it, and no two free blocks may sit side by side (they would have merged).
 * Also recounts the bitmap against the free-page counter. Returns the number of problems found (0 = healthy). */
static int mm_check(void) {
    int bad = 0;
    for (int i = 0; i < MM_CHUNK_MAX; i++) {
        mm_chunk_t *c = &mm_chunks[i];
        if (!c->base) continue;
        u32 seen = 0, prev = 0; int prev_free = 0;
        for (mm_hdr_t *h = (mm_hdr_t *)c->base; h; h = mm_next(c, h)) {
            if (h->magic != MM_MAGIC_USED && h->magic != MM_MAGIC_FREE) { bad++; break; }
            if (h->size < MM_HDR + 16u || (h->size & 15u) || seen + h->size > c->bytes) { bad++; break; }
            if (h->prev_size != prev) bad++;
            int fr = h->magic == MM_MAGIC_FREE;
            if (fr && prev_free) bad++;
            seen += h->size; prev = h->size; prev_free = fr;
        }
        if (seen != c->bytes) bad++;
    }
    u32 clear = 0;
    for (u32 p = mm_bitmap_pages; p < mm_npages; p++) if (!mm_bit(p)) clear++;
    if (clear != mm_free_pg) bad++;
    return bad;
}

/* Boot: manage everything above the last fixed region. Stage 2 left the RAM size (KB, E801h) at 0x9200. */
#ifndef MW_MM_FIRST_ADDR
#define MW_MM_FIRST_ADDR 0x00900000u
#endif
static int mm_init_boot(void) {
    u32 total_kb = *(volatile u32 *)0x9200;
    u32 top = total_kb >= (MM_MAX_TOP / 1024u) ? MM_MAX_TOP : total_kb * 1024u;
    if (top <= MW_MM_FIRST_ADDR + 64u * 1024u) return 0;                 /* nothing worth managing (VGA_MIN_RAM_KB would already have refused to boot) */
    return mm_init_region((void *)(unsigned long)MW_MM_FIRST_ADDR, top - MW_MM_FIRST_ADDR);
}

#endif
