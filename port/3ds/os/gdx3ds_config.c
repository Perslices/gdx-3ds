/* port/3ds/os/gdx3ds_config.c -- minimal INI loader, see gdx3ds_config.h for the format.
 *
 * Design constraints: tiny, zero dependencies beyond libc, no dynamic allocation (fixed
 * table -- the whole point is a handful of keys), portable C so the host build can compile
 * and unit-test it identically to the 3DS build.
 */
#include "gdx3ds_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* n60lead: 128 silently dropped keys of real cards (launch variant blocks pushed them past it). 256
 * (~30 KiB BSS); overflow is now COUNTED and reported (gdx3ds_config_overflow), never silent. */
#define GDX3DS_CFG_MAX_ENTRIES 256
#define GDX3DS_CFG_SECTION_LEN 24
#define GDX3DS_CFG_KEY_LEN 32
#define GDX3DS_CFG_VALUE_LEN 64
#define GDX3DS_CFG_LINE_LEN 160

typedef struct {
    char section[GDX3DS_CFG_SECTION_LEN];
    char key[GDX3DS_CFG_KEY_LEN];
    char value[GDX3DS_CFG_VALUE_LEN];
} Gdx3dsCfgEntry;

static Gdx3dsCfgEntry sEntries[GDX3DS_CFG_MAX_ENTRIES];
static int sEntryCount = 0;
/* n60lead: keys the table could not hold (load, set or override), and the first one's name. */
static int sOverflowCount = 0;
static char sOverflowFirst[GDX3DS_CFG_SECTION_LEN + GDX3DS_CFG_KEY_LEN + 2];

static void gdx3ds_cfg_note_overflow(const char* section, const char* key) {
    if (sOverflowCount == 0) {
        snprintf(sOverflowFirst, sizeof(sOverflowFirst), "%s.%s", section, key);
    }
    sOverflowCount++;
}

/* hw bug n60-3d8, item 2: "gdiffuser.ini was rewritten again -- comments stripped,
 * [variant.C/G/V] lowercased, a stray [hud] section added." gdx3ds_config_save used to be an
 * unconditional whole-table dump (every save, regardless of WHAT changed, rewrote the entire
 * file from the table, which only ever remembers lower-cased section/key names and drops
 * comments by construction -- see its own old self-documented header line). It now patches
 * only the (section, key) pairs that actually changed since the last load/save, in place,
 * against the EXISTING file text, so every comment, section-name case and unrelated key
 * survives untouched, and a save with nothing dirty never touches the file at all -- "the game
 * must never rewrite gdiffuser.ini just from opening or navigating tabs or turning the screen
 * off" is enforced structurally (gdx3ds_menu.c also stopped CALLING save from those paths, but
 * even if something new forgot to, an empty dirty list is a no-op here). */
#define GDX3DS_CFG_MAX_DIRTY 32
#define GDX3DS_CFG_HUD_SECTION "hud" /* never written to the main ini -- see gdx3ds_config_save */

typedef struct {
    char section[GDX3DS_CFG_SECTION_LEN]; /* already lower-cased, like sEntries[].section */
    char key[GDX3DS_CFG_KEY_LEN];         /* already lower-cased, like sEntries[].key */
    int removed; /* 1 = gdx3ds_config_unset dropped this key: delete its line, never append it */
} Gdx3dsCfgDirty;

static Gdx3dsCfgDirty sDirty[GDX3DS_CFG_MAX_DIRTY];
static int sDirtyCount = 0;

/* wantSection/wantKey are already the caller's lower-cased, trimmed forms (every call site
 * below computes them the same way sEntries[] itself does). A later set/unset of the same key
 * updates the existing dirty entry in place rather than appending a duplicate. Best-effort: an
 * overflowed dirty list just means the next save may miss a key that is still correct in RAM
 * and will be caught by the save after that (or a reload) -- never a correctness hazard, only
 * a staleness window, and GDX3DS_CFG_MAX_DIRTY is generous for one menu session's worth of taps. */
