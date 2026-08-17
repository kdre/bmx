//
// viceemulatorcore.cpp
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

#include "viceemulatorcore.h"

#include "machines/machine_descriptor.h"
#include "vice_api.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "defs.h"

#if BMX_V3D_RENDER_TEST_KERNEL
#include "v3dtest/network_render_test.h"
#endif

#if defined(BMX_SID_WORKER) || defined(BMX_SID_DIAGNOSTICS)
#include "sidworker.h"
#endif

extern "C" {
#include "version.h"
#include "third_party/common/semaphore.h"

extern void circle_kernel_core_init_complete(int core);
}

#include "sid/sid.h"
#include "resid/sid.h"
#include "resid/filter.h"

#if !defined(VICE_VERSION_MAJOR) || \
    (VICE_VERSION_MAJOR < 3) || \
    (VICE_VERSION_MAJOR == 3 && VICE_VERSION_MINOR < 10)
#define BMC64_USE_RESID_PRECOMPUTE 1
#endif

ViceEmulatorCore::ViceEmulatorCore(CMemorySystem *pMemorySystem,
                                   int cyclesPerSecond) :
#ifdef BMC64_USE_EMU_MULTICORE
       CMultiCoreSupport(pMemorySystem),
#endif
       launch_(false), cyclesPerSecond_(cyclesPerSecond) {

  // These calls only allocate the sampling table. Population is
  // done by cores 1 and 2 in parellel below.
#if defined(BMC64_USE_EMU_MULTICORE) && !BMX_V3D_RENDER_TEST_KERNEL

#ifdef BMC64_USE_RESID_PRECOMPUTE
  passBandFreq_ = 19845; // 90%
  //unsigned clock = circle_get_arm_clock();
  //if (clock < 1400000000) {
     // For Pi3 models with a lower clock rate (<= 1.2Ghz)
     // we must lower the passband freq to avoid stuttering
     // in the worst case.  This logic must match the passband
     // percentage we set in menu.c in common dir.
     passBandFreq_ = 13230; // 60%
  //}

  reSID::SID::ComputeSamplingTable(cyclesPerSecond_,
                                   reSID::SAMPLE_RESAMPLE,
                                   SAMPLE_RATE, passBandFreq_, 0.97,
                                   0);
  reSID::SID::ComputeSamplingTable(cyclesPerSecond_,
                                   reSID::SAMPLE_RESAMPLE_FASTMEM,
                                   SAMPLE_RATE, passBandFreq_, 0.97,
                                   0);
#endif
#endif
}

ViceEmulatorCore::~ViceEmulatorCore(void) {}

void ViceEmulatorCore::WaitForLaunch() {
  printf("Core waiting for launch\n");
  bool waiting = true;
  while (waiting) {
    m_Lock.Acquire();
    if (launch_) {
      waiting = false;
    }
    m_Lock.Release();
  }
}

void ViceEmulatorCore::RunMainVice(bool wait) {
  if (wait) {
    WaitForLaunch();
  }

  // Call Vice's main_program

  // Use -soundsync 0 option for 'flexible'
  // sound sync.

  // Use -refresh 1 option to turn off the 'auto'
  // refresh which screws up badly after some time.
  // The vertical blank really messes up vice's
  // algorithm that decides to skip frames. Might
  // want to go back to using the open gl hook.
  // See arch/raspi/videoarch.c

  printf("Starting emulator main loop\n");

  bmc64::vice::RunMainProgram(timing_option_, m_options,
                              bmc64::CurrentMachine());
}

// Initializing the filters for each SID model takes quite a bit.
// This method instantiates a modified Filter object in ReSid.  The
// modified version lets us initialize both SIDs in parallel on seperate
// cores.  This saves a lot of boot time since when VICE eventually gets
// around to initializing the filters, they are already done and opening
// ReSid is very quick at that point.  See resid/filter.cc for
// modifications done for BMC64.
void ViceEmulatorCore::ComputeResidFilter(int model) {
#ifdef BMC64_USE_RESID_PRECOMPUTE
  reSID::Filter f(model);
#else
  (void)model;
#endif
}

