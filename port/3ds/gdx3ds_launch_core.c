/* port/3ds/gdx3ds_launch_core.c -- N60LAUNCH: the pure selection rules (see the header). */
#include "gdx3ds_launch_core.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "gdx3ds_config.h"

#define GDX_LAUNCH_SECTION_PREFIX "variant."

static const GdxLaunchButton kButtons[] = {
    {"hold_l", "L", GDX_LAUNCH_KEY_L},        {"hold_r", "R", GDX_LAUNCH_KEY_R},
    {"hold_x", "X", GDX_LAUNCH_KEY_X},        {"hold_y", "Y", GDX_LAUNCH_KEY_Y},
    {"hold_up", "UP", GDX_LAUNCH_KEY_DUP},    {"hold_down", "DOWN", GDX_LAUNCH_KEY_DDOWN},
};

const GdxLaunchButton* gdx_launch_buttons(int* count) {
    *count = (int)(sizeof(kButtons) / sizeof(kButtons[0]));
    return kButtons;
}

int gdx_launch_is_main(const char* name) {
    return name != NULL && strcasecmp(name, "main") == 0;
}

static void VariantSection(const char* name, char* out, size_t cap) {
    snprintf(out, cap, GDX_LAUNCH_SECTION_PREFIX "%s", name);
}

int gdx_launch_variant_exists(const char* name) {
    char section[48];
    const char* key;
    const char* value;
    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    if (gdx_launch_is_main(name)) {
        return 1;
    }
    VariantSection(name, section, sizeof(section));
    return gdx3ds_config_section_entry(section, 0, &key, &value);
}

static const char* BoundVariant(const GdxLaunchButton* button) {
    const char* name = gdx3ds_config_get_string("launch", button->bindKey, NULL);
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    return name;
}

uint32_t gdx_launch_bound_mask(void) {
    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
        if (BoundVariant(&kButtons[i]) != NULL) {
            mask |= kButtons[i].mask;
        }
    }
    return mask;
}

const char* gdx_launch_pick_held(uint32_t held, const GdxLaunchButton** button) {
    for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
        const char* name = BoundVariant(&kButtons[i]);
        if (name != NULL && (held & kButtons[i].mask) != 0) {
            if (button != NULL) {
                *button = &kButtons[i];
            }
            return name;
        }
    }
    return NULL;
}

static int IsCycleSeparator(char c) {
    return c == ',' || c == '/' || c == ':' || isspace((unsigned char)c);
}

int gdx_launch_cycle_parse(const char* list, char names[][GDX_LAUNCH_NAME_LEN], int max) {
    int count = 0;
    const char* p = list;
    if (list == NULL) {
        return 0;
    }
    while (*p != '\0' && count < max) {
        while (*p != '\0' && IsCycleSeparator(*p)) {
            p++;
        }
        size_t len = 0;
        while (p[len] != '\0' && !IsCycleSeparator(p[len])) {
            len++;
        }
        if (len == 0) {
            break;
        }
        if (len >= GDX_LAUNCH_NAME_LEN) {
            len = GDX_LAUNCH_NAME_LEN - 1;
        }
        memcpy(names[count], p, len);
        names[count][len] = '\0';
        count++;
        while (*p != '\0' && !IsCycleSeparator(*p)) {
            p++;
        }
    }
    return count;
}

int gdx_launch_cycle_pick(const char* list, unsigned boot, char* out, size_t cap) {
    char names[GDX_LAUNCH_MAX_CYCLE][GDX_LAUNCH_NAME_LEN];
    const int count = gdx_launch_cycle_parse(list, names, GDX_LAUNCH_MAX_CYCLE);
    if (count == 0) {
        return 0;
    }
    snprintf(out, cap, "%s", names[boot % (unsigned)count]);
    return count;
}

int gdx_launch_split_key(const char* raw, char* section, size_t sectionCap, char* key, size_t keyCap) {
    const char* dot;
    if (raw == NULL || raw[0] == '\0') {
        return 0;
    }
    dot = strchr(raw, '.');
    if (dot == NULL) {
        snprintf(section, sectionCap, "debug");
        snprintf(key, keyCap, "%s", raw);
        return 1;
    }
    if (dot == raw || dot[1] == '\0') {
        return 0;
    }
    snprintf(section, sectionCap, "%.*s", (int)(dot - raw), raw);
    snprintf(key, keyCap, "%s", dot + 1);
    return 1;
}

int gdx_launch_apply_variant(const char* name, GdxLaunchOverrideSink sink, void* ctx) {
    char variantSection[48];
    const char* rawKey;
    const char* value;
    int applied = 0;
    if (!gdx_launch_variant_exists(name)) {
        return -1;
    }
    if (gdx_launch_is_main(name)) {
        return 0;
    }
    VariantSection(name, variantSection, sizeof(variantSection));
    for (int i = 0; gdx3ds_config_section_entry(variantSection, i, &rawKey, &value); i++) {
        char section[32] = "?";
        char key[40];
        int rc = 1;
        snprintf(key, sizeof(key), "%s", rawKey);
        if (gdx_launch_split_key(rawKey, section, sizeof(section), key, sizeof(key))) {
            rc = gdx3ds_config_override(section, key, value);
        }
        if (rc == 0) {
            applied++;
        }
        if (sink != NULL) {
            sink(section, key, value, rc, ctx);
        }
    }
    return applied;
}

void gdx_launch_variant_summary(const char* name, char* out, size_t cap) {
    char variantSection[48];
    const char* key;
    const char* value;
    size_t used = 0;
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (name == NULL || gdx_launch_is_main(name)) {
        snprintf(out, cap, "the ini as is");
        return;
    }
    VariantSection(name, variantSection, sizeof(variantSection));
    for (int i = 0; gdx3ds_config_section_entry(variantSection, i, &key, &value) && used + 1 < cap; i++) {
        int n = snprintf(out + used, cap - used, "%s%s=%s", i ? " " : "", key, value);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }
}

void gdx_launch_log_tag(const char* name, char* out, size_t cap) {
    size_t used = 0;
    if (cap == 0) {
        return;
    }
    if (name != NULL && !gdx_launch_is_main(name)) {
        for (const char* p = name; *p != '\0' && used + 1 < cap; p++) {
            if (isalnum((unsigned char)*p) || *p == '_' || *p == '-') {
                out[used++] = *p;
            }
        }
    }
    out[used] = '\0';
}

static const GdxLaunchButton* ButtonForLabel(const char* label, size_t len) {
    for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
        if (strlen(kButtons[i].label) == len && strncasecmp(kButtons[i].label, label, len) == 0) {
            return &kButtons[i];
        }
    }
    return NULL;
}

int gdx_launch_parse_sim_hold(const char* spec, uint32_t* mask, int* fromFrame) {
    const char* at;
    const GdxLaunchButton* button;
    if (spec == NULL || spec[0] == '\0') {
        return 0;
    }
    at = strchr(spec, '@');
    button = ButtonForLabel(spec, at != NULL ? (size_t)(at - spec) : strlen(spec));
    if (button == NULL) {
        return 0;
    }
    *mask = button->mask;
    *fromFrame = 0;
    if (at != NULL) {
        int frame = 0;
        if (sscanf(at + 1, "%d", &frame) == 1 && frame > 0) {
            *fromFrame = frame;
        }
    }
    return 1;
}
