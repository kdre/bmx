#ifndef BMX_REMOTE_REMOTE_CAPTURE_H
#define BMX_REMOTE_REMOTE_CAPTURE_H

#include "remote/bmx_api_types.h"

#include <stddef.h>
#include <stdint.h>

namespace bmx {
namespace remote {

// Request-scoped capture storage. The object itself is boot-lightweight and
// owns no buffer unless a remote capture request is active.
class RemoteCapture {
public:
    RemoteCapture();
    ~RemoteCapture();

    bool Screenshot(uint32_t maximum_width, BmxBinaryPayload *payload);
    bool BeginAudio(uint32_t duration_ms, uint32_t sample_rate,
                    uint32_t channels, bool wav, uint32_t token);
    bool AppendAudio(const int16_t *samples, size_t sample_count,
                     uint32_t *token, BmxBinaryPayload *payload);
    void RequestAudioCancel(uint32_t token);
    void PumpAudioCancel();
    uint64_t drops() const { return drops_; }

private:
    RemoteCapture(const RemoteCapture &);
    RemoteCapture &operator=(const RemoteCapture &);

    void ResetAudio();

    int16_t *audio_;
    size_t audio_capacity_;
    size_t audio_size_;
    uint32_t audio_sample_rate_;
    uint32_t audio_channels_;
    bool audio_wav_;
    uint32_t audio_token_;
    uint32_t audio_cancel_token_;
    uint64_t drops_;
};

}  // namespace remote
}  // namespace bmx

#endif
