#include "remote/remote_capture.h"

#include "fbl.h"
extern "C" {
#include "third_party/common/circle.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace bmx {
namespace remote {

namespace {

uint8_t *g_screenshot_ppm_buffer = 0;
size_t g_screenshot_ppm_capacity = 0U;
volatile uint32_t g_screenshot_ppm_in_use = 0U;

bool AcquireScreenshotPpmBuffer(size_t capacity, uint8_t **data)
{
    if (capacity == 0U || data == 0) return false;
    uint32_t available = 0U;
    if (!__atomic_compare_exchange_n(&g_screenshot_ppm_in_use, &available, 1U,
                                     false, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) return false;
    if (g_screenshot_ppm_capacity < capacity) {
        uint8_t *replacement = static_cast<uint8_t *>(malloc(capacity));
        if (replacement == 0) {
            __atomic_store_n(&g_screenshot_ppm_in_use, 0U, __ATOMIC_RELEASE);
            return false;
        }
        free(g_screenshot_ppm_buffer);
        g_screenshot_ppm_buffer = replacement;
        g_screenshot_ppm_capacity = capacity;
    }
    *data = g_screenshot_ppm_buffer;
    return true;
}

void ReleaseScreenshotPpmBuffer(uint8_t *data)
{
    if (data == g_screenshot_ppm_buffer) {
        __atomic_store_n(&g_screenshot_ppm_in_use, 0U, __ATOMIC_RELEASE);
    }
}

struct CaptureDimensionsRequest {
    int *width;
    int *height;
};

int CaptureDimensionsOnPlatformCore(void *opaque)
{
    CaptureDimensionsRequest *request =
        static_cast<CaptureDimensionsRequest *>(opaque);
    return FrameBufferLayer::CaptureDimensions(request->width,
                                                request->height) ? 1 : 0;
}

struct CaptureRgb888Request {
    uint8_t *output;
    int width;
    int height;
    unsigned pitch;
};

int CaptureRgb888OnPlatformCore(void *opaque)
{
    CaptureRgb888Request *request =
        static_cast<CaptureRgb888Request *>(opaque);
    return FrameBufferLayer::CaptureRgb888(
        request->output, request->width, request->height,
        request->pitch) ? 1 : 0;
}

}  // namespace

RemoteCapture::RemoteCapture()
    : audio_(0), audio_capacity_(0U), audio_size_(0U),
      audio_sample_rate_(0U), audio_channels_(0U), audio_wav_(false),
      audio_token_(0U),
      audio_cancel_token_(0U), drops_(0U) {}

RemoteCapture::~RemoteCapture()
{
    ResetAudio();
    if (__atomic_load_n(&g_screenshot_ppm_in_use, __ATOMIC_ACQUIRE) == 0U) {
        free(g_screenshot_ppm_buffer);
        g_screenshot_ppm_buffer = 0;
        g_screenshot_ppm_capacity = 0U;
    }
}

void RemoteCapture::ResetAudio()
{
    free(audio_);
    audio_ = 0;
    audio_capacity_ = 0U;
    audio_size_ = 0U;
    audio_sample_rate_ = 0U;
    audio_channels_ = 0U;
    audio_wav_ = false;
    audio_token_ = 0U;
}

void RemoteCapture::RequestAudioCancel(uint32_t token)
{
    if (token != 0U) {
        __atomic_store_n(&audio_cancel_token_, token, __ATOMIC_RELEASE);
    }
}

void RemoteCapture::PumpAudioCancel()
{
    const uint32_t token = __atomic_exchange_n(
        &audio_cancel_token_, 0U, __ATOMIC_ACQ_REL);
    if (token != 0U && token == audio_token_) ResetAudio();
}

bool RemoteCapture::Screenshot(uint32_t maximum_width,
                               BmxBinaryPayload *payload)
{
    if (payload == 0) return false;
    memset(payload, 0, sizeof(*payload));
    int display_width = 0;
    int display_height = 0;
    CaptureDimensionsRequest dimensions = {&display_width, &display_height};
    if (circle_run_on_platform_core(
            CaptureDimensionsOnPlatformCore, &dimensions) != 1 ||
        display_width <= 0 || display_height <= 0) return false;
    int width = display_width;
    int height = display_height;
    if (maximum_width != 0U && maximum_width < (uint32_t)width) {
        width = (int)maximum_width;
        height = (int)(((uint64_t)display_height * maximum_width) /
                       (uint32_t)display_width);
        if (height < 1) height = 1;
    }
    char header[64U];
    const int header_size = snprintf(header, sizeof(header), "P6\n%d %d\n255\n",
                                     width, height);
    if (header_size <= 0 || (size_t)header_size >= sizeof(header)) return false;
    if ((size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > SIZE_MAX / 3U) return false;
    const size_t pixels_size = (size_t)width * (size_t)height * 3U;
    if (pixels_size > SIZE_MAX - (size_t)header_size) return false;
    if ((size_t)display_width > SIZE_MAX / (size_t)display_height ||
        (size_t)display_width * (size_t)display_height >
            (SIZE_MAX - sizeof(header)) / 3U) return false;
    const size_t capacity = sizeof(header) +
        (size_t)display_width * (size_t)display_height * 3U;
    uint8_t *data = 0;
    if (!AcquireScreenshotPpmBuffer(capacity, &data)) return false;
    memcpy(data, header, (size_t)header_size);
    CaptureRgb888Request capture = {
        data + header_size, width, height, (unsigned)width * 3U};
    if (circle_run_on_platform_core(
            CaptureRgb888OnPlatformCore, &capture) != 1) {
        ReleaseScreenshotPpmBuffer(data);
        return false;
    }
    payload->data = data;
    payload->size = (size_t)header_size + pixels_size;
    payload->width = (uint32_t)width;
    payload->height = (uint32_t)height;
    payload->release = ReleaseScreenshotPpmBuffer;
    return true;
}

bool RemoteCapture::BeginAudio(uint32_t duration_ms, uint32_t sample_rate,
                               uint32_t channels, bool wav, uint32_t token)
{
    PumpAudioCancel();
    if (audio_ != 0 || duration_ms < 100U || duration_ms > 5000U ||
        sample_rate == 0U || channels == 0U || channels > 8U || token == 0U) {
        ++drops_;
        return false;
    }
    const uint64_t frames =
        ((uint64_t)duration_ms * sample_rate + 999U) / 1000U;
    const uint64_t samples = frames * channels;
    if (samples == 0U || samples > SIZE_MAX / sizeof(int16_t)) {
        ++drops_;
        return false;
    }
    audio_ = (int16_t *)malloc((size_t)samples * sizeof(int16_t));
    if (audio_ == 0) {
        ++drops_;
        return false;
    }
    audio_capacity_ = (size_t)samples;
    audio_size_ = 0U;
    audio_sample_rate_ = sample_rate;
    audio_channels_ = channels;
    audio_wav_ = wav;
    audio_token_ = token;
    return true;
}

bool RemoteCapture::AppendAudio(const int16_t *samples, size_t sample_count,
                                uint32_t *token, BmxBinaryPayload *payload)
{
    if (audio_ == 0 || samples == 0 || token == 0 || payload == 0) return false;
    size_t remaining = audio_capacity_ - audio_size_;
    const size_t copy = sample_count < remaining ? sample_count : remaining;
    if (copy != 0U) {
        memcpy(audio_ + audio_size_, samples, copy * sizeof(int16_t));
        audio_size_ += copy;
    }
    if (audio_size_ < audio_capacity_) return false;

    memset(payload, 0, sizeof(*payload));
    payload->data = reinterpret_cast<uint8_t *>(audio_);
    payload->size = audio_size_ * sizeof(int16_t);
    payload->sample_rate = audio_sample_rate_;
    payload->channels = audio_channels_;
    payload->wav = audio_wav_;
    *token = audio_token_;
    audio_ = 0;
    audio_capacity_ = 0U;
    audio_size_ = 0U;
    audio_sample_rate_ = 0U;
    audio_channels_ = 0U;
    audio_wav_ = false;
    audio_token_ = 0U;
    return true;
}

}  // namespace remote
}  // namespace bmx
