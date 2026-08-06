/*
 * menu_switch.c
 *
 * Written by
 *  Randy Rossi <randy.rossi@gmail.com>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "menu_switch.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "circle.h"

#ifndef BMC64_SWITCH_BOOT_ROOT
#define BMC64_SWITCH_BOOT_ROOT ""
#endif

#define BMX_MACHINES_FILE "machines.ini"
#define BMX_ACTIVE_KERNEL_SELECTOR "bmx-active-kernel.txt"
#define BMX_KERNEL_SELECTOR_HEADER "# BMX-KERNEL-SELECTOR-V2\n"

#define ERROR_INVALID_CONFIG 1
#define ERROR_OPEN_INPUT 2
#define ERROR_OPEN_OUTPUT 4
#define ERROR_RENDER 8
#define ERROR_BACKUP 16
#define ERROR_COMMIT 32
#define ERROR_UNKNOWN_PI 64
#define ERROR_UNKNOWN_MACHINE 128

enum section_kind {
  SECTION_NONE,
  SECTION_VIDEO,
  SECTION_SCALING,
  SECTION_MACHINE,
};

struct pi_kernel_spec {
  int pi_model;
  const char *kernel_name;
};

static const struct pi_kernel_spec pi_kernel_specs[] = {
    {0, "kernel.img"},
    {1, "kernel.img"},
    {2, "kernel7.img"},
    {3, "kernel8-32.img"},
    {4, "kernel7l.img"},
    {5, "kernel_2712.img"},
};

static void boot_path(char *path, size_t path_size, const char *name) {
  snprintf(path, path_size, "%s/%s", BMC64_SWITCH_BOOT_ROOT, name);
}

static int copy_file(const char *from, const char *to) {
  FILE *input = fopen(from, "r");
  FILE *output;
  int c;
  int failed = 0;

  if (input == NULL) {
    return 1;
  }
  output = fopen(to, "w");
  if (output == NULL) {
    fclose(input);
    return 1;
  }

  while ((c = fgetc(input)) != EOF) {
    if (fputc(c, output) == EOF) {
      failed = 1;
      break;
    }
  }

  failed |= ferror(input) != 0;
  failed |= ferror(output) != 0;
  failed |= fclose(input) != 0;
  failed |= fclose(output) != 0;
  return failed;
}

static void rtrim(char *text) {
  size_t length;

  if (text == NULL) {
    return;
  }
  length = strlen(text);
  while (length > 0 && isspace((unsigned char)text[length - 1])) {
    text[--length] = '\0';
  }
}

static char *ltrim(char *text) {
  if (text == NULL) {
    return NULL;
  }
  while (*text != '\0' && isspace((unsigned char)*text)) {
    ++text;
  }
  return text;
}

static char *trim(char *text) {
  rtrim(text);
  return ltrim(text);
}

static int copy_checked(char *destination, size_t destination_size,
                        const char *source) {
  if (source == NULL || strlen(source) >= destination_size) {
    return 1;
  }
  strcpy(destination, source);
  return 0;
}

static int append_format(char *destination, size_t destination_size,
                         const char *format, ...) {
  size_t used = strlen(destination);
  va_list args;
  int written;

  if (used >= destination_size) {
    return 1;
  }
  va_start(args, format);
  written = vsnprintf(destination + used, destination_size - used, format, args);
  va_end(args);
  return written < 0 || (size_t)written >= destination_size - used;
}

static int config_error(int line_number, const char *message,
                        const char *detail) {
  if (line_number > 0) {
    fprintf(stderr, "%s:%d: %s%s%s\n", BMX_MACHINES_FILE, line_number,
            message, detail == NULL ? "" : ": ",
            detail == NULL ? "" : detail);
  } else {
    fprintf(stderr, "%s: %s%s%s\n", BMX_MACHINES_FILE, message,
            detail == NULL ? "" : ": ", detail == NULL ? "" : detail);
  }
  return 1;
}

static int valid_identifier(const char *identifier) {
  const unsigned char *p = (const unsigned char *)identifier;

  if (identifier == NULL || *identifier == '\0') {
    return 0;
  }
  while (*p != '\0') {
    if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') {
      return 0;
    }
    ++p;
  }
  return 1;
}

static int parse_int(const char *text, int *value) {
  char *end;
  long parsed;

  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno != 0 || end == text || *trim(end) != '\0' || parsed < 0 ||
      parsed > INT_MAX) {
    return 1;
  }
  *value = (int)parsed;
  return 0;
}

static int parse_signed_int(const char *text, int *value) {
  char *end;
  long parsed;

  errno = 0;
  parsed = strtol(text, &end, 10);
  if (errno != 0 || end == text || *trim(end) != '\0' || parsed < INT_MIN ||
      parsed > INT_MAX) {
    return 1;
  }
  *value = (int)parsed;
  return 0;
}

static int parse_bool(const char *text, int *value) {
  if (strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0) {
    *value = 1;
    return 0;
  }
  if (strcasecmp(text, "false") == 0 || strcmp(text, "0") == 0) {
    *value = 0;
    return 0;
  }
  return 1;
}

static int parse_unsigned_long(const char *text, unsigned long *value) {
  char *end;
  unsigned long parsed;

  if (text == NULL || text[0] == '-' || text[0] == '\0') {
    return 1;
  }
  errno = 0;
  parsed = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *trim(end) != '\0' || parsed == 0) {
    return 1;
  }
  *value = parsed;
  return 0;
}

static int parse_scaling(const char *text,
                         struct bmx_scaling_params *params) {
  char trailing;
  int count = sscanf(text, "%d,%d,%d,%d%c", &params->framebuffer_width,
                     &params->framebuffer_height, &params->scaled_width,
                     &params->scaled_height, &trailing);

  if (count != 4 || params->framebuffer_width <= 0 ||
      params->framebuffer_height <= 0 || params->scaled_width <= 0 ||
      params->scaled_height <= 0) {
    return 1;
  }
  params->present = 1;
  return 0;
}

static void init_video_mode(struct bmx_video_mode *mode) {
  memset(mode, 0, sizeof(*mode));
  mode->disable_overscan = -1;
  mode->sdtv_mode = -1;
  mode->hdmi_group = -1;
  mode->hdmi_mode = -1;
  mode->enable_dpi_lcd = -1;
  mode->display_default_lcd = -1;
  mode->dpi_group = -1;
  mode->dpi_mode = -1;
  mode->enable_dpi = -1;
  mode->framebuffer_width = -1;
  mode->framebuffer_height = -1;
  mode->framebuffer_depth = -1;
  mode->raster_skip = -1;
  mode->raster_skip2 = -1;
}

static BMC64MachineClass parse_machine_class(const char *name) {
  if (strcasecmp(name, "C64") == 0) {
    return BMC64_MACHINE_CLASS_C64;
  }
  if (strcasecmp(name, "SCPU64") == 0) {
    return BMC64_MACHINE_CLASS_SCPU64;
  }
  if (strcasecmp(name, "C128") == 0) {
    return BMC64_MACHINE_CLASS_C128;
  }
  if (strcasecmp(name, "VIC20") == 0) {
    return BMC64_MACHINE_CLASS_VIC20;
  }
  if (strcasecmp(name, "PLUS4") == 0) {
    return BMC64_MACHINE_CLASS_PLUS4;
  }
  if (strcasecmp(name, "PLUS4EMU") == 0) {
    return BMC64_MACHINE_CLASS_PLUS4EMU;
  }
  if (strcasecmp(name, "PET") == 0) {
    return BMC64_MACHINE_CLASS_PET;
  }
  return BMC64_MACHINE_CLASS_UNKNOWN;
}

const char *bmx_video_standard_name(BMC64VideoStandard standard) {
  switch (standard) {
    case BMC64_VIDEO_STANDARD_PAL:
      return "PAL";
    case BMC64_VIDEO_STANDARD_NTSC:
      return "NTSC";
    default:
      return "Unknown";
  }
}

const char *bmx_video_output_name(BMC64VideoOut output) {
  switch (output) {
    case BMC64_VIDEO_OUT_HDMI:
      return "HDMI";
    case BMC64_VIDEO_OUT_COMPOSITE:
      return "Composite";
    case BMC64_VIDEO_OUT_DPI:
      return "DPI";
    default:
      return "Unknown";
  }
}

static struct bmx_video_mode *video_mode_by_id_mutable(
    struct bmx_machine_config *config, const char *mode_id) {
  int i;

  for (i = 0; i < config->num_video_modes; ++i) {
    if (strcmp(config->video_modes[i].id, mode_id) == 0) {
      return &config->video_modes[i];
    }
  }
  return NULL;
}

static const struct bmx_scaling_set *scaling_set_by_id(
    const struct bmx_machine_config *config, const char *scaling_id) {
  int i;

  if (scaling_id == NULL || scaling_id[0] == '\0') {
    return NULL;
  }
  for (i = 0; i < config->num_scaling_sets; ++i) {
    if (strcmp(config->scaling_sets[i].id, scaling_id) == 0) {
      return &config->scaling_sets[i];
    }
  }
  return NULL;
}

static const struct bmx_scaling_params *scaling_params_for_mode(
    const struct bmx_scaling_set *scaling, const char *mode_id) {
  int i;

  if (scaling == NULL) {
    return NULL;
  }
  for (i = 0; i < scaling->num_entries; ++i) {
    if (strcmp(scaling->entries[i].mode_id, mode_id) == 0) {
      return &scaling->entries[i].params;
    }
  }
  return NULL;
}

const struct bmx_machine *bmx_machine_by_class(
    const struct bmx_machine_config *config,
    BMC64MachineClass machine_class) {
  int i;

  if (config == NULL) {
    return NULL;
  }
  for (i = 0; i < config->num_machines; ++i) {
    if (config->machines[i].machine_class == machine_class) {
      return &config->machines[i];
    }
  }
  return NULL;
}

const struct bmx_machine_mode *bmx_machine_mode_by_id(
    const struct bmx_machine *machine, const char *mode_id) {
  int i;

  if (machine == NULL || mode_id == NULL) {
    return NULL;
  }
  for (i = 0; i < machine->num_modes; ++i) {
    if (strcmp(machine->modes[i].mode_id, mode_id) == 0) {
      return &machine->modes[i];
    }
  }
  return NULL;
}

const struct bmx_machine_mode *bmx_machine_default_mode(
    const struct bmx_machine *machine) {
  if (machine == NULL) {
    return NULL;
  }
  return bmx_machine_mode_by_id(machine, machine->default_mode_id);
}

static int parse_video_option(struct bmx_video_mode *mode, const char *key,
                              const char *value) {
  if (strcmp(key, "label") == 0) {
    return copy_checked(mode->label, sizeof(mode->label), value);
  }
  if (strcmp(key, "standard") == 0) {
    if (strcasecmp(value, "PAL") == 0) {
      mode->standard = BMC64_VIDEO_STANDARD_PAL;
      return 0;
    }
    if (strcasecmp(value, "NTSC") == 0) {
      mode->standard = BMC64_VIDEO_STANDARD_NTSC;
      return 0;
    }
    return 1;
  }
  if (strcmp(key, "output") == 0) {
    if (strcasecmp(value, "HDMI") == 0) {
      mode->output = BMC64_VIDEO_OUT_HDMI;
      return 0;
    }
    if (strcasecmp(value, "Composite") == 0) {
      mode->output = BMC64_VIDEO_OUT_COMPOSITE;
      return 0;
    }
    if (strcasecmp(value, "DPI") == 0) {
      mode->output = BMC64_VIDEO_OUT_DPI;
      return 0;
    }
    return 1;
  }
  if (strcmp(key, "experimental") == 0) {
    return parse_bool(value, &mode->experimental);
  }
  if (strcmp(key, "disable_overscan") == 0) {
    return parse_int(value, &mode->disable_overscan);
  }
  if (strcmp(key, "sdtv_mode") == 0) {
    return parse_int(value, &mode->sdtv_mode);
  }
  if (strcmp(key, "hdmi_group") == 0) {
    return parse_int(value, &mode->hdmi_group);
  }
  if (strcmp(key, "hdmi_mode") == 0) {
    return parse_int(value, &mode->hdmi_mode);
  }
  if (strcmp(key, "hdmi_timings") == 0) {
    return copy_checked(mode->hdmi_timings, sizeof(mode->hdmi_timings), value);
  }
  if (strcmp(key, "hdmi_cvt") == 0) {
    return copy_checked(mode->hdmi_cvt, sizeof(mode->hdmi_cvt), value);
  }
  if (strcmp(key, "enable_dpi_lcd") == 0) {
    return parse_int(value, &mode->enable_dpi_lcd);
  }
  if (strcmp(key, "display_default_lcd") == 0) {
    return parse_int(value, &mode->display_default_lcd);
  }
  if (strcmp(key, "dpi_group") == 0) {
    return parse_int(value, &mode->dpi_group);
  }
  if (strcmp(key, "dpi_mode") == 0) {
    return parse_int(value, &mode->dpi_mode);
  }
  if (strcmp(key, "dpi_timings") == 0) {
    return copy_checked(mode->dpi_timings, sizeof(mode->dpi_timings), value);
  }
  if (strcmp(key, "dpi_output_format") == 0) {
    return copy_checked(mode->dpi_output_format,
                        sizeof(mode->dpi_output_format), value);
  }
  if (strcmp(key, "machine_timing") == 0) {
    return copy_checked(mode->machine_timing, sizeof(mode->machine_timing),
                        value);
  }
  if (strcmp(key, "cycles_per_second") == 0) {
    if (parse_unsigned_long(value, &mode->cycles_per_second) != 0) {
      return 1;
    }
    mode->have_cycles_per_second = 1;
    return 0;
  }
  if (strcmp(key, "audio_out") == 0) {
    if (strcmp(value, "auto") != 0 && strcmp(value, "hdmi") != 0 &&
        strcmp(value, "analog") != 0) {
      return 1;
    }
    return copy_checked(mode->audio_out, sizeof(mode->audio_out), value);
  }
  if (strcmp(key, "enable_dpi") == 0) {
    return parse_bool(value, &mode->enable_dpi);
  }
  if (strcmp(key, "framebuffer_width") == 0) {
    return parse_int(value, &mode->framebuffer_width);
  }
  if (strcmp(key, "framebuffer_height") == 0) {
    return parse_int(value, &mode->framebuffer_height);
  }
  if (strcmp(key, "framebuffer_depth") == 0) {
    return parse_int(value, &mode->framebuffer_depth);
  }
  if (strcmp(key, "raster_skip") == 0) {
    return parse_bool(value, &mode->raster_skip);
  }
  if (strcmp(key, "raster_skip2") == 0) {
    return parse_bool(value, &mode->raster_skip2);
  }
  return 1;
}

static int parse_machine_modes(struct bmx_machine *machine, char *value) {
  char *cursor = value;

  while (1) {
    char *comma = strchr(cursor, ',');
    char *mode_id;
    int i;

    if (comma != NULL) {
      *comma = '\0';
    }
    mode_id = trim(cursor);
    if (!valid_identifier(mode_id) ||
        machine->num_modes >= BMX_MAX_MACHINE_MODES) {
      return 1;
    }
    for (i = 0; i < machine->num_modes; ++i) {
      if (strcmp(machine->modes[i].mode_id, mode_id) == 0) {
        return 1;
      }
    }
    if (copy_checked(machine->modes[machine->num_modes].mode_id,
                     sizeof(machine->modes[machine->num_modes].mode_id),
                     mode_id) != 0) {
      return 1;
    }
    ++machine->num_modes;
    if (comma == NULL) {
      break;
    }
    cursor = comma + 1;
  }
  return 0;
}

static int parse_machine_option(struct bmx_machine *machine, const char *key,
                                char *value) {
  if (strcmp(key, "default") == 0) {
    return machine->default_mode_id[0] != '\0' || !valid_identifier(value) ||
           copy_checked(machine->default_mode_id,
                        sizeof(machine->default_mode_id), value);
  }
  if (strcmp(key, "modes") == 0) {
    return machine->num_modes != 0 || parse_machine_modes(machine, value);
  }
  if (strcmp(key, "scaling") == 0) {
    return machine->scaling_id[0] != '\0' || !valid_identifier(value) ||
           copy_checked(machine->scaling_id, sizeof(machine->scaling_id),
                        value);
  }
  if (strcmp(key, "scaling2") == 0) {
    return machine->scaling2_id[0] != '\0' || !valid_identifier(value) ||
           copy_checked(machine->scaling2_id, sizeof(machine->scaling2_id),
                        value);
  }
  return 1;
}

static int parse_scaling_option(struct bmx_scaling_set *scaling,
                                const char *key, const char *value) {
  struct bmx_scaling_entry *entry;
  int i;

  if (!valid_identifier(key) ||
      scaling->num_entries >= BMX_MAX_SCALING_ENTRIES) {
    return 1;
  }
  for (i = 0; i < scaling->num_entries; ++i) {
    if (strcmp(scaling->entries[i].mode_id, key) == 0) {
      return 1;
    }
  }
  entry = &scaling->entries[scaling->num_entries];
  if (copy_checked(entry->mode_id, sizeof(entry->mode_id), key) != 0 ||
      parse_scaling(value, &entry->params) != 0) {
    return 1;
  }
  ++scaling->num_entries;
  return 0;
}

static int video_mode_valid(const struct bmx_video_mode *mode) {
  int custom;
  int timing_matches_output = 0;

  if (mode->label[0] == '\0' ||
      mode->standard == BMC64_VIDEO_STANDARD_UNKNOWN ||
      mode->output == BMC64_VIDEO_OUT_UNKNOWN ||
      mode->machine_timing[0] == '\0') {
    return 0;
  }

  if (mode->standard == BMC64_VIDEO_STANDARD_PAL) {
    timing_matches_output =
        (mode->output == BMC64_VIDEO_OUT_HDMI &&
         (strcmp(mode->machine_timing, "pal-hdmi") == 0 ||
          strcmp(mode->machine_timing, "pal-custom") == 0)) ||
        (mode->output == BMC64_VIDEO_OUT_COMPOSITE &&
         strcmp(mode->machine_timing, "pal-composite") == 0) ||
        (mode->output == BMC64_VIDEO_OUT_DPI &&
         (strcmp(mode->machine_timing, "pal-dpi") == 0 ||
          strcmp(mode->machine_timing, "pal-custom") == 0));
  } else if (mode->standard == BMC64_VIDEO_STANDARD_NTSC) {
    timing_matches_output =
        (mode->output == BMC64_VIDEO_OUT_HDMI &&
         (strcmp(mode->machine_timing, "ntsc-hdmi") == 0 ||
          strcmp(mode->machine_timing, "ntsc-custom") == 0)) ||
        (mode->output == BMC64_VIDEO_OUT_COMPOSITE &&
         strcmp(mode->machine_timing, "ntsc-composite") == 0) ||
        (mode->output == BMC64_VIDEO_OUT_DPI &&
         (strcmp(mode->machine_timing, "ntsc-dpi") == 0 ||
          strcmp(mode->machine_timing, "ntsc-custom") == 0));
  }
  if (!timing_matches_output) {
    return 0;
  }

  if ((mode->output == BMC64_VIDEO_OUT_HDMI ||
       mode->output == BMC64_VIDEO_OUT_COMPOSITE) &&
      (mode->hdmi_group < 0 || mode->hdmi_mode < 0)) {
    return 0;
  }
  if (mode->output == BMC64_VIDEO_OUT_COMPOSITE && mode->sdtv_mode < 0) {
    return 0;
  }
  if (mode->output == BMC64_VIDEO_OUT_DPI &&
      (mode->enable_dpi != 1 || mode->dpi_group < 0 || mode->dpi_mode < 0)) {
    return 0;
  }
  if (mode->output == BMC64_VIDEO_OUT_HDMI && mode->hdmi_mode == 87 &&
      mode->hdmi_timings[0] == '\0' && mode->hdmi_cvt[0] == '\0') {
    return 0;
  }
  if (mode->output == BMC64_VIDEO_OUT_DPI && mode->dpi_mode == 87 &&
      mode->dpi_timings[0] == '\0') {
    return 0;
  }
  if (mode->framebuffer_width == 0 || mode->framebuffer_height == 0 ||
      mode->framebuffer_depth == 0) {
    return 0;
  }

  custom = strcmp(mode->machine_timing, "pal-custom") == 0 ||
           strcmp(mode->machine_timing, "ntsc-custom") == 0;
  if (custom && !mode->have_cycles_per_second) {
    return 0;
  }
  if (mode->hdmi_timings[0] != '\0' && mode->hdmi_cvt[0] != '\0') {
    return 0;
  }
  return 1;
}

static int resolve_machine_config(struct bmx_machine_config *config) {
  int i;
  int j;

  for (i = 0; i < config->num_video_modes; ++i) {
    if (!video_mode_valid(&config->video_modes[i])) {
      return config_error(0, "invalid or incomplete video mode",
                          config->video_modes[i].id);
    }
  }

  for (i = 0; i < config->num_scaling_sets; ++i) {
    for (j = 0; j < config->scaling_sets[i].num_entries; ++j) {
      if (video_mode_by_id_mutable(
              config, config->scaling_sets[i].entries[j].mode_id) == NULL) {
        return config_error(0, "scaling references unknown video mode",
                            config->scaling_sets[i].entries[j].mode_id);
      }
    }
  }

  for (i = 0; i < config->num_machines; ++i) {
    struct bmx_machine *machine = &config->machines[i];
    const struct bmx_scaling_set *scaling;
    const struct bmx_scaling_set *scaling2;

    if (machine->machine_class == BMC64_MACHINE_CLASS_UNKNOWN ||
        machine->num_modes == 0 || machine->default_mode_id[0] == '\0') {
      return config_error(0, "invalid or incomplete machine", machine->id);
    }
    for (j = 0; j < i; ++j) {
      if (config->machines[j].machine_class == machine->machine_class) {
        return config_error(0, "duplicate machine class", machine->id);
      }
    }

    scaling = scaling_set_by_id(config, machine->scaling_id);
    scaling2 = scaling_set_by_id(config, machine->scaling2_id);
    if (machine->scaling_id[0] != '\0' && scaling == NULL) {
      return config_error(0, "machine references unknown scaling set",
                          machine->scaling_id);
    }
    if (machine->scaling2_id[0] != '\0' && scaling2 == NULL) {
      return config_error(0, "machine references unknown scaling2 set",
                          machine->scaling2_id);
    }

    for (j = 0; j < machine->num_modes; ++j) {
      const struct bmx_scaling_params *params;
      machine->modes[j].video_mode =
          video_mode_by_id_mutable(config, machine->modes[j].mode_id);
      if (machine->modes[j].video_mode == NULL) {
        return config_error(0, "machine references unknown video mode",
                            machine->modes[j].mode_id);
      }
      params = scaling_params_for_mode(scaling, machine->modes[j].mode_id);
      if (params != NULL) {
        machine->modes[j].scaling = *params;
      }
      params = scaling_params_for_mode(scaling2, machine->modes[j].mode_id);
      if (params != NULL) {
        machine->modes[j].scaling2 = *params;
      }
    }
    if (bmx_machine_default_mode(machine) == NULL) {
      return config_error(0, "default is not listed in machine modes",
                          machine->default_mode_id);
    }
  }

  return config->num_video_modes == 0 || config->num_machines == 0
             ? config_error(0, "configuration has no modes or machines", NULL)
             : 0;
}

int load_machine_config(struct bmx_machine_config **result) {
  char path[CONFIG_TXT_LINE_LEN];
  char line[CONFIG_TXT_LINE_LEN];
  struct bmx_machine_config *config;
  enum section_kind section = SECTION_NONE;
  void *current = NULL;
  int line_number = 0;
  int format_seen = 0;
  FILE *file;

  if (result == NULL) {
    return 1;
  }
  *result = NULL;
  boot_path(path, sizeof(path), BMX_MACHINES_FILE);
  file = fopen(path, "r");
  if (file == NULL) {
    return config_error(0, "could not open configuration", path);
  }

  config = calloc(1, sizeof(*config));
  if (config == NULL) {
    fclose(file);
    return config_error(0, "out of memory", NULL);
  }

  while (fgets(line, sizeof(line), file) != NULL) {
    char *text;
    char *equals;
    char *key;
    char *value;

    ++line_number;
    if (strchr(line, '\n') == NULL && !feof(file)) {
      config_error(line_number, "line is too long", NULL);
      goto fail;
    }
    text = trim(line);
    if (*text == '\0' || *text == '#') {
      continue;
    }

    if (*text == '[') {
      char *close = strchr(text + 1, ']');
      char *header;
      char *space;
      char *kind;
      char *id;
      int i;

      if (!format_seen || close == NULL || *trim(close + 1) != '\0') {
        config_error(line_number, "invalid section header", text);
        goto fail;
      }
      *close = '\0';
      header = trim(text + 1);
      space = strpbrk(header, " \t");
      if (space == NULL) {
        config_error(line_number, "section needs a type and identifier",
                     header);
        goto fail;
      }
      *space = '\0';
      kind = header;
      id = trim(space + 1);
      if (!valid_identifier(id)) {
        config_error(line_number, "invalid section identifier", id);
        goto fail;
      }

      if (strcasecmp(kind, "video") == 0) {
        struct bmx_video_mode *mode;
        if (config->num_video_modes >= BMX_MAX_VIDEO_MODES ||
            video_mode_by_id_mutable(config, id) != NULL) {
          config_error(line_number, "duplicate or excessive video mode", id);
          goto fail;
        }
        mode = &config->video_modes[config->num_video_modes++];
        init_video_mode(mode);
        if (copy_checked(mode->id, sizeof(mode->id), id) != 0) {
          config_error(line_number, "video mode identifier is too long", id);
          goto fail;
        }
        section = SECTION_VIDEO;
        current = mode;
      } else if (strcasecmp(kind, "scaling") == 0) {
        struct bmx_scaling_set *scaling;
        if (config->num_scaling_sets >= BMX_MAX_SCALING_SETS) {
          config_error(line_number, "too many scaling sets", id);
          goto fail;
        }
        for (i = 0; i < config->num_scaling_sets; ++i) {
          if (strcmp(config->scaling_sets[i].id, id) == 0) {
            config_error(line_number, "duplicate scaling set", id);
            goto fail;
          }
        }
        scaling = &config->scaling_sets[config->num_scaling_sets++];
        if (copy_checked(scaling->id, sizeof(scaling->id), id) != 0) {
          config_error(line_number, "scaling identifier is too long", id);
          goto fail;
        }
        section = SECTION_SCALING;
        current = scaling;
      } else if (strcasecmp(kind, "machine") == 0) {
        struct bmx_machine *machine;
        if (config->num_machines >= BMX_MAX_MACHINES) {
          config_error(line_number, "too many machines", id);
          goto fail;
        }
        for (i = 0; i < config->num_machines; ++i) {
          if (strcasecmp(config->machines[i].id, id) == 0) {
            config_error(line_number, "duplicate machine", id);
            goto fail;
          }
        }
        machine = &config->machines[config->num_machines++];
        if (copy_checked(machine->id, sizeof(machine->id), id) != 0) {
          config_error(line_number, "machine identifier is too long", id);
          goto fail;
        }
        machine->machine_class = parse_machine_class(id);
        section = SECTION_MACHINE;
        current = machine;
      } else {
        config_error(line_number, "unknown section type", kind);
        goto fail;
      }
      continue;
    }

    equals = strchr(text, '=');
    if (equals == NULL) {
      config_error(line_number, "expected key=value", text);
      goto fail;
    }
    *equals = '\0';
    key = trim(text);
    value = trim(equals + 1);
    if (*key == '\0' || *value == '\0') {
      config_error(line_number, "empty key or value", NULL);
      goto fail;
    }

    if (section == SECTION_NONE) {
      if (strcmp(key, "format") != 0 || strcmp(value, "1") != 0 ||
          format_seen) {
        config_error(line_number, "expected format=1 before sections", NULL);
        goto fail;
      }
      format_seen = 1;
    } else if (section == SECTION_VIDEO) {
      if (parse_video_option((struct bmx_video_mode *)current, key, value) !=
          0) {
        config_error(line_number, "invalid video option", key);
        goto fail;
      }
    } else if (section == SECTION_SCALING) {
      if (parse_scaling_option((struct bmx_scaling_set *)current, key, value) !=
          0) {
        config_error(line_number, "invalid scaling entry", key);
        goto fail;
      }
    } else if (section == SECTION_MACHINE) {
      if (parse_machine_option((struct bmx_machine *)current, key, value) != 0) {
        config_error(line_number, "invalid machine option", key);
        goto fail;
      }
    }
  }

  if (ferror(file)) {
    config_error(line_number, "could not read configuration", path);
    goto fail;
  }
  if (!format_seen) {
    config_error(0, "missing format=1", NULL);
    goto fail;
  }
  if (resolve_machine_config(config) != 0) {
    goto fail;
  }
  fclose(file);
  *result = config;
  return 0;

fail:
  fclose(file);
  free(config);
  return 1;
}

void free_machine_config(struct bmx_machine_config *config) {
  free(config);
}

static const char *default_kernel_for_pi_model(int pi_model) {
  size_t i;

  for (i = 0; i < sizeof(pi_kernel_specs) / sizeof(pi_kernel_specs[0]); ++i) {
    if (pi_kernel_specs[i].pi_model == pi_model) {
      return pi_kernel_specs[i].kernel_name;
    }
  }
  return NULL;
}

static int model_uses_release_selector(int pi_model) {
  return pi_model == 4 || pi_model == 5;
}

static int normal_machine_kernel_name(const char *name, size_t length) {
  static const char *const machines[] = {
      "c64", "c64sc", "scpu64", "c128", "vic20", "plus4", "pet",
  };
  size_t i;

  for (i = 0; i < sizeof(machines) / sizeof(machines[0]); ++i) {
    if (strlen(machines[i]) == length &&
        strncmp(name, machines[i], length) == 0) {
      return 1;
    }
  }
  return 0;
}

static int parse_machine_kernel_name(const char *kernel_name,
                                     const char *expected_base) {
  const char *machine;
  size_t machine_length;

  if (kernel_name == NULL || expected_base == NULL ||
      strncmp(kernel_name, expected_base, strlen(expected_base)) != 0 ||
      kernel_name[strlen(expected_base)] != '.') {
    return 1;
  }
  machine = kernel_name + strlen(expected_base) + 1;
  machine_length = strlen(machine);
  return normal_machine_kernel_name(machine, machine_length) ? 0 : 1;
}

static int validate_active_machine_selector(int pi_model) {
  char path[CONFIG_TXT_LINE_LEN];
  char content[CONFIG_TXT_LINE_LEN];
  char kernel_name[VALUE_LEN];
  const char *base = default_kernel_for_pi_model(pi_model);
  const size_t header_length = strlen(BMX_KERNEL_SELECTOR_HEADER);
  const char *kernel_line;
  size_t length;
  size_t kernel_length;
  FILE *file;
  int extra;

  if (!model_uses_release_selector(pi_model) || base == NULL) {
    return 1;
  }
  boot_path(path, sizeof(path), BMX_ACTIVE_KERNEL_SELECTOR);
  file = fopen(path, "rb");
  if (file == NULL) {
    return 1;
  }
  length = fread(content, 1, sizeof(content) - 1, file);
  extra = fgetc(file);
  if (ferror(file) || extra != EOF) {
    fclose(file);
    return 1;
  }
  if (fclose(file) != 0) {
    return 1;
  }
  content[length] = '\0';
  if (length <= header_length + sizeof("kernel=\n") - 1 ||
      memcmp(content, BMX_KERNEL_SELECTOR_HEADER, header_length) != 0) {
    return 1;
  }
  kernel_line = content + header_length;
  if (memcmp(kernel_line, "kernel=", sizeof("kernel=") - 1) != 0 ||
      content[length - 1] != '\n') {
    return 1;
  }
  kernel_length = length - header_length - (sizeof("kernel=") - 1) - 1;
  if (kernel_length == 0 || kernel_length >= sizeof(kernel_name) ||
      memchr(kernel_line + sizeof("kernel=") - 1, '\n', kernel_length) !=
          NULL ||
      memchr(kernel_line + sizeof("kernel=") - 1, '\r', kernel_length) !=
          NULL ||
      memchr(kernel_line + sizeof("kernel=") - 1, '\0', kernel_length) !=
          NULL) {
    return 1;
  }
  memcpy(kernel_name, kernel_line + sizeof("kernel=") - 1, kernel_length);
  kernel_name[kernel_length] = '\0';
  return parse_machine_kernel_name(kernel_name, base);
}

static BMC64C64Core effective_c64_core(BMC64C64Core requested_core) {
  if (requested_core == BMC64_C64_CORE_X64 ||
      requested_core == BMC64_C64_CORE_X64SC) {
    return requested_core;
  }
  if (emux_c64_core == BMC64_C64_CORE_X64 ||
      emux_c64_core == BMC64_C64_CORE_X64SC) {
    return emux_c64_core;
  }
  return BMC64_C64_CORE_X64;
}

static const char *kernel_suffix_for_machine_class(
    BMC64MachineClass machine_class, BMC64C64Core requested_core) {
  switch (machine_class) {
    case BMC64_MACHINE_CLASS_C64:
      return effective_c64_core(requested_core) == BMC64_C64_CORE_X64SC
                 ? ".c64sc"
                 : ".c64";
    case BMC64_MACHINE_CLASS_SCPU64:
      return ".scpu64";
    case BMC64_MACHINE_CLASS_C128:
      return ".c128";
    case BMC64_MACHINE_CLASS_VIC20:
      return ".vic20";
    case BMC64_MACHINE_CLASS_PLUS4:
      return ".plus4";
    case BMC64_MACHINE_CLASS_PLUS4EMU:
      return ".plus4emu";
    case BMC64_MACHINE_CLASS_PET:
      return ".pet";
    default:
      return NULL;
  }
}

static int machine_kernel_name_status(BMC64MachineClass machine_class,
                                      BMC64C64Core requested_core,
                                      int pi_model, char *name,
                                      size_t name_size) {
  const char *kernel = default_kernel_for_pi_model(pi_model);
  const char *suffix =
      kernel_suffix_for_machine_class(machine_class, requested_core);

  if (name == NULL || name_size == 0 || suffix == NULL) {
    return ERROR_UNKNOWN_MACHINE;
  }
  if (kernel == NULL) {
    return ERROR_UNKNOWN_PI;
  }
  if (model_uses_release_selector(pi_model) &&
      validate_active_machine_selector(pi_model) != 0) {
    return BMC64_SWITCH_ERROR_SELECTOR_INVALID;
  }
  if (snprintf(name, name_size, "%s%s", kernel, suffix) >=
      (int)name_size) {
    return ERROR_INVALID_CONFIG;
  }
  return 0;
}

int switch_machine_kernel_name(BMC64MachineClass machine_class,
                               BMC64C64Core requested_core, int pi_model,
                               char *name, size_t name_size) {
  return machine_kernel_name_status(machine_class, requested_core, pi_model,
                                    name, name_size) == 0
             ? 0
             : 1;
}

int switch_c64_core_kernel_name(BMC64C64Core core, int pi_model, char *name,
                                size_t name_size) {
  if (core != BMC64_C64_CORE_X64 && core != BMC64_C64_CORE_X64SC) {
    return 1;
  }
  return switch_machine_kernel_name(BMC64_MACHINE_CLASS_C64, core, pi_model,
                                    name, name_size);
}

static int kernel_file_exists(const char *kernel_name) {
  char path[CONFIG_TXT_LINE_LEN];
  boot_path(path, sizeof(path), kernel_name);
  return access(path, R_OK) == 0;
}

int switch_check_selection(const struct bmx_machine *machine,
                           BMC64C64Core c64_core) {
  char kernel_name[VALUE_LEN];
  int status;

  if (machine == NULL) {
    return ERROR_UNKNOWN_MACHINE;
  }
  if (default_kernel_for_pi_model(circle_get_model()) == NULL) {
    return ERROR_UNKNOWN_PI;
  }
  status = machine_kernel_name_status(machine->machine_class, c64_core,
                                      circle_get_model(), kernel_name,
                                      sizeof(kernel_name));
  if (status != 0) {
    return status;
  }
  return kernel_file_exists(kernel_name) ? 0
                                         : BMC64_SWITCH_ERROR_KERNEL_MISSING;
}

void bmx_boot_plan_init(struct bmx_boot_plan *plan) {
  if (plan != NULL) {
    memset(plan, 0, sizeof(*plan));
  }
}

int bmx_boot_plan_manage_cmdline_key(struct bmx_boot_plan *plan,
                                     const char *key) {
  int i;

  if (plan == NULL || !valid_identifier(key) || strlen(key) >= KEY_LEN) {
    return 1;
  }
  for (i = 0; i < plan->num_managed_cmdline_keys; ++i) {
    if (strcmp(plan->managed_cmdline_keys[i], key) == 0) {
      return 0;
    }
  }
  if (plan->num_managed_cmdline_keys >= BMX_MAX_MANAGED_CMDLINE_KEYS) {
    return 1;
  }
  strcpy(plan->managed_cmdline_keys[plan->num_managed_cmdline_keys++], key);
  return 0;
}

int bmx_boot_plan_set_cmdline_option(struct bmx_boot_plan *plan,
                                     const char *key, const char *value) {
  if (plan == NULL || value == NULL || value[0] == '\0' ||
      strpbrk(value, " \t\r\n") != NULL ||
      bmx_boot_plan_manage_cmdline_key(plan, key) != 0) {
    return 1;
  }
  return append_format(plan->cmdline_options,
                       sizeof(plan->cmdline_options), "%s%s=%s",
                       plan->cmdline_options[0] == '\0' ? "" : " ", key,
                       value);
}

int bmx_boot_plan_set_developer_mode(struct bmx_boot_plan *plan, int enabled) {
  if (enabled != 0 && enabled != 1) {
    return 1;
  }
  if (!enabled) {
    return bmx_boot_plan_manage_cmdline_key(plan, "developer_mode");
  }
  return bmx_boot_plan_set_cmdline_option(plan, "developer_mode", "1");
}

int bmx_boot_plan_set_developer_password(struct bmx_boot_plan *plan,
                                         const char *password) {
  static const char hex[] = "0123456789ABCDEF";
  char encoded[BMX_DEVELOPER_PASSWORD_MAX_LEN * 3 + 1];
  size_t input_size;
  size_t output_size = 0;
  size_t i;

  if (plan == NULL || password == NULL) {
    return 1;
  }
  input_size = strlen(password);
  if (input_size > BMX_DEVELOPER_PASSWORD_MAX_LEN) {
    return 1;
  }
  if (input_size == 0) {
    return bmx_boot_plan_manage_cmdline_key(plan, "developer_password");
  }

  for (i = 0; i < input_size; ++i) {
    unsigned char value = (unsigned char)password[i];
    if ((value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || value == '-' || value == '_' ||
        value == '.' || value == '~') {
      encoded[output_size++] = (char)value;
    } else {
      encoded[output_size++] = '%';
      encoded[output_size++] = hex[value >> 4];
      encoded[output_size++] = hex[value & 0x0f];
    }
  }
  encoded[output_size] = '\0';
  return bmx_boot_plan_set_cmdline_option(plan, "developer_password",
                                          encoded);
}

int bmx_boot_plan_set_developer_log_buffer_kb(struct bmx_boot_plan *plan,
                                              unsigned buffer_kb) {
  char value[16];

  if (buffer_kb < BMX_DEVELOPER_LOG_BUFFER_MIN_KB ||
      buffer_kb > BMX_DEVELOPER_LOG_BUFFER_MAX_KB ||
      buffer_kb % BMX_DEVELOPER_LOG_BUFFER_STEP_KB != 0U) {
    return 1;
  }
  snprintf(value, sizeof value, "%u", buffer_kb);
  return bmx_boot_plan_set_cmdline_option(
      plan, "developer_log_buffer_kb", value);
}

static int boot_plan_add_config_option(struct bmx_boot_plan *plan,
                                       const char *key, const char *value) {
  const unsigned char *p;

  if (plan == NULL || key == NULL || key[0] == '\0' || value == NULL ||
      value[0] == '\0' || strpbrk(value, "\r\n") != NULL) {
    return 1;
  }
  for (p = (const unsigned char *)key; *p != '\0'; ++p) {
    if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.' && *p != ':') {
      return 1;
    }
  }
  return append_format(plan->config_options, sizeof(plan->config_options),
                       "%s=%s\n", key, value);
}

static int boot_plan_add_kernel_selection(struct bmx_boot_plan *plan,
                                          int pi_model,
                                          const char *selected_kernel) {
  const char *base = default_kernel_for_pi_model(pi_model);
  char fallback[VALUE_LEN];

  if (model_uses_release_selector(pi_model)) {
    return base == NULL ||
           snprintf(fallback, sizeof(fallback), "%s.c64", base) >=
               (int)sizeof(fallback) ||
           boot_plan_add_config_option(plan, "kernel", fallback) != 0 ||
           append_format(plan->config_options, sizeof(plan->config_options),
                         "include %s\n", BMX_ACTIVE_KERNEL_SELECTOR) != 0;
  }
  return boot_plan_add_config_option(plan, "kernel", selected_kernel);
}

static int release_config_options_are_canonical(const char *options,
                                                const char *fallback) {
  char expected_kernel[VALUE_LEN + 16];
  char expected_include[VALUE_LEN + 16];
  const char *line = options;
  int kernel_count = 0;
  int include_count = 0;
  int include_seen = 0;

  if (options == NULL || fallback == NULL ||
      snprintf(expected_kernel, sizeof(expected_kernel), "kernel=%s",
               fallback) >= (int)sizeof(expected_kernel) ||
      snprintf(expected_include, sizeof(expected_include), "include %s",
               BMX_ACTIVE_KERNEL_SELECTOR) >= (int)sizeof(expected_include)) {
    return 0;
  }
  while (*line != '\0') {
    const char *end = strchr(line, '\n');
    size_t length;

    if (end == NULL) {
      return 0;
    }
    length = (size_t)(end - line);
    if (strncmp(line, "kernel=", sizeof("kernel=") - 1) == 0) {
      if (length != strlen(expected_kernel) ||
          memcmp(line, expected_kernel, length) != 0 || include_seen) {
        return 0;
      }
      ++kernel_count;
    } else if (strncmp(line, "include ", sizeof("include ") - 1) == 0) {
      if (length != strlen(expected_include) ||
          memcmp(line, expected_include, length) != 0 || kernel_count != 1) {
        return 0;
      }
      ++include_count;
      include_seen = 1;
    }
    line = end + 1;
  }
  return kernel_count == 1 && include_count == 1;
}

static int boot_plan_add_config_int(struct bmx_boot_plan *plan,
                                    const char *key, int value) {
  char text[24];
  snprintf(text, sizeof(text), "%d", value);
  return boot_plan_add_config_option(plan, key, text);
}

static int overclock_value_valid(unsigned field, int value, int pi_model) {
  switch (field) {
    case BMX_OVERCLOCK_ARM_FREQ:
      return pi_model == 4 ? value >= 600 && value <= 2400
                           : pi_model == 5 && value >= 1500 && value <= 3200;
    case BMX_OVERCLOCK_CORE_FREQ:
    case BMX_OVERCLOCK_V3D_FREQ:
      return (pi_model == 4 || pi_model == 5) && value >= 100 && value <= 1200;
    case BMX_OVERCLOCK_VOLTAGE_DELTA:
      return (pi_model == 4 || pi_model == 5) &&
             value >= -100000 && value <= 100000;
    case BMX_OVERCLOCK_TEMP_LIMIT:
      return (pi_model == 4 || pi_model == 5) && value >= 1 && value <= 85;
    default:
      return 0;
  }
}

static int overclock_config_valid(const struct bmx_overclock_config *config,
                                  int pi_model) {
  if (config == NULL || (config->present & ~BMX_OVERCLOCK_ALL) != 0) {
    return 0;
  }
  return (!(config->present & BMX_OVERCLOCK_ARM_FREQ) ||
          overclock_value_valid(BMX_OVERCLOCK_ARM_FREQ,
                                config->arm_freq_mhz, pi_model)) &&
         (!(config->present & BMX_OVERCLOCK_CORE_FREQ) ||
          overclock_value_valid(BMX_OVERCLOCK_CORE_FREQ,
                                config->core_freq_mhz, pi_model)) &&
         (!(config->present & BMX_OVERCLOCK_V3D_FREQ) ||
          overclock_value_valid(BMX_OVERCLOCK_V3D_FREQ,
                                config->v3d_freq_mhz, pi_model)) &&
         (!(config->present & BMX_OVERCLOCK_VOLTAGE_DELTA) ||
          overclock_value_valid(BMX_OVERCLOCK_VOLTAGE_DELTA,
                                config->over_voltage_delta_uv, pi_model)) &&
         (!(config->present & BMX_OVERCLOCK_TEMP_LIMIT) ||
          overclock_value_valid(BMX_OVERCLOCK_TEMP_LIMIT,
                                config->temp_limit_c, pi_model));
}

int bmx_boot_plan_add_overclock(struct bmx_boot_plan *plan,
                               const struct bmx_overclock_config *config) {
  if (plan == NULL || !plan->update_config ||
      !overclock_config_valid(config, circle_get_model())) {
    return ERROR_INVALID_CONFIG;
  }
  if (((config->present & BMX_OVERCLOCK_ARM_FREQ) &&
       boot_plan_add_config_int(plan, "arm_freq", config->arm_freq_mhz)) ||
      ((config->present & BMX_OVERCLOCK_CORE_FREQ) &&
       boot_plan_add_config_int(plan, "core_freq", config->core_freq_mhz)) ||
      ((config->present & BMX_OVERCLOCK_V3D_FREQ) &&
       boot_plan_add_config_int(plan, "v3d_freq", config->v3d_freq_mhz)) ||
      ((config->present & BMX_OVERCLOCK_VOLTAGE_DELTA) &&
       boot_plan_add_config_int(plan, "over_voltage_delta",
                                config->over_voltage_delta_uv)) ||
      ((config->present & BMX_OVERCLOCK_TEMP_LIMIT) &&
       boot_plan_add_config_int(plan, "temp_limit", config->temp_limit_c))) {
    return ERROR_INVALID_CONFIG;
  }
  return 0;
}

static int boot_plan_set_cmdline_int(struct bmx_boot_plan *plan,
                                     const char *key, int value) {
  char text[24];
  snprintf(text, sizeof(text), "%d", value);
  return bmx_boot_plan_set_cmdline_option(plan, key, text);
}

static int prepare_machine_cmdline_keys(struct bmx_boot_plan *plan) {
  static const char *const keys[] = {
      "bmx_video_mode",       "cycles_per_refresh",
      "cycles_per_second",   "machine_timing",
      "audio_out",           "enable_dpi",
      "hdmi_group",          "hdmi_mode",
      "pi5kms_timings",
      "scaling_params",      "scaling_params2",
      "raster_skip",         "raster_skip2",
  };
  size_t i;

  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    if (bmx_boot_plan_manage_cmdline_key(plan, keys[i]) != 0) {
      return 1;
    }
  }
  return 0;
}

static int boot_plan_set_scaling(struct bmx_boot_plan *plan, const char *key,
                                 const struct bmx_scaling_params *params) {
  char value[96];

  if (params == NULL || !params->present) {
    return 0;
  }
  snprintf(value, sizeof(value), "%d,%d,%d,%d", params->framebuffer_width,
           params->framebuffer_height, params->scaled_width,
           params->scaled_height);
  return bmx_boot_plan_set_cmdline_option(plan, key, value);
}

static int boot_plan_set_pi5kms_timings(struct bmx_boot_plan *plan,
                                        const char *hdmi_timings) {
  char value[BMX_TIMINGS_LEN];
  size_t i;

  if (hdmi_timings == NULL || hdmi_timings[0] == '\0') {
    return 0;
  }
  if (copy_checked(value, sizeof(value), hdmi_timings) != 0) {
    return 1;
  }
  for (i = 0; value[i] != '\0'; ++i) {
    if (isspace((unsigned char)value[i])) {
      value[i] = ',';
    }
  }
  return bmx_boot_plan_set_cmdline_option(plan, "pi5kms_timings", value);
}

int switch_build_boot_plan(const struct bmx_machine *machine,
                           const struct bmx_machine_mode *binding,
                           BMC64C64Core c64_core,
                           struct bmx_boot_plan *plan) {
  const struct bmx_video_mode *mode;
  char number[32];
  int pi_model = circle_get_model();
  int status;

  if (machine == NULL || binding == NULL || binding->video_mode == NULL ||
      plan == NULL || binding->mode_id[0] == '\0' ||
      strcmp(binding->mode_id, binding->video_mode->id) != 0 ||
      bmx_machine_mode_by_id(machine, binding->mode_id) != binding) {
    return ERROR_INVALID_CONFIG;
  }
  status = switch_check_selection(machine, c64_core);
  if (status != 0) {
    return status;
  }

  bmx_boot_plan_init(plan);
  plan->update_config = 1;
  if (switch_machine_kernel_name(machine->machine_class, c64_core, pi_model,
                                 plan->kernel_name,
                                 sizeof(plan->kernel_name)) != 0 ||
      prepare_machine_cmdline_keys(plan) != 0 ||
      boot_plan_add_kernel_selection(plan, pi_model, plan->kernel_name) != 0) {
    return ERROR_INVALID_CONFIG;
  }

  mode = binding->video_mode;
#define ADD_CONFIG_INT_IF_PRESENT(field, key)                         \
  do {                                                                \
    if (mode->field >= 0 &&                                           \
        boot_plan_add_config_int(plan, (key), mode->field) != 0) {    \
      return ERROR_INVALID_CONFIG;                                    \
    }                                                                 \
  } while (0)

  ADD_CONFIG_INT_IF_PRESENT(disable_overscan, "disable_overscan");
  ADD_CONFIG_INT_IF_PRESENT(sdtv_mode, "sdtv_mode");
  ADD_CONFIG_INT_IF_PRESENT(hdmi_group, "hdmi_group");
  ADD_CONFIG_INT_IF_PRESENT(hdmi_mode, "hdmi_mode");
  if (pi_model == 4) {
    ADD_CONFIG_INT_IF_PRESENT(hdmi_group, "hdmi_group:0");
    ADD_CONFIG_INT_IF_PRESENT(hdmi_mode, "hdmi_mode:0");
  }
  if (mode->hdmi_timings[0] != '\0' &&
      boot_plan_add_config_option(plan, "hdmi_timings",
                                  mode->hdmi_timings) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->hdmi_cvt[0] != '\0' &&
      boot_plan_add_config_option(plan, "hdmi_cvt", mode->hdmi_cvt) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  ADD_CONFIG_INT_IF_PRESENT(enable_dpi_lcd, "enable_dpi_lcd");
  ADD_CONFIG_INT_IF_PRESENT(display_default_lcd, "display_default_lcd");
  ADD_CONFIG_INT_IF_PRESENT(dpi_group, "dpi_group");
  ADD_CONFIG_INT_IF_PRESENT(dpi_mode, "dpi_mode");
  if (mode->dpi_timings[0] != '\0' &&
      boot_plan_add_config_option(plan, "dpi_timings", mode->dpi_timings) !=
          0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->dpi_output_format[0] != '\0' &&
      boot_plan_add_config_option(plan, "dpi_output_format",
                                  mode->dpi_output_format) != 0) {
    return ERROR_INVALID_CONFIG;
  }
#undef ADD_CONFIG_INT_IF_PRESENT

  if (bmx_boot_plan_set_cmdline_option(plan, "bmx_video_mode", mode->id) != 0 ||
      bmx_boot_plan_set_cmdline_option(plan, "machine_timing",
                                       mode->machine_timing) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->have_cycles_per_second) {
    snprintf(number, sizeof(number), "%lu", mode->cycles_per_second);
    if (bmx_boot_plan_set_cmdline_option(plan, "cycles_per_second", number) !=
        0) {
      return ERROR_INVALID_CONFIG;
    }
  }
  if (mode->audio_out[0] != '\0' &&
      bmx_boot_plan_set_cmdline_option(plan, "audio_out", mode->audio_out) !=
          0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->enable_dpi >= 0 &&
      boot_plan_set_cmdline_int(plan, "enable_dpi", mode->enable_dpi) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->framebuffer_width >= 0 &&
      boot_plan_set_cmdline_int(plan, "framebuffer_width",
                                mode->framebuffer_width) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->framebuffer_height >= 0 &&
      boot_plan_set_cmdline_int(plan, "framebuffer_height",
                                mode->framebuffer_height) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->framebuffer_depth >= 0 &&
      boot_plan_set_cmdline_int(plan, "framebuffer_depth",
                                mode->framebuffer_depth) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->hdmi_group >= 0 &&
      boot_plan_set_cmdline_int(plan, "hdmi_group", mode->hdmi_group) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->hdmi_mode >= 0 &&
      boot_plan_set_cmdline_int(plan, "hdmi_mode", mode->hdmi_mode) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (boot_plan_set_pi5kms_timings(plan, mode->hdmi_timings) != 0 ||
      boot_plan_set_scaling(plan, "scaling_params", &binding->scaling) != 0 ||
      boot_plan_set_scaling(plan, "scaling_params2", &binding->scaling2) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->raster_skip >= 0 &&
      boot_plan_set_cmdline_int(plan, "raster_skip", mode->raster_skip) != 0) {
    return ERROR_INVALID_CONFIG;
  }
  if (mode->raster_skip2 >= 0 &&
      boot_plan_set_cmdline_int(plan, "raster_skip2", mode->raster_skip2) !=
          0) {
    return ERROR_INVALID_CONFIG;
  }
  return 0;
}

static int line_is_marker(const char *line, const char *marker) {
  char copy[CONFIG_TXT_LINE_LEN];
  char *text;

  if (copy_checked(copy, sizeof(copy), line) != 0) {
    return 0;
  }
  text = trim(copy);
  return strcmp(text, marker) == 0;
}

static int render_config_file(const struct bmx_boot_plan *plan,
                              const char *input_path,
                              const char *output_path) {
  FILE *input = fopen(input_path, "r");
  FILE *output;
  char line[CONFIG_TXT_LINE_LEN];
  int in_managed_block = 0;
  int found_begin = 0;
  int found_end = 0;

  if (input == NULL) {
    return ERROR_OPEN_INPUT;
  }
  output = fopen(output_path, "w");
  if (output == NULL) {
    fclose(input);
    return ERROR_OPEN_OUTPUT;
  }

  while (fgets(line, sizeof(line), input) != NULL) {
    if (strchr(line, '\n') == NULL && !feof(input)) {
      fclose(input);
      fclose(output);
      return ERROR_RENDER;
    }
    if (line_is_marker(line, BMX_CONFIG_BEGIN_MARKER)) {
      if (in_managed_block || found_begin) {
        fclose(input);
        fclose(output);
        return ERROR_RENDER;
      }
      found_begin = 1;
      in_managed_block = 1;
      if (fprintf(output, "%s\n%s%s\n", BMX_CONFIG_BEGIN_MARKER,
                  plan->config_options, BMX_CONFIG_END_MARKER) < 0) {
        fclose(input);
        fclose(output);
        return ERROR_RENDER;
      }
      continue;
    }
    if (line_is_marker(line, BMX_CONFIG_END_MARKER)) {
      if (!in_managed_block) {
        fclose(input);
        fclose(output);
        return ERROR_RENDER;
      }
      found_end = 1;
      in_managed_block = 0;
      continue;
    }
    if (!in_managed_block) {
      if (fputs(line, output) == EOF) {
        fclose(input);
        fclose(output);
        return ERROR_RENDER;
      }
    }
  }

  if (ferror(input) || ferror(output) || in_managed_block || !found_begin ||
      !found_end) {
    fclose(input);
    fclose(output);
    return ERROR_RENDER;
  }
  if (fclose(input) != 0) {
    fclose(output);
    return ERROR_RENDER;
  }
  if (fclose(output) != 0) {
    return ERROR_RENDER;
  }
  return 0;
}

static int cmdline_key_managed(const struct bmx_boot_plan *plan,
                               const char *key) {
  int i;
  for (i = 0; i < plan->num_managed_cmdline_keys; ++i) {
    if (strcmp(plan->managed_cmdline_keys[i], key) == 0) {
      return 1;
    }
  }
  return 0;
}

static int cmdline_content_line(const char *line) {
  while (*line != '\0' && isspace((unsigned char)*line)) {
    ++line;
  }
  return *line != '\0' && *line != '#';
}

static int render_cmdline(const struct bmx_boot_plan *plan, char *line,
                          size_t line_size) {
  char input[CONFIG_TXT_LINE_LEN];
  char output[CONFIG_TXT_LINE_LEN];
  char *token;

  if (copy_checked(input, sizeof(input), line) != 0) {
    return 1;
  }
  output[0] = '\0';
  token = strtok(input, " \t\r\n");
  while (token != NULL) {
    char *equals = strchr(token, '=');
    int managed;

    if (equals != NULL) {
      *equals = '\0';
    }
    managed = cmdline_key_managed(plan, token);
    if (equals != NULL) {
      *equals = '=';
    }
    if (!managed &&
        append_format(output, sizeof(output), "%s%s",
                      output[0] == '\0' ? "" : " ", token) != 0) {
      return 1;
    }
    token = strtok(NULL, " \t\r\n");
  }
  if (plan->cmdline_options[0] != '\0' &&
      append_format(output, sizeof(output), "%s%s",
                    output[0] == '\0' ? "" : " ",
                    plan->cmdline_options) != 0) {
    return 1;
  }
  if (snprintf(line, line_size, "%s\n", output) >= (int)line_size) {
    return 1;
  }
  return 0;
}

static int render_cmdline_file(const struct bmx_boot_plan *plan,
                               const char *input_path,
                               const char *output_path) {
  FILE *input = fopen(input_path, "r");
  FILE *output;
  char line[CONFIG_TXT_LINE_LEN];
  int replaced = 0;

  if (input == NULL) {
    return ERROR_OPEN_INPUT;
  }
  output = fopen(output_path, "w");
  if (output == NULL) {
    fclose(input);
    return ERROR_OPEN_OUTPUT;
  }

  while (fgets(line, sizeof(line), input) != NULL) {
    if (strchr(line, '\n') == NULL && !feof(input)) {
      fclose(input);
      fclose(output);
      return ERROR_RENDER;
    }
    if (cmdline_content_line(line)) {
      if (replaced || render_cmdline(plan, line, sizeof(line)) != 0) {
        fclose(input);
        fclose(output);
        return ERROR_RENDER;
      }
      replaced = 1;
    }
    if (fputs(line, output) == EOF) {
      fclose(input);
      fclose(output);
      return ERROR_RENDER;
    }
  }
  if (!replaced) {
    if (snprintf(line, sizeof(line), "%s\n", plan->cmdline_options) >=
        (int)sizeof(line)) {
      fclose(input);
      fclose(output);
      return ERROR_RENDER;
    }
    if (fputs(line, output) == EOF) {
      fclose(input);
      fclose(output);
      return ERROR_RENDER;
    }
  }

  if (ferror(input) || ferror(output)) {
    fclose(input);
    fclose(output);
    return ERROR_RENDER;
  }
  if (fclose(input) != 0) {
    fclose(output);
    return ERROR_RENDER;
  }
  if (fclose(output) != 0) {
    return ERROR_RENDER;
  }
  return 0;
}

static int render_kernel_selector_file(const char *kernel_name,
                                       const char *output_path) {
  FILE *output = fopen(output_path, "w");
  int failed;

  if (output == NULL) {
    return ERROR_OPEN_OUTPUT;
  }
  failed = fprintf(output, "%skernel=%s\n", BMX_KERNEL_SELECTOR_HEADER,
                   kernel_name) < 0 ||
           ferror(output);
  if (fclose(output) != 0) {
    failed = 1;
  }
  if (failed) {
    return ERROR_RENDER;
  }
  return 0;
}

/*
 * Circle maps link(2) to a same-volume FatFs rename.  On a POSIX host it
 * creates a hard link, after which unlinking the staged name has the same
 * final result.  The target is deliberately removed first: a power loss in
 * that small window leaves the include missing, so config.txt keeps using the
 * complete fallback kernel instead of observing a partially written selector.
 */
