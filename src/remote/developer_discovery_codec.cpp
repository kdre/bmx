#include "remote/developer_discovery_codec.h"

#include <string.h>

namespace bmx {
namespace remote {
namespace {

static const char kProbePrefix[] = "BMXDEV-DISCOVER/1 ";
static const char kReplyPrefix[] = "BMXDEV-HERE/1 ";
static const char kReplySuffix[] = " 80\n";
static_assert(kDeveloperDiscoveryHttpPort == 80U,
              "developer discovery reply suffix must match its HTTP port");

bool IsLowerHex(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f');
}

bool ValidNonce(const char *nonce)
{
    if (nonce == 0) return false;
    for (size_t index = 0U; index < kDeveloperDiscoveryNonceCharacters;
         ++index) {
        if (!IsLowerHex(nonce[index])) return false;
    }
    return nonce[kDeveloperDiscoveryNonceCharacters] == '\0';
}

}  // namespace

bool DecodeDeveloperDiscoveryProbe(const uint8_t *datagram, size_t size,
                                   DeveloperDiscoveryProbe *probe)
{
    const size_t prefix_size = sizeof(kProbePrefix) - 1U;
    const size_t expected_size =
        prefix_size + kDeveloperDiscoveryNonceCharacters + 1U;
    if (datagram == 0 || probe == 0 || size != expected_size ||
        memcmp(datagram, kProbePrefix, prefix_size) != 0 ||
        datagram[size - 1U] != '\n') {
        return false;
    }

    const uint8_t *nonce = datagram + prefix_size;
    for (size_t index = 0U; index < kDeveloperDiscoveryNonceCharacters;
         ++index) {
        if (!IsLowerHex(static_cast<char>(nonce[index]))) return false;
        probe->nonce[index] = static_cast<char>(nonce[index]);
    }
    probe->nonce[kDeveloperDiscoveryNonceCharacters] = '\0';
    return true;
}

size_t EncodeDeveloperDiscoveryReply(const DeveloperDiscoveryProbe &probe,
                                     uint8_t *output, size_t capacity)
{
    const size_t prefix_size = sizeof(kReplyPrefix) - 1U;
    const size_t suffix_size = sizeof(kReplySuffix) - 1U;
    const size_t required =
        prefix_size + kDeveloperDiscoveryNonceCharacters + suffix_size;
    if (output == 0 || capacity < required || !ValidNonce(probe.nonce)) {
        return 0U;
    }

    memcpy(output, kReplyPrefix, prefix_size);
    memcpy(output + prefix_size, probe.nonce,
           kDeveloperDiscoveryNonceCharacters);
    memcpy(output + prefix_size + kDeveloperDiscoveryNonceCharacters,
           kReplySuffix, suffix_size);
    return required;
}

}  // namespace remote
}  // namespace bmx
