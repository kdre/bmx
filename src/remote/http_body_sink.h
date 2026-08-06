#ifndef BMX_REMOTE_HTTP_BODY_SINK_H
#define BMX_REMOTE_HTTP_BODY_SINK_H

#include "remote/http_types.h"

namespace bmx {
namespace remote {

enum class HttpBodyAbortReason : uint8_t {
    MissingLength = 0,
    TooLarge,
    UnexpectedData,
    SinkRejected,
    ClientDisconnected,
    Timeout,
    TransportError,
    ServerStopped
};

// A router supplies one sink after inspecting the complete request head.  The
// server never buffers the complete body: it writes ordered fragments and
// calls Finish exactly once after Content-Length bytes.  Finish constructs the
// protocol-specific response.  A sink which rejects a body fragment may
// optionally construct its own error response from Reject(); this preserves
// backend-specific error classes without buffering the request body.  If no
// such response is supplied, if Finish reports failure, or on any other
// terminal path, the server calls Abort exactly once so the sink can release
// temporary state and its own lifetime.
class HttpBodySink {
 public:
    virtual ~HttpBodySink() {}

    virtual bool Write(const uint8_t *data, size_t size) = 0;
    virtual bool Reject(HttpResponse *response) {
        (void)response;
        return false;
    }
    virtual bool Finish(HttpResponse *response) = 0;
    virtual void Abort(HttpBodyAbortReason reason) = 0;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_HTTP_BODY_SINK_H
