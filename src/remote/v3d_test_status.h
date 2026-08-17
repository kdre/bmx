#ifndef BMX_REMOTE_V3D_TEST_STATUS_H
#define BMX_REMOTE_V3D_TEST_STATUS_H

#include <stdint.h>

namespace bmx {
namespace remote {

static const unsigned kV3dTestPhaseBytes = 32U;
static const unsigned kV3dTestCaseBytes = 128U;
static const unsigned kV3dTestResultBytes = 32U;
static const unsigned kV3dTestReviewCaseBytes = 128U;
static const unsigned kV3dTestReviewErrorBytes = 64U;

enum class V3dTestReviewAction : uint8_t {
  None = 0,
  Show,
  First,
  Last,
  Next,
  Previous,
  Continue
};

enum class V3dTestReviewRequestStatus : uint8_t {
  Accepted = 0,
  InvalidIndex,
  Busy,
  Unavailable
};

struct V3dTestReviewRequest {
  V3dTestReviewAction action;
  uint32_t index;
};

struct V3dTestStatusSnapshot {
  bool enabled;
  bool running;
  bool complete;
  char backend[16U];
  char phase[kV3dTestPhaseBytes];
  char current_case[kV3dTestCaseBytes];
  char result[kV3dTestResultBytes];
  uint32_t passed;
  uint32_t failed;
  uint32_t skipped;
  uint32_t unbaselined;
  uint32_t kms_passed;
  bool review_available;
  bool review_active;
  bool screenshot_available;
  uint32_t review_index;
  uint32_t review_total;
  uint32_t review_generation;
  char review_case[kV3dTestReviewCaseBytes];
  char review_error[kV3dTestReviewErrorBytes];
};

void PublishV3dTestStatus(const V3dTestStatusSnapshot &status);
bool ReadV3dTestStatus(V3dTestStatusSnapshot *status);
V3dTestReviewRequestStatus RequestV3dTestReview(
    V3dTestReviewAction action, uint32_t index);
bool TakeV3dTestReviewRequest(V3dTestReviewRequest *request);

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_V3D_TEST_STATUS_H