static int replace_with_staged_file(const char *staged_path,
                                    const char *target_path) {
  if ((unlink(target_path) != 0 && errno != ENOENT) ||
      link(staged_path, target_path) != 0) {
    return 1;
  }
  (void)unlink(staged_path);
  return 0;
}

int switch_apply_boot_plan(const struct bmx_boot_plan *plan) {
  char config_path[CONFIG_TXT_LINE_LEN];
  char config_new_path[CONFIG_TXT_LINE_LEN];
  char config_backup_path[CONFIG_TXT_LINE_LEN];
  char cmdline_path[CONFIG_TXT_LINE_LEN];
  char cmdline_new_path[CONFIG_TXT_LINE_LEN];
  char cmdline_backup_path[CONFIG_TXT_LINE_LEN];
  char selector_path[CONFIG_TXT_LINE_LEN];
  char selector_new_path[CONFIG_TXT_LINE_LEN];
  char selector_backup_path[CONFIG_TXT_LINE_LEN];
  char fallback[VALUE_LEN];
  const char *base;
  int pi_model;
  int update_selector;
  int update_cmdline;
  int selector_commit_attempted = 0;
  int status = 0;

  if (plan == NULL) {
    return ERROR_INVALID_CONFIG;
  }
  pi_model = circle_get_model();
  update_selector = plan->update_config && model_uses_release_selector(pi_model);
  if (plan->update_config) {
    if (plan->kernel_name[0] == '\0') {
      return ERROR_INVALID_CONFIG;
    }
    if (update_selector) {
      base = default_kernel_for_pi_model(pi_model);
      if (base == NULL ||
          snprintf(fallback, sizeof(fallback), "%s.c64", base) >=
              (int)sizeof(fallback) ||
          !release_config_options_are_canonical(plan->config_options,
                                                fallback) ||
          parse_machine_kernel_name(plan->kernel_name, base) != 0 ||
          validate_active_machine_selector(pi_model) != 0) {
        return BMC64_SWITCH_ERROR_SELECTOR_INVALID;
      }
    }
    if (!kernel_file_exists(plan->kernel_name)) {
      return BMC64_SWITCH_ERROR_KERNEL_MISSING;
    }
  }
  update_cmdline = plan->num_managed_cmdline_keys > 0 ||
                   plan->cmdline_options[0] != '\0';
  if (!plan->update_config && !update_cmdline) {
    return 0;
  }

  boot_path(config_path, sizeof(config_path), "config.txt");
  boot_path(config_new_path, sizeof(config_new_path), "config.new");
  boot_path(config_backup_path, sizeof(config_backup_path), "config.bak");
  boot_path(cmdline_path, sizeof(cmdline_path), "cmdline.txt");
  boot_path(cmdline_new_path, sizeof(cmdline_new_path), "cmdline.new");
  boot_path(cmdline_backup_path, sizeof(cmdline_backup_path), "cmdline.bak");
  boot_path(selector_path, sizeof(selector_path), BMX_ACTIVE_KERNEL_SELECTOR);
  boot_path(selector_new_path, sizeof(selector_new_path),
            BMX_ACTIVE_KERNEL_SELECTOR ".new");
  boot_path(selector_backup_path, sizeof(selector_backup_path),
            BMX_ACTIVE_KERNEL_SELECTOR ".bak");

  if (plan->update_config) {
    status = render_config_file(plan, config_path, config_new_path);
  }
  if (status == 0 && update_cmdline) {
    status = render_cmdline_file(plan, cmdline_path, cmdline_new_path);
  }
  if (status == 0 && update_selector) {
    status = render_kernel_selector_file(plan->kernel_name, selector_new_path);
  }
  if (status != 0) {
    unlink(config_new_path);
    unlink(cmdline_new_path);
    unlink(selector_new_path);
    return status;
  }

  if ((plan->update_config && copy_file(config_path, config_backup_path)) ||
      (update_cmdline && copy_file(cmdline_path, cmdline_backup_path)) ||
      (update_selector && copy_file(selector_path, selector_backup_path))) {
    unlink(config_new_path);
    unlink(cmdline_new_path);
    unlink(selector_new_path);
    unlink(config_backup_path);
    unlink(cmdline_backup_path);
    unlink(selector_backup_path);
    return ERROR_BACKUP;
  }

  if ((plan->update_config && copy_file(config_new_path, config_path)) ||
      (update_cmdline && copy_file(cmdline_new_path, cmdline_path))) {
    status = ERROR_COMMIT;
  } else if (update_selector) {
    selector_commit_attempted = 1;
    if (replace_with_staged_file(selector_new_path, selector_path) != 0) {
      status = ERROR_COMMIT;
    }
  }
  if (status == ERROR_COMMIT) {
    if (plan->update_config) {
      (void)copy_file(config_backup_path, config_path);
    }
    if (update_cmdline) {
      (void)copy_file(cmdline_backup_path, cmdline_path);
    }
    if (selector_commit_attempted) {
      (void)replace_with_staged_file(selector_backup_path, selector_path);
    }
  }

  unlink(config_new_path);
  unlink(cmdline_new_path);
  unlink(selector_new_path);
  unlink(config_backup_path);
  unlink(cmdline_backup_path);
  unlink(selector_backup_path);
  return status;
}

