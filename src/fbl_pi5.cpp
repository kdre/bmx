//
// fbl_pi5.cpp
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fbl.h"
#include "pi5kms/framebuffer_reuse.h"
#include "pi5kms/pi5_kms.h"
#include "third_party/common/circle.h"
#include "viceoptions.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "third_party/common/circle.h"
}

#ifndef ALIGN_UP
#define ALIGN_UP(x,y)  ((x + (y)-1) & ~((y)-1))
#endif

#define RGB565(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3)
#define ARGB(a,r,g,b) ((((uint32_t)(uint8_t)(a)) << 24) | \
                       (((uint32_t)(uint8_t)(r)) << 16) | \
                       (((uint32_t)(uint8_t)(g)) << 8) | \
                       ((uint32_t)(uint8_t)(b)))

#define PI5_FRAMEBUFFER_DEFAULT_DEPTH 16

namespace {

FrameBufferLayer *g_layers[FB_NUM_LAYERS];
uint8_t *g_compose_pixels = nullptr;
unsigned g_compose_pitch_bytes = 0;
unsigned g_framebuffer_bytes_per_pixel = 2;
int g_effective_width = 0;
int g_effective_height = 0;
bool g_kms_active = false;
bool g_interpolation_enabled = false;
constexpr unsigned kKmsBufferCount = 2;
pi5kms::Framebuffer g_kms_framebuffers[kKmsBufferCount] = {};
pi5kms::Framebuffer g_kms_hwscale_framebuffers[FB_NUM_LAYERS][kKmsBufferCount] = {};
bool g_kms_hwscale_framebuffer_valid[FB_NUM_LAYERS][kKmsBufferCount] = {};
unsigned g_kms_front_buffer_index = 0;

static uint16_t default_pal_565[256] = {
  RGB565(0x00, 0x00, 0x00),
  RGB565(0xFF, 0xFF, 0xFF),
  RGB565(0xFF, 0x00, 0x00),
  RGB565(0x70, 0xa4, 0xb2),
  RGB565(0x6f, 0x3d, 0x86),
  RGB565(0x58, 0x8d, 0x43),
  RGB565(0x35, 0x28, 0x79),
  RGB565(0xb8, 0xc7, 0x6f),
  RGB565(0x6f, 0x4f, 0x25),
  RGB565(0x43, 0x39, 0x00),
  RGB565(0x9a, 0x67, 0x59),
  RGB565(0x44, 0x44, 0x44),
  RGB565(0x6c, 0x6c, 0x6c),
  RGB565(0x9a, 0xd2, 0x84),
  RGB565(0x6c, 0x5e, 0xb5),
  RGB565(0x95, 0x95, 0x95),
};

static uint32_t default_pal_argb[256] = {
  ARGB(0xFF, 0x00, 0x00, 0x00),
  ARGB(0xFF, 0xFF, 0xFF, 0xFF),
  ARGB(0xFF, 0xFF, 0x00, 0x00),
  ARGB(0xFF, 0x70, 0xa4, 0xb2),
  ARGB(0xFF, 0x6f, 0x3d, 0x86),
  ARGB(0xFF, 0x58, 0x8d, 0x43),
  ARGB(0xFF, 0x35, 0x28, 0x79),
  ARGB(0xFF, 0xb8, 0xc7, 0x6f),
  ARGB(0xFF, 0x6f, 0x4f, 0x25),
  ARGB(0xFF, 0x43, 0x39, 0x00),
  ARGB(0xFF, 0x9a, 0x67, 0x59),
  ARGB(0xFF, 0x44, 0x44, 0x44),
  ARGB(0xFF, 0x6c, 0x6c, 0x6c),
  ARGB(0xFF, 0x9a, 0xd2, 0x84),
  ARGB(0xFF, 0x6c, 0x5e, 0xb5),
  ARGB(0xFF, 0x95, 0x95, 0x95),
  ARGB(0x00, 0x00, 0x00, 0x00),
};

int min_int(int a, int b) {
  return a < b ? a : b;
}

int max_int(int a, int b) {
  return a > b ? a : b;
}

void fit_to_available_preserving_aspect(int *width, int *height,
                                        int avail_width, int avail_height) {
  if (*width <= 0 || *height <= 0 ||
      avail_width <= 0 || avail_height <= 0) {
    return;
  }

  int64_t height_for_full_width =
      ((int64_t) avail_width * (int64_t) *height) / (int64_t) *width;
  if (height_for_full_width <= avail_height) {
    *height = (int) height_for_full_width;
    *width = avail_width;
  } else {
    *width = (int) (((int64_t) avail_height * (int64_t) *width) /
                    (int64_t) *height);
    *height = avail_height;
  }
}

unsigned sanitize_framebuffer_depth(unsigned depth) {
  if (depth == 32) {
    return 32;
  }
  return PI5_FRAMEBUFFER_DEFAULT_DEPTH;
}

unsigned bytes_per_pixel_from_depth(unsigned depth) {
  return sanitize_framebuffer_depth(depth) / 8;
}

unsigned framebuffer_bytes_per_pixel(CBcmFrameBuffer *screen,
                                     unsigned requested_depth) {
  unsigned width = screen->GetWidth();
  unsigned pitch = screen->GetPitch();
  if (width != 0 && pitch != 0) {
    unsigned pitch_bytes_per_pixel = pitch / width;
    if (pitch_bytes_per_pixel == 2 || pitch_bytes_per_pixel == 4) {
      return pitch_bytes_per_pixel;
    }
  }

  unsigned depth_bytes_per_pixel = screen->GetDepth() / 8;
  if (depth_bytes_per_pixel == 2 || depth_bytes_per_pixel == 4) {
    return depth_bytes_per_pixel;
  }

  return bytes_per_pixel_from_depth(requested_depth);
}

uint16_t argb_to_rgb565(uint32_t argb) {
  uint8_t r = (uint8_t) (argb >> 16);
  uint8_t g = (uint8_t) (argb >> 8);
  uint8_t b = (uint8_t) argb;

  return ((uint16_t) (r >> 3) << 11) |
         ((uint16_t) (g >> 2) << 5) |
         (uint16_t) (b >> 3);
}

uint32_t rgb565_to_argb8888(uint16_t rgb) {
  uint32_t r = (uint32_t) (rgb >> 11) & 0x1F;
  uint32_t g = (uint32_t) (rgb >> 5) & 0x3F;
  uint32_t b = (uint32_t) rgb & 0x1F;

  r = (r << 3) | (r >> 2);
  g = (g << 2) | (g >> 4);
  b = (b << 3) | (b >> 2);

  return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline unsigned lerp_channel(unsigned a, unsigned b, unsigned frac) {
  return ((a * (256 - frac)) + (b * frac) + 128) >> 8;
}

static inline unsigned argb_a(uint32_t argb) {
  return (argb >> 24) & 0xFF;
}

static inline unsigned argb_r(uint32_t argb) {
  return (argb >> 16) & 0xFF;
}

static inline unsigned argb_g(uint32_t argb) {
  return (argb >> 8) & 0xFF;
}

static inline unsigned argb_b(uint32_t argb) {
  return argb & 0xFF;
}

struct Rgb565Channels {
  unsigned r;
  unsigned g;
  unsigned b;
};

static inline Rgb565Channels unpack_rgb565(uint16_t rgb) {
  return {
    (unsigned) (rgb >> 11) & 0x1F,
    (unsigned) (rgb >> 5) & 0x3F,
    (unsigned) rgb & 0x1F
  };
}

uint16_t bilinear_rgb565(uint16_t c00,
                         uint16_t c10,
                         uint16_t c01,
                         uint16_t c11,
                         unsigned frac_x,
                         unsigned frac_y) {
  Rgb565Channels p00 = unpack_rgb565(c00);
  Rgb565Channels p10 = unpack_rgb565(c10);
  Rgb565Channels p01 = unpack_rgb565(c01);
  Rgb565Channels p11 = unpack_rgb565(c11);

  unsigned r0 = lerp_channel(p00.r, p10.r, frac_x);
  unsigned g0 = lerp_channel(p00.g, p10.g, frac_x);
  unsigned b0 = lerp_channel(p00.b, p10.b, frac_x);
  unsigned r1 = lerp_channel(p01.r, p11.r, frac_x);
  unsigned g1 = lerp_channel(p01.g, p11.g, frac_x);
  unsigned b1 = lerp_channel(p01.b, p11.b, frac_x);

  return (uint16_t) ((lerp_channel(r0, r1, frac_y) << 11) |
                     (lerp_channel(g0, g1, frac_y) << 5) |
                     lerp_channel(b0, b1, frac_y));
}

uint32_t bilinear_argb8888(uint32_t c00,
                           uint32_t c10,
                           uint32_t c01,
                           uint32_t c11,
                           unsigned frac_x,
                           unsigned frac_y) {
  unsigned a0 = lerp_channel(argb_a(c00), argb_a(c10), frac_x);
  unsigned r0 = lerp_channel(argb_r(c00), argb_r(c10), frac_x);
  unsigned g0 = lerp_channel(argb_g(c00), argb_g(c10), frac_x);
  unsigned b0 = lerp_channel(argb_b(c00), argb_b(c10), frac_x);
  unsigned a1 = lerp_channel(argb_a(c01), argb_a(c11), frac_x);
  unsigned r1 = lerp_channel(argb_r(c01), argb_r(c11), frac_x);
  unsigned g1 = lerp_channel(argb_g(c01), argb_g(c11), frac_x);
  unsigned b1 = lerp_channel(argb_b(c01), argb_b(c11), frac_x);

  unsigned a = lerp_channel(a0, a1, frac_y);
  unsigned r = lerp_channel(r0, r1, frac_y);
  unsigned g = lerp_channel(g0, g1, frac_y);
  unsigned b = lerp_channel(b0, b1, frac_y);

  return (a << 24) | (r << 16) | (g << 8) | b;
}

void write_rgb565_pixel(uint8_t *row, int x, uint16_t rgb) {
  if (g_framebuffer_bytes_per_pixel == 2) {
    ((uint16_t *) row)[x] = rgb;
  } else {
    ((uint32_t *) row)[x] = rgb565_to_argb8888(rgb);
  }
}

void write_argb_pixel(uint8_t *row, int x, uint32_t argb) {
  if (g_framebuffer_bytes_per_pixel == 2) {
    ((uint16_t *) row)[x] = argb_to_rgb565(argb);
  } else {
    ((uint32_t *) row)[x] = argb;
  }
}

uint32_t read_argb_pixel(const uint8_t *row, int x) {
  if (g_framebuffer_bytes_per_pixel == 2) {
    return rgb565_to_argb8888(((const uint16_t *) row)[x]);
  }
  return ((const uint32_t *) row)[x];
}

static inline unsigned alpha_blend_channel(unsigned src,
                                           unsigned dst,
                                           unsigned alpha) {
  return (src * alpha + dst * (255 - alpha) + 127) / 255;
}

void blend_argb_pixel(uint8_t *row, int x, uint32_t argb) {
  unsigned alpha = argb_a(argb);
  if (alpha == 0) {
    return;
  }
  if (alpha == 255) {
    write_argb_pixel(row, x, argb);
    return;
  }

  uint32_t dst = read_argb_pixel(row, x);
  unsigned r = alpha_blend_channel(argb_r(argb), argb_r(dst), alpha);
  unsigned g = alpha_blend_channel(argb_g(argb), argb_g(dst), alpha);
  unsigned b = alpha_blend_channel(argb_b(argb), argb_b(dst), alpha);
  write_argb_pixel(row, x, 0xFF000000u | (r << 16) | (g << 8) | b);
}

int effective_screen_width(CBcmFrameBuffer *screen,
                           unsigned bytes_per_pixel) {
  int width = (int) screen->GetWidth();
  if (screen->GetPitch() != 0 && bytes_per_pixel != 0) {
    width = min_int(width, (int) (screen->GetPitch() / bytes_per_pixel));
  }
  return width;
}

int effective_screen_height(CBcmFrameBuffer *screen) {
  int height = (int) screen->GetHeight();
  if (screen->GetPitch() != 0 && screen->GetSize() != 0) {
    height = min_int(height, (int) (screen->GetSize() / screen->GetPitch()));
  }
  return height;
}

void sort_layers(FrameBufferLayer **layers, unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    for (unsigned j = i + 1; j < count; j++) {
      if (layers[j]->GetLayer() < layers[i]->GetLayer()) {
        FrameBufferLayer *tmp = layers[i];
        layers[i] = layers[j];
        layers[j] = tmp;
      }
    }
  }
}

unsigned kms_back_buffer_index() {
  return g_kms_front_buffer_index ^ 1U;
}

pi5kms::Plane make_kms_framebuffer_plane(const pi5kms::Framebuffer &fb) {
  pi5kms::Plane plane = {
    (u32)(uintptr)fb.pixels,
    fb.pitch,
    fb.width,
    fb.height,
    fb.depth,
    pi5kms::kPixelFormatRgb565,
    pi5kms::kScaleFilterNearest,
    {0, 0, fb.width, fb.height},
    {0, 0, fb.width, fb.height}
  };

  return plane;
}

bool present_kms_framebuffer(unsigned buffer_index, bool wait_for_vblank) {
  if (buffer_index >= kKmsBufferCount) {
    return false;
  }

  pi5kms::Framebuffer &fb = g_kms_framebuffers[buffer_index];
  pi5kms::Plane plane = make_kms_framebuffer_plane(fb);
  if (!pi5kms::PresentScanout(&plane, 1, fb.width, fb.height,
                              wait_for_vblank)) {
    return false;
  }

  g_kms_front_buffer_index = buffer_index;
  return true;
}

void destroy_kms_scanout_framebuffers() {
  for (unsigned i = 0; i < kKmsBufferCount; ++i) {
    pi5kms::DestroyFramebuffer(&g_kms_framebuffers[i]);
  }
  g_kms_front_buffer_index = 0;
}

bool ensure_hwscale_framebuffer(unsigned slot, unsigned buffer_index,
                                u32 width, u32 height,
                                u32 depth, bool *recreated) {
  if (slot >= FB_NUM_LAYERS || buffer_index >= kKmsBufferCount) {
    return false;
  }

  if (recreated != nullptr) {
    *recreated = false;
  }

  pi5kms::Framebuffer *fb = &g_kms_hwscale_framebuffers[slot][buffer_index];
  if (fb->pixels != nullptr &&
      pi5kms::CanReuseFramebuffer(fb->width, fb->height, fb->depth,
                                  width, height, depth)) {
    return true;
  }

  u32 allocation_width = width;
  u32 allocation_height = height;
  if (fb->pixels != nullptr && fb->depth == depth) {
    // Never trade one existing dimension for another. Keeping capacity
    // monotonic prevents alternating aspect ratios from recreating large DMA
    // blocks that Circle cannot return to its bucket allocator.
    allocation_width = pi5kms::ExpandedFramebufferDimension(
        fb->width, allocation_width);
    allocation_height = pi5kms::ExpandedFramebufferDimension(
        fb->height, allocation_height);
  }

  if (fb->pixels != nullptr) {
    pi5kms::DestroyFramebuffer(fb);
    g_kms_hwscale_framebuffer_valid[slot][buffer_index] = false;
  }

  if (!pi5kms::CreateFramebuffer(allocation_width, allocation_height,
                                 depth, fb)) {
    return false;
  }
  g_kms_hwscale_framebuffer_valid[slot][buffer_index] = false;
  if (recreated != nullptr) {
    *recreated = true;
  }
  return true;
}

} // namespace

