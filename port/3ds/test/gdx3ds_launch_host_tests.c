/* port/3ds/test/gdx3ds_launch_host_tests.c -- N60LAUNCH: the launch-variant selection rules
 * (gdx3ds_launch_core.c) over the real ini loader (gdx3ds_config.c), on the build machine.
 *
 * Build: cc -O1 -I port/3ds -I port/3ds/os port/3ds/gdx3ds_launch_core.c port/3ds/os/gdx3ds_config.c
 *        <this> -o launch_host_tests   (run in a scratch CWD: it writes its fixture ini there)
 */
#include "gdx3ds_config.h"
#include "gdx3ds_launch_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int sFailures = 0;
static int sChecks = 0;

static void check(int cond, const char* what) {
    sChecks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        sFailures++;
    }
}

static const char* kPath = "gdx3ds_launch_host_tests.ini";

/* The n60-3d3 card's sections, verbatim in spirit: main ini + c5steal, RUN N, RUN P. */
static const char* kCardIni = "[debug]\n"
                              "c5steal = 1\n"
                              "launch_variants = 1\n"
                              "\n"
                              "[variant.N]\n"
                              "ndsp_core1 = 1\n"
                              "\n"
                              "[variant.P]\n"
                              "core1_probe = 1\n"
                              "core1_probe_pcts = 30,50,80\n"
                              "core1_probe_window = 240\n"
                              "native.all = 0          ; another section, spelled section.key\n"
                              "\n"
                              "[launch]\n"
                              "hold_l = N\n"
                              "hold_r = P\n";

static void load_ini(const char* body) {
    FILE* f = fopen(kPath, "w");
    if (f == NULL) {
        printf("FAIL: cannot write %s\n", kPath);
        exit(1);
    }
    fputs(body, f);
    fclose(f);
    if (gdx3ds_config_load(kPath) != 0) {
        printf("FAIL: cannot load %s\n", kPath);
        exit(1);
    }
}

typedef struct SinkLog {
    int calls;
    int failures;
    char last[96];
} SinkLog;

static void RecordOverride(const char* section, const char* key, const char* value, int rc, void* ctx) {
    SinkLog* log = (SinkLog*)ctx;
    log->calls++;
    if (rc != 0) {
        log->failures++;
    }
    snprintf(log->last, sizeof(log->last), "[%s] %s = %s", section, key, value);
}

static void test_section_enumeration(void) {
    const char* key = NULL;
    const char* value = NULL;
    load_ini(kCardIni);
    check(gdx3ds_config_section_entry("variant.n", 0, &key, &value) == 1, "variant.N entry 0 exists");
    check(key != NULL && strcmp(key, "ndsp_core1") == 0 && strcmp(value, "1") == 0, "variant.N entry 0 is ndsp_core1=1");
    check(gdx3ds_config_section_entry("VARIANT.N", 1, &key, &value) == 0, "variant.N has exactly one entry");
    check(gdx3ds_config_section_entry("variant.p", 3, &key, &value) == 1 && strcmp(key, "native.all") == 0,
          "variant.P entry 3 keeps its dotted key");
    check(gdx3ds_config_entry_count() == 9, "entry count counts every key");
    check(gdx3ds_config_entry_capacity() == 256, "capacity is the fixed table size");
}

