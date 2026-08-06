#ifndef BMX_REMOTE_HTTP_TYPES_H
#define BMX_REMOTE_HTTP_TYPES_H

#include <stddef.h>
#include <stdint.h>

namespace bmx {
namespace remote {

static const size_t kHttpMaximumHeaderBytes = 8192U;
static const size_t kHttpMaximumRequestTargetBytes = 2048U;
static const size_t kHttpMaximumRequestHeaders = 64U;
static const size_t kHttpMaximumResponseHeaders = 16U;
static const size_t kHttpMaximumResponseHeaderBytes = 2048U;
static const size_t kHttpConnectionReadBufferBytes = 2048U;
static const size_t kHttpResponseStreamBufferBytes = 1024U;
static const size_t kHttpMaximumConnections = 2U;

struct HttpStringView {
    const char *data;
    size_t size;

    HttpStringView() : data(0), size(0U) {}
    HttpStringView(const char *value, size_t value_size)
        : data(value), size(value_size) {}

    bool empty() const { return size == 0U; }
};

HttpStringView HttpString(const char *value);
bool HttpStringEquals(HttpStringView value, const char *expected);
bool HttpStringEqualsInsensitive(HttpStringView value, const char *expected);
bool HttpStringStartsWith(HttpStringView value, HttpStringView prefix);

enum class HttpMethod : uint8_t {
    Get = 0,
    Head,
    Put,
    Post
};

const char *HttpMethodName(HttpMethod method);

struct HttpHeaderView {
    HttpStringView name;
    HttpStringView value;
};

struct HttpRequestHead {
    HttpMethod method;
    HttpStringView raw_target;
    HttpStringView raw_path;
    HttpStringView raw_query;
    bool has_query;
    HttpHeaderView headers[kHttpMaximumRequestHeaders];
    size_t header_count;
    bool has_content_length;
    uint64_t content_length;
    unsigned content_length_count;

    HttpRequestHead();

    bool Header(const char *name, HttpStringView *value) const;
    size_t HeaderCount(const char *name) const;
};

struct HttpResponseHeader {
    HttpStringView name;
    HttpStringView value;
};

enum class HttpStreamReadResult : uint8_t {
    Data = 0,
    WouldBlock,
    End,
    Error
};

class HttpResponseStream {
 public:
    virtual ~HttpResponseStream() {}

    // Data must return a non-zero byte count not exceeding capacity.
    virtual HttpStreamReadResult Read(uint8_t *output, size_t capacity,
                                      size_t *size) = 0;
    // Called exactly once if a started stream is terminated abnormally.
    virtual void Cancel() = 0;
};

enum class HttpCompletionReason : uint8_t {
    ResponseSent = 0,
    ClientDisconnected,
    Timeout,
    TransportError,
    StreamError,
    ServerStopped,
    InvalidResponse
};

class HttpCompletion {
 public:
    virtual ~HttpCompletion() {}
    virtual void Complete(HttpCompletionReason reason) = 0;
};

enum class HttpResponseBodyKind : uint8_t {
    Empty = 0,
    Fixed,
    HeadOnly,
    Stream
};

// Header names and values, fixed bodies, streams and completion hooks remain
// caller-owned until the connection's completion hook runs (or the connection
// closes when no hook was supplied).
struct HttpResponse {
    unsigned status_code;
    HttpStringView reason_phrase;
    HttpResponseHeader headers[kHttpMaximumResponseHeaders];
    size_t header_count;
    HttpResponseBodyKind body_kind;
    const uint8_t *fixed_body;
    size_t fixed_body_size;
    uint64_t declared_content_length;
    HttpResponseStream *stream;
    HttpCompletion *completion;

    HttpResponse();

    void Reset(unsigned status, const char *reason = 0);
    bool AddHeader(HttpStringView name, HttpStringView value);
    bool AddHeader(const char *name, const char *value);
    void SetEmptyBody();
    void SetFixedBody(const uint8_t *body, size_t size);
    void SetFixedText(const char *body);
    // A HeadOnly response is valid only for an incoming HEAD request.  It
    // advertises a representation length but never supplies representation
    // bytes.
    void SetHeadOnly(uint64_t content_length);
    // Streams are framed by connection close and deliberately have no
    // Content-Length or chunked transfer coding.
    void SetStream(HttpResponseStream *body_stream);
};

const char *HttpReasonPhrase(unsigned status_code);

enum class HttpIoStatus : uint8_t {
    Progress = 0,
    WouldBlock,
    Closed,
    Error
};

struct HttpIoResult {
    HttpIoStatus status;
    size_t size;

    HttpIoResult(HttpIoStatus io_status = HttpIoStatus::WouldBlock,
                 size_t io_size = 0U)
        : status(io_status), size(io_size) {}
};

// The Circle adapter is intentionally outside the protocol core.  A concrete
// transport maps the platform's non-blocking socket result to these four
// unambiguous outcomes.
class HttpTransport {
 public:
    virtual ~HttpTransport() {}
    virtual HttpIoResult Read(uint8_t *output, size_t capacity) = 0;
    virtual HttpIoResult Write(const uint8_t *data, size_t size) = 0;
    virtual void Close() = 0;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_HTTP_TYPES_H