bool FrameBufferLayer::CopyLayerSourceToHwscaleFramebuffer(FrameBufferLayer *layer,
                                                           unsigned buffer_index) {
  if (layer == nullptr || layer->layer_ < 0 ||
      layer->layer_ >= FB_NUM_LAYERS ||
      buffer_index >= kKmsBufferCount) {
    return false;
  }

  const unsigned slot = (unsigned)layer->layer_;
  const u32 depth = layer->transparency_ ? 32U : 16U;
  const u32 hw_width = (u32)layer->src_w_;
  const u32 hw_height = (u32)layer->src_h_;
  pi5kms::Framebuffer *fb = &g_kms_hwscale_framebuffers[slot][buffer_index];
  bool recreated = false;
  if (!ensure_hwscale_framebuffer(slot, buffer_index,
                                  hw_width, hw_height, depth,
                                  &recreated)) {
    return false;
  }
  if (!recreated && !layer->dirty_ &&
      g_kms_hwscale_framebuffer_valid[slot][buffer_index]) {
    return true;
  }

  if (layer->transparency_) {
    for (int y = 0; y < layer->src_h_; ++y) {
      uint32_t *dst_row =
          (uint32_t *)(fb->pixels + (u32)y * fb->pitch);
      const int src_y = layer->src_y_ + y;
      const uint8_t *src_row = layer->pixels_ + src_y * layer->fb_pitch_;
      for (int x = 0; x < layer->src_w_; ++x) {
        dst_row[x] = layer->pal_argb_[src_row[layer->src_x_ + x]];
      }
    }

    pi5kms::FlushFramebuffer(*fb);
    g_kms_hwscale_framebuffer_valid[slot][buffer_index] = true;
    layer->dirty_ = false;
    return true;
  }

  for (int y = 0; y < layer->src_h_; ++y) {
    const int src_y = layer->src_y_ + y;

    if (layer->pixelmode_ == 0) {
      uint16_t *dst_row =
          (uint16_t *)(fb->pixels + (u32)y * fb->pitch);
      const uint8_t *src_row = layer->pixels_ + src_y * layer->fb_pitch_;
      for (int x = 0; x < layer->src_w_; ++x) {
        dst_row[x] = layer->pal_565_[src_row[layer->src_x_ + x]];
      }
    } else {
      uint16_t *dst_row =
          (uint16_t *)(fb->pixels + (u32)y * fb->pitch);
      const uint16_t *src_row =
          (const uint16_t *)(layer->pixels_ + src_y * layer->fb_pitch_);
      memcpy(dst_row, src_row + layer->src_x_,
             (size_t)layer->src_w_ * sizeof(uint16_t));
    }
  }

  pi5kms::FlushFramebuffer(*fb);
  g_kms_hwscale_framebuffer_valid[slot][buffer_index] = true;
  layer->dirty_ = false;
  return true;
}

