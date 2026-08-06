#include "remote/developer_log_device.h"

namespace bmx {
namespace remote {

DeveloperLogDevice::DeveloperLogDevice(CDevice *target,
                                       DeveloperLogRing *ring)
    : CDevice(), target_(target), ring_(ring)
{
}

int DeveloperLogDevice::Write(const void *buffer, size_t count)
{
    if (buffer == 0 && count != 0U) return -1;
    if (ring_ != 0) ring_->Append(buffer, count);
    return target_ != 0 ? target_->Write(buffer, count)
                        : static_cast<int>(count);
}

}  // namespace remote
}  // namespace bmx
