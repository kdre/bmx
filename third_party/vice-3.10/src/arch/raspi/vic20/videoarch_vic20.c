/*
 * videoarch_vic20.c
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

#include "videoarch_vic20.h"

#include "emux_api.h"
#include "resources.h"
#include "vic20/vic20.h"
#include "vic20/vic20mem.h"
#include "vic20/vic20memrom.h"

static unsigned int vice_color_palette[] = {
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0xF0, 0xF0,
    0x60, 0x00, 0x60, 0x00, 0xA0, 0x00, 0x00, 0x00, 0xF0, 0xD0, 0xD0, 0x00,
    0xC0, 0xA0, 0x00, 0xFF, 0xA0, 0x00, 0xF0, 0x80, 0x80, 0x00, 0xFF, 0xFF,
    0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0xA0, 0xFF, 0xFF, 0xFF, 0x00,
};

void set_refresh_rate(struct video_canvas_s *canvas) {
  if (is_ntsc()) {
    canvas->refreshrate = VIC20_NTSC_RFSH_PER_SEC;
  } else {
    canvas->refreshrate = VIC20_PAL_RFSH_PER_SEC;
  }
}

void set_video_font(void) {
  int i;
  video_font = vic20memrom_chargen_rom + 0x800;
  raw_video_font = vic20memrom_chargen_rom;
  for (i = 0; i < 256; ++i) {
    video_font_translate[i] = 8 * ascii_to_petscii[i];
  }
}

unsigned int *raspi_get_palette(int display, int index) {
  (void)display;
  (void)index;
  return vice_color_palette;
}

void set_canvas_size(int index, int *w, int *h, int *gw, int *gh) {
  *w = 448;
  *h = 284;
  *gw = 22*8*2;
  *gh = 23*8;
}

void set_canvas_borders(int index, int *w, int *h) {
  if (is_ntsc()) {
      *w = 84;
      *h = 48;
  } else {
      *w = 108;
      *h = 50;
  }
}

void set_filter(int display, int value) {
  resources_set_int("VICFilter", value);
}

int get_filter(int display) {
  int value;
  resources_get_int("VICFilter", &value);
  return value;
}