bool FrameBufferLayer::CanUseKmsDirectScanout(FrameBufferLayer *layer,
                                              int screen_w,
                                              int screen_h) {
  if (!g_kms_active || layer == nullptr) {
    return false;
  }
  if (layer->transparency_ && layer->pixelmode_ != 0) {
    return false;
  }
  if (!layer->transparency_ &&
      layer->pixelmode_ != 0 && layer->pixelmode_ != 1) {
    return false;
  }
  if (layer->src_w_ <= 0 || layer->src_h_ <= 0 ||
      layer->dst_w_ <= 0 || layer->dst_h_ <= 0) {
    return false;
  }
  if (layer->src_x_ < 0 || layer->src_y_ < 0 ||
      layer->src_x_ + layer->src_w_ > layer->fb_width_ ||
      layer->src_y_ + layer->src_h_ > layer->fb_height_) {
    return false;
  }
  if (layer->dst_x_ < 0 || layer->dst_y_ < 0 ||
      layer->dst_x_ + layer->dst_w_ > screen_w ||
      layer->dst_y_ + layer->dst_h_ > screen_h) {
    return false;
  }
  return true;
}

bool FrameBufferLayer::TryKmsDirectScanout(FrameBufferLayer **layers,
                                           unsigned layer_count,
                                           int screen_w,
                                           int screen_h,
                                           bool wait_for_vblank) {
  if (layer_count == 0 || layer_count > FB_NUM_LAYERS) {
    return false;
  }

  const unsigned buffer_index = kms_back_buffer_index();
  pi5kms::Plane planes[FB_NUM_LAYERS];
  for (unsigned i = 0; i < layer_count; ++i) {
    FrameBufferLayer *layer = layers[i];
    if (!CanUseKmsDirectScanout(layer, screen_w, screen_h) ||
        !CopyLayerSourceToHwscaleFramebuffer(layer, buffer_index)) {
      return false;
    }

    const unsigned slot = (unsigned)layer->layer_;
    const pi5kms::Framebuffer &fb =
        g_kms_hwscale_framebuffers[slot][buffer_index];
    const pi5kms::PixelFormat format =
        layer->transparency_ ? pi5kms::kPixelFormatArgb8888
                             : pi5kms::kPixelFormatRgb565;
    planes[i] = {
      (u32)(uintptr)fb.pixels,
      fb.pitch,
      fb.width,
      fb.height,
      fb.depth,
      format,
      g_interpolation_enabled ? pi5kms::kScaleFilterMitchell
                              : pi5kms::kScaleFilterNearest,
      {0, 0, (u32)layer->src_w_, (u32)layer->src_h_},
      {layer->dst_x_, layer->dst_y_,
       (u32)layer->dst_w_, (u32)layer->dst_h_}
    };
  }

  if (!pi5kms::PresentScanout(planes, layer_count,
                              (u32)screen_w, (u32)screen_h,
                              wait_for_vblank)) {
    return false;
  }

  g_kms_front_buffer_index = buffer_index;
  return true;
}

