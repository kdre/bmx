#include "remote/file_response_stream.h"

namespace bmx {
namespace remote {
namespace {

uint64_t g_read_errors = 0U;

}  // namespace

void ResetFileResponseStreamDiagnostics() { g_read_errors = 0U; }

uint64_t FileResponseStreamReadErrors() { return g_read_errors; }

UpdateFileResponseStream::UpdateFileResponseStream(
    void *context, bmx::update::UpdateFileSystem *file_system,
    bmx::update::UpdateReadFile *file, uint64_t size,
    FileResponseCloseVolume close_volume, FileResponseYield yield)
    : context_(context), file_system_(file_system), file_(file), size_(size),
      offset_(0U), close_volume_(close_volume), yield_(yield), released_(false) {
}

UpdateFileResponseStream::~UpdateFileResponseStream() { Release(); }

HttpStreamReadResult UpdateFileResponseStream::Read(
    uint8_t *output, size_t capacity, size_t *size) {
    if (output == 0 || size == 0 || file_ == 0) {
        ++g_read_errors;
        return HttpStreamReadResult::Error;
    }
    if (offset_ == size_) {
        *size = 0U;
        return HttpStreamReadResult::End;
    }
    size_t count = capacity;
    if (static_cast<uint64_t>(count) > size_ - offset_) {
        count = static_cast<size_t>(size_ - offset_);
    }
    if (count == 0U || !file_->ReadAt(offset_, output, count)) {
        ++g_read_errors;
        *size = 0U;
        return HttpStreamReadResult::Error;
    }
    offset_ += static_cast<uint64_t>(count);
    *size = count;
    if (yield_ != 0) yield_(context_);
    return HttpStreamReadResult::Data;
}

void UpdateFileResponseStream::Cancel() { Release(); }

void UpdateFileResponseStream::Complete(HttpCompletionReason) {
    Release();
    delete this;
}

void UpdateFileResponseStream::Release() {
    if (released_) return;
    released_ = true;
    if (file_ != 0) (void)file_->Close();
    file_ = 0;
    if (file_system_ != 0 && close_volume_ != 0) {
        close_volume_(context_, file_system_);
    }
    file_system_ = 0;
}

}  // namespace remote
}  // namespace bmx
