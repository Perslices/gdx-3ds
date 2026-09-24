/* port/3ds/os/test/gdx3ds_config_host_tests.c — host-compiled tests for the ini table.
 *
 * gdx3ds_config.c is plain C over stdio/string, so the table logic can be exercised on
 * the build machine. That matters for gdx3ds_config_unset: the hardware console is the
 * only place the net control path runs, and the user is mid-test on it, so this is how
 * the guest-side behaviour of `INI DEL` gets verified without touching the console.
 *
 * Build: cc -O1 -I port/3ds/os port/3ds/os/gdx3ds_config.c <this> -o config_host_tests
 */

#include "gdx3ds_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sFailures = 0;

static void check(int cond, const char* what) {
    printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) {
        sFailures++;
    }
}

static const char* kPath = "gdx3ds_config_host_tests.ini";

static void write_ini(const char* body) {
    FILE* f = fopen(kPath, "w");
    if (f == NULL) {
        printf("FAIL: cannot write %s\n", kPath);
        exit(1);
    }
    fputs(body, f);
    fclose(f);
}

static int file_contains(const char* needle) {
    char buf[4096];
    size_t n;
    FILE* f = fopen(kPath, "r");
    if (f == NULL) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

int main(void) {
    write_ini("[debug]\ninput_replay = race-mutecity-start\nverbose = 1\n"
              "\n[display]\nborder_mode = 2\n");
    check(gdx3ds_config_load(kPath) == 0, "load the ini");

    /* The bug: setting the empty string leaves the ENTRY in place, so the getter returns
       "" instead of the default and save writes the key back out — the replay stays armed. */
    check(gdx3ds_config_set_string("debug", "input_replay", "") == 0, "set empty string");
    check(strcmp(gdx3ds_config_get_string("debug", "input_replay", "DEFAULT"), "") == 0,
          "an empty value is NOT the default (this is why clearing did not work)");
    check(gdx3ds_config_save(kPath) == 0, "save after the empty set");
    check(file_contains("input_replay"), "the key survives a save when set empty");

    /* The fix. */
    check(gdx3ds_config_unset("debug", "input_replay") == 0, "unset returns 0 when removed");
    check(strcmp(gdx3ds_config_get_string("debug", "input_replay", "DEFAULT"), "DEFAULT") == 0,
          "after unset the getter falls back to the DEFAULT");
    check(gdx3ds_config_save(kPath) == 0, "save after unset");
    check(!file_contains("input_replay"), "the key is gone from the saved ini");

    /* Neighbours, order and idempotence. */
    check(gdx3ds_config_get_int("debug", "verbose", 0) == 1, "a sibling key is untouched");
    check(gdx3ds_config_get_int("display", "border_mode", 0) == 2,
          "another section is untouched");
    check(file_contains("[debug]") && file_contains("[display]"),
          "both sections survive the save");
    check(gdx3ds_config_unset("debug", "input_replay") == 3,
          "unset returns 3 when the key is already absent");
    check(gdx3ds_config_unset("debug", "") == 1, "unset rejects an empty key");
    check(gdx3ds_config_unset("debug", NULL) == 1, "unset rejects a NULL key");

    /* Case folding matches the setter/getter. */
    check(gdx3ds_config_set_string("Debug", "TmemFast", "1") == 0, "set with mixed case");
    check(gdx3ds_config_unset("DEBUG", "tmemfast") == 0, "unset is case-insensitive too");
    check(gdx3ds_config_get_int("debug", "tmemfast", 9) == 9, "and the default returns");

    /* Order is preserved: unsetting the middle key must not reshuffle the rest. */
    write_ini("[debug]\na = 1\nb = 2\nc = 3\n");
    check(gdx3ds_config_load(kPath) == 0, "reload for the ordering check");
    check(gdx3ds_config_unset("debug", "b") == 0, "remove the middle key");
    check(gdx3ds_config_get_int("debug", "a", 0) == 1 &&
          gdx3ds_config_get_int("debug", "c", 0) == 3,
          "the keys either side keep their values");
    check(gdx3ds_config_save(kPath) == 0, "save after the middle removal");
    check(file_contains("a = 1") && file_contains("c = 3") && !file_contains("b = 2"),
          "the saved ini keeps a and c and drops b");

    remove(kPath);
    if (sFailures == 0) {
        printf("=== ALL PASS (0 failures) ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", sFailures);
    return 1;
}
