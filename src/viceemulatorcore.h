//
// viceemulatorcore.h
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

#ifndef viceemulatorcore_h
#define viceemulatorcore_h

#include <circle/memory.h>
#include <circle/multicore.h>
#include <circle/spinlock.h>
#include "emulatorcore.h"

extern "C" {
#include "third_party/common/circle.h"
}

#ifndef BMX_EMU_MULTICORE
#define BMX_EMU_MULTICORE 0
#endif

#if defined(ARM_ALLOW_MULTI_CORE) && BMX_EMU_MULTICORE
#define BMC64_USE_EMU_MULTICORE 1
#endif

class ViceEmulatorCore
 : public EmulatorCore
#ifdef BMC64_USE_EMU_MULTICORE
   ,public CMultiCoreSupport
#endif
{
public:
  ViceEmulatorCore(CMemorySystem *pMemorySystem, int cyclesPerSecond);
  ~ViceEmulatorCore(void);

  void Run(unsigned nCore)
#ifdef BMC64_USE_EMU_MULTICORE
      override
#endif
  ;

#ifndef BMC64_USE_EMU_MULTICORE
  bool Initialize() { return true; }
#endif

  bool Init(ViceOptions* options) override;
  void LaunchEmulator(char *timing_option) override;

private:
  bool launch_;
  int cyclesPerSecond_;
  int passBandFreq_;
  char timing_option_[8];
  CSpinLock m_Lock;
  ViceOptions *m_options;

  void RunMainVice(bool wait);
  void WaitForLaunch();
  void ComputeResidFilter(int model);
};

#endif