bool FrameBufferLayer::initialized_ = false;
CBcmFrameBuffer *FrameBufferLayer::screen_ = nullptr;
uint8_t *FrameBufferLayer::screen_pixels_ = nullptr;
unsigned FrameBufferLayer::screen_pitch_bytes_ = 0;
unsigned FrameBufferLayer::screen_bytes_per_pixel_ = 2;

void FrameBufferLayer::DrawLayerNearest(FrameBufferLayer *layer,
                                        int start_x,
                                        int end_x,
                                        int start_y,
                                        int end_y,
                                        int64_t x_step,
                                        int64_t y_step) {
  for (int y = start_y; y < end_y; y++) {
    int src_y = layer->src_y_ + (int) ((((int64_t) (y - layer->dst_y_)) * y_step) >> 16);
    src_y = min_int(max_int(src_y, 0), layer->fb_height_ - 1);

    uint8_t *dst_row = g_compose_pixels + y * g_compose_pitch_bytes;
    int64_t x_acc = ((int64_t) (start_x - layer->dst_x_)) * x_step;

    for (int x = start_x; x < end_x; x++) {
      int src_x = layer->src_x_ + (int) (x_acc >> 16);
      x_acc += x_step;
      src_x = min_int(max_int(src_x, 0), layer->fb_width_ - 1);

      if (layer->pixelmode_ == 0) {
        const uint8_t *src_row = layer->pixels_ + src_y * layer->fb_pitch_;
        uint8_t index = src_row[src_x];
        if (layer->transparency_) {
          uint32_t argb = layer->pal_argb_[index];
          if ((argb >> 24) == 0) {
            continue;
          }
          write_argb_pixel(dst_row, x, argb);
        } else {
          write_rgb565_pixel(dst_row, x, layer->pal_565_[index]);
        }
      } else {
        const uint16_t *src_row =
            (const uint16_t *) (layer->pixels_ + src_y * layer->fb_pitch_);
        write_rgb565_pixel(dst_row, x, src_row[src_x]);
      }
    }
  }
}

