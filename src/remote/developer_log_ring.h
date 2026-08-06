#ifndef BMX_REMOTE_DEVELOPER_LOG_RING_H
#define BMX_REMOTE_DEVELOPER_LOG_RING_H

#include "developer_settings.h"

#include <stddef.h>
#include <stdint.h>

#if defined(RASPI_COMPILE)
#include <circle/spinlock.h>
#else
#include <mutex>
#endif

namespace bmx {
namespace remote {

// The ring exists only while developer_mode=1. Sequence numbers count every
// byte presented to the BMX log sink, including bytes which have since been
// overwritten.
static const size_t kDeveloperLogDefaultCapacity =
    BMX_DEVELOPER_LOG_BUFFER_DEFAULT_KB * 1024U;

struct DeveloperLogWindow {
    uint64_t oldest;
    uint64_t next;
};

struct DeveloperLogRead {
    uint64_t requested;
    uint64_t start;
    uint64_t next;
    uint64_t oldest;
    size_t size;
    bool gap;
};

class DeveloperLogRing {
public:
    explicit DeveloperLogRing(size_t capacity = kDeveloperLogDefaultCapacity);
    ~DeveloperLogRing();

    void Append(const void *data, size_t size);
    DeveloperLogWindow Window() const;
    bool valid() const { return bytes_ != 0 && capacity_ != 0U; }
    size_t capacity() const { return capacity_; }

    // Copies at most capacity bytes and returns the exact sequence interval
    // represented by the copy. A request older than the retained window is
    // advanced to oldest and marked as a gap. A request in the future starts
    // at the current end and returns no bytes.
    DeveloperLogRead Read(uint64_t sequence, void *destination,
                          size_t capacity) const;

private:
    DeveloperLogRing(const DeveloperLogRing &);
    DeveloperLogRing &operator=(const DeveloperLogRing &);

    void Lock() const;
    void Unlock() const;

#if defined(RASPI_COMPILE)
    mutable CSpinLock lock_;
#else
    mutable std::mutex lock_;
#endif
    uint8_t *bytes_;
    size_t capacity_;
    uint64_t next_sequence_;
};

// Installed once during early BMX initialization, before CGlueStdioInit().
// The disabled path is a null-pointer check and does not allocate anything.
void SetDeveloperLogRing(DeveloperLogRing *ring);
DeveloperLogRing *GetDeveloperLogRing();
void CaptureDeveloperLog(const void *data, size_t size);

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_DEVELOPER_LOG_RING_H
