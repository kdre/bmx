#include "pi5_kms.h"
#include "scaling_order.h"

#include <circle/bcm2835.h>
#include <circle/bcmpropertytags.h>
#include <circle/memory.h>
#include <circle/memio.h>
#include <circle/new.h>
#include <circle/synchronize64.h>
#include <circle/timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ALIGN_UP
#define ALIGN_UP(x, y) (((x) + (y)-1) & ~((y)-1))
#endif

namespace pi5kms {

namespace {

constexpr uintptr kPv0Base = ARM_IO_BASE + 0x410000;
constexpr uintptr kHvsBase = ARM_IO_BASE + 0x580000;
constexpr uintptr kHdmi0HdBase = ARM_IO_BASE + 0x720000;
constexpr uintptr kHdmi0Base = ARM_IO_BASE + 0x701400;
constexpr uintptr kHdmi0DvpBase = ARM_IO_BASE + 0x701000;
constexpr uintptr kHdmi0PhyBase = ARM_IO_BASE + 0x701D00;
constexpr uintptr kHdmi0RmBase = ARM_IO_BASE + 0x702000;

constexpr unsigned kClockPixel = 10;
constexpr unsigned kClockM2mc = 13;
constexpr unsigned kClockPixelBvb = 14;
constexpr unsigned kClockDisp = 16;

constexpr u32 kPvControl = 0x000;
constexpr u32 kPvVControl = 0x004;
constexpr u32 kPvHorza = 0x00C;
constexpr u32 kPvHorzb = 0x010;
constexpr u32 kPvVerta = 0x014;
constexpr u32 kPvVertb = 0x018;
constexpr u32 kPvMuxCfg = 0x034;
constexpr u32 kPvPipeInitCtrl = 0x040;
constexpr u32 kPvControlEnable = 1U << 0;
constexpr u32 kPvControlFifoClear = 1U << 1;
constexpr u32 kPvVControlVideoEnable = 1U << 0;

constexpr u32 kHvsVersion = 0x000;
constexpr u32 kHvsVersionD0 = 0x54;
constexpr u32 kHvsControl = 0x020;
constexpr u32 kHvsControlEnable = 1U << 31;
constexpr u32 kHvs6cDisp0Ctrl0 = 0x030;
constexpr u32 kHvs6cDisp0Ctrl1 = 0x034;
constexpr u32 kHvs6cDisp0Bgnd0 = 0x038;
constexpr u32 kHvs6cDisp0Lptrs = 0x03C;
constexpr u32 kHvs6cDisp0Status = 0x044;
constexpr u32 kHvs6cDisp0Dl = 0x048;
constexpr u32 kHvs6cDisp0Run = 0x04C;
constexpr u32 kHvs6dDisp0Ctrl0 = 0x100;
constexpr u32 kHvs6dDisp0Ctrl1 = 0x104;
constexpr u32 kHvs6dDisp0Bgnd0 = 0x108;
constexpr u32 kHvs6dDisp0Lptrs = 0x110;
constexpr u32 kHvs6dDisp0Cob = 0x114;
constexpr u32 kHvs6dDisp0Status = 0x118;
constexpr u32 kHvs6dDisp0Dl = 0x11C;
constexpr u32 kHvs6dDisp0Run = 0x120;
constexpr u32 kHvsDlistStart = 0x4000;
constexpr u32 kHvsFilterMitchellSlot = 16;
constexpr u32 kHvsFilterNearestSlot = 32;
constexpr u32 kHvsDlistSlot0 = 64;
constexpr u32 kHvsDlistSlot1 = 224;
constexpr u32 kHvsStatusModeShift = 13;
constexpr u32 kHvsStatusModeMask = 3U << kHvsStatusModeShift;
constexpr u32 kHvsStatusModeEof = 3;
constexpr u32 kHvsDispCtrl0Enable = 1U << 31;
constexpr u32 kHvsDispCtrl0Reset = 1U << 30;
constexpr u32 kHvsDispCtrl1Interlace = 1U << 0;
constexpr u32 kHvsDispCtrl1BgEnable = 1U << 8;
constexpr u32 kHvsCtl0End = 1U << 31;
constexpr u32 kHvsCtl0Valid = 1U << 30;
constexpr u32 kHvsCtl0AddrModeLinear = 0;
constexpr u32 kHvsCtl0Unity = 1U << 15;
constexpr u32 kHvsPtr0UpmBaseShift = 16;
constexpr u32 kHvsPtr0UpmHandleShift = 10;
constexpr u32 kHvsPtr0UpmBufferLines2 = 0;
constexpr u32 kHvsPtr0UpmBufferLinesShift = 8;
constexpr u32 kHvsUpmWordSize = 256;
constexpr u32 kHvsUpmSlotBytes = 64 * 1024;
constexpr u32 kHvsLbmSlotWords = 256;
constexpr u32 kHvsPixelFormatRgb565 = 4;
constexpr u32 kHvsPixelFormatRgba8888 = 7;
constexpr u32 kHvsPixelOrderXrgb = 2;
constexpr u32 kHvsPixelOrderArgb = 2;
constexpr u32 kHvsAlphaMaskNone = 0;
constexpr u32 kHvsAlphaMaskFixed = 3;
constexpr u32 kHvsOpaqueAlpha = 0xFFFU;
constexpr u32 kHvsSclHPpfVPpf = 0;
constexpr u32 kHvsSclHTpzVPpf = 1;
constexpr u32 kHvsSclHPpfVTpz = 2;
constexpr u32 kHvsSclHTpzVTpz = 3;
constexpr u32 kHvsSclHPpfVNone = 4;
constexpr u32 kHvsSclHNoneVPpf = 5;
constexpr u32 kHvsSclHNoneVTpz = 6;
constexpr u32 kHvsSclHTpzVNone = 7;
constexpr u32 kHvsPpfNoInterp = 1U << 31;
constexpr u32 kHvsPpfAgc = 1U << 30;
constexpr unsigned kHvsPpfPhaseBits = 6;
constexpr unsigned kHvsFilterKernelWords = 11;

bool g_hvs_filter_kernels_uploaded = false;
unsigned g_hvs_submitted_dlist = 0;
bool g_hvs_has_active_dlist = false;
bool g_hvs_vblank_timeout_logged = false;

constexpr u32 HvsField(u32 value, unsigned shift) {
  return value << shift;
}

constexpr u32 HvsPpfFilterWord(int c0, int c1, int c2) {
  return ((u32)(c0 & 0x1FF) << 0) |
         ((u32)(c1 & 0x1FF) << 9) |
         ((u32)(c2 & 0x1FF) << 18);
}

constexpr u32 kHvsMitchellKernel[6] = {
  HvsPpfFilterWord(0, -2, -6),
  HvsPpfFilterWord(-8, -10, -8),
  HvsPpfFilterWord(-3, 2, 18),
  HvsPpfFilterWord(50, 82, 119),
  HvsPpfFilterWord(155, 187, 213),
  HvsPpfFilterWord(227, 227, 0),
};

constexpr u32 kHvsNearestKernel[6] = {
  HvsPpfFilterWord(0, 0, 0),
  HvsPpfFilterWord(0, 0, 0),
  HvsPpfFilterWord(1, 1, 1),
  HvsPpfFilterWord(1, 255, 255),
  HvsPpfFilterWord(255, 255, 255),
  HvsPpfFilterWord(255, 255, 0),
};

constexpr u32 kHdmiFifoCtl = 0x07C;
constexpr u32 kHdmiSchedulerControl = 0x0E8;
constexpr u32 kHdmiHorza = 0x0EC;
constexpr u32 kHdmiHorzb = 0x0F0;
constexpr u32 kHdmiVerta0 = 0x0F4;
constexpr u32 kHdmiVertb0 = 0x0F8;
constexpr u32 kHdmiVerta1 = 0x100;
constexpr u32 kHdmiVertb1 = 0x104;
constexpr u32 kHdmiMiscControl = 0x114;
constexpr u32 kHdmiClockStop = 0x0BC;
constexpr u32 kHdmiSchedulerManualFormat = 1U << 15;
constexpr u32 kHdmiSchedulerIgnoreVsyncPredicts = 1U << 5;
constexpr u32 kHdmiFifoRecenter = 1U << 6;
constexpr u32 kHdmiFifoValidWriteMask = 0x0000EFFF;

constexpr u32 kPhyResetCtl = 0x000;
constexpr u32 kPhyPowerupCtl = 0x004;
constexpr u32 kPhyCtl0 = 0x008;
constexpr u32 kPhyCtl1 = 0x00C;
constexpr u32 kPhyCtl2 = 0x010;
constexpr u32 kPhyCtlCk = 0x014;
constexpr u32 kPhyPllRefclk = 0x01C;
constexpr u32 kPhyPllPostKdiv = 0x028;
constexpr u32 kPhyPllVcoclkDiv = 0x02C;
constexpr u32 kPhyPllCfg = 0x044;
constexpr u32 kPhyTmdsClkWordSel = 0x054;
constexpr u32 kPhyPllMisc0 = 0x060;
constexpr u32 kPhyPllMisc1 = 0x064;
constexpr u32 kPhyPllMisc2 = 0x068;
constexpr u32 kPhyPllMisc3 = 0x06C;
constexpr u32 kPhyPllMisc4 = 0x070;
constexpr u32 kPhyPllMisc5 = 0x074;
constexpr u32 kPhyPllMisc6 = 0x078;
constexpr u32 kPhyPllMisc7 = 0x07C;
constexpr u32 kPhyPllMisc8 = 0x080;
constexpr u32 kPhyPllResetCtl = 0x190;
constexpr u32 kPhyPllPowerupCtl = 0x194;
constexpr u32 kPhyPllPostKdivBypass = 1U << 4;
constexpr u32 kPhyPllResetPllResetb = 1U << 0;

constexpr u32 kRmOffset = 0x018;

constexpr unsigned long long kVc6VcoMin = 8000000000ULL;
constexpr unsigned long long kVc6VcoMax = 12000000000ULL;
constexpr unsigned long long kOscillatorFrequency = 54000000ULL;

u32 ReadReg(uintptr base, u32 offset) {
  return read32(base + offset);
}

void WriteReg(uintptr base, u32 offset, u32 value) {
  write32(base + offset, value);
}

bool SetClockRate(unsigned clock_id, u32 rate) {
  CBcmPropertyTags tags;
  TPropertyTagSetClockRate tag;
  tag.nClockId = clock_id;
  tag.nRate = rate;
  tag.nSkipSettingTurbo = 0;
  return tags.GetTag(PROPTAG_SET_CLOCK_RATE, &tag, sizeof tag, 12);
}

u32 MakeHdmiHorza(const Mode &mode) {
  const u32 sync_flags = (mode.h_sync_positive ? (1U << 15) : 0) |
                         (mode.v_sync_positive ? (1U << 14) : 0);
  return ((u32)mode.h_front_porch << 16) | sync_flags | mode.width;
}

u32 MakeHdmiHorzb(const Mode &mode) {
  return ((u32)mode.h_back_porch << 16) | mode.h_sync;
}

u32 MakeHdmiVerta(const Mode &mode) {
  return ((u32)mode.v_sync << 24) | ((u32)mode.v_front_porch << 16) |
         mode.height;
}

u32 MakeHdmiVertb(const Mode &mode) {
  return mode.v_back_porch;
}

u32 MakePvHorza(const Mode &mode) {
  return ((u32)(mode.h_back_porch / 2) << 16) | (mode.h_sync / 2);
}

u32 MakePvHorzb(const Mode &mode) {
  return ((u32)(mode.h_front_porch / 2) << 16) | (mode.width / 2);
}

u32 MakePvVerta(const Mode &mode) {
  return ((u32)mode.v_back_porch << 16) | mode.v_sync;
}

u32 MakePvVertb(const Mode &mode) {
  return ((u32)mode.v_front_porch << 16) | mode.height;
}

u32 HvsDisp0Ctrl0Offset() {
  const u32 version = ReadReg(kHvsBase, kHvsVersion) & 0xFF;
  return version == kHvsVersionD0 ? kHvs6dDisp0Ctrl0 : kHvs6cDisp0Ctrl0;
}

u32 HvsDisp0Ctrl1Offset() {
  const u32 version = ReadReg(kHvsBase, kHvsVersion) & 0xFF;
  return version == kHvsVersionD0 ? kHvs6dDisp0Ctrl1 : kHvs6cDisp0Ctrl1;
}

u32 HvsDisp0LptrsOffset() {
  const u32 version = ReadReg(kHvsBase, kHvsVersion) & 0xFF;
  return version == kHvsVersionD0 ? kHvs6dDisp0Lptrs : kHvs6cDisp0Lptrs;
}

u32 HvsDisp0StatusOffset() {
  const u32 version = ReadReg(kHvsBase, kHvsVersion) & 0xFF;
  return version == kHvsVersionD0 ? kHvs6dDisp0Status : kHvs6cDisp0Status;
}

u32 HvsDisp0Bgnd0Offset() {
  const u32 version = ReadReg(kHvsBase, kHvsVersion) & 0xFF;
  return version == kHvsVersionD0 ? kHvs6dDisp0Bgnd0 : kHvs6cDisp0Bgnd0;
}

u32 MakeHvsDispCtrl0(const Mode &mode) {
  return kHvsDispCtrl0Enable |
         (((u32)mode.width - 1U) << 16) |
         ((u32)mode.height - 1U);
}

void ProgramHvsDisplay0(const Mode &mode) {
  const u32 ctrl0 = HvsDisp0Ctrl0Offset();
  const u32 ctrl1 = HvsDisp0Ctrl1Offset();
  const u32 ctrl1_before = ReadReg(kHvsBase, ctrl1);

  WriteReg(kHvsBase, kHvsControl, ReadReg(kHvsBase, kHvsControl) | kHvsControlEnable);
  WriteReg(kHvsBase, ctrl0, kHvsDispCtrl0Reset);
  WriteReg(kHvsBase, ctrl1, ctrl1_before & ~kHvsDispCtrl1Interlace);
  WriteReg(kHvsBase, ctrl0, MakeHvsDispCtrl0(mode));
}

bool IsValidRect(const Rect &rect) {
  return rect.width != 0 && rect.height != 0 &&
         rect.x >= 0 && rect.y >= 0 &&
         rect.width <= 0x1FFFU && rect.height <= 0x1FFFU &&
         (u32)rect.x <= 0x1FFFU && (u32)rect.y <= 0x1FFFU;
}

bool IsUnityPlane(const Plane &plane) {
  return plane.source.width == plane.destination.width &&
         plane.source.height == plane.destination.height;
}

bool IsFullscreenOpaquePlane(const Plane &plane,
                             u32 display_width,
                             u32 display_height) {
  return plane.format == kPixelFormatRgb565 &&
         plane.destination.x == 0 &&
         plane.destination.y == 0 &&
         plane.destination.width == display_width &&
         plane.destination.height == display_height;
}

bool NeedsBackgroundFill(const Plane *planes, unsigned plane_count,
                         u32 display_width, u32 display_height) {
  if (plane_count == 0 ||
      !IsFullscreenOpaquePlane(planes[0], display_width, display_height)) {
    return true;
  }

  for (unsigned i = 0; i < plane_count; ++i) {
    if (planes[i].format == kPixelFormatArgb8888) {
      return true;
    }
  }

  return false;
}

HvsScaling GetHvsScaling(u32 src, u32 dst) {
  if (src == dst) {
    return kHvsScalingNone;
  }

  if (3U * dst >= 2U * src) {
    return kHvsScalingPpf;
  }
  return kHvsScalingTpz;
}

u32 MakeHvsSclField(HvsScaling x_scaling, HvsScaling y_scaling) {
  if (x_scaling == kHvsScalingPpf && y_scaling == kHvsScalingPpf) {
    return kHvsSclHPpfVPpf;
  }
  if (x_scaling == kHvsScalingTpz && y_scaling == kHvsScalingPpf) {
    return kHvsSclHTpzVPpf;
  }
  if (x_scaling == kHvsScalingPpf && y_scaling == kHvsScalingTpz) {
    return kHvsSclHPpfVTpz;
  }
  if (x_scaling == kHvsScalingTpz && y_scaling == kHvsScalingTpz) {
    return kHvsSclHTpzVTpz;
  }
  if (x_scaling == kHvsScalingPpf && y_scaling == kHvsScalingNone) {
    return kHvsSclHPpfVNone;
  }
  if (x_scaling == kHvsScalingNone && y_scaling == kHvsScalingPpf) {
    return kHvsSclHNoneVPpf;
  }
  if (x_scaling == kHvsScalingNone && y_scaling == kHvsScalingTpz) {
    return kHvsSclHNoneVTpz;
  }
  if (x_scaling == kHvsScalingTpz && y_scaling == kHvsScalingNone) {
    return kHvsSclHTpzVNone;
  }
  return 0;
}

u32 MakeHvsPlaneCtl0(unsigned next_words, bool unity,
                     PixelFormat format, u32 scl_field) {
  u32 hvs_format;
  u32 pixel_order;
  u32 alpha_mask;

  switch (format) {
    case kPixelFormatRgb565:
      hvs_format = kHvsPixelFormatRgb565;
      pixel_order = kHvsPixelOrderXrgb;
      alpha_mask = kHvsAlphaMaskFixed;
      break;
    case kPixelFormatArgb8888:
      hvs_format = kHvsPixelFormatRgba8888;
      pixel_order = kHvsPixelOrderArgb;
      alpha_mask = kHvsAlphaMaskNone;
      break;
    default:
      return 0;
  }

  return kHvsCtl0Valid |
         HvsField(next_words & 0x3FU, 24) |
         HvsField(kHvsCtl0AddrModeLinear, 20) |
         HvsField(alpha_mask, 18) |
         (unity ? kHvsCtl0Unity : 0) |
         HvsField(pixel_order, 13) |
         HvsField(scl_field, 8) |
         HvsField(scl_field, 5) |
         hvs_format;
}

u32 MakeHvsPos0(const Rect &rect) {
  return ((u32)rect.y << 16) | (u32)rect.x;
}

u32 MakeHvsPos2(u32 width, u32 height) {
  return ((height - 1U) << 16) | (width - 1U);
}

u32 MakeHvsPos1(const Rect &rect) {
  return ((rect.height - 1U) << 16) | (rect.width - 1U);
}

u32 MakeHvsPtr0(u32 framebuffer_bus_address, unsigned upm_slot) {
  const u32 ptr_upper =
      (u32)(((u64)framebuffer_bus_address >> 32) & 0xFFU);
  const u32 upm_base =
      (upm_slot * kHvsUpmSlotBytes) / kHvsUpmWordSize;
  const u32 upm_handle = upm_slot & 0x1FU;

  return ptr_upper |
         HvsField(upm_base, kHvsPtr0UpmBaseShift) |
         HvsField(upm_handle, kHvsPtr0UpmHandleShift) |
         HvsField(kHvsPtr0UpmBufferLines2, kHvsPtr0UpmBufferLinesShift);
}

u32 MakeHvsLbmBase(unsigned lbm_slot) {
  return lbm_slot * kHvsLbmSlotWords;
}

u32 MakeHvsPpfWord(u32 src, u32 dst, u32 xy, bool nearest) {
  const u32 src_fixed = src << 16;
  u32 scale = src_fixed / dst;
  int offset = (int)((xy & 0xFFFFU) >> (16 - kHvsPpfPhaseBits));
  offset += -(1 << (kHvsPpfPhaseBits - 1));

  scale &= ~1U;
  int offset2 = (int)(src_fixed - dst * scale);
  offset2 >>= 16 - kHvsPpfPhaseBits;
  int phase = offset + (offset2 >> 1);
  if (phase >= (1 << kHvsPpfPhaseBits)) {
    phase = (1 << kHvsPpfPhaseBits) - 1;
  }
  phase &= (1U << (kHvsPpfPhaseBits + 1U)) - 1U;

  return (nearest ? kHvsPpfNoInterp : 0) |
         kHvsPpfAgc |
         HvsField(scale & 0x1FFFFU, 8) |
         (u32)phase;
}

u32 MakeHvsTpzWord0(u32 src, u32 dst) {
  u32 scale;
  if ((dst << 16) < (src << 16)) {
    scale = (src << 16) / dst;
  } else {
    scale = (1U << 16) + 1U;
  }
  return HvsField(scale & 0x1FFFFFU, 8);
}

u32 MakeHvsTpzWord1(u32 src, u32 dst) {
  u32 scale;
  u32 recip;
  if ((dst << 16) < (src << 16)) {
    scale = (src << 16) / dst;
    recip = ~0U / scale;
  } else {
    recip = (1U << 16) - 1U;
  }
  return recip & 0xFFFFU;
}

void WriteDlistWord(unsigned index, u32 value);

void UploadHvsFilterKernel(unsigned slot, const u32 kernel[6]) {
  for (unsigned i = 0; i < kHvsFilterKernelWords; ++i) {
    const unsigned src = i < 6 ? i : (kHvsFilterKernelWords - i - 1);
    WriteDlistWord(slot + i, kernel[src]);
  }
}

void EnsureHvsFilterKernelsUploaded() {
  if (g_hvs_filter_kernels_uploaded) {
    return;
  }

  UploadHvsFilterKernel(kHvsFilterMitchellSlot, kHvsMitchellKernel);
  UploadHvsFilterKernel(kHvsFilterNearestSlot, kHvsNearestKernel);
  g_hvs_filter_kernels_uploaded = true;
}

void WriteDlistWord(unsigned index, u32 value) {
  WriteReg(kHvsBase, kHvsDlistStart + index * sizeof(u32), value);
}

void ProgramHvsCob0() {
  const u32 version = ReadReg(kHvsBase, kHvsVersion) & 0xFF;
  if (version != kHvsVersionD0) {
    return;
  }

  constexpr u32 line_width = 3840;
  constexpr u32 num_lines = 4;
  u32 top = line_width + line_width * num_lines;
  u32 base = top + 16;
  top += line_width * num_lines;

  WriteReg(kHvsBase, kHvs6dDisp0Cob, (top << 16) | base);
}

bool AppendPlaneDlist(const Plane &plane,
                      unsigned upm_slot,
                      u32 *dlist, unsigned capacity, unsigned *count) {
  if (dlist == nullptr || count == nullptr || *count >= capacity) {
    return false;
  }
  if (capacity - *count < 32) {
    return false;
  }

  const bool unity = IsUnityPlane(plane);
  const HvsScaling x_scaling =
      GetHvsScaling(plane.source.width, plane.destination.width);
  const HvsScaling y_scaling =
      GetHvsScaling(plane.source.height, plane.destination.height);
  const u32 scl_field = unity ? 0 : MakeHvsSclField(x_scaling, y_scaling);
  const bool nearest = plane.filter == kScaleFilterNearest;

  const u32 bytes_per_pixel = plane.depth / 8;
  const u32 source_offset =
      (u32)plane.source.y * plane.pitch +
      (u32)plane.source.x * bytes_per_pixel;
  const u32 framebuffer_bus_address =
      plane.framebuffer_bus_address + source_offset;

  const unsigned start = *count;
  dlist[(*count)++] = 0;
  dlist[(*count)++] = MakeHvsPos0(plane.destination);
  dlist[(*count)++] = kHvsOpaqueAlpha << 4;
  if (!unity) {
    dlist[(*count)++] = MakeHvsPos1(plane.destination);
  }
  dlist[(*count)++] = MakeHvsPos2(plane.source.width, plane.source.height);
  dlist[(*count)++] = 0xC0C0C0C0U;
  dlist[(*count)++] = MakeHvsPtr0(framebuffer_bus_address, upm_slot);
  dlist[(*count)++] = framebuffer_bus_address;
  dlist[(*count)++] = plane.pitch & 0x1FFFFU;

  if (!unity && y_scaling != kHvsScalingNone) {
    dlist[(*count)++] = MakeHvsLbmBase(upm_slot);
  }

  if (!unity) {
    HvsScalingParameter parameter_order[4];
    const unsigned parameter_count = BuildHvsScalingParameterOrder(
        x_scaling, y_scaling, parameter_order);
    for (unsigned i = 0; i < parameter_count; ++i) {
      switch (parameter_order[i]) {
        case kHvsHorizontalPpf:
          dlist[(*count)++] = MakeHvsPpfWord(plane.source.width,
                                             plane.destination.width,
                                             (u32)plane.source.x << 16,
                                             nearest);
          break;
        case kHvsVerticalPpf:
          dlist[(*count)++] = MakeHvsPpfWord(plane.source.height,
                                             plane.destination.height,
                                             (u32)plane.source.y << 16,
                                             nearest);
          dlist[(*count)++] = 0xC0C0C0C0U;
          break;
        case kHvsHorizontalTpz:
          dlist[(*count)++] = MakeHvsTpzWord0(plane.source.width,
                                              plane.destination.width);
          dlist[(*count)++] = MakeHvsTpzWord1(plane.source.width,
                                              plane.destination.width);
          break;
        case kHvsVerticalTpz:
          dlist[(*count)++] = MakeHvsTpzWord0(plane.source.height,
                                              plane.destination.height);
          dlist[(*count)++] = MakeHvsTpzWord1(plane.source.height,
                                              plane.destination.height);
          dlist[(*count)++] = 0xC0C0C0C0U;
          break;
      }
    }

    if (x_scaling == kHvsScalingPpf || y_scaling == kHvsScalingPpf) {
      const u32 kernel_slot = nearest ? kHvsFilterNearestSlot
                                      : kHvsFilterMitchellSlot;
      dlist[(*count)++] = kernel_slot;
      dlist[(*count)++] = kernel_slot;
      dlist[(*count)++] = kernel_slot;
      dlist[(*count)++] = kernel_slot;
    }
  }

  if (*count >= capacity) {
    return false;
  }
  // NEXT skips over this per-plane END. The composed CRTC list still needs
  // a final END after the last plane, matching the Linux VC6 HVS path.
  dlist[(*count)++] = kHvsCtl0End;

  const unsigned plane_words = *count - start;
  if (plane_words > 0x3FU) {
    return false;
  }

  dlist[start] = MakeHvsPlaneCtl0(plane_words, unity, plane.format, scl_field);
  if (dlist[start] == 0) {
    return false;
  }

  return true;
}

bool ValidatePlane(const Plane &plane, u32 display_width,
                   u32 display_height) {
  if (plane.framebuffer_bus_address == 0 || plane.pitch == 0 ||
      plane.width == 0 || plane.height == 0 ||
      display_width == 0 || display_height == 0) {
    return false;
  }

  if ((plane.format == kPixelFormatRgb565 && plane.depth != 16) ||
      (plane.format == kPixelFormatArgb8888 && plane.depth != 32) ||
      (plane.format != kPixelFormatRgb565 &&
       plane.format != kPixelFormatArgb8888)) {
    printf("boot: pi5kms unsupported plane format depth %u format %u\r\n",
           plane.depth, (unsigned)plane.format);
    return false;
  }
  if (plane.filter != kScaleFilterNearest &&
      plane.filter != kScaleFilterMitchell) {
    printf("boot: pi5kms unsupported scale filter %u\r\n",
           (unsigned)plane.filter);
    return false;
  }

  if (plane.pitch > 0x1FFFFU || plane.width > 0x1FFFU ||
      plane.height > 0x1FFFU || display_width > 0x1FFFU ||
      display_height > 0x1FFFU) {
    printf("boot: pi5kms plane geometry out of range\r\n");
    return false;
  }

  if (!IsValidRect(plane.source) || !IsValidRect(plane.destination)) {
    printf("boot: pi5kms invalid plane rect\r\n");
    return false;
  }

  if ((u32)plane.source.x + plane.source.width > plane.width ||
      (u32)plane.source.y + plane.source.height > plane.height ||
      (u32)plane.destination.x + plane.destination.width > display_width ||
      (u32)plane.destination.y + plane.destination.height > display_height) {
    printf("boot: pi5kms plane rect outside source or display\r\n");
    return false;
  }

  return true;
}

void LogScanoutPlanes(const Plane *planes, unsigned plane_count,
                      u32 dlist_slot) {
  for (unsigned i = 0; i < plane_count; ++i) {
    printf("boot: pi5kms scanout plane %u fb 0x%08x src %d,%d %ux%u "
           "dst %d,%d %ux%u pitch %u depth %u dlist %u\r\n",
           i,
           planes[i].framebuffer_bus_address,
           planes[i].source.x, planes[i].source.y,
           planes[i].source.width, planes[i].source.height,
           planes[i].destination.x, planes[i].destination.y,
           planes[i].destination.width, planes[i].destination.height,
           planes[i].pitch, planes[i].depth, dlist_slot);
  }
}

bool BuildScanoutDlist(const Plane *planes, unsigned plane_count,
                       u32 display_width, u32 display_height,
                       u32 *dlist, unsigned dlist_capacity,
                       unsigned *dlist_count) {
  if (planes == nullptr || plane_count == 0 ||
      dlist == nullptr || dlist_count == nullptr) {
    return false;
  }

  for (unsigned i = 0; i < plane_count; ++i) {
    if (!ValidatePlane(planes[i], display_width, display_height)) {
      return false;
    }
  }

  EnsureHvsFilterKernelsUploaded();

  unsigned count = 0;
  for (unsigned i = 0; i < plane_count; ++i) {
    if (!AppendPlaneDlist(planes[i], i, dlist, dlist_capacity, &count)) {
      printf("boot: pi5kms display list build failed at plane %u\r\n", i);
      return false;
    }
  }

  if (count >= dlist_capacity) {
    printf("boot: pi5kms display list overflow %u\r\n", count);
    return false;
  }
  dlist[count++] = kHvsCtl0End;
  *dlist_count = count;
  return true;
}

u32 HvsDlistSlot(unsigned dlist_index) {
  return dlist_index == 0 ? kHvsDlistSlot0 : kHvsDlistSlot1;
}

bool ProgramScanout(const Plane *planes, unsigned plane_count,
                    u32 display_width, u32 display_height,
                    bool wait_for_vblank, bool log_planes,
                    bool program_channel) {
  if (planes == nullptr || plane_count == 0) {
    return false;
  }

  const unsigned dlist_index =
      (program_channel || !g_hvs_has_active_dlist)
          ? 0
          : (g_hvs_submitted_dlist ^ 1U);
  const u32 dlist_slot = HvsDlistSlot(dlist_index);
  const u32 ctrl0 = HvsDisp0Ctrl0Offset();
  const u32 ctrl1 = HvsDisp0Ctrl1Offset();
  const u32 bgnd0 = HvsDisp0Bgnd0Offset();
  const u32 lptrs = HvsDisp0LptrsOffset();
  const bool background_fill =
      NeedsBackgroundFill(planes, plane_count, display_width, display_height);

  u32 dlist[160];
  const unsigned dlist_capacity = sizeof(dlist) / sizeof(dlist[0]);
  unsigned count = 0;
  if (!BuildScanoutDlist(planes, plane_count, display_width, display_height,
                         dlist, dlist_capacity, &count)) {
    return false;
  }

  if (log_planes) {
    LogScanoutPlanes(planes, plane_count, dlist_slot);
  }

  if (program_channel) {
    WriteReg(kHvsBase, kHvsControl,
             ReadReg(kHvsBase, kHvsControl) | kHvsControlEnable);
  }

  if (!program_channel &&
      wait_for_vblank &&
      g_hvs_has_active_dlist &&
      !WaitForVBlank()) {
    if (!g_hvs_vblank_timeout_logged) {
      printf("boot: pi5kms vblank wait timed out; presenting anyway\r\n");
      g_hvs_vblank_timeout_logged = true;
    }
  }

  for (unsigned i = 0; i < count; ++i) {
    WriteDlistWord(dlist_slot + i, dlist[i]);
  }

  if (program_channel) {
    ProgramHvsCob0();
    WriteReg(kHvsBase, bgnd0, 0);
  }

  if (background_fill) {
    WriteReg(kHvsBase, ctrl1,
             ReadReg(kHvsBase, ctrl1) | kHvsDispCtrl1BgEnable);
  } else {
    WriteReg(kHvsBase, ctrl1,
             ReadReg(kHvsBase, ctrl1) & ~kHvsDispCtrl1BgEnable);
  }

  WriteReg(kHvsBase, lptrs, dlist_slot);
  if (program_channel) {
    WriteReg(kHvsBase, ctrl0,
             kHvsDispCtrl0Enable |
             ((display_width - 1U) << 16) |
             (display_height - 1U));
  }

  g_hvs_submitted_dlist = dlist_index;
  g_hvs_has_active_dlist = true;
  return true;
}

u32 GetHsmClock(u32 tmds_rate) {
  const unsigned long long hsm =
      ((unsigned long long)tmds_rate * 101ULL + 99ULL) / 100ULL;
  return hsm < 120000000ULL ? 120000000 : (u32)hsm;
}

u32 GetBvbClock(u32 tmds_rate) {
  if (tmds_rate > 297000000) {
    return 300000000;
  }
  if (tmds_rate > 148500000) {
    return 150000000;
  }
  return 75000000;
}

unsigned long long GetVc6VcoFreq(u32 tmds_rate, u32 *vco_div) {
  u32 div = 0;
  while ((unsigned long long)tmds_rate * div * 10ULL < kVc6VcoMin) {
    ++div;
  }
  const u32 min_div = div;

  while ((unsigned long long)tmds_rate * (div + 1U) * 10ULL < kVc6VcoMax) {
    ++div;
  }
  const u32 max_div = div;

  div = min_div + (max_div - min_div) / 2U;
  *vco_div = div;
  return (unsigned long long)tmds_rate * div * 10ULL;
}

u32 GetRmOffset(unsigned long long vco_freq) {
  unsigned long long offset = vco_freq * 2ULL;
  offset <<= 22;
  offset /= kOscillatorFrequency;
  offset >>= 2;
  return (u32)offset;
}

void InitVc6Phy(u32 tmds_rate) {
  u32 vco_div = 0;
  const unsigned long long vco_freq = GetVc6VcoFreq(tmds_rate, &vco_div);
  const u32 rm_offset = 0x80000000U | GetRmOffset(vco_freq);

  if (tmds_rate > 222000000) {
    printf("boot: pi5kms warning tmds %u above validated lane table\r\n",
           tmds_rate);
  }

  WriteReg(kHdmi0PhyBase, kPhyResetCtl, 0);
  WriteReg(kHdmi0PhyBase, kPhyPowerupCtl, 0);
  WriteReg(kHdmi0PhyBase, kPhyPllPostKdiv, kPhyPllPostKdivBypass);

  WriteReg(kHdmi0PhyBase, kPhyPllMisc0, 0x810C6000);
  WriteReg(kHdmi0PhyBase, kPhyPllMisc1, 0x00B8C451);
  WriteReg(kHdmi0PhyBase, kPhyPllMisc2, 0x46402E31);
  WriteReg(kHdmi0PhyBase, kPhyPllMisc3, 0x00B8C005);
  WriteReg(kHdmi0PhyBase, kPhyPllMisc4, 0x42410261);
  WriteReg(kHdmi0PhyBase, kPhyPllMisc5, 0xCC021001);
  WriteReg(kHdmi0PhyBase, kPhyPllMisc6, 0xC8301C80);
  WriteReg(kHdmi0PhyBase, kPhyPllMisc7, 0xB0804444);
  WriteReg(kHdmi0PhyBase, kPhyPllMisc8, 0xF80F8000);

  WriteReg(kHdmi0PhyBase, kPhyPllRefclk, 0x00002036);
  WriteReg(kHdmi0PhyBase, kPhyResetCtl, 0x0000007F);
  WriteReg(kHdmi0RmBase, kRmOffset, rm_offset);
  WriteReg(kHdmi0PhyBase, kPhyPllVcoclkDiv, 0x00000400 | vco_div);
  WriteReg(kHdmi0PhyBase, kPhyPllCfg, 0);
  WriteReg(kHdmi0PhyBase, kPhyPllPostKdiv, 0x00000009);

  WriteReg(kHdmi0PhyBase, kPhyCtl0, 0x80828700);
  WriteReg(kHdmi0PhyBase, kPhyCtl1, 0x80828700);
  WriteReg(kHdmi0PhyBase, kPhyCtl2, 0x80828700);
  WriteReg(kHdmi0PhyBase, kPhyCtlCk, 0x80828700);
  WriteReg(kHdmi0PhyBase, kPhyTmdsClkWordSel, 0);

  WriteReg(kHdmi0PhyBase, kPhyPowerupCtl, 0x000001CF);
  WriteReg(kHdmi0PhyBase, kPhyPllPowerupCtl, 0x00000001);
  WriteReg(kHdmi0PhyBase, kPhyPllResetCtl,
           ReadReg(kHdmi0PhyBase, kPhyPllResetCtl) & ~kPhyPllResetPllResetb);
  WriteReg(kHdmi0PhyBase, kPhyPllResetCtl,
           ReadReg(kHdmi0PhyBase, kPhyPllResetCtl) | kPhyPllResetPllResetb);
}

bool ResolveStandardMode(unsigned hdmi_group, unsigned hdmi_mode,
                         Mode *mode) {
  if (hdmi_group != 1) {
    return false;
  }

  switch (hdmi_mode) {
    case 4:
      *mode = {1280, 720, 74250000, 110, 40, 220, 5, 5, 20, true, true};
      return true;
    case 19:
      *mode = {1280, 720, 74250000, 440, 40, 220, 5, 5, 20, true, true};
      return true;
    case 16:
      *mode = {1920, 1080, 148500000, 88, 44, 148, 4, 5, 36, true, true};
      return true;
    case 31:
      *mode = {1920, 1080, 148500000, 528, 44, 148, 4, 5, 36, true, true};
      return true;
    default:
      return false;
  }
}

bool ResolveNamedMode(const char *name, Mode *mode) {
  if (name == nullptr || *name == '\0') {
    return false;
  }

  if (strcmp(name, "1280x800@60") == 0) {
    *mode = {1280, 800, 83500000, 72, 128, 200, 3, 6, 22, false, true};
    return true;
  }
  if (strcmp(name, "1280x800@50") == 0) {
    *mode = {1280, 800, 68000000, 56, 128, 184, 3, 6, 17, false, true};
    return true;
  }
  if (strcmp(name, "1920x1200@60") == 0) {
    *mode = {1920, 1200, 193250000, 136, 200, 336, 3, 6, 36, false, true};
    return true;
  }
  if (strcmp(name, "1920x1200@50") == 0) {
    *mode = {1920, 1200, 158250000, 120, 200, 320, 3, 6, 29, false, true};
    return true;
  }

  return false;
}

bool ParseTimings(const char *timings, Mode *mode) {
  if (timings == nullptr || *timings == '\0') {
    return false;
  }

  unsigned values[17];
  const char *p = timings;
  for (unsigned i = 0; i < 17; ++i) {
    char *end = nullptr;
    values[i] = strtoul(p, &end, 10);
    if (end == p) {
      return false;
    }
    p = end;
    while (*p == ',' || *p == ' ') {
      ++p;
    }
  }

  mode->width = values[0];
  mode->h_sync_positive = values[1] != 0;
  mode->h_front_porch = values[2];
  mode->h_sync = values[3];
  mode->h_back_porch = values[4];
  mode->height = values[5];
  mode->v_sync_positive = values[6] != 0;
  mode->v_front_porch = values[7];
  mode->v_sync = values[8];
  mode->v_back_porch = values[9];
  mode->pixel_clock = values[15];

  return mode->width != 0 && mode->height != 0 && mode->pixel_clock != 0;
}

}  // namespace

bool ResolveBmcMode(unsigned hdmi_group, unsigned hdmi_mode,
                    const char *hdmi_timings, const char *named_mode,
                    Mode *mode) {
  if (mode == nullptr) {
    return false;
  }

  if (ResolveNamedMode(named_mode, mode)) {
    return true;
  }
  if (hdmi_group == 2 && hdmi_mode == 87 && ParseTimings(hdmi_timings, mode)) {
    return true;
  }
  return ResolveStandardMode(hdmi_group, hdmi_mode, mode);
}

bool SetMode(const Mode &mode) {
  if ((mode.width & 1) || (mode.h_front_porch & 1) || (mode.h_sync & 1) ||
      (mode.h_back_porch & 1)) {
    printf("boot: pi5kms unsupported odd horizontal timing\r\n");
    return false;
  }

  printf("boot: pi5kms set %ux%u pclk %u\r\n",
         mode.width, mode.height, mode.pixel_clock);

  const u32 pv_control_before = ReadReg(kPv0Base, kPvControl);
  WriteReg(kPv0Base, kPvControl, pv_control_before & ~kPvControlEnable);
  WriteReg(kPv0Base, kPvControl,
           (pv_control_before & ~kPvControlEnable) | kPvControlFifoClear);

  if (!SetClockRate(kClockPixel, mode.pixel_clock) ||
      !SetClockRate(kClockM2mc, GetHsmClock(mode.pixel_clock)) ||
      !SetClockRate(kClockPixelBvb, GetBvbClock(mode.pixel_clock)) ||
      !SetClockRate(kClockDisp, GetHsmClock(mode.pixel_clock))) {
    printf("boot: pi5kms clock setup failed\r\n");
    WriteReg(kPv0Base, kPvControl, pv_control_before);
    return false;
  }

  InitVc6Phy(mode.pixel_clock);

  WriteReg(kHdmi0DvpBase, kHdmiClockStop, 0);
  WriteReg(kHdmi0Base, kHdmiSchedulerControl,
           ReadReg(kHdmi0Base, kHdmiSchedulerControl) |
               kHdmiSchedulerManualFormat |
               kHdmiSchedulerIgnoreVsyncPredicts);
  WriteReg(kHdmi0Base, kHdmiHorza, MakeHdmiHorza(mode));
  WriteReg(kHdmi0Base, kHdmiHorzb, MakeHdmiHorzb(mode));
  WriteReg(kHdmi0Base, kHdmiVerta0, MakeHdmiVerta(mode));
  WriteReg(kHdmi0Base, kHdmiVertb0, MakeHdmiVertb(mode));
  WriteReg(kHdmi0Base, kHdmiVerta1, MakeHdmiVerta(mode));
  WriteReg(kHdmi0Base, kHdmiVertb1, MakeHdmiVertb(mode));
  WriteReg(kHdmi0Base, kHdmiMiscControl,
           ReadReg(kHdmi0Base, kHdmiMiscControl) & ~0xFU);

  WriteReg(kPv0Base, kPvHorza, MakePvHorza(mode));
  WriteReg(kPv0Base, kPvHorzb, MakePvHorzb(mode));
  WriteReg(kPv0Base, kPvVerta, MakePvVerta(mode));
  WriteReg(kPv0Base, kPvVertb, MakePvVertb(mode));
  WriteReg(kPv0Base, kPvVControl,
           ReadReg(kPv0Base, kPvVControl) | kPvVControlVideoEnable);
  WriteReg(kPv0Base, kPvMuxCfg, 0x00000020);
  WriteReg(kPv0Base, kPvPipeInitCtrl, 0x00030002);

  ProgramHvsDisplay0(mode);

  const u32 fifo = ReadReg(kHdmi0Base, kHdmiFifoCtl) & kHdmiFifoValidWriteMask;
  WriteReg(kHdmi0Base, kHdmiFifoCtl, fifo & ~kHdmiFifoRecenter);
  WriteReg(kHdmi0Base, kHdmiFifoCtl, fifo | kHdmiFifoRecenter);

  WriteReg(kPv0Base, kPvControl, (pv_control_before | kPvControlEnable) &
                                     ~kPvControlFifoClear);

  printf("boot: pi5kms set complete\r\n");
  return true;
}

bool CreateFramebuffer(u32 width, u32 height, u32 depth, Framebuffer *fb) {
  if (fb == nullptr || width == 0 || height == 0 ||
      (depth != 16 && depth != 32)) {
    return false;
  }

  const u32 bytes_per_pixel = depth / 8;
  const u32 pitch = ALIGN_UP(width * bytes_per_pixel, 64);
  const u32 size = pitch * height;
  uint8_t *pixels = new (HEAP_DMA30) uint8_t[size];
  if (pixels == nullptr) {
    return false;
  }

  fb->pixels = pixels;
  fb->width = width;
  fb->height = height;
  fb->pitch = pitch;
  fb->depth = depth;
  fb->size = size;

  ClearFramebuffer(*fb);

  printf("boot: pi5kms framebuffer 0x%08x %ux%u depth %u pitch %u size %u\r\n",
         (u32)(uintptr)fb->pixels, fb->width, fb->height, fb->depth,
         fb->pitch, fb->size);

  return true;
}

void DestroyFramebuffer(Framebuffer *fb) {
  if (fb == nullptr) {
    return;
  }

  delete[] fb->pixels;
  fb->pixels = nullptr;
  fb->width = 0;
  fb->height = 0;
  fb->pitch = 0;
  fb->depth = 0;
  fb->size = 0;
}

void ClearFramebuffer(const Framebuffer &fb) {
  if (fb.pixels == nullptr || fb.size == 0) {
    return;
  }

  memset(fb.pixels, 0, fb.size);
  FlushFramebuffer(fb);
}

void FlushFramebuffer(const Framebuffer &fb) {
  if (fb.pixels == nullptr || fb.size == 0) {
    return;
  }

  CleanDataCacheRange((u64)(uintptr)fb.pixels, fb.size);
}

u32 HvsDisplay0Mode() {
  return (ReadReg(kHvsBase, HvsDisp0StatusOffset()) &
          kHvsStatusModeMask) >> kHvsStatusModeShift;
}

bool WaitForVBlank(unsigned timeout_us) {
  const unsigned start = CTimer::GetClockTicks();

  while ((unsigned)(CTimer::GetClockTicks() - start) < timeout_us) {
    if (HvsDisplay0Mode() == kHvsStatusModeEof) {
      return true;
    }
    CTimer::SimpleusDelay(10);
  }

  return false;
}

bool ConfigureScanout(const Framebuffer &fb) {
  if (fb.depth != 16) {
    printf("boot: pi5kms framebuffer scanout supports RGB565 depth 16 only\r\n");
    return false;
  }
  return ConfigureScanout((u32)(uintptr)fb.pixels, fb.pitch, fb.width,
                          fb.height, fb.depth);
}

bool ConfigureScanout(u32 framebuffer_bus_address, u32 pitch, u32 width,
                      u32 height, u32 depth) {
  if (depth != 16) {
    printf("boot: pi5kms scanout supports RGB565 depth 16 only\r\n");
    return false;
  }

  Plane plane = {
    framebuffer_bus_address,
    pitch,
    width,
    height,
    depth,
    kPixelFormatRgb565,
    kScaleFilterNearest,
    {0, 0, width, height},
    {0, 0, width, height}
  };

  return ConfigureScanout(plane, width, height);
}

bool ConfigureScanout(const Plane &plane, u32 display_width,
                      u32 display_height) {
  return ConfigureScanout(&plane, 1, display_width, display_height);
}

bool ConfigureScanout(const Plane *planes, unsigned plane_count,
                      u32 display_width, u32 display_height) {
  return ProgramScanout(planes, plane_count, display_width, display_height,
                        false, true, true);
}

bool PresentScanout(const Plane *planes, unsigned plane_count,
                    u32 display_width, u32 display_height,
                    bool wait_for_vblank) {
  return ProgramScanout(planes, plane_count, display_width, display_height,
                        wait_for_vblank, false, false);
}

}  // namespace pi5kms
