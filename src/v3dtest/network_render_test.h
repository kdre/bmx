#ifndef BMX_V3DTEST_NETWORK_RENDER_TEST_H
#define BMX_V3DTEST_NETWORK_RENDER_TEST_H

#include "remote/bmx_api_types.h"

#include <stdint.h>

namespace bmx_v3d_test {

// Runs on Core 1 after the normal BMX storage, network and RemoteService
// initialization has completed. This function does not return.
void RunNetworkRenderTest();

// Safe points used by the matrix runners so an uploaded replacement kernel
// can be rebooted into even while a long mode test is in progress.
void PollRemoteControl();
void PublishProgress(const char *phase, const char *current_case);
bool CaptureReviewScreenshot(uint32_t maximum_width,
                             bmx::remote::BmxBinaryPayload *payload);

}  // namespace bmx_v3d_test

#endif  // BMX_V3DTEST_NETWORK_RENDER_TEST_H
