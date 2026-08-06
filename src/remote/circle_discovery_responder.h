#ifndef BMX_REMOTE_CIRCLE_DISCOVERY_RESPONDER_H
#define BMX_REMOTE_CIRCLE_DISCOVERY_RESPONDER_H

#include <stdint.h>

class CNetSubSystem;
class CSocket;

namespace bmx {
namespace remote {

// Thin Circle adapter around the platform-neutral discovery codec.  It owns
// one UDP socket and is polled by the existing RemoteService task.
class CircleDiscoveryResponder {
public:
    CircleDiscoveryResponder();
    ~CircleDiscoveryResponder();

    bool Initialize(CNetSubSystem *network);

    // Drains a fair bounded batch without blocking and rate-limits replies.
    // Invalid probes and transient UDP errors are not fatal. False means the
    // Circle socket itself is no longer connected.
    bool Poll(uint64_t now_ms, bool *more_pending);

private:
    CircleDiscoveryResponder(const CircleDiscoveryResponder &);
    CircleDiscoveryResponder &operator=(const CircleDiscoveryResponder &);

    void Close();

    CNetSubSystem *network_;
    CSocket *socket_;
    uint64_t next_reply_ms_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_CIRCLE_DISCOVERY_RESPONDER_H