// In addition to initializing the filters in parellel during boot, we
// compute the resampling tables for the two resampling methods.
void ViceEmulatorCore::Run(unsigned nCore) {
  assert(nCore > 0);
  switch (nCore) {
  case 1:
#if BMX_V3D_RENDER_TEST_KERNEL
    printf("multicore: core 1 role=v3d-render-test\r\n");
    WaitForLaunch();
    bmx_v3d_test::RunNetworkRenderTest();
#else
    printf("multicore: core 1 role=vice\r\n");
    RunMainVice(true);
#endif
    break;
  case 2:
    // Core 2 will initialize 6581 filter data. Then partition 1
    // of the resampling tables. Then sleep.
#ifdef BMC64_USE_EMU_MULTICORE
#if defined(BMC64_USE_RESID_PRECOMPUTE) && !BMX_V3D_RENDER_TEST_KERNEL
    ComputeResidFilter(0);
    reSID::SID::ComputeSamplingTable(cyclesPerSecond_,
                                     reSID::SAMPLE_RESAMPLE,
                                     SAMPLE_RATE, passBandFreq_, 0.97,
                                     1);
    reSID::SID::ComputeSamplingTable(cyclesPerSecond_,
                                     reSID::SAMPLE_RESAMPLE_FASTMEM,
                                     SAMPLE_RATE, passBandFreq_, 0.97,
                                     1);
#endif
    circle_kernel_core_init_complete(2);
#endif
    break;
  case 3:
    // Core 3 will initialize 8580 filter data. Then partition 2
    // of the resampling tables. Then sleep.
#ifdef BMC64_USE_EMU_MULTICORE
#if defined(BMC64_USE_RESID_PRECOMPUTE) && !BMX_V3D_RENDER_TEST_KERNEL
    ComputeResidFilter(1);
    reSID::SID::ComputeSamplingTable(cyclesPerSecond_,
                                     reSID::SAMPLE_RESAMPLE,
                                     SAMPLE_RATE, passBandFreq_, 0.97,
                                     2);
    reSID::SID::ComputeSamplingTable(cyclesPerSecond_,
                                     reSID::SAMPLE_RESAMPLE_FASTMEM,
                                     SAMPLE_RATE, passBandFreq_, 0.97,
                                     2);
#endif
    circle_kernel_core_init_complete(3);
#endif
    break;
  }

  #if defined(BMC64_USE_EMU_MULTICORE) && defined(RASPI_COMPILE) && \
      defined(BMC64_USE_RESID_PRECOMPUTE) && !BMX_V3D_RENDER_TEST_KERNEL
  if (nCore == 2) {
     while (true) {
        sem_dec(&sid_job);
        sid_job_func(sid_job_psid, sid_job_pbuf, sid_job_nr,
                     2, &sid_job_delta_t);
        sem_inc(&sid_done);
     }
  }
  #endif

  #if defined(BMC64_USE_EMU_MULTICORE) && BMX_SID_WORKER && \
      !BMX_V3D_RENDER_TEST_KERNEL && \
      !defined(BMC64_USE_RESID_PRECOMPUTE)
  if (nCore == 2) {
    printf("multicore: core 2 role=sid-worker\r\n");
    bmx_sid_worker_run();
  }
  #endif

#ifdef BMC64_USE_EMU_MULTICORE
  printf("Core %d idle\n", nCore);
#if AARCH == 64
  asm("dsb sy\n\t"
      "1: wfi\n\t"
      "b 1b\n\t");
#else
  asm("dsb\n\t"
      "1: wfi\n\t"
      "b 1b\n\t");
#endif
#endif
}

bool ViceEmulatorCore::Init(ViceOptions* options) {
  m_options = options;
  return Initialize();
}

void ViceEmulatorCore::LaunchEmulator(char *timing_option) {
  snprintf(timing_option_, sizeof timing_option_, "%s", timing_option);
#ifdef BMC64_USE_EMU_MULTICORE
  m_Lock.Acquire();
  launch_ = true;
  m_Lock.Release();
#else
  RunMainVice(false);
#endif
}
