/* port/3ds/test/gdx3ds_heapcfg_host_tests.c -- the libctru heap-split formula, checked
 * against grants the GUEST actually printed (NEXT.md C4b / C4e).
 *
 * The formula in port/3ds/include/gdx3ds_heapcfg.h was transcribed by hand from the
 * disassembly of the libctru archive this port links. A transcription is a claim, and the
 * whole Old 3DS budget rests on it -- including the projection that says the shipped build
 * is ~19 MB short on a memtype-0 console. So the first three cases below are not synthetic:
 * they are the three `[mem-budget] heapGrant=/linGrant=` pairs from
 * docs/research/native/o3dsbudget-progress.md section 1.3, fed back in as
 * avail = heapGrant + linGrant. If the transcription were wrong, these would not reproduce.
 *
 * Case 3 is the one that has never been observed: it is the projection for the 64 MB
 * memtype-0 layout, and it is marked as such. It is here so that the number the track is
 * planned around is computed by the same expressions the build uses, not by a spreadsheet.
 */
#include <stdio.h>

#include "gdx3ds_heapcfg.h"

static int failures = 0;

static void expect_eq(const char* what, unsigned long got, unsigned long want) {
    if (got != want) {
        printf("FAIL %-52s got=%lu want=%lu\n", what, got, want);
        failures++;
    } else {
        printf("ok   %-52s %lu\n", what, got);
    }
}

/* Every split libctru produces must commit the whole page-aligned budget and nothing more:
 * __system_allocateHeaps svcBreaks on heap+linear > avail, and it never leaves a gap. */
static void expect_exact_split(const char* what, unsigned long avail) {
    const unsigned long h = GDX3DS_HEAPCFG_AUTO_HEAP(avail);
    const unsigned long l = GDX3DS_HEAPCFG_AUTO_LINEAR(avail);
    char buf[128];
    snprintf(buf, sizeof(buf), "%s: heap+linear == page_down(avail)", what);
    expect_eq(buf, h + l, GDX3DS_HEAPCFG_PAGE_DOWN(avail));
    snprintf(buf, sizeof(buf), "%s: heap page-aligned", what);
    expect_eq(buf, h & 0xFFFUL, 0UL);
    snprintf(buf, sizeof(buf), "%s: linear page-aligned", what);
    expect_eq(buf, l & 0xFFFUL, 0UL);
}

