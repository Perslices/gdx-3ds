/* port/3ds/test/gdx3ds_menu_tabbar_host_tests.c -- the MENU tab bar's column layout
 * (gdx3ds_menu_tabbar_layout.h), against every combination of the two tabs whose presence is
 * dynamic: MAP ([debug] bottom_map) and RACE (reachable only mid-race, once the DASH takeover
 * has been dismissed -- see gdx3ds_menu.c's TAB_RACE enum comment).
 *
 * hw bug n60-3d8, item 1: "it had the race tab on debug but was unclickable because OFF was
 * cover[ing] it". The algorithm under test here was never actually the bug (every combination
 * below already comes out overlap-free -- that is exactly what this file proves, and keeps
 * proving on every future label change). The real bug was gdx3ds_menu.c calling this layout
 * exactly once, at init, and caching the result while RACE's actual visibility changed live
 * underneath it -- see MenuLayoutTabs()'s own comment in that file for the fix. This test
 * exists so the one thing that WOULD still be a real bug -- the layout math itself producing
 * an overlap for some future label set -- gets caught here, on a host, instead of on
 * hardware.
 *
 * kLabels below is a hand copy of gdx3ds_menu.c's kTabLabels (TAB_STATUS..TAB_ABOUT, i.e.
 * TAB_COUNT == 10) -- keep the two in sync if that array ever changes. */
#include <stdio.h>
#include <string.h>

#include "gdx3ds_menu_tabbar_layout.h"

#define MENU_COLS 40
#define TAB_COUNT 10
#define TAB_MAP 7
#define TAB_RACE 8

static const char* const kLabels[TAB_COUNT] = {
    "STA", "DSP", "3D", "AUD", "IN", "LG", "DBG", "MAP", "RACE", "AB",
};

static int sFailures = 0;

static void Check(int ok, const char* what) {
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        sFailures++;
    }
}

/* 1 if [a0,a1] and [b0,b1] (both inclusive, 1-based) share a column. An entry with c1 < c0
 * (the "hidden" sentinel, e.g. {0,-1}) can never overlap anything -- the range is empty. */
static int RangesOverlap(int a0, int a1, int b0, int b1) {
    if (a1 < a0 || b1 < b0) {
        return 0;
    }
    return a0 <= b1 && b0 <= a1;
}

static void CheckCombo(int mapShown, int raceShown) {
    int hidden[TAB_COUNT];
    Gdx3dsMenuTabCols cols[TAB_COUNT];
    int offC0 = -1, offC1 = -1;
    char tag[64];
    int t, u;

    snprintf(tag, sizeof(tag), "MAP=%d RACE=%d", mapShown, raceShown);

    for (t = 0; t < TAB_COUNT; t++) {
        hidden[t] = 0;
    }
    hidden[TAB_MAP] = !mapShown;
    hidden[TAB_RACE] = !raceShown;

    gdx3ds_menu_layout_tabs(kLabels, hidden, TAB_COUNT, MENU_COLS, cols, &offC0, &offC1);

    /* --- within 320px / 40 columns, and OFF always reaches the right edge exactly. ----- */
    {
        char what[96];
        snprintf(what, sizeof(what), "%s: offC0 >= 1", tag);
        Check(offC0 >= 1, what);
        snprintf(what, sizeof(what), "%s: offC0 <= offC1", tag);
        Check(offC0 <= offC1, what);
        snprintf(what, sizeof(what), "%s: offC1 == MENU_COLS (40)", tag);
        Check(offC1 == MENU_COLS, what);
    }
    for (t = 0; t < TAB_COUNT; t++) {
        char what[96];
        if (hidden[t]) {
            snprintf(what, sizeof(what), "%s: tab %d hidden -> unreachable (c1 < c0)", tag, t);
            Check(cols[t].c1 < cols[t].c0, what);
            continue;
        }
        snprintf(what, sizeof(what), "%s: tab %d c0 >= 1", tag, t);
        Check(cols[t].c0 >= 1, what);
        snprintf(what, sizeof(what), "%s: tab %d c1 <= MENU_COLS (40)", tag, t);
        Check(cols[t].c1 <= MENU_COLS, what);
        snprintf(what, sizeof(what), "%s: tab %d c0 <= c1", tag, t);
        Check(cols[t].c0 <= cols[t].c1, what);
    }

    /* --- no overlap between any two VISIBLE tabs. -------------------------------------- */
    for (t = 0; t < TAB_COUNT; t++) {
        if (hidden[t]) {
            continue;
        }
        for (u = t + 1; u < TAB_COUNT; u++) {
            char what[96];
            if (hidden[u]) {
                continue;
            }
            snprintf(what, sizeof(what), "%s: tab %d/%d do not overlap", tag, t, u);
            Check(!RangesOverlap(cols[t].c0, cols[t].c1, cols[u].c0, cols[u].c1), what);
        }
    }

    /* --- the actual n60-3d8 bug: OFF must not overlap any visible tab, MAP/RACE above
     * all -- this is the exact pair the hardware report was about. --------------------- */
    for (t = 0; t < TAB_COUNT; t++) {
        char what[96];
        if (hidden[t]) {
            continue;
        }
        snprintf(what, sizeof(what), "%s: tab %d does not overlap OFF", tag, t);
        Check(!RangesOverlap(cols[t].c0, cols[t].c1, offC0, offC1), what);
    }
}

int main(void) {
    CheckCombo(0, 0);
    CheckCombo(1, 0);
    CheckCombo(0, 1);
    CheckCombo(1, 1); /* the worst case, and the one hardware actually hit */

    printf(sFailures == 0 ? "=== ALL PASS (0 failures) ===\n" : "=== %d FAILURES ===\n", sFailures);
    return sFailures == 0 ? 0 : 1;
}