static void test_hold_bindings(void) {
    const GdxLaunchButton* button = NULL;
    load_ini(kCardIni);
    check(gdx_launch_bound_mask() == (GDX_LAUNCH_KEY_L | GDX_LAUNCH_KEY_R), "L and R are bound");
    check(gdx_launch_pick_held(0, &button) == NULL, "nothing held = no pick");
    check(gdx_launch_pick_held(GDX_LAUNCH_KEY_X | GDX_LAUNCH_KEY_START, &button) == NULL, "unbound keys = no pick");
    const char* name = gdx_launch_pick_held(GDX_LAUNCH_KEY_L, &button);
    check(name != NULL && strcmp(name, "N") == 0 && strcmp(button->bindKey, "hold_l") == 0, "L picks N via hold_l");
    name = gdx_launch_pick_held(GDX_LAUNCH_KEY_R | (1u << 0), &button);
    check(name != NULL && strcmp(name, "P") == 0 && strcmp(button->label, "R") == 0, "R (+A) picks P");
    name = gdx_launch_pick_held(GDX_LAUNCH_KEY_L | GDX_LAUNCH_KEY_R, &button);
    check(name != NULL && strcmp(name, "N") == 0, "L+R: table order, L wins");
}

static void test_cycle(void) {
    char names[GDX_LAUNCH_MAX_CYCLE][GDX_LAUNCH_NAME_LEN];
    char pick[GDX_LAUNCH_NAME_LEN] = "untouched";
    check(gdx_launch_cycle_parse("main,N,P", names, GDX_LAUNCH_MAX_CYCLE) == 3, "comma list parses to 3");
    check(strcmp(names[2], "P") == 0, "third entry is P");
    check(gdx_launch_cycle_parse(" main / N :P ", names, GDX_LAUNCH_MAX_CYCLE) == 3, "/ : and spaces separate too");
    check(gdx_launch_cycle_parse("", names, GDX_LAUNCH_MAX_CYCLE) == 0, "empty list");
    check(gdx_launch_cycle_parse(NULL, names, GDX_LAUNCH_MAX_CYCLE) == 0, "NULL list");
    check(gdx_launch_cycle_parse("a,b,c,d,e,f,g,h,i,j", names, GDX_LAUNCH_MAX_CYCLE) == GDX_LAUNCH_MAX_CYCLE,
          "list capped at GDX_LAUNCH_MAX_CYCLE");
    check(gdx_launch_cycle_parse("averyveryverylongvariantname", names, 1) == 1 &&
              strlen(names[0]) == GDX_LAUNCH_NAME_LEN - 1,
          "a long name is truncated, not overflowed");
    check(gdx_launch_cycle_pick("", 4, pick, sizeof(pick)) == 0 && strcmp(pick, "untouched") == 0,
          "empty cycle leaves out untouched");
    const char* expect[] = {"main", "N", "P", "main", "N", "P", "main"};
    for (unsigned boot = 0; boot < 7; boot++) {
        gdx_launch_cycle_pick("main/N/P", boot, pick, sizeof(pick));
        check(strcmp(pick, expect[boot]) == 0, "boot k runs entry k % 3");
    }
}

static void test_split_key(void) {
    char section[32];
    char key[40];
    check(gdx_launch_split_key("ndsp_core1", section, sizeof(section), key, sizeof(key)) == 1 &&
              strcmp(section, "debug") == 0 && strcmp(key, "ndsp_core1") == 0,
          "a bare key is [debug]");
    check(gdx_launch_split_key("native.all", section, sizeof(section), key, sizeof(key)) == 1 &&
              strcmp(section, "native") == 0 && strcmp(key, "all") == 0,
          "section.key targets the section");
    check(gdx_launch_split_key(".x", section, sizeof(section), key, sizeof(key)) == 0, "empty section rejected");
    check(gdx_launch_split_key("x.", section, sizeof(section), key, sizeof(key)) == 0, "empty key rejected");
    check(gdx_launch_split_key("", section, sizeof(section), key, sizeof(key)) == 0, "empty rejected");
}