static void gdx3ds_cfg_mark_dirty(const char* wantSection, const char* wantKey, int removed) {
    int i;
    for (i = 0; i < sDirtyCount; i++) {
        if (strcmp(sDirty[i].section, wantSection) == 0 && strcmp(sDirty[i].key, wantKey) == 0) {
            sDirty[i].removed = removed;
            return;
        }
    }
    if (sDirtyCount >= GDX3DS_CFG_MAX_DIRTY) {
        return;
    }
    memcpy(sDirty[sDirtyCount].section, wantSection, sizeof(sDirty[sDirtyCount].section));
    memcpy(sDirty[sDirtyCount].key, wantKey, sizeof(sDirty[sDirtyCount].key));
    sDirty[sDirtyCount].removed = removed;
    sDirtyCount++;
}

int gdx3ds_config_overflow(const char** firstKey) {
    if (firstKey != NULL) {
        *firstKey = sOverflowCount != 0 ? sOverflowFirst : "";
    }
    return sOverflowCount;
}
/* n3ds: bumped by every write to the INI or override table, so the preset layer re-resolves
 * `[perf] preset` only when it could have changed. */
static unsigned sTableGen = 1;
/* Set once the first gdx3ds_config_load attempt COMPLETES (missing file included --
 * defaults are then final). Lets one-shot consumers that latch a key at first use
 * (e.g. the gputrace latch in gdx3ds_gpu_prof.c) defer until the table is real
 * instead of latching a fallback forever. */
static int sLoadedOnce = 0;

int gdx3ds_config_loaded(void) {
    return sLoadedOnce;
}

static void gdx3ds_cfg_copy_lower(char* dst, size_t dstLen, const char* src) {
    size_t i;
    for (i = 0; i + 1 < dstLen && src[i] != '\0'; i++) {
        dst[i] = (char) tolower((unsigned char) src[i]);
    }
    dst[i] = '\0';
}