int switch_read_active_video_mode(char *mode_id, size_t mode_id_size) {
  char path[CONFIG_TXT_LINE_LEN];
  char line[CONFIG_TXT_LINE_LEN];
  FILE *file;

  if (mode_id == NULL || mode_id_size == 0) {
    return 1;
  }
  mode_id[0] = '\0';
  boot_path(path, sizeof(path), "cmdline.txt");
  file = fopen(path, "r");
  if (file == NULL) {
    return 1;
  }

  while (fgets(line, sizeof(line), file) != NULL) {
    char *token;
    if (!cmdline_content_line(line)) {
      continue;
    }
    token = strtok(line, " \t\r\n");
    while (token != NULL) {
      static const char prefix[] = "bmx_video_mode=";
      if (strncmp(token, prefix, sizeof(prefix) - 1) == 0) {
        const char *value = token + sizeof(prefix) - 1;
        int result = !valid_identifier(value) ||
                     copy_checked(mode_id, mode_id_size, value);
        fclose(file);
        return result;
      }
      token = strtok(NULL, " \t\r\n");
    }
    break;
  }
  fclose(file);
  return 1;
}

static unsigned overclock_field_for_key(const char *key) {
  if (strcmp(key, "arm_freq") == 0) {
    return BMX_OVERCLOCK_ARM_FREQ;
  }
  if (strcmp(key, "core_freq") == 0) {
    return BMX_OVERCLOCK_CORE_FREQ;
  }
  if (strcmp(key, "v3d_freq") == 0) {
    return BMX_OVERCLOCK_V3D_FREQ;
  }
  if (strcmp(key, "over_voltage_delta") == 0) {
    return BMX_OVERCLOCK_VOLTAGE_DELTA;
  }
  if (strcmp(key, "temp_limit") == 0) {
    return BMX_OVERCLOCK_TEMP_LIMIT;
  }
  return 0;
}

