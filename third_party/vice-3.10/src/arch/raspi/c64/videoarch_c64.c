/*
 * videoarch_c64.c
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

#include "videoarch_c64.h"

#include "emux_api.h"
#include "resources.h"
#include "c64/c64.h"
#include "c64/c64mem.h"

static unsigned int vice_color_palette[] = {
    0x00, 0x00, 0x00, 0xFD, 0xFE, 0xFC, 0xBE, 0x1A, 0x24, 0x30, 0xE6, 0xC6,
    0xB4, 0x1A, 0xE2, 0x1F, 0xD2, 0x1E, 0x21, 0x1B, 0xAE, 0xDF, 0xF6, 0x0A,
    0xB8, 0x41, 0x04, 0x6A, 0x33, 0x04, 0xFE, 0x4A, 0x57, 0x42, 0x45, 0x40,
    0x70, 0x74, 0x6F, 0x59, 0xFE, 0x59, 0x5F, 0x53, 0xFE, 0xA4, 0xA7, 0xA2,
};

void set_refresh_rate(struct video_canvas_s *canvas) {
  if (is_ntsc()) {
    canvas->refreshrate = C64_NTSC_RFSH_PER_SEC;
  } else {
    canvas->refreshrate = C64_PAL_RFSH_PER_SEC;
  }
}

void set_video_font(void) {
  raw_video_font = mem_chargen_rom;
}

unsigned int *raspi_get_palette(int display, int index) {
  (void)display;
  (void)index;
  return vice_color_palette;
}

void set_canvas_size(int index, int* w, int *h, int *gw, int *gh) {
  *w = 384;
  *h = 272;
  *gw = 40*8;
  *gh = 25*8;
}

void set_canvas_borders(int index, int *w, int *h) {
  if (is_ntsc()) {
     *w = 32;
     *h = 23;
  } else {
     *w = 32;
     *h = 36;
  }
}

void set_filter(int display, int value) {
  resources_set_int("VICIIFilter", value);
}

int get_filter(int display) {
  int value;
  resources_get_int("VICIIFilter", &value);
  return value;
}
