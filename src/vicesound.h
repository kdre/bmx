//
// vicesound.h
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

#ifndef _vice_sound_h
#define _vice_sound_h

#include "defs.h"
#include "sound_output_priority.h"
#include "sound_types.h"
#include <circle/interrupt.h>
#include <circle/spinlock.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/types.h>

#include <stdint.h>

// This is the fragment size we give to vice.
#define FRAG_SIZE 256

// This is the number of fragments we want for our buffer.
#define NUM_FRAGS 16

// 16 bit sound means this many bytes per sample.
#define BYTES_PER_SAMPLE 2

class ViceSound {
public:
  ViceSound(CInterruptSystem *pInterrupt,
            TVCHIQSoundDestination Destination = VCHIQSoundDestinationAuto,
            unsigned SampleRate = SAMPLE_RATE);

  ~ViceSound(void);

  static SoundOutputPriority DefaultOutputPriority(void);
  static unsigned SelectSampleRate(void);
  static boolean USBOutputAvailable(void);
  static boolean GetUSBOutputProduct(char *buffer, unsigned buffer_size);

  boolean Playback(int volume, int channels, SoundOutputPriority priority);
  boolean PlaybackActive(void) const;
  boolean HDMIOutputSelected(void) const;
  boolean USBOutputSelected(void) const;
  void USBPlugAndPlayChanged(boolean usbOutputAvailable,
                             SoundOutputPriority priority);
  void CancelPlayback(void);
  void SetSampleRate(unsigned SampleRate);
  void SetControl(int nVolume,
                  TVCHIQSoundDestination Destination = VCHIQSoundDestinationUnknown);
  unsigned AddChunk(s16 *pBuffer, unsigned nChunkSize);
  unsigned BufferSpaceSamples();
  unsigned QueueSizeFrames(void) const { return mQueueSizeFrames; }
  unsigned QueueFillFrames(void) const { return mQueueFillFrames; }
  unsigned QueueMinimumFillFrames(void) const { return mQueueMinimumFillFrames; }
  uint64_t WriteWaitCount(void) const { return mWriteWaitCount; }

private:
  enum OutputDevice {
    OutputNone,
    OutputHDMI,
    OutputUSB
  };

  CSoundBaseDevice *mSoundDevice;
  boolean StartHDMI(void);
  boolean StartUSB(void);
  OutputDevice mOutputDevice;
  CInterruptSystem *mInterrupt;
  TVCHIQSoundDestination mDestination;
  unsigned mSampleRate;
  unsigned mQueueSizeFrames;
  unsigned mNumChannels;
  unsigned mQueueFillFrames;
  unsigned mQueueMinimumFillFrames;
  uint64_t mWriteWaitCount;
  int mVolumePercent;
  CSpinLock mControlLock;
};

#endif // VICE_SOUND_H