static void test_apply_variants(void) {
    SinkLog log = {0};
    load_ini(kCardIni);
    check(gdx_launch_variant_exists("main") && gdx_launch_variant_exists("MAIN"), "main always exists");
    check(gdx_launch_variant_exists("n") && gdx_launch_variant_exists("P"), "N and P exist (any case)");
    check(!gdx_launch_variant_exists("Q") && !gdx_launch_variant_exists("") && !gdx_launch_variant_exists(NULL),
          "unknown / empty names do not");
    check(gdx_launch_apply_variant("Q", RecordOverride, &log) == -1 && log.calls == 0, "unknown variant: -1, nothing forced");

    check(gdx_launch_apply_variant("main", RecordOverride, &log) == 0 && log.calls == 0, "main forces nothing");
    check(gdx3ds_config_get_int("debug", "ndsp_core1", 0) == 0, "main: ndsp_core1 stays default");
    check(gdx3ds_config_get_int("debug", "c5steal", 0) == 1, "main: the ini's own keys stand");

    check(gdx_launch_apply_variant("P", RecordOverride, &log) == 4 && log.failures == 0, "P forces 4 keys");
    check(gdx3ds_config_get_int("debug", "core1_probe", 0) == 1, "P: core1_probe=1");
    check(strcmp(gdx3ds_config_get_string("debug", "core1_probe_pcts", ""), "30,50,80") == 0,
          "P: a comma list survives the loader and the override");
    check(gdx3ds_config_get_int("native", "all", 1) == 0, "P: [native] all=0 through section.key");
    check(strcmp(log.last, "[native] all = 0") == 0, "the sink sees the split section/key");
    check(gdx3ds_config_get_int("debug", "ndsp_core1", 0) == 0, "P does not force N's key");
}

static void test_override_table_full(void) {
    char body[4096];
    int used = snprintf(body, sizeof(body), "[variant.big]\n");
    for (int i = 0; i < 60; i++) {
        used += snprintf(body + used, sizeof(body) - (size_t)used, "k%d = %d\n", i, i);
    }
    SinkLog log = {0};
    load_ini(body);
    const int applied = gdx_launch_apply_variant("big", RecordOverride, &log);
    check(log.calls == 60, "the sink sees every entry");
    check(applied < 60 && log.failures == 60 - applied, "a full override table is reported, not silent");
}

static void test_summary_and_tag(void) {
    char out[64];
    load_ini(kCardIni);
    gdx_launch_variant_summary("N", out, sizeof(out));
    check(strcmp(out, "ndsp_core1=1") == 0, "N summary");
    gdx_launch_variant_summary("main", out, sizeof(out));
    check(strcmp(out, "the ini as is") == 0, "main summary");
    gdx_launch_variant_summary("P", out, 12);
    check(strlen(out) <= 11, "summary truncates to cap");
    gdx_launch_log_tag("N", out, sizeof(out));
    check(strcmp(out, "N") == 0, "N logs to log-N.txt");
    gdx_launch_log_tag("main", out, sizeof(out));
    check(out[0] == '\0', "main keeps log.txt");
    gdx_launch_log_tag("a/../b c", out, sizeof(out));
    check(strcmp(out, "abc") == 0, "path characters are dropped from the tag");
}

static void test_sim_hold(void) {
    uint32_t mask = 0;
    int from = -1;
    check(gdx_launch_parse_sim_hold("l@30", &mask, &from) == 1 && mask == GDX_LAUNCH_KEY_L && from == 30, "l@30");
    check(gdx_launch_parse_sim_hold("R", &mask, &from) == 1 && mask == GDX_LAUNCH_KEY_R && from == 0, "R from frame 0");
    check(gdx_launch_parse_sim_hold("down@5", &mask, &from) == 1 && mask == GDX_LAUNCH_KEY_DDOWN, "down@5");
    check(gdx_launch_parse_sim_hold("q@3", &mask, &from) == 0, "unknown button rejected");
    check(gdx_launch_parse_sim_hold("", &mask, &from) == 0, "empty rejected");
}

int main(void) {
    test_section_enumeration();
    test_hold_bindings();
    test_cycle();
    test_split_key();
    test_apply_variants();
    test_override_table_full();
    test_summary_and_tag();
    test_sim_hold();
    remove(kPath);
    printf("launch_host_tests: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
