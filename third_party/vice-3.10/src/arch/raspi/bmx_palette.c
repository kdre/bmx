/*
 * bmx_palette.c - BMX palette discovery and selection.
 */

#include "bmx_palette.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lib.h"
#include "log.h"
#include "emux_api.h"

#define BMX_PALETTE_DISPLAYS 2
#define BMX_PALETTE_MAX_ENTRIES 256
#define BMX_PALETTE_TAG_LEN 63
#define BMX_PALETTE_DIR_LEN 96
#define BMX_PALETTE_PATH_LEN 384
#define BMX_PALETTE_LOCATOR_LEN 288
#define BMX_PALETTE_MAX_FILE_SIZE (64U * 1024U)
/* Leaves one blank column after "Palette" in the nested C128 video menus. */
#define BMX_PALETTE_MENU_VALUE_CHARS 29

typedef enum {
    BMX_PALETTE_BUILTIN = 0,
    BMX_PALETTE_SYSTEM,
    BMX_PALETTE_USER
} bmx_palette_source_t;

typedef struct {
    bmx_palette_source_t source;
    char filename[MAX_STR_VAL_LEN];
    char display_name[BMX_PALETTE_TAG_LEN + 1];
} bmx_palette_choice_t;

typedef struct {
    int configured;
    unsigned int expected_entries;
    char expected_type[16];
    char builtin_id[32];
    char builtin_name[BMX_PALETTE_TAG_LEN + 1];
    char system_dir[BMX_PALETTE_DIR_LEN];
    char user_dir[BMX_PALETTE_DIR_LEN];
    const unsigned int *fallback_rgb;
    const char *const *legacy_files;
    unsigned int legacy_count;
    struct menu_item *menu;
    bmx_palette_choice_t choices[MAX_CHOICES];
    int choice_count;
    int active_choice;
    int omitted_count;
    char requested[BMX_PALETTE_LOCATOR_LEN];
    unsigned char active[BMX_PALETTE_MAX_ENTRIES][3];
} bmx_palette_state_t;

static bmx_palette_state_t palette_states[BMX_PALETTE_DISPLAYS];

static bmx_palette_state_t *state_for_display(int display)
{
    if (display < 0 || display >= BMX_PALETTE_DISPLAYS) {
        return NULL;
    }
    return &palette_states[display];
}

static int has_vpl_extension(const char *name)
{
    size_t len;

    if (name == NULL) {
        return 0;
    }
    len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".vpl") == 0;
}

static int valid_palette_filename(const char *name)
{
    if (!has_vpl_extension(name) || strlen(name) >= MAX_STR_VAL_LEN) {
        return 0;
    }
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL
        && strchr(name, ':') == NULL;
}

static void trim(char *text)
{
    char *start = text;
    size_t len;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

static void read_palette_tags(const char *path, char *type, size_t type_size,
                              char *name, size_t name_size)
{
    FILE *file;
    char line[160];

    type[0] = '\0';
    name[0] = '\0';
    file = fopen(path, "r");
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *tag = line;
        while (isspace((unsigned char)*tag)) {
            tag++;
        }
        if (*tag++ != '#') {
            continue;
        }
        while (isspace((unsigned char)*tag)) {
            tag++;
        }
        if (strncasecmp(tag, "TYPE:", 5) == 0) {
            snprintf(type, type_size, "%s", tag + 5);
            trim(type);
        } else if (strncasecmp(tag, "NAME:", 5) == 0) {
            snprintf(name, name_size, "%s", tag + 5);
            trim(name);
        }
    }
    fclose(file);
}

static void choice_path(const bmx_palette_state_t *state,
                        const bmx_palette_choice_t *choice,
                        char *path, size_t path_size)
{
    const char *dir = choice->source == BMX_PALETTE_SYSTEM
                          ? state->system_dir : state->user_dir;
    snprintf(path, path_size, "%s/%s", dir, choice->filename);
}

