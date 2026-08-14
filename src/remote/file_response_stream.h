#ifndef BMX_REMOTE_FILE_RESPONSE_STREAM_H
#define BMX_REMOTE_FILE_RESPONSE_STREAM_H

#include "remote/http_types.h"
#include "update/update_filesystem.h"

namespace bmx {
namespace remote {

typedef void (*FileResponseCloseVolume)(
    void *context, bmx::update::UpdateFileSystem *file_system);
typedef void (*FileResponseYield)(void *context);

void ResetFileResponseStreamDiagnostics();
uint64_t FileResponseStreamReadErrors();

class UpdateFileResponseStream : public HttpResponseStream,
                                 public HttpCompletion {
public:
    UpdateFileResponseStream(
        void *context, bmx::update::UpdateFileSystem *file_system,
        bmx::update::UpdateReadFile *file, uint64_t size,
        FileResponseCloseVolume close_volume, FileResponseYield yield);
    ~UpdateFileResponseStream() override;

    HttpStreamReadResult Read(uint8_t *output, size_t capacity,
                              size_t *size) override;
    void Cancel() override;
    void Complete(HttpCompletionReason reason) override;

private:
    void Release();

    void *context_;
    bmx::update::UpdateFileSystem *file_system_;
    bmx::update::UpdateReadFile *file_;
    uint64_t size_;
    uint64_t offset_;
    FileResponseCloseVolume close_volume_;
    FileResponseYield yield_;
    bool released_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_FILE_RESPONSE_STREAM_H
