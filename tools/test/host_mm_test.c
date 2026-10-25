/* SPDX-License-Identifier: GPL-3.0-only */
/* host_mm_test.c -- kernel/mm.h on an ordinary array: the page allocator (contiguous runs, exhaustion, refused bad frees),
 * the heap (alignment, split/merge, chunk growth and release, krealloc, double free, wild pointers, a smashed header found by
 * mm_check) and a randomized run where every live block carries a pattern that must still be intact when it is freed.
 *   tools/test/run_host_mm.sh */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define IO_H
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            i32;
#include "../../kernel/mm.h"

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

#define ARENA (8u * 1024u * 1024u)
static u8 *arena;
static void fresh(void) { CHECK(mm_init_region(arena, ARENA)); }

static void test_pages(void) {
    fresh();
    mm_stats_t s; mm_stats(&s);
    CHECK(s.total_pages + 1 == ARENA / MM_PAGE);                       /* one page of bitmap for 2048 pages */
    u32 total = s.free_pages;
    u8 *a = mm_pages_alloc(3), *b = mm_pages_alloc(1);
    CHECK(a && b && b == a + 3 * MM_PAGE);
    CHECK(((unsigned long)a & (MM_PAGE - 1)) == 0);
    mm_stats(&s); CHECK(s.free_pages == total - 4);
    CHECK(mm_pages_free(a, 3) == 1);
    CHECK(mm_pages_free(a, 3) == 0);                                   /* double free: refused */
    CHECK(mm_pages_free(a + 1, 1) == 0);                               /* not a page boundary */
    CHECK(mm_pages_free(arena - MM_PAGE, 1) == 0 && mm_pages_free(arena, 1) == 0);   /* outside / the bitmap itself */
    u8 *c = mm_pages_alloc(2);                                         /* the hole at a (3 pages) is reused for 2 */
    CHECK(c != 0 && mm_check() == 0);
    CHECK(mm_pages_alloc(total) == 0);                                 /* more than is free */
    u8 *z = mm_page_alloc(); for (u32 i = 0; i < MM_PAGE; i++) CHECK(z[i] == 0 || (checks--, failed++, printf("page not zeroed\n"), 0)); mm_pages_free(z, 1);
    mm_pages_free(c, 2); mm_pages_free(b, 1);
    mm_stats(&s); CHECK(s.free_pages == total && s.bad_frees == 4);
    /* exhaustion and recovery */
    void *all = mm_pages_alloc(total);
    CHECK(all != 0); mm_stats(&s); CHECK(s.free_pages == 0);
    CHECK(mm_page_alloc() == 0 && kmalloc(16) == 0);
    mm_pages_free(all, total); mm_stats(&s); CHECK(s.free_pages == total);
}

static void test_heap_basic(void) {
    fresh();
    u8 *p = kmalloc(1), *q = kmalloc(100), *r = kmalloc(5000);
    CHECK(p && q && r);
    CHECK((((unsigned long)p | (unsigned long)q | (unsigned long)r) & 15u) == 0);   /* 16-aligned */
    CHECK(mm_usable(p) >= 1 && mm_usable(q) >= 100 && mm_usable(r) >= 5000);
    memset(p, 1, 1); memset(q, 2, 100); memset(r, 3, 5000);
    CHECK(mm_check() == 0);
    CHECK(kfree(q) == 1); CHECK(mm_check() == 0);
    CHECK(kfree(q) == 0);                                              /* double free refused ... */
    u8 *q2 = kmalloc(100); CHECK(q2 == q);                             /* ... and the hole is reused */
    CHECK(kfree((u8 *)q2 + 16) == 0);                                  /* inside a block, not its start */
    CHECK(kfree((void *)(arena + 64)) == 0);                           /* not in any chunk */
    CHECK(kfree(0) == 1);                                              /* free(NULL) is fine */
    CHECK(kmalloc(0) == 0);
    kfree(p); kfree(q2); kfree(r);
    CHECK(mm_check() == 0);
    mm_stats_t s; mm_stats(&s);
    CHECK(s.heap_chunks == 1 && s.heap_used == 0 && s.heap_free == s.heap_bytes);   /* everything merged back into one block */
    u8 *z = kzalloc(300); int zero = 1; for (int i = 0; i < 300; i++) if (z[i]) zero = 0; CHECK(zero); kfree(z);
}