/* Trim leading/trailing whitespace in place; returns the first non-space character. */
static char* gdx3ds_cfg_trim(char* s) {
    char* end;
    while (isspace((unsigned char) *s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char) end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

/* Cut a trailing comment ('#' or ';'), respecting nothing fancy -- values needing literal
 * '#'/';' are out of scope for a handful of numeric/bool keys. */
static void gdx3ds_cfg_strip_comment(char* s) {
    char* p = strpbrk(s, "#;");
    if (p != NULL) {
        *p = '\0';
    }
}

int gdx3ds_config_load(const char* path) {
    char line[GDX3DS_CFG_LINE_LEN];
    char section[GDX3DS_CFG_SECTION_LEN] = "";
    FILE* fp;

    sEntryCount = 0; /* reload replaces the table even if the file is gone */
    sOverflowCount = 0;
    sOverflowFirst[0] = '\0';
    sTableGen++;
    sDirtyCount = 0; /* hw bug n60-3d8, item 2: whatever is on disk is now the truth again --
                       * no edit is still pending against it. See gdx3ds_config_save. */

    fp = fopen(path, "r");
    if (fp == NULL) {
        sLoadedOnce = 1; /* defaults are final: latch-at-first-use consumers may proceed */
        return 1;        /* missing file = defaults; normal on first boot */
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* s;
        char* eq;
        gdx3ds_cfg_strip_comment(line);
        s = gdx3ds_cfg_trim(line);
        if (*s == '\0') {
            continue;
        }
        if (*s == '[') {
            char* close = strchr(s, ']');
            if (close != NULL) {
                *close = '\0';
                gdx3ds_cfg_copy_lower(section, sizeof(section), gdx3ds_cfg_trim(s + 1));
            }
            continue;
        }
        eq = strchr(s, '=');
        if (eq == NULL) {
            continue; /* not a key=value line */
        }
        *eq = '\0';
        if (sEntryCount >= GDX3DS_CFG_MAX_ENTRIES) {
            gdx3ds_cfg_note_overflow(section, gdx3ds_cfg_trim(s)); /* table full: counted, reported */
            continue;
        }
        {
            Gdx3dsCfgEntry* e = &sEntries[sEntryCount];
            char* key = gdx3ds_cfg_trim(s);
            char* value = gdx3ds_cfg_trim(eq + 1);
            if (*key == '\0') {
                continue;
            }
            memcpy(e->section, section, sizeof(e->section));
            gdx3ds_cfg_copy_lower(e->key, sizeof(e->key), key);
            /* Values keep their case (paths etc.); only lookup keys are folded. */
            snprintf(e->value, sizeof(e->value), "%s", value);
            sEntryCount++;
        }
    }

    fclose(fp);
    sLoadedOnce = 1;
    return 0;
}

/* ---- RAM-only overrides (O3DSHOME3 boot bisect) --------------------------------------
 * A second, tiny table consulted BEFORE the INI table by every getter and never written by
 * gdx3ds_config_save, so a boot-time variant (a button held at launch) can force levers
 * without the menu's write-back persisting them into the user's gdiffuser.ini. */
#define GDX3DS_CFG_MAX_OVERRIDES 48
static Gdx3dsCfgEntry sOverrides[GDX3DS_CFG_MAX_OVERRIDES];
static int sOverrideCount = 0;

static const char* gdx3ds_cfg_find_override(const char* wantSection, const char* wantKey) {
    int i;
    for (i = 0; i < sOverrideCount; i++) {
        if (strcmp(sOverrides[i].section, wantSection) == 0 && strcmp(sOverrides[i].key, wantKey) == 0) {
            return sOverrides[i].value;
        }
    }
    return NULL;
}

int gdx3ds_config_override(const char* section, const char* key, const char* value) {
    char wantSection[GDX3DS_CFG_SECTION_LEN];
    char wantKey[GDX3DS_CFG_KEY_LEN];
    int i;

    if (key == NULL || value == NULL) {
        return 1;
    }
    gdx3ds_cfg_copy_lower(wantSection, sizeof(wantSection), (section != NULL) ? section : "");
    gdx3ds_cfg_copy_lower(wantKey, sizeof(wantKey), key);
    for (i = 0; i < sOverrideCount; i++) {
        if (strcmp(sOverrides[i].section, wantSection) == 0 && strcmp(sOverrides[i].key, wantKey) == 0) {
            snprintf(sOverrides[i].value, sizeof(sOverrides[i].value), "%s", value);
            sTableGen++;
            return 0;
        }
    }
    if (sOverrideCount >= GDX3DS_CFG_MAX_OVERRIDES) {
        gdx3ds_cfg_note_overflow(wantSection, wantKey);
        return 2;
    }
    memcpy(sOverrides[sOverrideCount].section, wantSection, sizeof(wantSection));
    memcpy(sOverrides[sOverrideCount].key, wantKey, sizeof(wantKey));
    snprintf(sOverrides[sOverrideCount].value, sizeof(sOverrides[sOverrideCount].value), "%s", value);
    sOverrideCount++;
    sTableGen++;
    return 0;
}

int gdx3ds_config_section_entry(const char* section, int index, const char** key, const char** value) {
    char wantSection[GDX3DS_CFG_SECTION_LEN];
    int seen = 0;
    int i;

    gdx3ds_cfg_copy_lower(wantSection, sizeof(wantSection), (section != NULL) ? section : "");
    for (i = 0; i < sEntryCount; i++) {
        if (strcmp(sEntries[i].section, wantSection) != 0) {
            continue;
        }
        if (seen == index) {
            *key = sEntries[i].key;
            *value = sEntries[i].value;
            return 1;
        }
        seen++;
    }
    return 0;
}

int gdx3ds_config_entry_count(void) {
    return sEntryCount;
}

int gdx3ds_config_entry_capacity(void) {
    return GDX3DS_CFG_MAX_ENTRIES;
}

/* The override table, then the INI table; NULL when neither has the key (lower-cased args). */
static const char* gdx3ds_cfg_find_explicit(const char* wantSection, const char* wantKey) {
    const char* forced = gdx3ds_cfg_find_override(wantSection, wantKey);
    int i;
    if (forced != NULL) {
        return forced;
    }
    for (i = 0; i < sEntryCount; i++) {
        if (strcmp(sEntries[i].section, wantSection) == 0 && strcmp(sEntries[i].key, wantKey) == 0) {
            return sEntries[i].value;
        }
    }
    return NULL;
}

/* ---- n3ds: the preset defaults layer ---------------------------------------------------
 * Consulted AFTER the override and INI tables and BEFORE the caller's compiled-in fallback, so
 * a preset only re-defaults keys the ini leaves out. Resolved from `[perf] preset` (explicit
 * tables only) once per table generation; no resolver or no matching preset = no layer, every
 * lookup exactly as before. */
static Gdx3dsCfgPresetResolver sPresetResolver = NULL;
static const Gdx3dsCfgPresetEntry* sPreset = NULL;
static int sPresetCount = 0;
static unsigned sPresetGen = 0;
/* n60lead: AUTO. "" = unset (today's behaviour: an absent/"auto" [perf] preset applies no
 * table). Set by the caller once it knows the console model; see gdx3ds_config_set_auto_preset_name. */
#define GDX3DS_CFG_AUTO_PRESET_LEN 24
static char sAutoPresetName[GDX3DS_CFG_AUTO_PRESET_LEN] = {0};
/* The name actually fed to sPresetResolver this generation (ini literal, or the auto
 * substitution), for gdx3ds_config_preset_name(). "" = neither. */
static char sResolvedPresetName[GDX3DS_CFG_AUTO_PRESET_LEN] = {0};

void gdx3ds_config_set_preset_resolver(Gdx3dsCfgPresetResolver resolver) {
    sPresetResolver = resolver;
    sPresetGen = 0; /* re-resolve at the next lookup */
}

void gdx3ds_config_set_auto_preset_name(const char* name) {
    size_t n;
    if (name == NULL) {
        sAutoPresetName[0] = '\0';
    } else {
        n = strlen(name);
        if (n >= sizeof(sAutoPresetName)) {
            n = sizeof(sAutoPresetName) - 1;
        }
        memcpy(sAutoPresetName, name, n);
        sAutoPresetName[n] = '\0';
    }
    sPresetGen = 0; /* re-resolve at the next lookup, like set_preset_resolver */
}

static void gdx3ds_cfg_remember_resolved_name(const char* name) {
    size_t n;
    if (name == NULL) {
        sResolvedPresetName[0] = '\0';
        return;
    }
    n = strlen(name);
    if (n >= sizeof(sResolvedPresetName)) {
        n = sizeof(sResolvedPresetName) - 1;
    }
    memcpy(sResolvedPresetName, name, n);
    sResolvedPresetName[n] = '\0';
}

static void gdx3ds_cfg_resolve_preset(void) {
    const char* name;
    if (sPresetGen == sTableGen) {
        return;
    }
    sPresetGen = sTableGen;
    sPreset = NULL;
    sPresetCount = 0;
    gdx3ds_cfg_remember_resolved_name(NULL);
    if (sPresetResolver == NULL) {
        return;
    }
    name = gdx3ds_cfg_find_explicit("perf", "preset");
    if ((name == NULL || strcasecmp(name, "auto") == 0) && sAutoPresetName[0] != '\0') {
        name = sAutoPresetName; /* ini leaves it absent/"auto": the model-resolved default */
    }
    sPreset = sPresetResolver(name, &sPresetCount);
    if (sPreset == NULL) {
        sPresetCount = 0;
    } else {
        /* Only remember the name when it actually resolved to a table: gdx3ds_config_preset_name
         * reports the ACTIVE table, not every name tried (e.g. an explicit "off" is handed to
         * the resolver too, and must still read back as "no preset", not "off"). */
        gdx3ds_cfg_remember_resolved_name(name);
    }
}

const char* gdx3ds_config_preset_name(void) {
    gdx3ds_cfg_resolve_preset();
    return sResolvedPresetName[0] != '\0' ? sResolvedPresetName : NULL;
}

static const char* gdx3ds_cfg_find_preset(const char* wantSection, const char* wantKey) {
    int i;
    gdx3ds_cfg_resolve_preset();
    for (i = 0; i < sPresetCount; i++) {
        if (strcmp(sPreset[i].section, wantSection) == 0 && strcmp(sPreset[i].key, wantKey) == 0) {
            return sPreset[i].value;
        }
    }
    return NULL;
}

int gdx3ds_config_preset_count(void) {
    gdx3ds_cfg_resolve_preset();
    return sPresetCount;
}

const char* gdx3ds_config_get_string(const char* section, const char* key, const char* fallback) {
    char wantSection[GDX3DS_CFG_SECTION_LEN];
    char wantKey[GDX3DS_CFG_KEY_LEN];
    const char* v;

    gdx3ds_cfg_copy_lower(wantSection, sizeof(wantSection), (section != NULL) ? section : "");
    gdx3ds_cfg_copy_lower(wantKey, sizeof(wantKey), (key != NULL) ? key : "");

    v = gdx3ds_cfg_find_explicit(wantSection, wantKey);
    if (v != NULL) {
        return v;
    }
    if (sPresetResolver != NULL) {
        v = gdx3ds_cfg_find_preset(wantSection, wantKey);
        if (v != NULL) {
            return v;
        }
    }
    return fallback;
}

int gdx3ds_config_get_int(const char* section, const char* key, int fallback) {
    const char* v = gdx3ds_config_get_string(section, key, NULL);
    char* end;
    long parsed;
    if (v == NULL || *v == '\0') {
        return fallback;
    }
    parsed = strtol(v, &end, 0); /* base 0: accepts decimal and 0x hex */
    if (end == v) {
        return fallback; /* not a number at all */
    }
    return (int) parsed;
}

/* ---- MENU write-back (v1 bottom-screen menu) --------------------------------------
 * The menu persists user-changed values by updating the in-memory table and rewriting
 * the INI from it. The loader stores EVERY key it parses (unknown sections/keys
 * included), so a rewrite preserves foreign keys; only comments/ordering are lost,
 * which the header line documents. */

int gdx3ds_config_set_string(const char* section, const char* key, const char* value) {
    char wantSection[GDX3DS_CFG_SECTION_LEN];
    char wantKey[GDX3DS_CFG_KEY_LEN];
    int i;

    if (key == NULL || value == NULL) {
        return 1;
    }
    gdx3ds_cfg_copy_lower(wantSection, sizeof(wantSection), (section != NULL) ? section : "");
    gdx3ds_cfg_copy_lower(wantKey, sizeof(wantKey), key);
    if (wantKey[0] == '\0') {
        return 1;
    }

    for (i = 0; i < sEntryCount; i++) {
        if (strcmp(sEntries[i].section, wantSection) == 0 && strcmp(sEntries[i].key, wantKey) == 0) {
            snprintf(sEntries[i].value, sizeof(sEntries[i].value), "%s", value);
            sTableGen++;
            gdx3ds_cfg_mark_dirty(wantSection, wantKey, 0);
            return 0;
        }
    }
    if (sEntryCount >= GDX3DS_CFG_MAX_ENTRIES) {
        gdx3ds_cfg_note_overflow(wantSection, wantKey);
        return 2; /* table full: value applies in RAM callers, but cannot persist */
    }
    memcpy(sEntries[sEntryCount].section, wantSection, sizeof(wantSection));
    memcpy(sEntries[sEntryCount].key, wantKey, sizeof(wantKey));
    snprintf(sEntries[sEntryCount].value, sizeof(sEntries[sEntryCount].value), "%s", value);
    sEntryCount++;
    sTableGen++;
    gdx3ds_cfg_mark_dirty(wantSection, wantKey, 0);
    return 0;
}

int gdx3ds_config_unset(const char* section, const char* key) {
    char wantSection[GDX3DS_CFG_SECTION_LEN];
    char wantKey[GDX3DS_CFG_KEY_LEN];
    int i;

    if (key == NULL) {
        return 1;
    }
    gdx3ds_cfg_copy_lower(wantSection, sizeof(wantSection), (section != NULL) ? section : "");
    gdx3ds_cfg_copy_lower(wantKey, sizeof(wantKey), key);
    if (wantKey[0] == '\0') {
        return 1;
    }

    for (i = 0; i < sEntryCount; i++) {
        if (strcmp(sEntries[i].section, wantSection) == 0 && strcmp(sEntries[i].key, wantKey) == 0) {
            /* Shift the tail down rather than swapping with the last entry: gdx3ds_config_save
             * writes the table in order, so swapping would silently reshuffle the user's ini. */
            for (; i + 1 < sEntryCount; i++) {
                sEntries[i] = sEntries[i + 1];
            }
            sEntryCount--;
            memset(&sEntries[sEntryCount], 0, sizeof(sEntries[sEntryCount]));
            sTableGen++;
            gdx3ds_cfg_mark_dirty(wantSection, wantKey, 1);
            return 0;
        }
    }
    return 3; /* nothing to remove — the key is absent either way */
}

int gdx3ds_config_set_int(const char* section, const char* key, int value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    return gdx3ds_config_set_string(section, key, buf);
}

/* sEntries[] only (never the RAM-only override table -- see gdx3ds_config_override's own
 * "never saved" contract); section/key must already be the lower-cased, trimmed form. "" when
 * a dirty, non-removed entry's value has somehow gone missing (should not happen: the entry
 * was just set), so a save still emits something sane rather than garbage. */
static const char* gdx3ds_cfg_find_entry_value(const char* wantSection, const char* wantKey) {
    int i;
    for (i = 0; i < sEntryCount; i++) {
        if (strcmp(sEntries[i].section, wantSection) == 0 && strcmp(sEntries[i].key, wantKey) == 0) {
            return sEntries[i].value;
        }
    }
    return "";
}

static int gdx3ds_cfg_dirty_is_hud(const Gdx3dsCfgDirty* d) {
    return strcmp(d->section, GDX3DS_CFG_HUD_SECTION) == 0;
}

/* Called when the surgical save (below) is about to leave `section` (a new [header] line, or
 * EOF) -- appends any still-pending dirty entry for that exact section right at the end of its
 * block, so a newly-set key that was not already a line in the file lands next to its
 * siblings instead of at the end of the whole document. Marks every entry it looks at as
 * handled, including ones it deliberately does not write ([hud], or a removal that was never
 * present), so gdx3ds_cfg_flush_new_sections (the file-level EOF pass) never looks at them again. */
static void gdx3ds_cfg_flush_section(FILE* dst, const char* section, int* handled) {
    int i;
    const int isHud = strcmp(section, GDX3DS_CFG_HUD_SECTION) == 0;
    for (i = 0; i < sDirtyCount; i++) {
        if (handled[i] || strcmp(sDirty[i].section, section) != 0) {
            continue;
        }
        handled[i] = 1;
        if (isHud || sDirty[i].removed) {
            continue; /* hw bug n60-3d8, item 2: never appended; a removal has nothing to append */
        }
        fprintf(dst, "%s = %s\n", sDirty[i].key, gdx3ds_cfg_find_entry_value(section, sDirty[i].key));
    }
}

/* Any dirty entry still unhandled after the whole file has been streamed through belongs to a
 * section that never appeared in the file at all -- a brand new [section]. Groups them so a
 * section that gains two keys in the same save gets one new header, not two. */
static void gdx3ds_cfg_flush_new_sections(FILE* dst, int* handled) {
    int i;
    for (i = 0; i < sDirtyCount; i++) {
        int j;
        if (handled[i]) {
            continue;
        }
        if (sDirty[i].removed || gdx3ds_cfg_dirty_is_hud(&sDirty[i])) {
            handled[i] = 1; /* nothing to remove that was never there; [hud] never written */
            continue;
        }
        fprintf(dst, "\n[%s]\n", sDirty[i].section);
        fprintf(dst, "%s = %s\n", sDirty[i].key, gdx3ds_cfg_find_entry_value(sDirty[i].section, sDirty[i].key));
        handled[i] = 1;
        for (j = i + 1; j < sDirtyCount; j++) {
            if (!handled[j] && !sDirty[j].removed && !gdx3ds_cfg_dirty_is_hud(&sDirty[j]) &&
                strcmp(sDirty[j].section, sDirty[i].section) == 0) {
                fprintf(dst, "%s = %s\n", sDirty[j].key, gdx3ds_cfg_find_entry_value(sDirty[j].section, sDirty[j].key));
                handled[j] = 1;
            }
        }
    }
}

/* No existing file to preserve anything from (first boot, or a fresh SD card): the original
 * whole-table dump, MINUS [hud] (hw bug n60-3d8, item 2 -- never written to the main ini, even
 * on a from-scratch create). Emits sections in first-appearance order; each entry exactly once. */
static int gdx3ds_cfg_save_fresh(const char* path) {
    FILE* fp = fopen(path, "w");
    int i;
    int j;
    if (fp == NULL) {
        return 1;
    }
    fprintf(fp, "; gdiffuser.ini - auto-generated (no prior file to preserve this round)\n");
    for (i = 0; i < sEntryCount; i++) {
        if (sEntries[i].section[0] == '\0') {
            fprintf(fp, "%s = %s\n", sEntries[i].key, sEntries[i].value);
        }
    }
    for (i = 0; i < sEntryCount; i++) {
        int seen = 0;
        if (sEntries[i].section[0] == '\0' || strcmp(sEntries[i].section, GDX3DS_CFG_HUD_SECTION) == 0) {
            continue;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(sEntries[j].section, sEntries[i].section) == 0) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }
        fprintf(fp, "\n[%s]\n", sEntries[i].section);
        for (j = i; j < sEntryCount; j++) {
            if (strcmp(sEntries[j].section, sEntries[i].section) == 0) {
                fprintf(fp, "%s = %s\n", sEntries[j].key, sEntries[j].value);
            }
        }
    }
    fclose(fp);
    return 0;
}

/* See the big comment above GDX3DS_CFG_MAX_DIRTY for why this patches `path` in place instead
 * of rewriting it wholesale. */
int gdx3ds_config_save(const char* path) {
    FILE* src;
    FILE* dst;
    char tmpPath[GDX3DS_CFG_LINE_LEN + 8];
    int handled[GDX3DS_CFG_MAX_DIRTY] = {0};
    char curSection[GDX3DS_CFG_SECTION_LEN] = ""; /* lower-cased; "" = before any [header] yet */
    int inHudSection = 0;
    int anyRealDirt = 0;
    int i;

    for (i = 0; i < sDirtyCount; i++) {
        if (!gdx3ds_cfg_dirty_is_hud(&sDirty[i])) {
            anyRealDirt = 1;
            break;
        }
    }
    if (!anyRealDirt) {
        /* Nothing changed since the last save/load that could ever legally reach the file
         * (an all-[hud] or empty dirty list) -- hw bug n60-3d8, item 2's hard rule ("never
         * rewrite gdiffuser.ini just from...") holds even if some future caller forgets to
         * gate its own MenuSaveCfg()-equivalent: this leaves the file COMPLETELY untouched. */
        sDirtyCount = 0;
        return 0;
    }

    src = fopen(path, "r");
    if (src == NULL) {
        int rc = gdx3ds_cfg_save_fresh(path);
        if (rc == 0) {
            sDirtyCount = 0;
        }
        return rc;
    }

    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    dst = fopen(tmpPath, "w");
    if (dst == NULL) {
        fclose(src);
        return 1;
    }

    {
        char line[GDX3DS_CFG_LINE_LEN];
        while (fgets(line, sizeof(line), src) != NULL) {
            char scratch[GDX3DS_CFG_LINE_LEN];
            char* t;
            snprintf(scratch, sizeof(scratch), "%s", line); /* classify a COPY; `line` is what
                                                               * an untouched row writes back,
                                                               * byte for byte, comment and all. */
            gdx3ds_cfg_strip_comment(scratch);
            t = gdx3ds_cfg_trim(scratch);

            if (*t == '[') {
                char* close = strchr(t, ']');
                if (close == NULL) {
                    /* Matches gdx3ds_config_load: a line starting with '[' but never closed is
                     * not a header at all -- it does not change curSection, and (unlike load,
                     * which just drops it) is preserved verbatim here since it was never ours
                     * to interpret. */
                    fputs(line, dst);
                    continue;
                }
                gdx3ds_cfg_flush_section(dst, curSection, handled); /* leaving curSection */
                *close = '\0';
                gdx3ds_cfg_copy_lower(curSection, sizeof(curSection), gdx3ds_cfg_trim(t + 1));
                inHudSection = (strcmp(curSection, GDX3DS_CFG_HUD_SECTION) == 0);
                if (!inHudSection) {
                    fputs(line, dst); /* the header line exactly as on disk -- case included */
                }
                continue;
            }
            if (inHudSection) {
                continue; /* hw bug n60-3d8, item 2: drop every pre-existing [hud] line */
            }
            if (*t != '\0') {
                char* eq = strchr(t, '=');
                if (eq != NULL) {
                    char keyBuf[GDX3DS_CFG_KEY_LEN];
                    char* rawKey;
                    int di;
                    *eq = '\0';
                    rawKey = gdx3ds_cfg_trim(t);
                    gdx3ds_cfg_copy_lower(keyBuf, sizeof(keyBuf), rawKey);
                    for (di = 0; di < sDirtyCount; di++) {
                        if (!handled[di] && strcmp(sDirty[di].section, curSection) == 0 &&
                            strcmp(sDirty[di].key, keyBuf) == 0) {
                            break;
                        }
                    }
                    if (di < sDirtyCount) {
                        handled[di] = 1;
                        if (!sDirty[di].removed) {
                            fprintf(dst, "%s = %s\n", rawKey,
                                    gdx3ds_cfg_find_entry_value(curSection, keyBuf));
                        } /* removed: drop the line */
                        continue;
                    }
                }
            }
            fputs(line, dst); /* blank, comment-only, malformed, or an untouched key: verbatim */
        }
        gdx3ds_cfg_flush_section(dst, curSection, handled); /* EOF while still inside a section */
        gdx3ds_cfg_flush_new_sections(dst, handled);        /* dirty sections never seen above */
    }

    fclose(src);
    fclose(dst);
    /* gdx3ds_fs_sd.c's own replace-on-sdmc pattern: try the plain rename first, and only fall
     * back to remove()+rename() if the devoptab refuses to replace an existing destination --
     * either way the .tmp file is complete, so the worst case leaves it on disk under one of
     * the two names rather than losing data. */
    if (rename(tmpPath, path) != 0) {
        remove(path);
        if (rename(tmpPath, path) != 0) {
            return 1; /* the .tmp file survives on disk for a human to recover from */
        }
    }
    sDirtyCount = 0;
    return 0;
}

int gdx3ds_config_get_bool(const char* section, const char* key, int fallback) {
    const char* v = gdx3ds_config_get_string(section, key, NULL);
    if (v == NULL || *v == '\0') {
        return fallback;
    }
    if (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0 ||
        strcasecmp(v, "on") == 0) {
        return 1;
    }
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0 ||
        strcasecmp(v, "off") == 0) {
        return 0;
    }
    return fallback;
}