static int overclock_key_conflicts_with_menu(const char *key) {
  return overclock_field_for_key(key) != 0 || strcmp(key, "arm_boost") == 0 ||
         strcmp(key, "gpu_freq") == 0 || strcmp(key, "over_voltage") == 0 ||
         strcmp(key, "over_voltage_min") == 0 ||
         strcmp(key, "force_turbo") == 0 ||
         strcmp(key, "core_freq_fixed") == 0;
}

static void overclock_set_value(struct bmx_overclock_config *config,
                                unsigned field, int value) {
  switch (field) {
    case BMX_OVERCLOCK_ARM_FREQ:
      config->arm_freq_mhz = value;
      break;
    case BMX_OVERCLOCK_CORE_FREQ:
      config->core_freq_mhz = value;
      break;
    case BMX_OVERCLOCK_V3D_FREQ:
      config->v3d_freq_mhz = value;
      break;
    case BMX_OVERCLOCK_VOLTAGE_DELTA:
      config->over_voltage_delta_uv = value;
      break;
    case BMX_OVERCLOCK_TEMP_LIMIT:
      config->temp_limit_c = value;
      break;
    default:
      break;
  }
}

int switch_read_overclock_config(struct bmx_overclock_config *config) {
  char path[CONFIG_TXT_LINE_LEN];
  char line[CONFIG_TXT_LINE_LEN];
  FILE *file;
  int in_managed_block = 0;
  int found_begin = 0;
  int found_end = 0;
  int conflict = 0;
  int invalid = 0;

  if (config == NULL) {
    return BMX_OVERCLOCK_READ_INVALID;
  }
  memset(config, 0, sizeof(*config));
  boot_path(path, sizeof(path), "config.txt");
  file = fopen(path, "r");
  if (file == NULL) {
    return BMX_OVERCLOCK_READ_INVALID;
  }

  while (fgets(line, sizeof(line), file) != NULL) {
    char *text;
    char *equals;
    char *key;
    char *value_text;
    unsigned field;
    int value;

    if (strchr(line, '\n') == NULL && !feof(file)) {
      invalid = 1;
      break;
    }
    if (line_is_marker(line, BMX_CONFIG_BEGIN_MARKER)) {
      if (in_managed_block || found_begin || found_end) {
        invalid = 1;
        break;
      }
      found_begin = 1;
      in_managed_block = 1;
      continue;
    }
    if (line_is_marker(line, BMX_CONFIG_END_MARKER)) {
      if (!in_managed_block || found_end) {
        invalid = 1;
        break;
      }
      found_end = 1;
      in_managed_block = 0;
      continue;
    }

    text = trim(line);
    if (text[0] == '\0' || text[0] == '#') {
      continue;
    }
    equals = strchr(text, '=');
    if (equals == NULL) {
      continue;
    }
    *equals = '\0';
    key = trim(text);
    value_text = trim(equals + 1);
    field = overclock_field_for_key(key);
    if (!overclock_key_conflicts_with_menu(key)) {
      continue;
    }
    if (!in_managed_block) {
      conflict = 1;
      continue;
    }
    if (field == 0) {
      invalid = 1;
      break;
    }
    if ((config->present & field) != 0 ||
        parse_signed_int(value_text, &value) != 0) {
      invalid = 1;
      break;
    }
    config->present |= field;
    overclock_set_value(config, field, value);
  }

  if (ferror(file)) {
    invalid = 1;
  }
  fclose(file);
  if (invalid || in_managed_block || !found_begin || !found_end ||
      !overclock_config_valid(config, circle_get_model())) {
    memset(config, 0, sizeof(*config));
    return BMX_OVERCLOCK_READ_INVALID;
  }
  if (conflict) {
    memset(config, 0, sizeof(*config));
    return BMX_OVERCLOCK_READ_CONFLICT;
  }
  return BMX_OVERCLOCK_READ_OK;
}

