/* port/tests/gdx_net_enabled_default_host_tests.c -- truth table for
 * gdx_net_enabled_resolve / gdx_net_enabled_was_defaulted (port/gdx_net_enabled_default.c,
 * n60lead hwlink round).
 *
 * WHY THIS TEST EXISTS. The emulator lanes (Azahar, RetroArch) cannot simulate a 3dslink
 * netloader launch -- there is no __3dslink_host to set, so a hardware-launch-method A/B
 * cannot run there (confirmed empirically: a full retro-lane run with --model new and no
 * [net] key in the ini produced zero [net] receipts, exactly the always-off-emulator
 * case this table covers). The default-resolution LOGIC itself is still fully covered
 * here, against the real function gdx3ds_net.c calls -- not a reimplementation of it.
 *
 * Compiled standalone (tools/ci-3ds.sh) against port/gdx_net_enabled_default.c alone --
 * no <3ds.h>/socket/thread dependency, which is also why that logic was split out of
 * port/3ds/net/gdx3ds_net.c into its own TU (see that file's own top comment).
 */
#include <stdio.h>

#include "gdx_net_enabled_default.h"

static int sFailures = 0;

static void expect_eq(const char* what, int got, int want) {
    if (got != want) {
        printf("FAIL: %s -- got %d want %d\n", what, got, want);
        sFailures++;
    }
}

/* An explicit ini key always wins, whichever way the launch method would have voted. */
static void test_explicit_ini_key_always_wins(void) {
    expect_eq("explicit 1, netloader", gdx_net_enabled_resolve(1, 1), 1);
    expect_eq("explicit 1, no netloader", gdx_net_enabled_resolve(1, 0), 1);
    expect_eq("explicit 0, netloader", gdx_net_enabled_resolve(0, 1), 0);
    expect_eq("explicit 0, no netloader", gdx_net_enabled_resolve(0, 0), 0);
}

/* No usable ini key (the sentinel came back unchanged): the launch method decides. */
static void test_sentinel_defers_to_the_launch_method(void) {
    expect_eq("sentinel, netloader -> ON",
              gdx_net_enabled_resolve(GDX_NET_ENABLED_SENTINEL, 1), 1);
    expect_eq("sentinel, no netloader -> OFF",
              gdx_net_enabled_resolve(GDX_NET_ENABLED_SENTINEL, 0), 0);
}

/* Any value gdx3ds_config_get_bool cannot have produced from a real ini line (not 0, not
 * 1 -- the sentinel is just the specific one gdx3ds_net.c happens to pass) is treated the
 * same as the sentinel: the function only ever special-cases exactly 0 and exactly 1. */
static void test_any_non_0_1_value_defers_too(void) {
    expect_eq("-7, netloader -> ON", gdx_net_enabled_resolve(-7, 1), 1);
    expect_eq("42, no netloader -> OFF", gdx_net_enabled_resolve(42, 0), 0);
}

static void test_was_defaulted_flag(void) {
    expect_eq("was_defaulted(sentinel)",
              gdx_net_enabled_was_defaulted(GDX_NET_ENABLED_SENTINEL), 1);
    expect_eq("was_defaulted(0)", gdx_net_enabled_was_defaulted(0), 0);
    expect_eq("was_defaulted(1)", gdx_net_enabled_was_defaulted(1), 0);
    expect_eq("was_defaulted(99)", gdx_net_enabled_was_defaulted(99), 1);
}

/* The sentinel itself must sit outside the two real bool values -- if it ever collided
 * with 0 or 1 the whole scheme silently stops distinguishing "explicit" from "absent". */
static void test_sentinel_is_not_a_real_bool_value(void) {
    expect_eq("sentinel != 0", GDX_NET_ENABLED_SENTINEL == 0, 0);
    expect_eq("sentinel != 1", GDX_NET_ENABLED_SENTINEL == 1, 0);
}

int main(void) {
    test_explicit_ini_key_always_wins();
    test_sentinel_defers_to_the_launch_method();
    test_any_non_0_1_value_defers_too();
    test_was_defaulted_flag();
    test_sentinel_is_not_a_real_bool_value();

    if (sFailures != 0) {
        printf("=== %d FAILURE(S) ===\n", sFailures);
        return 1;
    }
    printf("=== ALL PASS (0 failures) ===\n");
    return 0;
}
