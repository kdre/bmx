#ifndef BMX_REMOTE_HTTP_RESPONSE_WRITER_H
#define BMX_REMOTE_HTTP_RESPONSE_WRITER_H

#include "remote/http_types.h"

namespace bmx {
namespace remote {

enum class HttpResponseStartStatus : uint8_t {
    Ok = 0,
    InvalidResponse,
    TooManyHeaders,
    HeaderTooLarge,
    InvalidHeader,
    InvalidFraming
};

enum class HttpResponseWriteStatus : uint8_t {
    Pending = 0,
    WaitingForStream,
    Complete,
    ClientDisconnected,
    TransportError,
    StreamError,
    InvalidState
};

// Serializes one response and advances it through arbitrary partial writes.
// Poll performs at most one transport Write call.  Streams are pulled only
// when their previous fragment has been completely sent.
class HttpResponseWriter {
 public:
    HttpResponseWriter();

    HttpResponseStartStatus Start(const HttpResponse &response,
                                  bool suppress_body);
    HttpResponseWriteStatus Poll(HttpTransport *transport,
                                 bool *made_progress);
    void Abort();

    bool active() const;
    bool complete() const;
    bool has_pending_output() const;
    bool streaming() const;
    HttpCompletion *completion() const { return response_.completion; }

 private:
    enum State : uint8_t {
        kIdle = 0,
        kHeaders,
        kFixedBody,
        kStreamBody,
        kComplete,
        kFailed
    };

    bool Append(const char *value, size_t size);
    bool Append(HttpStringView value);
    bool AppendUnsigned(uint64_t value);
    HttpResponseWriteStatus WritePending(HttpTransport *transport,
                                         const uint8_t *data, size_t size,
                                         size_t *offset,
                                         bool *made_progress);
    void EnterBodyState();

    HttpResponse response_;
    State state_;
    bool suppress_body_;
    bool stream_activated_;
    bool stream_finished_;
    bool stream_cancelled_;
    uint8_t header_[kHttpMaximumResponseHeaderBytes];
    size_t header_size_;
    size_t header_offset_;
    size_t fixed_offset_;
    uint8_t stream_buffer_[kHttpResponseStreamBufferBytes];
    size_t stream_size_;
    size_t stream_offset_;
};

const char *HttpResponseStartStatusString(HttpResponseStartStatus status);

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_HTTP_RESPONSE_WRITER_H
