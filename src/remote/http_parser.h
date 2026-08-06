#ifndef BMX_REMOTE_HTTP_PARSER_H
#define BMX_REMOTE_HTTP_PARSER_H

#include "remote/http_types.h"

namespace bmx {
namespace remote {

enum class HttpRequestParseStatus : uint8_t {
    NeedMoreData = 0,
    Complete,
    InvalidArgument,
    HeaderTooLarge,
    InvalidLineEnding,
    InvalidRequestLine,
    UnsupportedMethod,
    UnsupportedVersion,
    InvalidRequestTarget,
    RequestTargetTooLong,
    InvalidHeader,
    TooManyHeaders,
    InvalidContentLength,
    ConflictingContentLength,
    UnsupportedTransferEncoding,
    AmbiguousBodyLength
};

// A single-request, incremental HTTP/1.1 request-head parser.  Feed consumes
// exactly through CRLF CRLF and deliberately leaves any bytes from the first
// body fragment to its caller.  Views in request() point into this parser and
// remain valid until Reset() or destruction.
class HttpRequestParser {
 public:
    explicit HttpRequestParser(
        size_t maximum_header_bytes = kHttpMaximumHeaderBytes);

    void Reset();
    HttpRequestParseStatus Feed(const uint8_t *data, size_t size,
                                size_t *consumed);

    HttpRequestParseStatus status() const { return status_; }
    const HttpRequestHead &request() const { return request_; }
    size_t buffered_bytes() const { return size_; }
    size_t maximum_header_bytes() const { return maximum_header_bytes_; }

 private:
    HttpRequestParseStatus Fail(HttpRequestParseStatus status);
    HttpRequestParseStatus ParseCompleteHead();

    uint8_t buffer_[kHttpMaximumHeaderBytes];
    size_t size_;
    size_t maximum_header_bytes_;
    bool previous_was_cr_;
    HttpRequestParseStatus status_;
    HttpRequestHead request_;
};

const char *HttpRequestParseStatusString(HttpRequestParseStatus status);

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_HTTP_PARSER_H
