//
// vicesound.cpp
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

#include "vicesound.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <strings.h>
}

#include <circle/devicenameservice.h>
#include <circle/koptions.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/sound/usbsoundbasedevice.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/usb/usbaudiostreaming.h>

namespace {

const unsigned HDMI_CHUNK_WORDS = 384 * 4;

#if BMX_SID_DIAGNOSTICS
void increment(volatile uint32_t *value, uint32_t amount = 1U) {
  __atomic_fetch_add(value, amount, __ATOMIC_RELAXED);
}

void update_max(volatile uint32_t *maximum, uint32_t value) {
  uint32_t current = __atomic_load_n(maximum, __ATOMIC_RELAXED);
  while (current < value &&
         !__atomic_compare_exchange_n(maximum, &current, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

void update_min(volatile uint32_t *minimum, uint32_t value) {
  uint32_t current = __atomic_load_n(minimum, __ATOMIC_RELAXED);
  while (current > value &&
         !__atomic_compare_exchange_n(minimum, &current, value, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
  }
}

class DiagnosticHDMISoundBaseDevice : public CHDMISoundBaseDevice {
public:
  DiagnosticHDMISoundBaseDevice(CInterruptSystem *interrupt,
                                unsigned sample_rate,
                                unsigned chunk_words,
                                ViceSoundDiagnostics *diagnostics)
      : CHDMISoundBaseDevice(interrupt, sample_rate, chunk_words),
        diagnostics_(diagnostics), last_chunk_us_(0U),
        last_underrun_us_(0U) {
    const uint32_t frames = chunk_words / 2U;
    __atomic_store_n(&diagnostics_->hdmi_chunk_frames, frames,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&diagnostics_->hdmi_chunk_expected_us,
                     sample_rate != 0U
                         ? static_cast<uint32_t>(
                               (static_cast<uint64_t>(frames) * 1000000U +
                                sample_rate / 2U) /
                               sample_rate)
                         : 0U,
                     __ATOMIC_RELAXED);
  }

protected:
  unsigned GetChunk(u32 *buffer, unsigned chunk_words) override {
    const uint32_t started_us = CTimer::GetClockTicks();
    increment(&diagnostics_->hdmi_chunk_calls);

    const uint32_t previous_us = last_chunk_us_;
    last_chunk_us_ = started_us;
    if (previous_us != 0U) {
      const uint32_t gap_us = started_us - previous_us;
      update_max(&diagnostics_->hdmi_chunk_gap_max_us, gap_us);
      const uint32_t expected_us = __atomic_load_n(
          &diagnostics_->hdmi_chunk_expected_us, __ATOMIC_RELAXED);
      if (expected_us != 0U &&
          gap_us > expected_us + expected_us / 4U) {
        increment(&diagnostics_->hdmi_chunk_late_calls);
        __atomic_store_n(&diagnostics_->hdmi_chunk_last_late_ms,
                         started_us / 1000U, __ATOMIC_RELAXED);
      }
    }

    const uint32_t required_frames = chunk_words / GetHWTXChannels();
    const uint32_t available_frames = GetQueueFramesAvail();
    __atomic_store_n(&diagnostics_->hdmi_queue_fill_frames,
                     available_frames, __ATOMIC_RELAXED);

    bool armed = __atomic_load_n(&diagnostics_->hdmi_armed,
                                 __ATOMIC_ACQUIRE) != 0U;
    if (!armed && available_frames >= required_frames) {
      __atomic_store_n(&diagnostics_->hdmi_armed, 1U, __ATOMIC_RELEASE);
      armed = true;
    }

    if (armed) {
      const uint32_t remaining_frames =
          available_frames > required_frames
              ? available_frames - required_frames : 0U;
      update_min(&diagnostics_->hdmi_queue_margin_min_frames,
                 remaining_frames);
      if (available_frames < required_frames) {
        const uint32_t missing_frames = required_frames - available_frames;
        increment(&diagnostics_->hdmi_underrun_chunks);
        increment(&diagnostics_->hdmi_underrun_frames, missing_frames);
        __atomic_store_n(&diagnostics_->hdmi_last_underrun_ms,
                         started_us / 1000U, __ATOMIC_RELAXED);
        if (last_underrun_us_ != 0U) {
          const uint32_t interval_us = started_us - last_underrun_us_;
          update_min(&diagnostics_->hdmi_underrun_interval_min_us,
                     interval_us);
          update_max(&diagnostics_->hdmi_underrun_interval_max_us,
                     interval_us);
        }
        last_underrun_us_ = started_us;
      }
    }

    const unsigned result =
        CHDMISoundBaseDevice::GetChunk(buffer, chunk_words);
    update_max(&diagnostics_->hdmi_refill_max_us,
               CTimer::GetClockTicks() - started_us);
    return result;
  }

private:
  ViceSoundDiagnostics *diagnostics_;
  uint32_t last_chunk_us_;
  uint32_t last_underrun_us_;
};
#endif

int clamp_percent(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 100) {
    return 100;
  }
  return value;
}

bool sound_device_matches(const char *configured, const char *name) {
  return configured && strcasecmp(configured, name) == 0;
}

bool sound_device_is_usb(const char *configured) {
  return sound_device_matches(configured, "usb") ||
         sound_device_matches(configured, "sndusb");
}

bool sample_rate_supported(
    const CUSBAudioStreamingDevice::TDeviceInfo &info,
    unsigned sample_rate) {
  for (unsigned i = 0; i < info.SampleRateRanges; i++) {
    const unsigned min_rate = info.SampleRateRange[i].Min;
    const unsigned max_rate = info.SampleRateRange[i].Max;
    const unsigned resolution = info.SampleRateRange[i].Resolution;
    if (min_rate <= sample_rate && sample_rate <= max_rate &&
        (!resolution || (sample_rate - min_rate) % resolution == 0)) {
      return true;
    }
  }

  return false;
}

CUSBAudioStreamingDevice *find_usb_output_device(void) {
  for (unsigned subdevice = 1; ; subdevice++) {
    CString device_name;
    device_name.Format("uaudio1-%u", subdevice);

    CDevice *device =
        CDeviceNameService::Get()->GetDevice(device_name, FALSE);
    if (!device) {
      break;
    }

    CUSBAudioStreamingDevice *streaming_device =
        static_cast<CUSBAudioStreamingDevice *>(device);
    if (streaming_device->GetDeviceInfo().IsOutput) {
      return streaming_device;
    }
  }

  return nullptr;
}

bool start_sound_device(CSoundBaseDevice *device, const char *name,
                        unsigned queue_size_frames, unsigned channels) {
  if (!device->AllocateQueueFrames(queue_size_frames)) {
    printf("boot: sound %s queue allocation failed\r\n", name);
    return false;
  }

  device->SetWriteFormat(SoundFormatSigned16, channels);
  if (!device->Start()) {
    printf("boot: sound %s start failed\r\n", name);
    return false;
  }

  printf("boot: sound %s ready channels %u\r\n", name, channels);
  return true;
}

} // namespace

SoundOutputPriority ViceSound::DefaultOutputPriority(void) {
  const char *sounddev = CKernelOptions::Get()->GetSoundDevice();
  if (sound_device_is_usb(sounddev)) {
    return SOUND_OUTPUT_PRIORITY_USB_HDMI;
  }
  return SOUND_OUTPUT_PRIORITY_HDMI_USB;
}

unsigned ViceSound::SelectSampleRate(void) {
  CUSBAudioStreamingDevice *streaming_device = find_usb_output_device();
  if (streaming_device) {
    CUSBAudioStreamingDevice::TDeviceInfo info =
        streaming_device->GetDeviceInfo();

    static const unsigned preferred_rates[] = {SAMPLE_RATE, 48000};
    for (unsigned i = 0; i < sizeof preferred_rates / sizeof preferred_rates[0];
         i++) {
      if (sample_rate_supported(info, preferred_rates[i])) {
        return preferred_rates[i];
      }
    }

    for (unsigned i = 0; i < info.SampleRateRanges; i++) {
      if (info.SampleRateRange[i].Min) {
        return info.SampleRateRange[i].Min;
      }
    }
  }

  return SAMPLE_RATE;
}

boolean ViceSound::USBOutputAvailable(void) {
  return find_usb_output_device() != nullptr;
}

boolean ViceSound::GetUSBOutputProduct(char *buffer, unsigned buffer_size) {
  CUSBAudioStreamingDevice *streaming_device = find_usb_output_device();
  if (buffer != nullptr && buffer_size > 0) {
    const char *product = streaming_device != nullptr
                              ? streaming_device->GetProperty(
                                    CDevice::PropertyProduct)
                              : nullptr;
    strncpy(buffer, product != nullptr ? product : "", buffer_size - 1U);
    buffer[buffer_size - 1U] = '\0';
  }
  return streaming_device != nullptr;
}

ViceSound::ViceSound(CInterruptSystem *pInterrupt,
                     TVCHIQSoundDestination Destination,
                     unsigned SampleRate)
    : mSoundDevice(nullptr),
      mOutputDevice(OutputNone),
      mInterrupt(pInterrupt),
      mDestination(Destination),
      mSampleRate(SampleRate),
      mQueueSizeFrames(FRAG_SIZE * NUM_FRAGS),
      mNumChannels(2),
      mQueueFillFrames(0),
      mQueueMinimumFillFrames(FRAG_SIZE * NUM_FRAGS),
      mQueueDiagnosticsArmed(FALSE),
      mWriteWaitCount(0U),
      mVolumePercent(100), mDiagnosticsLastSample{0, 0},
      mDiagnosticsLastSampleValid{FALSE, FALSE}, mDiagnosticsZeroRun(0U),
      mDiagnosticsChannelZeroRun{0U, 0U},
      mDiagnosticsConstantRun{0U, 0U} {
  memset(&mDiagnostics, 0, sizeof mDiagnostics);
  mDiagnostics.enabled = BMX_SID_DIAGNOSTICS ? 1U : 0U;
}

ViceSound::~ViceSound(void) {
  CancelPlayback();
}

boolean ViceSound::StartHDMI(void) {
#if BMX_SID_DIAGNOSTICS
  mSoundDevice = new DiagnosticHDMISoundBaseDevice(
      mInterrupt, mSampleRate, HDMI_CHUNK_WORDS, &mDiagnostics);
#else
  mSoundDevice = new CHDMISoundBaseDevice(mInterrupt, mSampleRate,
                                          HDMI_CHUNK_WORDS);
#endif
  if (start_sound_device(mSoundDevice, "hdmi",
                         mQueueSizeFrames, mNumChannels)) {
    mOutputDevice = OutputHDMI;
    return TRUE;
  }

  delete mSoundDevice;
  mSoundDevice = nullptr;
  return FALSE;
}

boolean ViceSound::StartUSB(void) {
  mSoundDevice = new CUSBSoundBaseDevice(mSampleRate,
                                         CUSBSoundBaseDevice::DeviceModeTXOnly,
                                         0);
  if (start_sound_device(mSoundDevice, "usb",
                         mQueueSizeFrames, mNumChannels)) {
    mOutputDevice = OutputUSB;
    return TRUE;
  }

  delete mSoundDevice;
  mSoundDevice = nullptr;
  return FALSE;
}

boolean ViceSound::Playback(int volume, int channels,
                            SoundOutputPriority priority) {
  CancelPlayback();

  mQueueFillFrames = 0;
  mQueueMinimumFillFrames = mQueueSizeFrames;
  mQueueDiagnosticsArmed = FALSE;
  memset(&mDiagnostics, 0, sizeof mDiagnostics);
  mDiagnostics.enabled = BMX_SID_DIAGNOSTICS ? 1U : 0U;
  mDiagnostics.hdmi_queue_margin_min_frames = UINT32_MAX;
  mDiagnostics.hdmi_underrun_interval_min_us = UINT32_MAX;
  mDiagnosticsLastSample[0] = mDiagnosticsLastSample[1] = 0;
  mDiagnosticsLastSampleValid[0] =
      mDiagnosticsLastSampleValid[1] = FALSE;
  mDiagnosticsZeroRun = 0U;
  mDiagnosticsChannelZeroRun[0] = mDiagnosticsChannelZeroRun[1] = 0U;
  mDiagnosticsConstantRun[0] = mDiagnosticsConstantRun[1] = 0U;

  mNumChannels = channels >= 1 ? (unsigned) channels : 1;
  mControlLock.Acquire();
  mVolumePercent = clamp_percent(volume);
  mControlLock.Release();

  boolean usb_output_available = USBOutputAvailable();
  if (priority == SOUND_OUTPUT_PRIORITY_USB_HDMI && usb_output_available) {
    return StartUSB() || StartHDMI();
  }
  if (StartHDMI()) {
    return TRUE;
  }
  return usb_output_available && StartUSB();
}

boolean ViceSound::PlaybackActive(void) const {
  return mSoundDevice != nullptr && mSoundDevice->IsActive();
}

boolean ViceSound::HDMIOutputSelected(void) const {
  return mSoundDevice != nullptr && mOutputDevice == OutputHDMI;
}

boolean ViceSound::USBOutputSelected(void) const {
  return mSoundDevice != nullptr && mOutputDevice == OutputUSB;
}

void ViceSound::USBPlugAndPlayChanged(boolean usbOutputAvailable,
                                      SoundOutputPriority priority) {
  if (PlaybackActive()) {
    if (mOutputDevice == OutputUSB && usbOutputAvailable) {
      return;
    }

    if (   mOutputDevice == OutputHDMI
        && (   priority == SOUND_OUTPUT_PRIORITY_HDMI_USB
            || !usbOutputAvailable)) {
      return;
    }
  }

  const char *previous = mOutputDevice == OutputUSB ? "usb"
                         : mOutputDevice == OutputHDMI ? "hdmi"
                                                       : "none";
  printf("sound: USB topology changed, restarting %s output\r\n", previous);
  SoundOutputPriority restart_priority =
      usbOutputAvailable ? priority : SOUND_OUTPUT_PRIORITY_HDMI_USB;
  Playback(mVolumePercent, mNumChannels, restart_priority);
}

void ViceSound::CancelPlayback(void) {
  if (!mSoundDevice) {
    return;
  }

  mSoundDevice->Cancel();
  delete mSoundDevice;
  mSoundDevice = nullptr;
  mOutputDevice = OutputNone;
}

void ViceSound::SetSampleRate(unsigned SampleRate) {
  assert(!mSoundDevice);
  mSampleRate = SampleRate;
}

void ViceSound::SetControl(int nVolume, TVCHIQSoundDestination Destination) {
  mControlLock.Acquire();
  mVolumePercent = clamp_percent(nVolume);
  if (Destination < VCHIQSoundDestinationUnknown) {
    mDestination = Destination;
  }
  mControlLock.Release();
}

unsigned ViceSound::AddChunk(s16 *pBuffer, unsigned nChunkSize) {
  if (!mSoundDevice) {
    return 0;
  }

#if BMX_SID_DIAGNOSTICS
  const uint32_t write_started_us = CTimer::GetClockTicks();
  const uint32_t previous_write_us = __atomic_exchange_n(
      &mDiagnostics.write_last_us, write_started_us, __ATOMIC_RELAXED);
  increment(&mDiagnostics.write_calls);
  increment(&mDiagnostics.write_frames,
            mNumChannels != 0U ? nChunkSize / mNumChannels : nChunkSize);
  if (previous_write_us != 0U) {
    const uint32_t gap_us = write_started_us - previous_write_us;
    update_max(&mDiagnostics.write_gap_max_us, gap_us);
    if (gap_us > 10000U) {
      increment(&mDiagnostics.write_gap_over_10ms);
      __atomic_store_n(&mDiagnostics.write_last_gap_over_10ms_ms,
                       write_started_us / 1000U, __ATOMIC_RELAXED);
    }
    if (gap_us > 20000U) increment(&mDiagnostics.write_gap_over_20ms);
    if (gap_us > 40000U) increment(&mDiagnostics.write_gap_over_40ms);
  }
  uint32_t waits_this_call = 0U;

  const uint32_t channels = mNumChannels == 0U ? 1U : mNumChannels;
  const uint32_t frames = nChunkSize / channels;
  uint32_t delta_max[2] = {0U, 0U};
  uint32_t delta_over_4096[2] = {0U, 0U};
  uint32_t delta_over_8192[2] = {0U, 0U};
  uint32_t zero_samples[2] = {0U, 0U};
  uint32_t zero_run_max[2] = {mDiagnosticsChannelZeroRun[0],
                              mDiagnosticsChannelZeroRun[1]};
  uint32_t constant_run_max[2] = {mDiagnosticsConstantRun[0],
                                  mDiagnosticsConstantRun[1]};
  uint32_t zero_frames = 0U;
  uint32_t zero_frame_run_max = mDiagnosticsZeroRun;
  for (uint32_t frame = 0U; frame < frames; ++frame) {
    bool zero_frame = true;
    for (uint32_t channel = 0U; channel < channels; ++channel) {
      const s16 sample = pBuffer[frame * channels + channel];
      zero_frame = zero_frame && sample == 0;
      if (channel >= 2U) {
        continue;
      }
      if (sample == 0) {
        ++zero_samples[channel];
        ++mDiagnosticsChannelZeroRun[channel];
        if (mDiagnosticsChannelZeroRun[channel] > zero_run_max[channel]) {
          zero_run_max[channel] = mDiagnosticsChannelZeroRun[channel];
        }
      } else {
        mDiagnosticsChannelZeroRun[channel] = 0U;
      }
      if (mDiagnosticsLastSampleValid[channel]) {
        int32_t delta = static_cast<int32_t>(sample) -
                        mDiagnosticsLastSample[channel];
        const uint32_t absolute_delta =
            static_cast<uint32_t>(delta < 0 ? -delta : delta);
        if (absolute_delta > delta_max[channel]) {
          delta_max[channel] = absolute_delta;
        }
        if (absolute_delta > 4096U) ++delta_over_4096[channel];
        if (absolute_delta > 8192U) ++delta_over_8192[channel];
        if (absolute_delta == 0U) {
          ++mDiagnosticsConstantRun[channel];
          if (mDiagnosticsConstantRun[channel] > constant_run_max[channel]) {
            constant_run_max[channel] = mDiagnosticsConstantRun[channel];
          }
        } else {
          mDiagnosticsConstantRun[channel] = 1U;
        }
      } else {
        mDiagnosticsConstantRun[channel] = 1U;
        if (constant_run_max[channel] == 0U) {
          constant_run_max[channel] = 1U;
        }
      }
      mDiagnosticsLastSample[channel] = sample;
      mDiagnosticsLastSampleValid[channel] = TRUE;
    }
    if (zero_frame) {
      ++zero_frames;
      ++mDiagnosticsZeroRun;
      if (mDiagnosticsZeroRun > zero_frame_run_max) {
        zero_frame_run_max = mDiagnosticsZeroRun;
      }
    } else {
      mDiagnosticsZeroRun = 0U;
    }
  }
  increment(&mDiagnostics.pcm_frames, frames);
  update_max(&mDiagnostics.pcm_delta_max_ch0, delta_max[0]);
  update_max(&mDiagnostics.pcm_delta_max_ch1, delta_max[1]);
  increment(&mDiagnostics.pcm_delta_over_4096_ch0, delta_over_4096[0]);
  increment(&mDiagnostics.pcm_delta_over_4096_ch1, delta_over_4096[1]);
  increment(&mDiagnostics.pcm_delta_over_8192_ch0, delta_over_8192[0]);
  increment(&mDiagnostics.pcm_delta_over_8192_ch1, delta_over_8192[1]);
  increment(&mDiagnostics.pcm_zero_frames, zero_frames);
  update_max(&mDiagnostics.pcm_zero_run_max, zero_frame_run_max);
  increment(&mDiagnostics.pcm_zero_samples_ch0, zero_samples[0]);
  increment(&mDiagnostics.pcm_zero_samples_ch1, zero_samples[1]);
  update_max(&mDiagnostics.pcm_zero_run_max_ch0, zero_run_max[0]);
  update_max(&mDiagnostics.pcm_zero_run_max_ch1, zero_run_max[1]);
  update_max(&mDiagnostics.pcm_constant_run_max_ch0, constant_run_max[0]);
  update_max(&mDiagnostics.pcm_constant_run_max_ch1, constant_run_max[1]);
#endif

  size_t total_bytes = (size_t) nChunkSize * BYTES_PER_SAMPLE;
  const s16 *write_buffer = pBuffer;
  s16 *scaled_buffer = nullptr;
  mControlLock.Acquire();
  int volume_percent = mVolumePercent;
  mControlLock.Release();

  if (volume_percent != 100) {
    scaled_buffer = (s16 *) malloc(total_bytes);
    if (!scaled_buffer) {
      return 0;
    }

    for (unsigned i = 0; i < nChunkSize; i++) {
      scaled_buffer[i] = (s16) ((int) pBuffer[i] * volume_percent / 100);
    }
    write_buffer = scaled_buffer;
  }

  size_t written = 0;
  while (written < total_bytes) {
    if (!mSoundDevice->IsActive()) {
      break;
    }

    int consumed = mSoundDevice->Write(((const char *) write_buffer) + written,
                                       total_bytes - written);
    if (consumed > 0) {
      written += (size_t) consumed;
      continue;
    }
    ++mWriteWaitCount;
#if BMX_SID_DIAGNOSTICS
    ++waits_this_call;
#endif
    // The audio queue is drained by the device/IRQ path. In multicore builds
    // Circle's scheduler belongs to core 0, so the VICE core waits only for
    // device progress and never enters the scheduler here.
    CTimer::SimpleusDelay(50U);
  }

  if (scaled_buffer) {
    free(scaled_buffer);
  }

#if BMX_SID_DIAGNOSTICS
  const uint32_t write_duration_us =
      CTimer::GetClockTicks() - write_started_us;
  update_max(&mDiagnostics.write_duration_max_us, write_duration_us);
  if (waits_this_call != 0U) {
    increment(&mDiagnostics.write_blocked_calls);
    update_max(&mDiagnostics.write_blocked_max_us, write_duration_us);
  }
  if (written != total_bytes) {
    increment(&mDiagnostics.write_short_calls);
  }
#endif

  if (written != 0U && mSoundDevice->IsActive()) {
    const unsigned used_frames = mSoundDevice->GetQueueFramesAvail();
    mQueueFillFrames = used_frames;
    if (!mQueueDiagnosticsArmed) {
      mQueueMinimumFillFrames = used_frames;
      mQueueDiagnosticsArmed = TRUE;
    } else if (used_frames < mQueueMinimumFillFrames) {
      mQueueMinimumFillFrames = used_frames;
    }
#if BMX_SID_DIAGNOSTICS
    if (mOutputDevice == OutputHDMI &&
        __atomic_load_n(&mDiagnostics.hdmi_armed,
                        __ATOMIC_ACQUIRE) == 0U) {
      __atomic_store_n(&mDiagnostics.hdmi_armed, 1U, __ATOMIC_RELEASE);
    }
#endif
  }

  return 0;
}

unsigned ViceSound::BufferSpaceSamples() {
  if (!mSoundDevice || !mSoundDevice->IsActive()) {
    return mQueueSizeFrames;
  }

  unsigned used_frames = mSoundDevice->GetQueueFramesAvail();
  if (used_frames >= mQueueSizeFrames) {
    mQueueFillFrames = mQueueSizeFrames;
    if (mQueueDiagnosticsArmed &&
        mQueueFillFrames < mQueueMinimumFillFrames) {
      mQueueMinimumFillFrames = mQueueFillFrames;
    }
    return 0;
  }

  mQueueFillFrames = used_frames;
  if (mQueueDiagnosticsArmed &&
      mQueueFillFrames < mQueueMinimumFillFrames) {
    mQueueMinimumFillFrames = mQueueFillFrames;
  }

  return mQueueSizeFrames - used_frames;
}

void ViceSound::GetDiagnostics(ViceSoundDiagnostics *diagnostics) const {
  if (diagnostics == nullptr) {
    return;
  }

#define LOAD_DIAGNOSTIC(field)                                                \
  diagnostics->field =                                                       \
      __atomic_load_n(&mDiagnostics.field, __ATOMIC_RELAXED)
  LOAD_DIAGNOSTIC(enabled);
  LOAD_DIAGNOSTIC(write_calls);
  LOAD_DIAGNOSTIC(write_frames);
  LOAD_DIAGNOSTIC(write_gap_max_us);
  LOAD_DIAGNOSTIC(write_gap_over_10ms);
  LOAD_DIAGNOSTIC(write_gap_over_20ms);
  LOAD_DIAGNOSTIC(write_gap_over_40ms);
  LOAD_DIAGNOSTIC(write_last_gap_over_10ms_ms);
  LOAD_DIAGNOSTIC(write_duration_max_us);
  LOAD_DIAGNOSTIC(write_blocked_calls);
  LOAD_DIAGNOSTIC(write_blocked_max_us);
  LOAD_DIAGNOSTIC(write_short_calls);
  LOAD_DIAGNOSTIC(write_last_us);
  LOAD_DIAGNOSTIC(hdmi_armed);
  LOAD_DIAGNOSTIC(hdmi_chunk_frames);
  LOAD_DIAGNOSTIC(hdmi_chunk_expected_us);
  LOAD_DIAGNOSTIC(hdmi_chunk_calls);
  LOAD_DIAGNOSTIC(hdmi_chunk_gap_max_us);
  LOAD_DIAGNOSTIC(hdmi_chunk_late_calls);
  LOAD_DIAGNOSTIC(hdmi_chunk_last_late_ms);
  LOAD_DIAGNOSTIC(hdmi_refill_max_us);
  LOAD_DIAGNOSTIC(hdmi_queue_fill_frames);
  LOAD_DIAGNOSTIC(hdmi_queue_margin_min_frames);
  LOAD_DIAGNOSTIC(hdmi_underrun_chunks);
  LOAD_DIAGNOSTIC(hdmi_underrun_frames);
  LOAD_DIAGNOSTIC(hdmi_last_underrun_ms);
  LOAD_DIAGNOSTIC(hdmi_underrun_interval_min_us);
  LOAD_DIAGNOSTIC(hdmi_underrun_interval_max_us);
  LOAD_DIAGNOSTIC(pcm_frames);
  LOAD_DIAGNOSTIC(pcm_delta_max_ch0);
  LOAD_DIAGNOSTIC(pcm_delta_max_ch1);
  LOAD_DIAGNOSTIC(pcm_delta_over_4096_ch0);
  LOAD_DIAGNOSTIC(pcm_delta_over_4096_ch1);
  LOAD_DIAGNOSTIC(pcm_delta_over_8192_ch0);
  LOAD_DIAGNOSTIC(pcm_delta_over_8192_ch1);
  LOAD_DIAGNOSTIC(pcm_zero_frames);
  LOAD_DIAGNOSTIC(pcm_zero_run_max);
  LOAD_DIAGNOSTIC(pcm_zero_samples_ch0);
  LOAD_DIAGNOSTIC(pcm_zero_samples_ch1);
  LOAD_DIAGNOSTIC(pcm_zero_run_max_ch0);
  LOAD_DIAGNOSTIC(pcm_zero_run_max_ch1);
  LOAD_DIAGNOSTIC(pcm_constant_run_max_ch0);
  LOAD_DIAGNOSTIC(pcm_constant_run_max_ch1);
#undef LOAD_DIAGNOSTIC

  if (!diagnostics->hdmi_armed ||
      diagnostics->hdmi_queue_margin_min_frames == UINT32_MAX) {
    diagnostics->hdmi_queue_margin_min_frames = 0U;
  }
  if (diagnostics->hdmi_underrun_interval_min_us == UINT32_MAX) {
    diagnostics->hdmi_underrun_interval_min_us = 0U;
  }
}
