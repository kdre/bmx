/*
 * bmx_palette.h - BMX palette discovery and selection.
 */

#ifndef BMX_PALETTE_H
#define BMX_PALETTE_H

#include "palette.h"

struct menu_item;

struct menu_item *bmx_palette_create_menu(
    int menu_id, struct menu_item *parent, int display,
    const char *builtin_id, const char *builtin_name,
    const char *expected_type, unsigned int expected_entries,
    const char *directory, const unsigned int *fallback_rgb,
    const char *const *legacy_files, unsigned int legacy_count);

int bmx_palette_select(int display, int choice);
int bmx_palette_apply_configured(int display);
int bmx_palette_set_setting(int display, const char *setting);
const char *bmx_palette_get_setting(int display);
int bmx_palette_copy_active(int display, palette_t *palette);

#endif
