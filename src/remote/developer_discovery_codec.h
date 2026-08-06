#ifndef BMX_REMOTE_DEVELOPER_DISCOVERY_CODEC_H
#define BMX_REMOTE_DEVELOPER_DISCOVERY_CODEC_H

#include <stddef.h>
#include <stdint.h>

namespace bmx {
namespace remote {

static const uint16_t kDeveloperDiscoveryUdpPort = 6464U;
static const uint16_t kDeveloperDiscoveryHttpPort = 80U;
static const size_t kDeveloperDiscoveryNonceCharacters = 32U;

struct DeveloperDiscoveryProbe {
    char nonce[kDeveloperDiscoveryNonceCharacters + 1U];
};

// The codec is independent of Circle and performs no allocation.  The wire
// grammar is deliberately exact so arbitrary UDP traffic is silently ignored.
bool DecodeDeveloperDiscoveryProbe(const uint8_t *datagram, size_t size,
                                   DeveloperDiscoveryProbe *probe);

// Returns the exact datagram size, or zero for invalid arguments/capacity.
// The returned bytes are not NUL-terminated.
size_t EncodeDeveloperDiscoveryReply(const DeveloperDiscoveryProbe &probe,
                                     uint8_t *output, size_t capacity);

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_DEVELOPER_DISCOVERY_CODEC_H