void switch_safe(void) {
  struct bmx_boot_plan plan;
  char kernel_name[VALUE_LEN];

  if (switch_machine_kernel_name(BMC64_MACHINE_CLASS_C64,
                                 BMC64_C64_CORE_X64, circle_get_model(),
                                 kernel_name, sizeof(kernel_name)) != 0 ||
      !kernel_file_exists(kernel_name)) {
    return;
  }

  bmx_boot_plan_init(&plan);
  plan.update_config = 1;
  strcpy(plan.kernel_name, kernel_name);
  if (prepare_machine_cmdline_keys(&plan) != 0 ||
      boot_plan_add_kernel_selection(&plan, circle_get_model(), kernel_name) !=
          0 ||
      boot_plan_add_config_int(&plan, "disable_overscan", 1) != 0 ||
      boot_plan_add_config_int(&plan, "sdtv_mode", 18) != 0 ||
      boot_plan_add_config_int(&plan, "hdmi_group", 1) != 0 ||
      boot_plan_add_config_int(&plan, "hdmi_mode", 19) != 0 ||
      (circle_get_model() == 4 &&
       (boot_plan_add_config_int(&plan, "hdmi_group:0", 1) != 0 ||
        boot_plan_add_config_int(&plan, "hdmi_mode:0", 19) != 0)) ||
      bmx_boot_plan_set_cmdline_option(&plan, "bmx_video_mode",
                                       "pal-hdmi-720p") != 0 ||
      bmx_boot_plan_set_cmdline_option(&plan, "machine_timing",
                                       "pal-hdmi") != 0 ||
      bmx_boot_plan_set_cmdline_option(&plan, "hdmi_group", "1") != 0 ||
      bmx_boot_plan_set_cmdline_option(&plan, "hdmi_mode", "19") != 0 ||
      bmx_boot_plan_set_cmdline_option(&plan, "scaling_params",
                                       "384,240,1152,720") != 0) {
    return;
  }
  switch_apply_boot_plan(&plan);
}