void FrameBufferLayer::DrawLayerBilinear(FrameBufferLayer *layer,
                                         int start_x,
                                         int end_x,
                                         int start_y,
                                         int end_y,
                                         int64_t x_step,
                                         int64_t y_step) {
  int max_src_x = min_int(layer->src_x_ + layer->src_w_ - 1,
                          layer->fb_width_ - 1);
  int max_src_y = min_int(layer->src_y_ + layer->src_h_ - 1,
                          layer->fb_height_ - 1);

  for (int y = start_y; y < end_y; y++) {
    int64_t y_acc = ((int64_t) (y - layer->dst_y_)) * y_step;
    int src_y0 = layer->src_y_ + (int) (y_acc >> 16);
    unsigned frac_y = (unsigned) ((y_acc >> 8) & 0xFF);
    src_y0 = min_int(max_int(src_y0, layer->src_y_), max_src_y);
    int src_y1 = min_int(src_y0 + 1, max_src_y);

    uint8_t *dst_row = g_compose_pixels + y * g_compose_pitch_bytes;
    int64_t x_acc = ((int64_t) (start_x - layer->dst_x_)) * x_step;

    for (int x = start_x; x < end_x; x++) {
      int src_x0 = layer->src_x_ + (int) (x_acc >> 16);
      unsigned frac_x = (unsigned) ((x_acc >> 8) & 0xFF);
      x_acc += x_step;
      src_x0 = min_int(max_int(src_x0, layer->src_x_), max_src_x);
      int src_x1 = min_int(src_x0 + 1, max_src_x);

      if (layer->pixelmode_ == 0) {
        const uint8_t *src_row0 = layer->pixels_ + src_y0 * layer->fb_pitch_;
        const uint8_t *src_row1 = layer->pixels_ + src_y1 * layer->fb_pitch_;
        if (layer->transparency_) {
          uint32_t c00 = layer->pal_argb_[src_row0[src_x0]];
          uint32_t c10 = layer->pal_argb_[src_row0[src_x1]];
          uint32_t c01 = layer->pal_argb_[src_row1[src_x0]];
          uint32_t c11 = layer->pal_argb_[src_row1[src_x1]];
          blend_argb_pixel(dst_row, x,
                           bilinear_argb8888(c00, c10, c01, c11,
                                             frac_x, frac_y));
        } else {
          uint16_t c00 = layer->pal_565_[src_row0[src_x0]];
          uint16_t c10 = layer->pal_565_[src_row0[src_x1]];
          uint16_t c01 = layer->pal_565_[src_row1[src_x0]];
          uint16_t c11 = layer->pal_565_[src_row1[src_x1]];
          write_rgb565_pixel(dst_row, x,
                             bilinear_rgb565(c00, c10, c01, c11,
                                             frac_x, frac_y));
        }
      } else {
        const uint16_t *src_row0 =
            (const uint16_t *) (layer->pixels_ + src_y0 * layer->fb_pitch_);
        const uint16_t *src_row1 =
            (const uint16_t *) (layer->pixels_ + src_y1 * layer->fb_pitch_);
        write_rgb565_pixel(dst_row, x,
                           bilinear_rgb565(src_row0[src_x0],
                                           src_row0[src_x1],
                                           src_row1[src_x0],
                                           src_row1[src_x1],
                                           frac_x,
                                           frac_y));
      }
    }
  }
}

FrameBufferLayer::FrameBufferLayer()
    : pixels_(nullptr),
      fb_width_(0),
      fb_height_(0),
      fb_pitch_(0),
      layer_(0),
      transparency_(false),
      hstretch_(1.6),
      vstretch_(1.0),
      hintstr_(0),
      vintstr_(0),
      use_hintstr_(0),
      use_vintstr_(0),
      valign_(0),
      vpadding_(0),
      halign_(0),
      hpadding_(0),
      h_center_offset_(0),
      v_center_offset_(0),
      rnum_(0),
      leftPadding_(0),
      rightPadding_(0),
      topPadding_(0),
      bottomPadding_(0),
      display_width_(0),
      display_height_(0),
      src_x_(0),
      src_y_(0),
      src_w_(0),
      src_h_(0),
      dst_x_(0),
      dst_y_(0),
      dst_w_(0),
      dst_h_(0),
      showing_(false),
      allocated_(false),
      pixelmode_(0),
      bytes_per_pixel_(1),
      uses_shader_(false),
      dirty_(true) {
  memcpy(pal_565_, default_pal_565, sizeof(pal_565_));
  memcpy(pal_argb_, default_pal_argb, sizeof(pal_argb_));
}

FrameBufferLayer::~FrameBufferLayer() {
  if (allocated_) {
    Free();
  }
}

void FrameBufferLayer::MarkDirty() {
  dirty_ = true;
  if (layer_ >= 0 && layer_ < FB_NUM_LAYERS) {
    for (unsigned i = 0; i < kKmsBufferCount; ++i) {
      g_kms_hwscale_framebuffer_valid[layer_][i] = false;
    }
  }
}

