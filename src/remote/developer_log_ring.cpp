#include "remote/developer_log_ring.h"

#include <stdlib.h>
#include <string.h>

namespace bmx {
namespace remote {
namespace {

DeveloperLogRing *g_developer_log_ring = 0;

}  // namespace

DeveloperLogRing::DeveloperLogRing(size_t capacity)
    : lock_(), bytes_(0), capacity_(0U), next_sequence_(0U)
{
    if (capacity == 0U) return;
    bytes_ = static_cast<uint8_t *>(malloc(capacity));
    if (bytes_ != 0) capacity_ = capacity;
}

DeveloperLogRing::~DeveloperLogRing()
{
    free(bytes_);
    bytes_ = 0;
    capacity_ = 0U;
}

void DeveloperLogRing::Lock() const
{
#if defined(RASPI_COMPILE)
    lock_.Acquire();
#else
    lock_.lock();
#endif
}

void DeveloperLogRing::Unlock() const
{
#if defined(RASPI_COMPILE)
    lock_.Release();
#else
    lock_.unlock();
#endif
}

void DeveloperLogRing::Append(const void *data, size_t size)
{
    if (!valid() || data == 0 || size == 0U) return;

    const uint8_t *source = static_cast<const uint8_t *>(data);
    const size_t retained = size > capacity_ ? capacity_ : size;
    const size_t skipped = size - retained;

    Lock();
    const uint64_t first_sequence =
        next_sequence_ + static_cast<uint64_t>(skipped);
    size_t position = static_cast<size_t>(
        first_sequence % static_cast<uint64_t>(capacity_));
    size_t first = capacity_ - position;
    if (first > retained) first = retained;
    memcpy(bytes_ + position, source + skipped, first);
    if (first != retained) {
        memcpy(bytes_, source + skipped + first, retained - first);
    }
    next_sequence_ += static_cast<uint64_t>(size);
    Unlock();
}

DeveloperLogWindow DeveloperLogRing::Window() const
{
    DeveloperLogWindow result;
    Lock();
    result.next = next_sequence_;
    result.oldest = result.next > capacity_
                        ? result.next - capacity_
                        : 0U;
    Unlock();
    return result;
}

DeveloperLogRead DeveloperLogRing::Read(uint64_t sequence,
                                        void *destination,
                                        size_t capacity) const
{
    DeveloperLogRead result;
    result.requested = sequence;
    result.start = sequence;
    result.next = sequence;
    result.oldest = 0U;
    result.size = 0U;
    result.gap = false;

    if (!valid() || (destination == 0 && capacity != 0U)) return result;

    Lock();
    const uint64_t newest = next_sequence_;
    result.oldest = newest > capacity_
                        ? newest - capacity_
                        : 0U;
    if (result.start < result.oldest) {
        result.start = result.oldest;
        result.gap = true;
    } else if (result.start > newest) {
        result.start = newest;
    }

    uint64_t available = newest - result.start;
    if (available > static_cast<uint64_t>(capacity)) {
        available = static_cast<uint64_t>(capacity);
    }
    result.size = static_cast<size_t>(available);
    const size_t position = static_cast<size_t>(
        result.start % static_cast<uint64_t>(capacity_));
    size_t first = capacity_ - position;
    if (first > result.size) first = result.size;
    if (first != 0U) memcpy(destination, bytes_ + position, first);
    if (first != result.size) {
        memcpy(static_cast<uint8_t *>(destination) + first, bytes_,
               result.size - first);
    }
    result.next = result.start + static_cast<uint64_t>(result.size);
    Unlock();
    return result;
}

void SetDeveloperLogRing(DeveloperLogRing *ring)
{
    g_developer_log_ring = ring;
}

DeveloperLogRing *GetDeveloperLogRing()
{
    return g_developer_log_ring;
}

void CaptureDeveloperLog(const void *data, size_t size)
{
    DeveloperLogRing *ring = g_developer_log_ring;
    if (ring != 0) ring->Append(data, size);
}

}  // namespace remote
}  // namespace bmx
