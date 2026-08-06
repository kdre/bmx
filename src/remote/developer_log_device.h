#ifndef BMX_REMOTE_DEVELOPER_LOG_DEVICE_H
#define BMX_REMOTE_DEVELOPER_LOG_DEVICE_H

#include "remote/developer_log_ring.h"

#include <circle/device.h>

namespace bmx {
namespace remote {

// CLogger target which mirrors the exact formatted text into the developer
// ring and then preserves the original serial/null-device destination.
class DeveloperLogDevice : public CDevice {
public:
    DeveloperLogDevice(CDevice *target, DeveloperLogRing *ring);
    int Write(const void *buffer, size_t count) override;

private:
    CDevice *target_;
    DeveloperLogRing *ring_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_DEVELOPER_LOG_DEVICE_H