static int load_choice(const bmx_palette_state_t *state,
                       const bmx_palette_choice_t *choice,
                       unsigned char output[BMX_PALETTE_MAX_ENTRIES][3])
{
    palette_t *palette;
    char path[BMX_PALETTE_PATH_LEN];
    char type[BMX_PALETTE_TAG_LEN + 1];
    char name[BMX_PALETTE_TAG_LEN + 1];
    struct stat st;
    unsigned int i;
    int result;

    if (choice->source == BMX_PALETTE_BUILTIN) {
        for (i = 0; i < state->expected_entries; i++) {
            output[i][0] = (unsigned char)state->fallback_rgb[i * 3];
            output[i][1] = (unsigned char)state->fallback_rgb[i * 3 + 1];
            output[i][2] = (unsigned char)state->fallback_rgb[i * 3 + 2];
        }
        return 0;
    }

    choice_path(state, choice, path, sizeof(path));
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)
        || st.st_size <= 0 || (unsigned long)st.st_size > BMX_PALETTE_MAX_FILE_SIZE) {
        return -1;
    }

    read_palette_tags(path, type, sizeof(type), name, sizeof(name));
    if (type[0] != '\0' && strcasecmp(type, state->expected_type) != 0) {
        log_error(LOG_DEFAULT, "Palette `%s' has type `%s', expected `%s'.",
                  path, type, state->expected_type);
        return -1;
    }

    palette = palette_create(state->expected_entries, NULL);
    if (palette == NULL) {
        return -1;
    }
    result = palette_load_path(path, palette);
    if (result == 0) {
        for (i = 0; i < state->expected_entries; i++) {
            output[i][0] = palette->entries[i].red;
            output[i][1] = palette->entries[i].green;
            output[i][2] = palette->entries[i].blue;
        }
    }
    palette_free(palette);
    return result;
}

static int filename_compare(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    int result = strcasecmp(*a, *b);
    return result != 0 ? result : strcmp(*a, *b);
}

static void set_menu_label(char *output, size_t output_size,
                           const char *name, bmx_palette_source_t source)
{
    const char *suffix = source == BMX_PALETTE_BUILTIN ? " [b]"
                         : source == BMX_PALETTE_SYSTEM ? " [s]" : " [u]";
    size_t max_len = BMX_PALETTE_MENU_VALUE_CHARS;
    size_t suffix_len = strlen(suffix);
    size_t name_len;
    size_t available;

    if (output == NULL || output_size == 0) {
        return;
    }
    if (max_len >= output_size) {
        max_len = output_size - 1;
    }
    if (name == NULL) {
        name = "";
    }
    name_len = strlen(name);
    if (name_len + suffix_len <= max_len) {
        snprintf(output, output_size, "%s%s", name, suffix);
        return;
    }

    available = max_len > suffix_len ? max_len - suffix_len : 0;
    if (available > 3) {
        snprintf(output, output_size, "%.*s...%s",
                 (int)(available - 3), name, suffix);
    } else {
        snprintf(output, output_size, "%.*s%s",
                 (int)available, name, suffix);
    }
}

static void add_directory_choices(bmx_palette_state_t *state,
                                  bmx_palette_source_t source)
{
    const char *directory = source == BMX_PALETTE_SYSTEM
                                ? state->system_dir : state->user_dir;
    DIR *dir;
    struct dirent *entry;
    char **names = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t i;

    dir = opendir(directory);
    if (dir == NULL) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char **grown;
        if (!valid_palette_filename(entry->d_name)) {
            continue;
        }
        if (count == capacity) {
            size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
            grown = lib_realloc(names, new_capacity * sizeof(*names));
            if (grown == NULL) {
                break;
            }
            names = grown;
            capacity = new_capacity;
        }
        names[count++] = lib_strdup(entry->d_name);
    }
    closedir(dir);

    qsort(names, count, sizeof(*names), filename_compare);
    for (i = 0; i < count; i++) {
        bmx_palette_choice_t candidate;
        unsigned char colors[BMX_PALETTE_MAX_ENTRIES][3];
        char path[BMX_PALETTE_PATH_LEN];
        char type[BMX_PALETTE_TAG_LEN + 1];
        char palette_name[BMX_PALETTE_TAG_LEN + 1];

        memset(&candidate, 0, sizeof(candidate));
        candidate.source = source;
        snprintf(candidate.filename, sizeof(candidate.filename), "%s", names[i]);
        choice_path(state, &candidate, path, sizeof(path));
        read_palette_tags(path, type, sizeof(type), palette_name,
                          sizeof(palette_name));
        if (palette_name[0] == '\0') {
            snprintf(palette_name, sizeof(palette_name), "%s", names[i]);
            palette_name[strlen(palette_name) - 4] = '\0';
        }
        snprintf(candidate.display_name, sizeof(candidate.display_name), "%s",
                 palette_name);

        if (load_choice(state, &candidate, colors) != 0) {
            log_error(LOG_DEFAULT, "Ignoring invalid BMX palette `%s'.", path);
        } else if (state->choice_count >= MAX_CHOICES) {
            state->omitted_count++;
        } else {
            state->choices[state->choice_count++] = candidate;
        }
        lib_free(names[i]);
    }
    lib_free(names);
}

