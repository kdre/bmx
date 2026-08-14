/*
 * videoarch_pet.c
 *
 * Written by
 *  Randy Rossi <randy.rossi@gmail.com>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "videoarch_pet.h"

#include <stdlib.h>
#include <string.h>

#include "emux_api.h"
#include "font.h"
#include "pet/pet.h"
#include "pet/petmem.h"
#include "pet/petmodel.h"
#include "resources.h"

static unsigned int green_color_palette[] = {
    0x00, 0x00, 0x00, 0x41, 0xFF, 0x00,
};

void set_refresh_rate(struct video_canvas_s *canvas) {
  if (is_ntsc()) {
    canvas->refreshrate = PET_NTSC_RFSH_PER_SEC;
  } else {
    canvas->refreshrate = PET_PAL_RFSH_PER_SEC;
  }
}

void set_video_font(void) {
  int i;
  size_t bytes_read = 0;
  FILE* fp = NULL;
  static uint8_t chargen[4096];
  static const char *chargen_paths[] = {
    "PET/CHARGEN",
    "PET/characters-2.901447-10.bin",
    "PET/characters-1.901447-08.bin",
    "PET/chargen.de",
    "PET/characters.901640-01.bin",
    NULL
  };

  for (i = 0; chargen_paths[i] != NULL && fp == NULL; ++i) {
    fp = fopen(chargen_paths[i], "r");
  }

  if (fp != NULL) {
    bytes_read = fread(chargen, 1, sizeof(chargen), fp);
    fclose(fp);
  }

  if (bytes_read == 2048) {
    memcpy(chargen + 2048, chargen, 2048);
    bytes_read = sizeof(chargen);
  }

  if (bytes_read >= 2048) {
    raw_video_font = chargen;
  } else {
    raw_video_font = (uint8_t *)&font8x8;
  }
}

unsigned int *raspi_get_palette(int display, int index) {
  (void)display;
  (void)index;
  return green_color_palette;
}

void set_canvas_size(int index, int *w, int *h, int *gw, int *gh) {
  int size;
  resources_get_int("VideoSize", &size);
  if (size == 40) {
    *w = 384;
    *h = 272;
    *gw = 40*8;
    *gh = 240;
    return;
  }
  *w = 704;
  *h = 272;
  *gw = 80*8;
  *gh = 240;
}

void set_canvas_borders(int index, int *w, int *h) {
  *w = 32;
  *h = 8;
}

void set_filter(int display, int value) {
  resources_set_int("CrtcFilter", value);
}

int get_filter(int display) {
  int value;
  resources_get_int("CrtcFilter", &value);
  return value;
}
