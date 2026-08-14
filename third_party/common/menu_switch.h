/*
 * menu_switch.h
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

#ifndef RASPI_MENU_SWITCH_H
#define RASPI_MENU_SWITCH_H

#include <stddef.h>

#include "emux_api.h"

#define CONFIG_TXT_LINE_LEN 1024
#define KEY_LEN 32
#define VALUE_LEN 192

#define BMX_MODE_ID_LEN 48
#define BMX_LABEL_LEN 64
#define BMX_MACHINE_ID_LEN 16
#define BMX_SCALING_ID_LEN 32
#define BMX_MACHINE_TIMING_LEN 24
#define BMX_AUDIO_OUT_LEN 16
#define BMX_TIMINGS_LEN 192

#define BMX_MAX_VIDEO_MODES 32
#define BMX_MAX_SCALING_SETS 16
#define BMX_MAX_SCALING_ENTRIES 32
#define BMX_MAX_MACHINES 8
#define BMX_MAX_MACHINE_MODES 16
#define BMX_MAX_MANAGED_CMDLINE_KEYS 64
#define BMX_CONFIG_BLOCK_LEN 2048

#define BMX_CONFIG_BEGIN_MARKER "# BEGIN BMX MANAGED"
#define BMX_CONFIG_END_MARKER "# END BMX MANAGED"

#define BMC64_SWITCH_ERROR_KERNEL_MISSING 1024
#define BMC64_SWITCH_ERROR_SELECTOR_INVALID 2048

#define BMX_OVERCLOCK_ARM_FREQ (1U << 0)
#define BMX_OVERCLOCK_CORE_FREQ (1U << 1)
#define BMX_OVERCLOCK_V3D_FREQ (1U << 2)
#define BMX_OVERCLOCK_VOLTAGE_DELTA (1U << 3)
#define BMX_OVERCLOCK_TEMP_LIMIT (1U << 4)
#define BMX_OVERCLOCK_ALL (BMX_OVERCLOCK_ARM_FREQ | \
                           BMX_OVERCLOCK_CORE_FREQ | \
                           BMX_OVERCLOCK_V3D_FREQ | \
                           BMX_OVERCLOCK_VOLTAGE_DELTA | \
                           BMX_OVERCLOCK_TEMP_LIMIT)

typedef enum {
  BMX_OVERCLOCK_READ_OK = 0,
  BMX_OVERCLOCK_READ_INVALID = 1,
  BMX_OVERCLOCK_READ_CONFLICT = 2,
} BMXOverclockReadStatus;

typedef enum {
  BMC64_VIDEO_STANDARD_UNKNOWN,
  BMC64_VIDEO_STANDARD_NTSC,
  BMC64_VIDEO_STANDARD_PAL,
} BMC64VideoStandard;

typedef enum {
  BMC64_VIDEO_OUT_UNKNOWN,
  BMC64_VIDEO_OUT_HDMI,
  BMC64_VIDEO_OUT_COMPOSITE,
  BMC64_VIDEO_OUT_DPI,
} BMC64VideoOut;

struct bmx_scaling_params {
  int present;
  int framebuffer_width;
  int framebuffer_height;
  int scaled_width;
  int scaled_height;
};

struct bmx_video_mode {
  char id[BMX_MODE_ID_LEN];
  char label[BMX_LABEL_LEN];
  BMC64VideoStandard standard;
  BMC64VideoOut output;
  int experimental;

  int disable_overscan;
  int sdtv_mode;
  int hdmi_group;
  int hdmi_mode;
  char hdmi_timings[BMX_TIMINGS_LEN];
  char hdmi_cvt[BMX_TIMINGS_LEN];

  int enable_dpi_lcd;
  int display_default_lcd;
  int dpi_group;
  int dpi_mode;
  char dpi_timings[BMX_TIMINGS_LEN];
  char dpi_output_format[VALUE_LEN];

  char machine_timing[BMX_MACHINE_TIMING_LEN];
  unsigned long cycles_per_second;
  int have_cycles_per_second;
  char audio_out[BMX_AUDIO_OUT_LEN];
  int enable_dpi;
  int framebuffer_width;
  int framebuffer_height;
  int framebuffer_depth;
  int raster_skip;
  int raster_skip2;
};

struct bmx_scaling_entry {
  char mode_id[BMX_MODE_ID_LEN];
  struct bmx_scaling_params params;
};

struct bmx_scaling_set {
  char id[BMX_SCALING_ID_LEN];
  int num_entries;
  struct bmx_scaling_entry entries[BMX_MAX_SCALING_ENTRIES];
};

struct bmx_machine_mode {
  char mode_id[BMX_MODE_ID_LEN];
  const struct bmx_video_mode *video_mode;
  struct bmx_scaling_params scaling;
  struct bmx_scaling_params scaling2;
};

struct bmx_machine {
  char id[BMX_MACHINE_ID_LEN];
  BMC64MachineClass machine_class;
  char default_mode_id[BMX_MODE_ID_LEN];
  char scaling_id[BMX_SCALING_ID_LEN];
  char scaling2_id[BMX_SCALING_ID_LEN];
  int num_modes;
  struct bmx_machine_mode modes[BMX_MAX_MACHINE_MODES];
};

struct bmx_machine_config {
  int num_video_modes;
  int num_scaling_sets;
  int num_machines;
  struct bmx_video_mode video_modes[BMX_MAX_VIDEO_MODES];
  struct bmx_scaling_set scaling_sets[BMX_MAX_SCALING_SETS];
  struct bmx_machine machines[BMX_MAX_MACHINES];
};

struct bmx_boot_plan {
  int update_config;
  char kernel_name[VALUE_LEN];
  char config_options[BMX_CONFIG_BLOCK_LEN];
  char cmdline_options[CONFIG_TXT_LINE_LEN];
  int num_managed_cmdline_keys;
  char managed_cmdline_keys[BMX_MAX_MANAGED_CMDLINE_KEYS][KEY_LEN];
};

struct bmx_overclock_config {
  unsigned present;
  int arm_freq_mhz;
  int core_freq_mhz;
  int v3d_freq_mhz;
  int over_voltage_delta_uv;
  int temp_limit_c;
};

int load_machine_config(struct bmx_machine_config **config);
void free_machine_config(struct bmx_machine_config *config);

const struct bmx_machine *bmx_machine_by_class(
    const struct bmx_machine_config *config, BMC64MachineClass machine_class);
const struct bmx_machine_mode *bmx_machine_mode_by_id(
    const struct bmx_machine *machine, const char *mode_id);
const struct bmx_machine_mode *bmx_machine_default_mode(
    const struct bmx_machine *machine);
const char *bmx_video_standard_name(BMC64VideoStandard standard);
const char *bmx_video_output_name(BMC64VideoOut output);

int switch_read_active_video_mode(char *mode_id, size_t mode_id_size);
int switch_read_overclock_config(struct bmx_overclock_config *config);

void bmx_boot_plan_init(struct bmx_boot_plan *plan);
int bmx_boot_plan_manage_cmdline_key(struct bmx_boot_plan *plan,
                                     const char *key);
int bmx_boot_plan_set_cmdline_option(struct bmx_boot_plan *plan,
                                     const char *key, const char *value);
int bmx_boot_plan_set_developer_mode(struct bmx_boot_plan *plan, int enabled);
int bmx_boot_plan_set_developer_password(struct bmx_boot_plan *plan,
                                         const char *password);
int bmx_boot_plan_set_developer_log_buffer_kb(struct bmx_boot_plan *plan,
                                              unsigned buffer_kb);
int bmx_boot_plan_set_api_mode(struct bmx_boot_plan *plan, int enabled);
int bmx_boot_plan_set_api_password(struct bmx_boot_plan *plan,
                                   const char *password);
int bmx_boot_plan_add_overclock(struct bmx_boot_plan *plan,
                               const struct bmx_overclock_config *config);

int switch_check_selection(const struct bmx_machine *machine,
                           BMC64C64Core c64_core);
int switch_build_boot_plan(const struct bmx_machine *machine,
                           const struct bmx_machine_mode *mode,
                           BMC64C64Core c64_core,
                           struct bmx_boot_plan *plan);
int switch_apply_boot_plan(const struct bmx_boot_plan *plan);

int switch_machine_kernel_name(BMC64MachineClass machine_class,
                               BMC64C64Core c64_core, int pi_model,
                               char *name, size_t name_size);
int switch_c64_core_kernel_name(BMC64C64Core core, int pi_model,
                                char *name, size_t name_size);

/* Apply the built-in PAL HDMI safe mode and select x64. */
void switch_safe(void);

#endif