static void set_fallback_active(bmx_palette_state_t *state)
{
    (void)load_choice(state, &state->choices[0], state->active);
    state->active_choice = 0;
}

static void choice_locator(const bmx_palette_state_t *state,
                           const bmx_palette_choice_t *choice,
                           char *locator, size_t locator_size)
{
    if (choice->source == BMX_PALETTE_BUILTIN) {
        snprintf(locator, locator_size, "builtin:%s", state->builtin_id);
    } else {
        snprintf(locator, locator_size, "%s:%s",
                 choice->source == BMX_PALETTE_SYSTEM ? "system" : "user",
                 choice->filename);
    }
}

static int find_requested_choice(const bmx_palette_state_t *state)
{
    int i;
    char locator[BMX_PALETTE_LOCATOR_LEN];

    for (i = 0; i < state->choice_count; i++) {
        choice_locator(state, &state->choices[i], locator, sizeof(locator));
        if (strcasecmp(locator, state->requested) == 0) {
            return i;
        }
    }
    return -1;
}

struct menu_item *bmx_palette_create_menu(
    int menu_id, struct menu_item *parent, int display,
    const char *builtin_id, const char *builtin_name,
    const char *expected_type, unsigned int expected_entries,
    const char *directory, const unsigned int *fallback_rgb,
    const char *const *legacy_files, unsigned int legacy_count)
{
    bmx_palette_state_t *state = state_for_display(display);
    int i;

    if (state == NULL || expected_entries == 0
        || expected_entries > BMX_PALETTE_MAX_ENTRIES
        || fallback_rgb == NULL) {
        return NULL;
    }

    memset(state, 0, sizeof(*state));
    state->configured = 1;
    state->expected_entries = expected_entries;
    state->fallback_rgb = fallback_rgb;
    state->legacy_files = legacy_files;
    state->legacy_count = legacy_count;
    snprintf(state->expected_type, sizeof(state->expected_type), "%s",
             expected_type);
    snprintf(state->builtin_id, sizeof(state->builtin_id), "%s", builtin_id);
    snprintf(state->builtin_name, sizeof(state->builtin_name), "%s",
             builtin_name);
    snprintf(state->system_dir, sizeof(state->system_dir),
             "SYS:/palettes/%s", directory);
    snprintf(state->user_dir, sizeof(state->user_dir),
             "USER:/palettes/%s", directory);

    state->choices[0].source = BMX_PALETTE_BUILTIN;
    snprintf(state->choices[0].display_name,
             sizeof(state->choices[0].display_name), "%s", builtin_name);
    state->choice_count = 1;
    snprintf(state->requested, sizeof(state->requested), "builtin:%s",
             builtin_id);
    set_fallback_active(state);

    add_directory_choices(state, BMX_PALETTE_SYSTEM);
    add_directory_choices(state, BMX_PALETTE_USER);

    state->menu = ui_menu_add_multiple_choice(menu_id, parent, "Palette");
    if (state->menu == NULL) {
        return NULL;
    }
    state->menu->num_choices = state->choice_count;
    state->menu->value = 0;
    state->menu->choice_ints[0] = 0;
    set_menu_label(state->menu->choices[0], MAX_MENU_STR, builtin_name,
                   BMX_PALETTE_BUILTIN);
    for (i = 1; i < state->choice_count; i++) {
        set_menu_label(state->menu->choices[i], MAX_MENU_STR,
                       state->choices[i].display_name,
                       state->choices[i].source);
        state->menu->choice_ints[i] = i;
    }
    if (state->omitted_count > 0) {
        log_warning(LOG_DEFAULT, "%d palette files omitted for display %d "
                    "(menu limit %d).", state->omitted_count, display,
                    MAX_CHOICES);
    }
    return state->menu;
}

