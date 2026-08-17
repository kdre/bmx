#ifndef BMX_V3DTEST_REVIEW_CAPTURE_H
#define BMX_V3DTEST_REVIEW_CAPTURE_H

#include "remote/bmx_api_types.h"

#include <stdint.h>

namespace bmx_v3d_test {

struct ReviewImage {
  const uint8_t *pixels;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t display_width;
  uint32_t display_height;
  uint32_t destination_x;
  uint32_t destination_y;
  uint32_t destination_width;
  uint32_t destination_height;
};

// Builds a full-display PPM: pixels outside the rendered destination are
// black, matching the native-KMS background used by the review scanout.
bool BuildReviewPpm(const ReviewImage &image, uint32_t maximum_width,
                    bmx::remote::BmxBinaryPayload *payload);

}  // namespace bmx_v3d_test

#endif  // BMX_V3DTEST_REVIEW_CAPTURE_H