bool FrameBufferLayer::Initialize() {
  if (initialized_) {
    return true;
  }

  ViceOptions *options = ViceOptions::Get();
  unsigned requested_width = options ? options->GetFramebufferWidth() : 0;
  unsigned requested_height = options ? options->GetFramebufferHeight() : 0;
  unsigned requested_depth = options ? options->GetFramebufferDepth() :
                                      PI5_FRAMEBUFFER_DEFAULT_DEPTH;
  requested_depth = sanitize_framebuffer_depth(requested_depth);

  pi5kms::Mode kms_mode;
  bool kms_mode_resolved = false;
  if (options && options->Pi5KmsEnabled()) {
    kms_mode_resolved =
        pi5kms::ResolveBmcMode(options->GetHdmiGroup(),
                               options->GetHdmiMode(),
                               options->GetPi5KmsTimings(),
                               options->GetPi5KmsMode(),
                               &kms_mode);
    if (kms_mode_resolved) {
      if (requested_width == 0) {
        requested_width = kms_mode.width;
      }
      if (requested_height == 0) {
        requested_height = kms_mode.height;
      }
    } else {
      printf("boot: pi5kms no matching mode, using firmware mode\r\n");
    }
  }

  printf("boot: pi5 fbl request %ux%u depth %u\r\n",
         requested_width, requested_height, requested_depth);

  if (kms_mode_resolved && requested_depth != 16) {
    printf("boot: pi5kms forcing framebuffer depth 16 from %u\r\n",
           requested_depth);
    requested_depth = 16;
  }

  if (kms_mode_resolved && !pi5kms::SetMode(kms_mode)) {
    printf("boot: pi5kms mode switch failed, using firmware mode\r\n");
    kms_mode_resolved = false;
  }

  memset(g_layers, 0, sizeof(g_layers));
  g_kms_active = false;
  screen_pixels_ = nullptr;
  screen_pitch_bytes_ = 0;

  if (kms_mode_resolved) {
    screen_ = nullptr;
    if (requested_depth != 16) {
      printf("boot: pi5kms unsupported framebuffer depth %u\r\n",
             requested_depth);
      return false;
    }

    g_effective_width = (int) kms_mode.width;
    g_effective_height = (int) kms_mode.height;
    if (g_effective_width <= 0 || g_effective_height <= 0) {
      printf("boot: pi5kms invalid framebuffer dimensions %dx%d\r\n",
             g_effective_width, g_effective_height);
      return false;
    }

    for (unsigned i = 0; i < kKmsBufferCount; ++i) {
      if (!pi5kms::CreateFramebuffer(kms_mode.width, kms_mode.height,
                                     requested_depth,
                                     &g_kms_framebuffers[i])) {
        printf("boot: pi5kms framebuffer %u allocation failed\r\n", i);
        destroy_kms_scanout_framebuffers();
        return false;
      }
    }
    g_kms_front_buffer_index = 0;
    screen_pixels_ = g_kms_framebuffers[g_kms_front_buffer_index].pixels;
    screen_pitch_bytes_ = g_kms_framebuffers[g_kms_front_buffer_index].pitch;
    screen_bytes_per_pixel_ =
        g_kms_framebuffers[g_kms_front_buffer_index].depth / 8;
    g_framebuffer_bytes_per_pixel = screen_bytes_per_pixel_;
    if (screen_pixels_ == nullptr || screen_pitch_bytes_ == 0 ||
        screen_bytes_per_pixel_ != 2) {
      printf("boot: pi5kms invalid framebuffer layout\r\n");
      destroy_kms_scanout_framebuffers();
      return false;
    }

    pi5kms::Plane initial_plane =
        make_kms_framebuffer_plane(g_kms_framebuffers[g_kms_front_buffer_index]);
    if (!pi5kms::ConfigureScanout(initial_plane, kms_mode.width, kms_mode.height)) {
      printf("boot: pi5kms scanout setup failed\r\n");
      destroy_kms_scanout_framebuffers();
      return false;
    }
    g_kms_active = true;
  } else {
    screen_ = new CBcmFrameBuffer(requested_width,
                                  requested_height,
                                  requested_depth);
    if (screen_ == nullptr) {
      printf("boot: pi5 firmware framebuffer allocation failed\r\n");
      return false;
    }
    if (!screen_->Initialize()) {
      printf("boot: pi5 firmware framebuffer initialization failed\r\n");
      delete screen_;
      screen_ = nullptr;
      return false;
    }

    screen_pixels_ = (uint8_t *) (uintptr) screen_->GetBuffer();
    screen_pitch_bytes_ = screen_->GetPitch();
    screen_bytes_per_pixel_ =
        framebuffer_bytes_per_pixel(screen_, requested_depth);
    g_framebuffer_bytes_per_pixel = screen_bytes_per_pixel_;

    g_effective_width = effective_screen_width(screen_, screen_bytes_per_pixel_);
    g_effective_height = effective_screen_height(screen_);
    if (screen_pixels_ == nullptr || screen_pitch_bytes_ == 0 ||
        (screen_bytes_per_pixel_ != 2 && screen_bytes_per_pixel_ != 4) ||
        g_effective_width <= 0 || g_effective_height <= 0) {
      printf("boot: pi5 firmware framebuffer returned invalid layout\r\n");
      delete screen_;
      screen_ = nullptr;
      screen_pixels_ = nullptr;
      screen_pitch_bytes_ = 0;
      return false;
    }
  }

  g_compose_pitch_bytes = (unsigned) g_effective_width *
                          screen_bytes_per_pixel_;
  g_compose_pixels = (uint8_t *) malloc((size_t) g_compose_pitch_bytes *
                                        (size_t) g_effective_height);
  if (g_compose_pixels == nullptr) {
    printf("boot: pi5 composition framebuffer allocation failed\r\n");
    if (g_kms_active) {
      destroy_kms_scanout_framebuffers();
      g_kms_active = false;
    } else {
      delete screen_;
      screen_ = nullptr;
    }
    screen_pixels_ = nullptr;
    screen_pitch_bytes_ = 0;
    return false;
  }

  memset(g_compose_pixels, 0, (size_t) g_compose_pitch_bytes *
         (size_t) g_effective_height);
  memset(screen_pixels_, 0, (size_t) g_effective_height * screen_pitch_bytes_);

  if (g_kms_active) {
    for (unsigned i = 0; i < kKmsBufferCount; ++i) {
      pi5kms::FlushFramebuffer(g_kms_framebuffers[i]);
    }
    printf("boot: pi5 fbl kms hw %dx%d depth %u pitch %u size %u buffer 0x%08x\r\n",
           g_effective_width, g_effective_height, requested_depth,
           screen_pitch_bytes_, g_kms_framebuffers[g_kms_front_buffer_index].size,
           (u32) (uintptr) screen_pixels_);
  } else {
    printf("boot: pi5 fbl hw %ux%u virt %ux%u depth %u pitch %u size %u buffer 0x%08x\r\n",
           screen_->GetWidth(), screen_->GetHeight(),
           screen_->GetVirtWidth(), screen_->GetVirtHeight(),
           screen_->GetDepth(), screen_->GetPitch(), screen_->GetSize(),
           screen_->GetBuffer());
  }
  printf("boot: pi5 fbl direct bpp %u effective %dx%d compose_pitch %u\r\n",
         screen_bytes_per_pixel_, g_effective_width, g_effective_height,
         g_compose_pitch_bytes);
  initialized_ = true;
  return true;
}

bool FrameBufferLayer::OGLInit() { return false; }

int FrameBufferLayer::Allocate(int pixelmode, uint8_t **pixels,
                               int width, int height, int *pitch) {
  assert(!allocated_);
  if (!Initialize()) {
    return -1;
  }

  pixelmode_ = pixelmode;
  bytes_per_pixel_ = pixelmode == 1 ? 2 : 1;
  fb_width_ = width;
  fb_height_ = height;
  fb_pitch_ = ALIGN_UP(width * bytes_per_pixel_, 32);

  display_width_ = g_effective_width;
  display_height_ = g_effective_height;

  if (pixels_ == nullptr) {
    pixels_ = (uint8_t *) malloc(fb_pitch_ * fb_height_);
  }
  assert(pixels_ != nullptr);

  if (pixels) {
    *pixels = pixels_;
  }
  if (pitch) {
    *pitch = fb_pitch_;
  }

  src_x_ = 0;
  src_y_ = 0;
  src_w_ = width;
  src_h_ = height;
  dst_x_ = 0;
  dst_y_ = 0;
  dst_w_ = width;
  dst_h_ = height;
  allocated_ = true;
  MarkDirty();

  if (layer_ >= 0 && layer_ < FB_NUM_LAYERS && g_layers[layer_] == nullptr) {
    g_layers[layer_] = this;
  }

  printf("boot: pi5 fbl alloc layer %d pixelmode %d fb %dx%d pitch %d display %ux%u\r\n",
         layer_, pixelmode_, fb_width_, fb_height_, fb_pitch_,
         display_width_, display_height_);

  return 0;
}

