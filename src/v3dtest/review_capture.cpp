#include "v3dtest/review_capture.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bmx_v3d_test {

namespace {

uint8_t *g_review_ppm_buffer = nullptr;
size_t g_review_ppm_capacity = 0U;
volatile uint32_t g_review_ppm_in_use = 0U;

bool AcquireReviewPpmBuffer(size_t required_size, uint8_t **data) {
  if (data == nullptr || required_size == 0U) {
    return false;
  }
  uint32_t available = 0U;
  if (!__atomic_compare_exchange_n(&g_review_ppm_in_use, &available, 1U,
                                   false, __ATOMIC_ACQUIRE,
                                   __ATOMIC_RELAXED)) {
    return false;
  }
  if (g_review_ppm_capacity < required_size) {
    uint8_t *replacement = static_cast<uint8_t *>(malloc(required_size));
    if (replacement == nullptr) {
      __atomic_store_n(&g_review_ppm_in_use, 0U, __ATOMIC_RELEASE);
      return false;
    }
    free(g_review_ppm_buffer);
    g_review_ppm_buffer = replacement;
    g_review_ppm_capacity = required_size;
  }
  *data = g_review_ppm_buffer;
  return true;
}

void ReleaseReviewPpmBuffer(uint8_t *data) {
  if (data == g_review_ppm_buffer) {
    __atomic_store_n(&g_review_ppm_in_use, 0U, __ATOMIC_RELEASE);
  }
}

bool ValidImage(const ReviewImage &image) {
  return image.pixels != nullptr && image.width != 0U &&
      image.height != 0U && image.pitch >= image.width * 2U &&
      image.display_width != 0U && image.display_height != 0U &&
      image.destination_width != 0U && image.destination_height != 0U &&
      image.destination_x <= image.display_width &&
      image.destination_y <= image.display_height &&
      image.destination_width <= image.display_width - image.destination_x &&
      image.destination_height <=
          image.display_height - image.destination_y;
}

uint8_t Expand5(uint32_t value) {
  return static_cast<uint8_t>((value << 3U) | (value >> 2U));
}

uint8_t Expand6(uint32_t value) {
  return static_cast<uint8_t>((value << 2U) | (value >> 4U));
}

}  // namespace

bool BuildReviewPpm(const ReviewImage &image, uint32_t maximum_width,
                    bmx::remote::BmxBinaryPayload *payload) {
  if (payload == nullptr || !ValidImage(image)) {
    return false;
  }
  memset(payload, 0, sizeof *payload);

  uint32_t output_width = image.display_width;
  uint32_t output_height = image.display_height;
  if (maximum_width != 0U && maximum_width < output_width) {
    output_width = maximum_width;
    output_height = static_cast<uint32_t>(
        static_cast<uint64_t>(image.display_height) * output_width /
        image.display_width);
    if (output_height == 0U) {
      output_height = 1U;
    }
  }
  if (output_width > static_cast<uint32_t>(SIZE_MAX / output_height) ||
      static_cast<size_t>(output_width) * output_height > SIZE_MAX / 3U) {
    return false;
  }

  char header[64U];
  const int header_size = snprintf(header, sizeof header, "P6\n%u %u\n255\n",
                                   output_width, output_height);
  if (header_size <= 0 || static_cast<size_t>(header_size) >= sizeof header) {
    return false;
  }
  const size_t pixel_bytes =
      static_cast<size_t>(output_width) * output_height * 3U;
  if (pixel_bytes > SIZE_MAX - static_cast<size_t>(header_size)) {
    return false;
  }
  uint8_t *data = nullptr;
  if (!AcquireReviewPpmBuffer(
          static_cast<size_t>(header_size) + pixel_bytes, &data)) {
    return false;
  }
  memcpy(data, header, static_cast<size_t>(header_size));
  uint8_t *target = data + header_size;

  for (uint32_t y = 0U; y < output_height; ++y) {
    const uint32_t virtual_y = static_cast<uint32_t>(
        static_cast<uint64_t>(y) * image.display_height / output_height);
    for (uint32_t x = 0U; x < output_width; ++x) {
      const uint32_t virtual_x = static_cast<uint32_t>(
          static_cast<uint64_t>(x) * image.display_width / output_width);
      uint16_t rgb565 = 0U;
      if (virtual_x >= image.destination_x &&
          virtual_x < image.destination_x + image.destination_width &&
          virtual_y >= image.destination_y &&
          virtual_y < image.destination_y + image.destination_height) {
        const uint32_t source_x = static_cast<uint32_t>(
            static_cast<uint64_t>(virtual_x - image.destination_x) *
            image.width / image.destination_width);
        const uint32_t source_y = static_cast<uint32_t>(
            static_cast<uint64_t>(virtual_y - image.destination_y) *
            image.height / image.destination_height);
        const uint16_t *source = reinterpret_cast<const uint16_t *>(
            image.pixels + static_cast<size_t>(source_y) * image.pitch);
        rgb565 = source[source_x];
      }
      *target++ = Expand5((rgb565 >> 11U) & 31U);
      *target++ = Expand6((rgb565 >> 5U) & 63U);
      *target++ = Expand5(rgb565 & 31U);
    }
  }

  payload->data = data;
  payload->size = static_cast<size_t>(header_size) + pixel_bytes;
  payload->width = output_width;
  payload->height = output_height;
  payload->release = ReleaseReviewPpmBuffer;
  return true;
}

}  // namespace bmx_v3d_test
