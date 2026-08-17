#include "remote/v3d_test_status.h"

#include <string.h>

namespace bmx {
namespace remote {

namespace {

volatile uint32_t g_sequence = 0U;
V3dTestStatusSnapshot g_status = {};
volatile uint32_t g_review_action = 0U;
volatile uint32_t g_review_index = 0U;
volatile uint32_t g_review_posting = 0U;

}  // namespace

void PublishV3dTestStatus(const V3dTestStatusSnapshot &status) {
  __atomic_add_fetch(&g_sequence, 1U, __ATOMIC_SEQ_CST);
  memcpy(&g_status, &status, sizeof g_status);
  __atomic_add_fetch(&g_sequence, 1U, __ATOMIC_SEQ_CST);
}

bool ReadV3dTestStatus(V3dTestStatusSnapshot *status) {
  if (status == nullptr) {
    return false;
  }
  for (;;) {
    const uint32_t before = __atomic_load_n(&g_sequence, __ATOMIC_ACQUIRE);
    if ((before & 1U) != 0U) {
      continue;
    }
    memcpy(status, &g_status, sizeof *status);
    const uint32_t after = __atomic_load_n(&g_sequence, __ATOMIC_ACQUIRE);
    if (before == after) {
      return true;
    }
  }
}

V3dTestReviewRequestStatus RequestV3dTestReview(
    V3dTestReviewAction action, uint32_t index) {
  if (action == V3dTestReviewAction::None) {
    return V3dTestReviewRequestStatus::Unavailable;
  }
  V3dTestStatusSnapshot status = {};
  if (!ReadV3dTestStatus(&status) || !status.enabled ||
      !status.review_available || status.review_total == 0U) {
    return V3dTestReviewRequestStatus::Unavailable;
  }
  if (action == V3dTestReviewAction::Show && index >= status.review_total) {
    return V3dTestReviewRequestStatus::InvalidIndex;
  }
  uint32_t unlocked = 0U;
  if (!__atomic_compare_exchange_n(&g_review_posting, &unlocked, 1U, false,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return V3dTestReviewRequestStatus::Busy;
  }
  if (__atomic_load_n(&g_review_action, __ATOMIC_ACQUIRE) !=
      static_cast<uint32_t>(V3dTestReviewAction::None)) {
    __atomic_store_n(&g_review_posting, 0U, __ATOMIC_RELEASE);
    return V3dTestReviewRequestStatus::Busy;
  }
  __atomic_store_n(&g_review_index, index, __ATOMIC_RELAXED);
  uint32_t expected = static_cast<uint32_t>(V3dTestReviewAction::None);
  if (!__atomic_compare_exchange_n(
          &g_review_action, &expected, static_cast<uint32_t>(action), false,
          __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&g_review_posting, 0U, __ATOMIC_RELEASE);
    return V3dTestReviewRequestStatus::Busy;
  }
  __atomic_store_n(&g_review_posting, 0U, __ATOMIC_RELEASE);
  return V3dTestReviewRequestStatus::Accepted;
}

bool TakeV3dTestReviewRequest(V3dTestReviewRequest *request) {
  if (request == nullptr) {
    return false;
  }
  uint32_t action = __atomic_load_n(&g_review_action, __ATOMIC_ACQUIRE);
  if (action == static_cast<uint32_t>(V3dTestReviewAction::None)) {
    return false;
  }
  const uint32_t index = __atomic_load_n(&g_review_index, __ATOMIC_RELAXED);
  if (!__atomic_compare_exchange_n(
          &g_review_action, &action,
          static_cast<uint32_t>(V3dTestReviewAction::None), false,
          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return false;
  }
  request->action = static_cast<V3dTestReviewAction>(action);
  request->index = index;
  return true;
}

}  // namespace remote
}  // namespace bmx