int bmx_palette_select(int display, int choice)
{
    bmx_palette_state_t *state = state_for_display(display);
    unsigned char colors[BMX_PALETTE_MAX_ENTRIES][3];

    if (state == NULL || !state->configured || choice < 0
        || choice >= state->choice_count) {
        if (state != NULL && state->menu != NULL) {
            state->menu->value = state->active_choice;
        }
        return -1;
    }
    if (load_choice(state, &state->choices[choice], colors) != 0) {
        if (state->menu != NULL) {
            state->menu->value = state->active_choice;
        }
        return -1;
    }
    memcpy(state->active, colors,
           state->expected_entries * sizeof(state->active[0]));
    choice_locator(state, &state->choices[choice], state->requested,
                   sizeof(state->requested));
    state->active_choice = choice;
    if (state->menu != NULL) {
        state->menu->value = choice;
    }
    return 0;
}

int bmx_palette_apply_configured(int display)
{
    bmx_palette_state_t *state = state_for_display(display);
    unsigned char colors[BMX_PALETTE_MAX_ENTRIES][3];
    int choice;

    if (state == NULL || !state->configured) {
        return -1;
    }
    choice = find_requested_choice(state);
    if (choice < 0 || load_choice(state, &state->choices[choice], colors) != 0) {
        set_fallback_active(state);
        if (state->menu != NULL) {
            state->menu->value = 0;
        }
        log_warning(LOG_DEFAULT, "Configured palette `%s' is unavailable; "
                    "using builtin `%s'.", state->requested,
                    state->builtin_name);
        return -1;
    }
    memcpy(state->active, colors,
           state->expected_entries * sizeof(state->active[0]));
    state->active_choice = choice;
    if (state->menu != NULL) {
        state->menu->value = choice;
    }
    return 0;
}

static int parse_legacy_index(const char *setting, long *index)
{
    char *end;
    long value;

    if (setting == NULL || *setting == '\0') {
        return 0;
    }
    value = strtol(setting, &end, 10);
    if (*end != '\0') {
        return 0;
    }
    *index = value;
    return 1;
}

int bmx_palette_set_setting(int display, const char *setting)
{
    bmx_palette_state_t *state = state_for_display(display);
    long legacy_index;
    const char *filename;

    if (state == NULL || !state->configured || setting == NULL) {
        return -1;
    }

    if (parse_legacy_index(setting, &legacy_index)) {
        if (legacy_index <= 0 || (unsigned long)legacy_index >= state->legacy_count
            || state->legacy_files[legacy_index] == NULL) {
            char builtin_id[sizeof(state->builtin_id)];

            snprintf(builtin_id, sizeof(builtin_id), "%s", state->builtin_id);
            snprintf(state->requested, sizeof(state->requested), "builtin:%s",
                     builtin_id);
        } else {
            snprintf(state->requested, sizeof(state->requested), "system:%s",
                     state->legacy_files[legacy_index]);
        }
        return 0;
    }

    if (strncasecmp(setting, "builtin:", 8) == 0) {
        if (strcasecmp(setting + 8, state->builtin_id) != 0) {
            return -1;
        }
    } else if (strncasecmp(setting, "system:", 7) == 0) {
        filename = setting + 7;
        if (!valid_palette_filename(filename)) {
            return -1;
        }
    } else if (strncasecmp(setting, "user:", 5) == 0) {
        filename = setting + 5;
        if (!valid_palette_filename(filename)) {
            return -1;
        }
    } else {
        return -1;
    }

    snprintf(state->requested, sizeof(state->requested), "%s", setting);
    return 0;
}

const char *bmx_palette_get_setting(int display)
{
    bmx_palette_state_t *state = state_for_display(display);
    return state != NULL && state->configured ? state->requested : NULL;
}

int bmx_palette_copy_active(int display, palette_t *palette)
{
    bmx_palette_state_t *state = state_for_display(display);
    unsigned int i;

    if (state == NULL || !state->configured || palette == NULL
        || palette->num_entries != state->expected_entries) {
        return -1;
    }
    for (i = 0; i < state->expected_entries; i++) {
        palette->entries[i].red = state->active[i][0];
        palette->entries[i].green = state->active[i][1];
        palette->entries[i].blue = state->active[i][2];
    }
    return 0;
}
