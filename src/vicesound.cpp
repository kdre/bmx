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
      mWriteWaitCount(0U),
      mVolumePercent(100) {}

ViceSound::~ViceSound(void) {
  CancelPlayback();
}

boolean ViceSound::StartHDMI(void) {
  mSoundDevice = new CHDMISoundBaseDevice(mInterrupt, mSampleRate,
                                          HDMI_CHUNK_WORDS);
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
    // The audio queue is drained by the device/IRQ path. On Pi 5 the Circle
    // scheduler belongs to core 0, so the VICE core waits only for device
    // progress and never enters the scheduler here.
    CTimer::SimpleusDelay(50U);
  }

  if (scaled_buffer) {
    free(scaled_buffer);
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
    if (mQueueFillFrames < mQueueMinimumFillFrames) {
      mQueueMinimumFillFrames = mQueueFillFrames;
    }
    return 0;
  }

  mQueueFillFrames = used_frames;
  if (mQueueFillFrames < mQueueMinimumFillFrames) {
    mQueueMinimumFillFrames = mQueueFillFrames;
  }

  return mQueueSizeFrames - used_frames;
}