int FrameBufferLayer::ReAllocate(bool shader_enable) {
  (void) shader_enable;
  uses_shader_ = false;
  return 0;
}

void FrameBufferLayer::Clear() {
  assert(allocated_);
  memset(pixels_, 0, fb_pitch_ * fb_height_);
  MarkDirty();
}

void FrameBufferLayer::FreeInternal(bool keepPixels) {
  if (!allocated_) {
    return;
  }

  showing_ = false;
  allocated_ = false;

  if (!keepPixels) {
    free(pixels_);
    pixels_ = nullptr;
    fb_width_ = 0;
    fb_height_ = 0;
    fb_pitch_ = 0;
  }
}

void FrameBufferLayer::Free() {
  FreeInternal(false);
}

void FrameBufferLayer::Show() {
  if (showing_) {
    return;
  }

  assert(hstretch_ != 0);
  assert(vstretch_ != 0);

  int lpad_abs = display_width_ * leftPadding_;
  int rpad_abs = display_width_ * rightPadding_;
  int tpad_abs = display_height_ * topPadding_;
  int bpad_abs = display_height_ * bottomPadding_;

  int avail_width = display_width_ - lpad_abs - rpad_abs;
  int avail_height = display_height_ - tpad_abs - bpad_abs;

  int dst_w;
  int dst_h;

  if (hstretch_ < 0) {
    dst_w = avail_width * vstretch_;
    dst_h = avail_width / -hstretch_;
    if (dst_w > avail_width) {
      dst_w = avail_width;
    }
    if (dst_h > avail_height) {
      dst_h = avail_height;
    }
  } else {
    dst_h = avail_height * vstretch_;
    if (use_vintstr_) {
      dst_h = vintstr_;
    }
    dst_w = avail_height * hstretch_;
    if (use_hintstr_) {
      dst_w = hintstr_;
    }
    if (dst_h > avail_height) {
      dst_h = avail_height;
    }
    if (dst_w > avail_width) {
      dst_w = avail_width;
    }
  }

  if (use_hintstr_ && use_vintstr_) {
    fit_to_available_preserving_aspect(&dst_w, &dst_h,
                                       avail_width, avail_height);
  }

  int oy;
  switch (valign_) {
    case 0:
      oy = (avail_height - dst_h) / 2 + v_center_offset_;
      break;
    case -1:
      oy = vpadding_;
      break;
    case 1:
      oy = avail_height - dst_h - vpadding_;
      break;
    default:
      oy = 0;
      break;
  }

  int ox;
  switch (halign_) {
    case 0:
      ox = (avail_width - dst_w) / 2 + h_center_offset_;
      break;
    case -1:
      ox = hpadding_;
      break;
    case 1:
      ox = avail_width - dst_w - hpadding_;
      break;
    default:
      ox = 0;
      break;
  }

  dst_x_ = ox + lpad_abs;
  dst_y_ = oy + tpad_abs;
  dst_w_ = dst_w;
  dst_h_ = dst_h;
  showing_ = true;
  MarkDirty();

  printf("boot: pi5 fbl show layer %d src %d,%d %dx%d dst %d,%d %dx%d display %ux%u\r\n",
         layer_, src_x_, src_y_, src_w_, src_h_,
         dst_x_, dst_y_, dst_w_, dst_h_, display_width_, display_height_);

  PresentLayer(false, this);
}

void FrameBufferLayer::Hide() {
  if (!showing_) {
    return;
  }

  showing_ = false;
  PresentLayer(false, this);
}

void *FrameBufferLayer::GetPixels() {
  return pixels_;
}

void FrameBufferLayer::FrameReady(int to_offscreen) {
  (void) to_offscreen;
  MarkDirty();
}

void FrameBufferLayer::PresentLayer(bool sync, FrameBufferLayer *layer) {
  if (layer) {
    layer->MarkDirty();
  }
  PresentLayerList(sync, nullptr, 0);
}

void FrameBufferLayer::PresentLayers(bool sync, FrameBufferLayer *layers,
                                     uint32_t ready_mask) {
  if (layers) {
    for (unsigned i = 0; i < FB_NUM_LAYERS; i++) {
      if (ready_mask & FB_LAYER_MASK(i)) {
        layers[i].MarkDirty();
      }
    }
  }
  PresentLayerList(sync, nullptr, 0);
}

