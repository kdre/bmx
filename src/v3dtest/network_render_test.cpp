#include "v3dtest/network_render_test.h"

#include "fbl.h"
#include "remote/v3d_test_status.h"
#include "tools/pi5/v3d-render-test/kms_mode_runner.h"
#include "tools/pi5/v3d-render-test/render_matrix_runner.h"
#include "v3dcrt/v3d_crt.h"
#include "v3dtest/review_capture.h"

#include <circle/timer.h>

#include <stdio.h>
#include <string.h>

extern "C" void circle_v3d_test_poll_remote();

namespace bmx_v3d_test {

namespace {

constexpr uint16_t kPassColor = 0x07E0U;
constexpr uint16_t kUnbaselinedColor = 0xFFE0U;
constexpr uint16_t kFailColor = 0xF800U;

bmx::remote::V3dTestStatusSnapshot g_status = {};
volatile uint32_t g_review_frame_lock = 0U;
bool g_review_frame_valid = false;
ReviewImage g_review_frame = {};

bool TryLockReviewFrame() {
  uint32_t unlocked = 0U;
  return __atomic_compare_exchange_n(&g_review_frame_lock, &unlocked, 1U,
                                     false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED);
}

void LockReviewFrame() {
  while (!TryLockReviewFrame()) {
    asm volatile("yield" ::: "memory");
  }
}

void UnlockReviewFrame() {
  __atomic_store_n(&g_review_frame_lock, 0U, __ATOMIC_RELEASE);
}

const char *BackendName() {
#if RASPPI == 5
  return "v3d71";
#else
  return "v3d42";
#endif
}

void CopyText(char *target, size_t capacity, const char *source) {
  if (capacity == 0U) {
    return;
  }
  snprintf(target, capacity, "%s", source != nullptr ? source : "");
}

void Publish() {
  bmx::remote::PublishV3dTestStatus(g_status);
}

void ShowResult(pi5kms::Framebuffer *framebuffer, bool failed,
                bool unbaselined) {
  if (framebuffer == nullptr || framebuffer->pixels == nullptr ||
      framebuffer->depth != 16U) {
    return;
  }
  const uint16_t color = failed ? kFailColor :
      (unbaselined ? kUnbaselinedColor : kPassColor);
  for (uint32_t y = 0; y < framebuffer->height; ++y) {
    uint16_t *row = reinterpret_cast<uint16_t *>(
        framebuffer->pixels + y * framebuffer->pitch);
    for (uint32_t x = 0; x < framebuffer->width; ++x) {
      const bool border = x < 24U || y < 24U ||
          x + 24U >= framebuffer->width ||
          y + 24U >= framebuffer->height;
      row[x] = border ? 0xFFFFU : color;
    }
  }
  pi5kms::FlushFramebuffer(*framebuffer);
  (void)pi5kms::ConfigureScanout(*framebuffer);
}

bool PresentReview(pi5kms::Framebuffer *framebuffer,
                   const RenderReviewOutput &review) {
  if (framebuffer == nullptr || framebuffer->width == 0U ||
      framebuffer->height == 0U || review.pixels == nullptr ||
      review.plane.framebuffer_bus_address == 0U ||
      review.display_width == 0U || review.display_height == 0U) {
    return false;
  }

  uint32_t fitted_width = framebuffer->width;
  uint32_t fitted_height = static_cast<uint32_t>(
      static_cast<uint64_t>(review.display_height) * fitted_width /
      review.display_width);
  if (fitted_height > framebuffer->height) {
    fitted_height = framebuffer->height;
    fitted_width = static_cast<uint32_t>(
        static_cast<uint64_t>(review.display_width) * fitted_height /
        review.display_height);
  }
  if (fitted_width == 0U || fitted_height == 0U) {
    return false;
  }
  const uint32_t viewport_x = (framebuffer->width - fitted_width) / 2U;
  const uint32_t viewport_y = (framebuffer->height - fitted_height) / 2U;
  const uint32_t left = viewport_x + static_cast<uint32_t>(
      static_cast<uint64_t>(review.destination_x) * fitted_width /
      review.display_width);
  const uint32_t top = viewport_y + static_cast<uint32_t>(
      static_cast<uint64_t>(review.destination_y) * fitted_height /
      review.display_height);
  const uint32_t right = viewport_x + static_cast<uint32_t>(
      static_cast<uint64_t>(review.destination_x + review.destination_width) *
      fitted_width / review.display_width);
  const uint32_t bottom = viewport_y + static_cast<uint32_t>(
      static_cast<uint64_t>(review.destination_y +
                            review.destination_height) *
      fitted_height / review.display_height);
  if (right <= left || bottom <= top) {
    return false;
  }

  pi5kms::Plane plane = review.plane;
  plane.source = {0, 0, review.width, review.height};
  plane.destination = {
    static_cast<s32>(left), static_cast<s32>(top), right - left, bottom - top
  };
  return pi5kms::PresentScanout(&plane, 1U, framebuffer->width,
                                framebuffer->height, true);
}

uint32_t ReviewTargetIndex(
    const bmx::remote::V3dTestReviewRequest &request) {
  const uint32_t total = g_status.review_total;
  switch (request.action) {
    case bmx::remote::V3dTestReviewAction::Show:
      return request.index;
    case bmx::remote::V3dTestReviewAction::First:
      return 0U;
    case bmx::remote::V3dTestReviewAction::Last:
      return total - 1U;
    case bmx::remote::V3dTestReviewAction::Next:
      return g_status.review_active
          ? (g_status.review_index + 1U) % total : 0U;
    case bmx::remote::V3dTestReviewAction::Previous:
      return g_status.review_active
          ? (g_status.review_index + total - 1U) % total : total - 1U;
    case bmx::remote::V3dTestReviewAction::None:
    case bmx::remote::V3dTestReviewAction::Continue:
      return 0U;
  }
  return 0U;
}

void ProcessReviewRequest(pi5kms::Framebuffer *framebuffer, bool failed,
                          bool unbaselined) {
  bmx::remote::V3dTestReviewRequest request = {};
  if (!bmx::remote::TakeV3dTestReviewRequest(&request)) {
    return;
  }
  CopyText(g_status.review_error, sizeof g_status.review_error, "");

  if (request.action == bmx::remote::V3dTestReviewAction::Continue) {
    LockReviewFrame();
    g_review_frame_valid = false;
    memset(&g_review_frame, 0, sizeof g_review_frame);
    ShowResult(framebuffer, failed, unbaselined);
    UnlockReviewFrame();
    g_status.review_active = false;
    g_status.screenshot_available = false;
    CopyText(g_status.phase, sizeof g_status.phase, "idle");
    CopyText(g_status.current_case, sizeof g_status.current_case, "");
    CopyText(g_status.review_case, sizeof g_status.review_case, "");
    ++g_status.review_generation;
    Publish();
    printf("V3DTEST REVIEW action=continue status=PASS\r\n");
    return;
  }

  const uint32_t target = ReviewTargetIndex(request);
  char pending[32U];
  snprintf(pending, sizeof pending, "index=%u", target);
  CopyText(g_status.phase, sizeof g_status.phase, "review-rendering");
  CopyText(g_status.current_case, sizeof g_status.current_case, pending);
  Publish();

  RenderReviewOutput review = {};
  char label[bmx::remote::kV3dTestReviewCaseBytes];
  LockReviewFrame();
  g_review_frame_valid = false;
  memset(&g_review_frame, 0, sizeof g_review_frame);
  const bool rendered = RenderReviewCase(
      target, framebuffer, &review, label, sizeof label);
  const bool presented = rendered && PresentReview(framebuffer, review);
  if (presented) {
    g_review_frame.pixels = review.pixels;
    g_review_frame.width = review.width;
    g_review_frame.height = review.height;
    g_review_frame.pitch = review.pitch;
    g_review_frame.display_width = review.display_width;
    g_review_frame.display_height = review.display_height;
    g_review_frame.destination_x = review.destination_x;
    g_review_frame.destination_y = review.destination_y;
    g_review_frame.destination_width = review.destination_width;
    g_review_frame.destination_height = review.destination_height;
    g_review_frame_valid = true;
  }
  UnlockReviewFrame();

  if (presented) {
    g_status.review_active = true;
    g_status.screenshot_available = true;
    g_status.review_index = target;
    CopyText(g_status.phase, sizeof g_status.phase, "review");
    CopyText(g_status.current_case, sizeof g_status.current_case, label);
    CopyText(g_status.review_case, sizeof g_status.review_case, label);
    printf("V3DTEST REVIEW action=show index=%u total=%u case=%s "
           "status=PASS\r\n", target, g_status.review_total, label);
  } else {
    g_status.review_active = false;
    g_status.screenshot_available = false;
    CopyText(g_status.review_case, sizeof g_status.review_case, "");
    CopyText(g_status.phase, sizeof g_status.phase, "review-error");
    CopyText(g_status.review_error, sizeof g_status.review_error,
             rendered ? "scanout" : "render");
    printf("V3DTEST REVIEW action=show index=%u total=%u status=FAIL "
           "reason=%s\r\n", target, g_status.review_total,
           rendered ? "scanout" : "render");
  }
  ++g_status.review_generation;
  Publish();
}

void Finish(const KmsModeSummary &kms, const RenderMatrixSummary &render,
            pi5kms::Framebuffer *framebuffer) {
  const uint32_t failed = kms.failed + render.failed;
  const bool unbaselined = failed == 0U && render.unbaselined != 0U;
  g_status.running = false;
  g_status.complete = true;
  g_status.passed = render.passed;
  g_status.failed = failed;
  g_status.skipped = kms.skipped + render.skipped;
  g_status.unbaselined = render.unbaselined;
  g_status.kms_passed = kms.passed;
  CopyText(g_status.phase, sizeof g_status.phase, "idle");
  CopyText(g_status.current_case, sizeof g_status.current_case, "");
  CopyText(g_status.result, sizeof g_status.result,
           failed != 0U ? "FAIL" :
               (unbaselined ? "PASS_UNBASELINED" : "PASS"));
  Publish();

  printf("V3DTEST END status=%s passed=%u failed=%u skipped=%u "
         "unbaselined=%u kms_passed=%u\r\n",
         g_status.result, render.passed, failed, g_status.skipped,
         render.unbaselined, kms.passed);
  ShowResult(framebuffer, failed != 0U, unbaselined);
  printf("V3DTEST IDLE color=%s remote=ready\r\n",
         failed != 0U ? "red" : (unbaselined ? "yellow" : "green"));
}

}  // namespace

void PollRemoteControl() {
  circle_v3d_test_poll_remote();
}

void PublishProgress(const char *phase, const char *current_case) {
  CopyText(g_status.phase, sizeof g_status.phase, phase);
  CopyText(g_status.current_case, sizeof g_status.current_case, current_case);
  Publish();
  PollRemoteControl();
}

void RunNetworkRenderTest() {
  memset(&g_status, 0, sizeof g_status);
  g_status.enabled = true;
  g_status.running = true;
  CopyText(g_status.backend, sizeof g_status.backend, BackendName());
  CopyText(g_status.phase, sizeof g_status.phase, "boot");
  CopyText(g_status.result, sizeof g_status.result, "RUNNING");
  Publish();

  printf("V3DTEST NETWORK_BOOT backend=%s core=1 remote=core0\r\n",
         BackendName());
  PublishProgress("kms", "initializing");
  pi5kms::Framebuffer framebuffer = {};
  const KmsModeSummary kms = RunKmsModeMatrix(&framebuffer);

  RenderMatrixSummary render = {};
  if (kms.default_mode_ready) {
    PublishProgress("v3d", "initializing");
    v3dcrt::Configure(true, true, v3dcrt::kShaderCrt,
                      v3dcrt::kBootTestOff, false,
                      v3dcrt::kFragmentPackageDefault,
                      v3dcrt::kRenderResolutionOutput);
    if (v3dcrt::Initialize()) {
      PublishProgress("render", "matrix");
      render = RunRenderMatrix(&framebuffer);
      g_status.review_total = static_cast<uint32_t>(RenderReviewCaseCount());
      g_status.review_available = g_status.review_total != 0U;
    } else {
      render.failed = 1U;
      printf("V3DTEST RENDER_END status=FAIL passed=0 failed=1 "
             "skipped=0 unbaselined=0 reason=v3d-init\r\n");
    }
  } else {
    render.failed = 1U;
    printf("V3DTEST RENDER_END status=FAIL passed=0 failed=1 "
           "skipped=0 unbaselined=0 reason=kms-init\r\n");
  }

  Finish(kms, render, &framebuffer);
  const bool failed = kms.failed + render.failed != 0U;
  const bool unbaselined = !failed && render.unbaselined != 0U;
  for (;;) {
    PollRemoteControl();
    ProcessReviewRequest(&framebuffer, failed, unbaselined);
    CTimer::SimpleMsDelay(10U);
  }
}

bool CaptureReviewScreenshot(uint32_t maximum_width,
                             bmx::remote::BmxBinaryPayload *payload) {
  if (payload == nullptr || !TryLockReviewFrame()) {
    return false;
  }
  const bool captured = g_review_frame_valid &&
      BuildReviewPpm(g_review_frame, maximum_width, payload);
  UnlockReviewFrame();
  return captured;
}

}  // namespace bmx_v3d_test