int main(void) {
    /* --- 1. New 3DS, memtype 6. Run 20260924-104853-retro. ------------------------- */
    {
        const unsigned long avail = 85311488UL + 33554432UL; /* = 118,865,920 */
        expect_eq("n3ds memtype6: heapGrant", GDX3DS_HEAPCFG_AUTO_HEAP(avail), 85311488UL);
        expect_eq("n3ds memtype6: linGrant", GDX3DS_HEAPCFG_AUTO_LINEAR(avail), 33554432UL);
        expect_exact_split("n3ds memtype6", avail);
    }

    /* --- 2. Azahar's "Old 3DS", memtype 2 (96 MB extended). Run 20260924-105108. ---- */
    {
        const unsigned long avail = 55951360UL + 33554432UL; /* = 89,505,792 */
        expect_eq("o3ds memtype2: heapGrant", GDX3DS_HEAPCFG_AUTO_HEAP(avail), 55951360UL);
        expect_eq("o3ds memtype2: linGrant", GDX3DS_HEAPCFG_AUTO_LINEAR(avail), 33554432UL);
        expect_exact_split("o3ds memtype2", avail);
    }

    /* --- 3. PROJECTION, never observed: Old 3DS memtype 0, the 64 MB layout a .3dsx
     * inherits under hbmenu. avail = 64 MB region - 11,157,504 B of image+bss+slack, and
     * that constant is not a guess: cases 1 and 2 both give REGION - (heapGrant+linGrant)
     * = 11,157,504 exactly, on two different memtypes, for this same .3dsx.
     *
     * The answer is 24 MiB EXACTLY, and that is the load-bearing detail: on this branch
     * the heap is pinned at the cap and does not vary with avail at all, so it is 24 MiB
     * for any console budget between 24 MiB and 56 MiB. The "~23.9 MB" that the earlier
     * write-up carried came from comparing a decimal-MB linear against a 32 MiB cap. */
    {
        const unsigned long avail = 67108864UL - 11157504UL; /* = 55,951,360 */
        expect_eq("o3ds memtype0 (projected): avail", avail, 55951360UL);
        expect_eq("o3ds memtype0 (projected): heapGrant", GDX3DS_HEAPCFG_AUTO_HEAP(avail),
                  25165824UL); /* 24 MiB, capped */
        expect_eq("o3ds memtype0 (projected): linGrant", GDX3DS_HEAPCFG_AUTO_LINEAR(avail),
                  30785536UL); /* under the 32 MiB cap, so NOT capped */
        expect_exact_split("o3ds memtype0", avail);
    }

    /* --- 4. The cap is at the boundary, and it is `>` not `>=`. A budget small enough
     * that half of it is under 24 MiB takes the uncapped branch and splits evenly. */
    {
        const unsigned long avail = 33554432UL - 11157504UL; /* memtype 5, 32 MB region */
        expect_eq("o3ds memtype5: heapGrant", GDX3DS_HEAPCFG_AUTO_HEAP(avail), 11198464UL);
        expect_eq("o3ds memtype5: linGrant", GDX3DS_HEAPCFG_AUTO_LINEAR(avail), 11198464UL);
        expect_exact_split("o3ds memtype5", avail);
    }
    {
        /* exactly 48 MiB: h0 = l0 = 24 MiB, `heap > 24 MiB` is false -> even split */
        const unsigned long avail = 48UL * 1024UL * 1024UL;
        expect_eq("48MiB avail: heap (cap boundary, not exceeded)",
                  GDX3DS_HEAPCFG_AUTO_HEAP(avail), 25165824UL);
        expect_eq("48MiB avail: linear", GDX3DS_HEAPCFG_AUTO_LINEAR(avail), 25165824UL);
    }
    {
        /* one page over: h0 = 24 MiB + 4 KiB > cap -> heap pinned to 24 MiB */
        const unsigned long avail = 48UL * 1024UL * 1024UL + 8192UL;
        expect_eq("48MiB+8K avail: heap (cap exceeded)", GDX3DS_HEAPCFG_AUTO_HEAP(avail),
                  25165824UL);
        expect_eq("48MiB+8K avail: linear", GDX3DS_HEAPCFG_AUTO_LINEAR(avail),
                  25165824UL + 8192UL);
    }

    /* --- 5. Page alignment: avail is masked down before anything else happens. -------- */
    {
        const unsigned long ragged = 55951360UL + 3000UL;
        expect_eq("ragged avail: heap unchanged by sub-page bytes",
                  GDX3DS_HEAPCFG_AUTO_HEAP(ragged), GDX3DS_HEAPCFG_AUTO_HEAP(55951360UL));
        expect_eq("ragged avail: linear unchanged by sub-page bytes",
                  GDX3DS_HEAPCFG_AUTO_LINEAR(ragged), GDX3DS_HEAPCFG_AUTO_LINEAR(55951360UL));
    }

    /* --- 6. C4b: __ctru_linear_heap_size pinned, heap left at 0. NO 24 MiB CAP.
     * This is the branch the whole cut depends on, and the numbers here are the ones the
     * decision gets made on. Against a 44.43 MB requirement, 12 MiB of linear is NOT
     * enough on a memtype-0 console -- it lands 1.06 MB short before any headroom at all. */
    {
        const unsigned long avail = 55951360UL; /* memtype 0, projected */
        expect_eq("C4b linear=12MiB -> heap", 
                  GDX3DS_HEAPCFG_HEAP_GIVEN_LINEAR(avail, 12UL * 1024UL * 1024UL), 43368448UL);
        expect_eq("C4b linear=10MiB -> heap",
                  GDX3DS_HEAPCFG_HEAP_GIVEN_LINEAR(avail, 10UL * 1024UL * 1024UL), 45465600UL);
        expect_eq("C4b linear=9MiB -> heap",
                  GDX3DS_HEAPCFG_HEAP_GIVEN_LINEAR(avail, 9UL * 1024UL * 1024UL), 46514176UL);
        /* measured peak linear use, levers off, race-mutecity-start: 7,702,784 B */
        expect_eq("C4b linear=9MiB is above the measured linUsed peak",
                  (9UL * 1024UL * 1024UL) > 7702784UL ? 1UL : 0UL, 1UL);
    }

    printf(failures == 0 ? "=== ALL PASS (0 failures) ===\n" : "=== %d FAILURES ===\n", failures);
    return failures == 0 ? 0 : 1;
}
