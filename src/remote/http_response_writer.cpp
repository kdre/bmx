#include "remote/http_response_writer.h"

#include <string.h>

namespace bmx {
namespace remote {

namespace {

bool IsTokenCharacter(unsigned char value) {
    if ((value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9')) {
        return true;
    }
    return strchr("!#$%&'*+-.^_`|~", static_cast<int>(value)) != 0;
}

bool ValidHeaderName(HttpStringView name) {
    if (name.data == 0 || name.size == 0U) return false;
    for (size_t i = 0U; i < name.size; ++i) {
        if (!IsTokenCharacter(static_cast<unsigned char>(name.data[i]))) {
            return false;
        }
    }
    return true;
}

bool ValidHeaderValue(HttpStringView value) {
    if (value.data == 0 && value.size != 0U) return false;
    for (size_t i = 0U; i < value.size; ++i) {
        const unsigned char c = static_cast<unsigned char>(value.data[i]);
        if ((c < 0x20U && c != '\t') || c == 0x7fU) return false;
    }
    return true;
}

bool ValidReasonPhrase(HttpStringView value) {
    if (value.data == 0 || value.size == 0U) return false;
    for (size_t i = 0U; i < value.size; ++i) {
        const unsigned char c = static_cast<unsigned char>(value.data[i]);
        if (c < 0x20U || c > 0x7eU) return false;
    }
    return true;
}

bool IsFramingHeader(HttpStringView name) {
    return HttpStringEqualsInsensitive(name, "Content-Length") ||
           HttpStringEqualsInsensitive(name, "Transfer-Encoding") ||
           HttpStringEqualsInsensitive(name, "Connection");
}

}  // namespace

HttpResponseWriter::HttpResponseWriter()
    : response_(),
      state_(kIdle),
      suppress_body_(false),
      stream_activated_(false),
      stream_finished_(false),
      stream_cancelled_(false),
      header_size_(0U),
      header_offset_(0U),
      fixed_offset_(0U),
      stream_size_(0U),
      stream_offset_(0U) {}

bool HttpResponseWriter::Append(const char *value, size_t size) {
    if ((value == 0 && size != 0U) ||
        size > sizeof(header_) - header_size_) {
        return false;
    }
    if (size != 0U) memcpy(header_ + header_size_, value, size);
    header_size_ += size;
    return true;
}

bool HttpResponseWriter::Append(HttpStringView value) {
    return Append(value.data, value.size);
}

bool HttpResponseWriter::AppendUnsigned(uint64_t value) {
    char reversed[21U];
    size_t count = 0U;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    if (count > sizeof(header_) - header_size_) return false;
    while (count != 0U) header_[header_size_++] = reversed[--count];
    return true;
}

HttpResponseStartStatus HttpResponseWriter::Start(
    const HttpResponse &response, bool suppress_body) {
    if (active()) Abort();
    response_ = response;
    state_ = kFailed;
    suppress_body_ = suppress_body;
    stream_activated_ = false;
    stream_finished_ = false;
    stream_cancelled_ = false;
    header_size_ = 0U;
    header_offset_ = 0U;
    fixed_offset_ = 0U;
    stream_size_ = 0U;
    stream_offset_ = 0U;

    if (response_.status_code < 100U || response_.status_code > 599U ||
        !ValidReasonPhrase(response_.reason_phrase) ||
        response_.header_count > kHttpMaximumResponseHeaders) {
        return HttpResponseStartStatus::InvalidResponse;
    }
    if (response_.body_kind == HttpResponseBodyKind::Fixed &&
        response_.fixed_body_size != 0U && response_.fixed_body == 0) {
        return HttpResponseStartStatus::InvalidResponse;
    }
    if (response_.body_kind == HttpResponseBodyKind::HeadOnly &&
        !suppress_body_) {
        return HttpResponseStartStatus::InvalidFraming;
    }
    if (response_.body_kind == HttpResponseBodyKind::Stream &&
        response_.stream == 0) {
        return HttpResponseStartStatus::InvalidResponse;
    }

    if (!Append("HTTP/1.1 ", 9U) ||
        !AppendUnsigned(static_cast<uint64_t>(response_.status_code)) ||
        !Append(" ", 1U) || !Append(response_.reason_phrase) ||
        !Append("\r\n", 2U)) {
        return HttpResponseStartStatus::HeaderTooLarge;
    }
    for (size_t i = 0U; i < response_.header_count; ++i) {
        const HttpResponseHeader &header = response_.headers[i];
        if (!ValidHeaderName(header.name) || !ValidHeaderValue(header.value) ||
            IsFramingHeader(header.name)) {
            return HttpResponseStartStatus::InvalidHeader;
        }
        if (!Append(header.name) || !Append(": ", 2U) ||
            !Append(header.value) || !Append("\r\n", 2U)) {
            return HttpResponseStartStatus::HeaderTooLarge;
        }
    }

    if (response_.body_kind != HttpResponseBodyKind::Stream) {
        uint64_t length = 0U;
        if (response_.body_kind == HttpResponseBodyKind::Fixed) {
            length = static_cast<uint64_t>(response_.fixed_body_size);
        } else if (response_.body_kind == HttpResponseBodyKind::HeadOnly) {
            length = response_.declared_content_length;
        }
        if (!Append("Content-Length: ", 16U) || !AppendUnsigned(length) ||
            !Append("\r\n", 2U)) {
            return HttpResponseStartStatus::HeaderTooLarge;
        }
    }
    if (!Append("Connection: close\r\n\r\n", 21U)) {
        return HttpResponseStartStatus::HeaderTooLarge;
    }

    // From this point on the response owns an accepted stream even while its
    // headers are still being sent.  An abnormal close must therefore invoke
    // Cancel() as well as the optional completion hook.
    stream_activated_ = response_.body_kind == HttpResponseBodyKind::Stream &&
                        !suppress_body_;
    state_ = kHeaders;
    return HttpResponseStartStatus::Ok;
}

void HttpResponseWriter::EnterBodyState() {
    if (suppress_body_ || response_.body_kind == HttpResponseBodyKind::Empty ||
        response_.body_kind == HttpResponseBodyKind::HeadOnly ||
        (response_.body_kind == HttpResponseBodyKind::Fixed &&
         response_.fixed_body_size == 0U)) {
        state_ = kComplete;
    } else if (response_.body_kind == HttpResponseBodyKind::Fixed) {
        state_ = kFixedBody;
    } else if (response_.body_kind == HttpResponseBodyKind::Stream) {
        state_ = kStreamBody;
    } else {
        state_ = kFailed;
    }
}

HttpResponseWriteStatus HttpResponseWriter::WritePending(
    HttpTransport *transport, const uint8_t *data, size_t size,
    size_t *offset, bool *made_progress) {
    if (transport == 0 || data == 0 || offset == 0 || *offset >= size) {
        return HttpResponseWriteStatus::InvalidState;
    }
    const size_t requested = size - *offset;
    const HttpIoResult result = transport->Write(data + *offset, requested);
    switch (result.status) {
    case HttpIoStatus::Progress:
        if (result.size == 0U || result.size > requested) {
            return HttpResponseWriteStatus::TransportError;
        }
        *offset += result.size;
        *made_progress = true;
        return HttpResponseWriteStatus::Pending;
    case HttpIoStatus::WouldBlock:
        if (result.size != 0U) return HttpResponseWriteStatus::TransportError;
        return HttpResponseWriteStatus::Pending;
    case HttpIoStatus::Closed:
        return HttpResponseWriteStatus::ClientDisconnected;
    case HttpIoStatus::Error:
        return HttpResponseWriteStatus::TransportError;
    }
    return HttpResponseWriteStatus::TransportError;
}

HttpResponseWriteStatus HttpResponseWriter::Poll(HttpTransport *transport,
                                                 bool *made_progress) {
    if (made_progress == 0) return HttpResponseWriteStatus::InvalidState;
    *made_progress = false;
    if (state_ == kComplete) return HttpResponseWriteStatus::Complete;
    if (state_ == kIdle || state_ == kFailed || transport == 0) {
        return HttpResponseWriteStatus::InvalidState;
    }

    if (state_ == kHeaders) {
        const HttpResponseWriteStatus status = WritePending(
            transport, header_, header_size_, &header_offset_, made_progress);
        if (status != HttpResponseWriteStatus::Pending) return status;
        if (header_offset_ == header_size_) {
            EnterBodyState();
            return state_ == kComplete ? HttpResponseWriteStatus::Complete
                                       : HttpResponseWriteStatus::Pending;
        }
        return HttpResponseWriteStatus::Pending;
    }

    if (state_ == kFixedBody) {
        const HttpResponseWriteStatus status = WritePending(
            transport, response_.fixed_body, response_.fixed_body_size,
            &fixed_offset_, made_progress);
        if (status != HttpResponseWriteStatus::Pending) return status;
        if (fixed_offset_ == response_.fixed_body_size) {
            state_ = kComplete;
            return HttpResponseWriteStatus::Complete;
        }
        return HttpResponseWriteStatus::Pending;
    }

    if (state_ != kStreamBody) return HttpResponseWriteStatus::InvalidState;
    if (stream_offset_ == stream_size_) {
        stream_offset_ = 0U;
        stream_size_ = 0U;
        size_t produced = 0U;
        const HttpStreamReadResult read = response_.stream->Read(
            stream_buffer_, sizeof(stream_buffer_), &produced);
        if (read == HttpStreamReadResult::WouldBlock) {
            return produced == 0U ? HttpResponseWriteStatus::WaitingForStream
                                  : HttpResponseWriteStatus::StreamError;
        }
        if (read == HttpStreamReadResult::End) {
            if (produced != 0U) return HttpResponseWriteStatus::StreamError;
            stream_finished_ = true;
            state_ = kComplete;
            return HttpResponseWriteStatus::Complete;
        }
        if (read == HttpStreamReadResult::Error || produced == 0U ||
            produced > sizeof(stream_buffer_)) {
            return HttpResponseWriteStatus::StreamError;
        }
        stream_size_ = produced;
    }

    const HttpResponseWriteStatus status = WritePending(
        transport, stream_buffer_, stream_size_, &stream_offset_,
        made_progress);
    if (status != HttpResponseWriteStatus::Pending) return status;
    return HttpResponseWriteStatus::Pending;
}

void HttpResponseWriter::Abort() {
    if (stream_activated_ && !stream_finished_ && !stream_cancelled_ &&
        response_.stream != 0) {
        response_.stream->Cancel();
        stream_cancelled_ = true;
    }
    if (state_ != kComplete) state_ = kFailed;
}

bool HttpResponseWriter::active() const {
    return state_ == kHeaders || state_ == kFixedBody ||
           state_ == kStreamBody;
}

bool HttpResponseWriter::complete() const { return state_ == kComplete; }

bool HttpResponseWriter::has_pending_output() const {
    if (state_ == kHeaders) return header_offset_ < header_size_;
    if (state_ == kFixedBody) return fixed_offset_ < response_.fixed_body_size;
    if (state_ == kStreamBody) return stream_offset_ < stream_size_;
    return false;
}

bool HttpResponseWriter::streaming() const {
    return response_.body_kind == HttpResponseBodyKind::Stream;
}

const char *HttpResponseStartStatusString(HttpResponseStartStatus status) {
    switch (status) {
    case HttpResponseStartStatus::Ok:
        return "ok";
    case HttpResponseStartStatus::InvalidResponse:
        return "invalid response";
    case HttpResponseStartStatus::TooManyHeaders:
        return "too many headers";
    case HttpResponseStartStatus::HeaderTooLarge:
        return "response header too large";
    case HttpResponseStartStatus::InvalidHeader:
        return "invalid response header";
    case HttpResponseStartStatus::InvalidFraming:
        return "invalid response framing";
    }
    return "unknown";
}

}  // namespace remote
}  // namespace bmx