void FrameBufferLayer::PresentLayerList(bool sync, FrameBufferLayer **layers,
                                        unsigned layer_list_count) {
  (void) layers;
  (void) layer_list_count;

  if (!initialized_) {
    return;
  }

  int screen_w = g_effective_width;
  int screen_h = g_effective_height;

  if (screen_w <= 0 || screen_h <= 0 || g_compose_pixels == nullptr) {
    return;
  }

  FrameBufferLayer *active[FB_NUM_LAYERS];
  unsigned active_count = 0;
  bool has_overlay_layer = false;
  bool has_ui_layer = false;
  for (unsigned i = 0; i < FB_NUM_LAYERS; i++) {
    if (g_layers[i] && g_layers[i]->Showing()) {
      active[active_count++] = g_layers[i];
      has_overlay_layer = has_overlay_layer || g_layers[i]->transparency_;
      has_ui_layer = has_ui_layer || i == FB_LAYER_UI;
    }
  }

  sort_layers(active, active_count);
  const bool wait_for_vblank = sync || has_ui_layer;
  if (TryKmsDirectScanout(active, active_count, screen_w, screen_h,
                          wait_for_vblank)) {
    if (sync && screen_ != nullptr) {
      screen_->WaitForVerticalSync();
    } else if (has_overlay_layer && screen_ != nullptr) {
      screen_->WaitForVerticalSync();
    }
    return;
  }

  memset(g_compose_pixels, 0, (size_t) g_compose_pitch_bytes *
         (size_t) screen_h);

  for (unsigned i = 0; i < active_count; i++) {
    FrameBufferLayer *layer = active[i];

    int start_x = max_int(0, layer->dst_x_);
    int end_x = min_int(screen_w, layer->dst_x_ + layer->dst_w_);
    int start_y = max_int(0, layer->dst_y_);
    int end_y = min_int(screen_h, layer->dst_y_ + layer->dst_h_);

    if (start_x >= end_x || start_y >= end_y ||
        layer->src_w_ <= 0 || layer->src_h_ <= 0 ||
        layer->fb_width_ <= 0 || layer->fb_height_ <= 0) {
      continue;
    }

    int64_t x_step = ((int64_t) layer->src_w_ << 16) / layer->dst_w_;
    int64_t y_step = ((int64_t) layer->src_h_ << 16) / layer->dst_h_;

    bool scaled = layer->src_w_ != layer->dst_w_ ||
                  layer->src_h_ != layer->dst_h_;
    bool can_interpolate = !layer->transparency_ || layer->pixelmode_ == 0;
    if (g_interpolation_enabled && scaled && can_interpolate) {
      DrawLayerBilinear(layer, start_x, end_x, start_y, end_y,
                        x_step, y_step);
    } else {
      DrawLayerNearest(layer, start_x, end_x, start_y, end_y,
                       x_step, y_step);
    }
  }

  if (!g_kms_active && sync && screen_ != nullptr) {
    screen_->WaitForVerticalSync();
  }

  size_t row_bytes = (size_t) screen_w * screen_bytes_per_pixel_;
  if (g_kms_active) {
    const unsigned buffer_index = kms_back_buffer_index();
    pi5kms::Framebuffer &fb = g_kms_framebuffers[buffer_index];
    for (int y = 0; y < screen_h; y++) {
      memcpy(fb.pixels + y * fb.pitch,
             g_compose_pixels + y * g_compose_pitch_bytes,
             row_bytes);
    }
    pi5kms::FlushFramebuffer(fb);
    if (!present_kms_framebuffer(buffer_index, wait_for_vblank)) {
      printf("boot: pi5kms software present failed\r\n");
    }
  } else {
    for (int y = 0; y < screen_h; y++) {
      memcpy(screen_pixels_ + y * screen_pitch_bytes_,
             g_compose_pixels + y * g_compose_pitch_bytes,
             row_bytes);
    }
  }
}

void FrameBufferLayer::SetPalette(uint8_t index, uint16_t rgb565) {
  pal_565_[index] = rgb565;
  MarkDirty();
}

void FrameBufferLayer::SetPalette(uint8_t index, uint32_t argb) {
  pal_argb_[index] = argb;
  MarkDirty();
}

void FrameBufferLayer::UpdatePalette() {
  if (showing_) {
    PresentLayer(false, this);
  }
}

void FrameBufferLayer::SetLayer(int layer) {
  layer_ = layer;
}

int FrameBufferLayer::GetLayer() {
  return layer_;
}

bool FrameBufferLayer::UsesShader() {
  return false;
}

bool FrameBufferLayer::Showing() {
  return showing_;
}

void FrameBufferLayer::SetTransparency(bool transparency) {
  transparency_ = transparency;
}

void FrameBufferLayer::SetSrcRect(int x, int y, int w, int h) {
  src_x_ = x;
  src_y_ = y;
  src_w_ = w;
  src_h_ = h;
  MarkDirty();
}

void FrameBufferLayer::SetStretch(double hstretch, double vstretch,
                                  int hintstr, int vintstr,
                                  int use_hintstr, int use_vintstr) {
  hstretch_ = hstretch;
  vstretch_ = vstretch;
  hintstr_ = hintstr;
  vintstr_ = vintstr;
  use_hintstr_ = use_hintstr;
  use_vintstr_ = use_vintstr;
}

void FrameBufferLayer::SetVerticalAlignment(int alignment, int padding) {
  valign_ = alignment;
  vpadding_ = padding;
}

void FrameBufferLayer::SetHorizontalAlignment(int alignment, int padding) {
  halign_ = alignment;
  hpadding_ = padding;
}

void FrameBufferLayer::SetPadding(double leftPadding,
                                  double rightPadding,
                                  double topPadding,
                                  double bottomPadding) {
  leftPadding_ = leftPadding;
  rightPadding_ = rightPadding;
  topPadding_ = topPadding;
  bottomPadding_ = bottomPadding;
}

void FrameBufferLayer::SetCenterOffset(int cx, int cy) {
  h_center_offset_ = cx;
  v_center_offset_ = cy;
}

void FrameBufferLayer::GetDimensions(int *display_w, int *display_h,
                                     int *fb_w, int *fb_h,
                                     int *src_w, int *src_h,
                                     int *dst_w, int *dst_h) {
  *display_w = display_width_;
  *display_h = display_height_;
  *fb_w = fb_width_;
  *fb_h = fb_height_;
  *src_w = src_w_;
  *src_h = src_h_;
  *dst_w = dst_w_;
  *dst_h = dst_h_;
}

void FrameBufferLayer::SetInterpolation(int enable) {
  bool new_value = enable != 0;
  if (g_interpolation_enabled == new_value) {
    return;
  }

  g_interpolation_enabled = new_value;
  if (initialized_) {
    PresentLayerList(false, nullptr, 0);
  }
}

void FrameBufferLayer::SetUsesShader(bool enable) {
  (void) enable;
  uses_shader_ = false;
}

void FrameBufferLayer::SetShaderParams(bool curvature,
                                       float curvature_x,
                                       float curvature_y,
                                       int mask,
                                       float mask_brightness,
                                       bool gamma,
                                       bool fake_gamma,
                                       bool scanlines,
                                       bool multisample,
                                       float scanline_weight,
                                       float scanline_gap_brightness,
                                       float bloom_factor,
                                       float input_gamma,
                                       float output_gamma,
                                       bool sharper,
                                       bool bilinear_interpolation) {
  (void) curvature;
  (void) curvature_x;
  (void) curvature_y;
  (void) mask;
  (void) mask_brightness;
  (void) gamma;
  (void) fake_gamma;
  (void) scanlines;
  (void) multisample;
  (void) scanline_weight;
  (void) scanline_gap_brightness;
  (void) bloom_factor;
  (void) input_gamma;
  (void) output_gamma;
  (void) sharper;
  (void) bilinear_interpolation;
  uses_shader_ = false;
}