static void test_grow_release(void) {
    fresh();
    mm_stats_t s; mm_stats(&s); u32 free0 = s.free_pages;
    u8 *a = kmalloc(60000), *b = kmalloc(60000), *c = kmalloc(300000);    /* the heap grows: a second and a third chunk */
    CHECK(a && b && c);
    mm_stats(&s); CHECK(s.heap_chunks >= 2 && s.free_pages < free0);
    CHECK(mm_check() == 0);
    kfree(c);                                                              /* an emptied chunk goes back to the page allocator */
    mm_stats_t t; mm_stats(&t); CHECK(t.free_pages > s.free_pages);
    kfree(a); kfree(b);
    mm_stats(&s); CHECK(s.heap_chunks == 1 && s.free_pages == free0 - (s.heap_bytes / MM_PAGE));
    CHECK(mm_check() == 0);
    CHECK(kmalloc(ARENA) == 0);                                            /* never fits: a clean failure */
    mm_stats(&s); CHECK(s.failed_allocs >= 1);
}

static void test_realloc(void) {
    fresh();
    u8 *p = krealloc(0, 40); CHECK(p != 0);
    for (int i = 0; i < 40; i++) p[i] = (u8)i;
    u8 *q = krealloc(p, 5000); CHECK(q != 0);
    int same = 1; for (int i = 0; i < 40; i++) if (q[i] != i) same = 0; CHECK(same);
    CHECK(krealloc(q, 10) == q);                                          /* shrinking keeps the block */
    CHECK(krealloc(q, 0) == 0);                                           /* realloc(p, 0) frees */
    CHECK(krealloc(arena + 100, 8) == 0);                                 /* a pointer that is not ours */
    CHECK(mm_check() == 0);
}

static void test_smash(void) {
    fresh();
    u8 *a = kmalloc(64), *b = kmalloc(64);
    CHECK(mm_check() == 0);
    ((u32 *)(b - MM_HDR))[2] = 0x12345678u;                               /* an overrun from a into b's header (the magic) */
    CHECK(mm_check() > 0);                                                /* found by the walk */
    CHECK(kfree(b) == 0);                                                 /* refused, not trusted */
    ((u32 *)(b - MM_HDR))[2] = MM_MAGIC_USED;                             /* repair for the next test */
    CHECK(mm_check() == 0);
    kfree(a); kfree(b);
}

static u32 rng = 12345;
static u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
#define LIVE 400
static void test_random(void) {
    fresh();
    struct { u8 *p; u32 n; u8 pat; } live[LIVE]; memset(live, 0, sizeof live);
    for (int step = 0; step < 60000; step++) {
        int i = (int)(rnd() % LIVE);
        if (live[i].p) {
            for (u32 k = 0; k < live[i].n; k++) if (live[i].p[k] != live[i].pat) { CHECK(!"block contents damaged"); break; }
            if (rnd() % 4 == 0) {                                          /* realloc: the old contents must survive */
                u32 nn = 1 + rnd() % 3000; u8 *q = krealloc(live[i].p, nn);
                if (q) {
                    u32 keep = nn < live[i].n ? nn : live[i].n;
                    for (u32 k = 0; k < keep; k++) if (q[k] != live[i].pat) { CHECK(!"realloc lost data"); break; }
                    memset(q, live[i].pat, nn); live[i].p = q; live[i].n = nn;
                }
            } else { CHECK(kfree(live[i].p) == 1); live[i].p = 0; }
        } else {
            u32 n = 1 + (rnd() % 10 == 0 ? rnd() % 90000 : rnd() % 700);
            u8 *p = kmalloc(n);
            if (p) { live[i].p = p; live[i].n = n; live[i].pat = (u8)(1 + rnd() % 250); memset(p, live[i].pat, n); }
        }
        if (step % 5000 == 0) CHECK(mm_check() == 0);
    }
    for (int i = 0; i < LIVE; i++) if (live[i].p) CHECK(kfree(live[i].p) == 1);
    CHECK(mm_check() == 0);
    mm_stats_t s; mm_stats(&s);
    CHECK(s.heap_used == 0 && s.heap_chunks == 1 && s.bad_frees == 0);
}

int main(void) {
    if (posix_memalign((void **)&arena, MM_PAGE, ARENA)) return 2;
    test_pages(); test_heap_basic(); test_grow_release(); test_realloc(); test_smash(); test_random();
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
