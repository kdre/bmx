#include "remote/http_parser.h"

#include <limits.h>
#include <string.h>

namespace bmx {
namespace remote {

namespace {

bool IsTokenCharacter(uint8_t value) {
    if ((value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9')) {
        return true;
    }
    return strchr("!#$%&'*+-.^_`|~", static_cast<int>(value)) != 0;
}

const uint8_t *FindCrlf(const uint8_t *begin, const uint8_t *end) {
    for (const uint8_t *cursor = begin; cursor + 1U < end; ++cursor) {
        if (cursor[0] == '\r' && cursor[1] == '\n') return cursor;
    }
    return 0;
}

bool ParseDecimal(HttpStringView value, uint64_t *output) {
    if (value.data == 0 || value.size == 0U || output == 0) return false;
    uint64_t result = 0U;
    for (size_t i = 0U; i < value.size; ++i) {
        const char c = value.data[i];
        if (c < '0' || c > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (result > (UINT64_MAX - digit) / 10U) return false;
        result = result * 10U + digit;
    }
    *output = result;
    return true;
}

HttpRequestParseStatus ParseMethod(const uint8_t *value, size_t size,
                                   HttpMethod *method) {
    if (size == 3U && memcmp(value, "GET", 3U) == 0) {
        *method = HttpMethod::Get;
    } else if (size == 4U && memcmp(value, "HEAD", 4U) == 0) {
        *method = HttpMethod::Head;
    } else if (size == 3U && memcmp(value, "PUT", 3U) == 0) {
        *method = HttpMethod::Put;
    } else if (size == 4U && memcmp(value, "POST", 4U) == 0) {
        *method = HttpMethod::Post;
    } else if (size == 6U && memcmp(value, "DELETE", 6U) == 0) {
        *method = HttpMethod::Delete;
    } else {
        return HttpRequestParseStatus::UnsupportedMethod;
    }
    return HttpRequestParseStatus::Complete;
}

}  // namespace

HttpRequestParser::HttpRequestParser(size_t maximum_header_bytes)
    : size_(0U),
      maximum_header_bytes_(maximum_header_bytes),
      previous_was_cr_(false),
      status_(HttpRequestParseStatus::NeedMoreData),
      request_() {
    if (maximum_header_bytes_ == 0U ||
        maximum_header_bytes_ > kHttpMaximumHeaderBytes) {
        maximum_header_bytes_ = kHttpMaximumHeaderBytes;
    }
}

void HttpRequestParser::Reset() {
    size_ = 0U;
    previous_was_cr_ = false;
    status_ = HttpRequestParseStatus::NeedMoreData;
    request_ = HttpRequestHead();
}

HttpRequestParseStatus HttpRequestParser::Fail(
    HttpRequestParseStatus status) {
    status_ = status;
    return status_;
}

HttpRequestParseStatus HttpRequestParser::Feed(const uint8_t *data,
                                               size_t size,
                                               size_t *consumed) {
    if (consumed == 0 || (data == 0 && size != 0U)) {
        return HttpRequestParseStatus::InvalidArgument;
    }
    *consumed = 0U;
    if (status_ != HttpRequestParseStatus::NeedMoreData) return status_;

    while (*consumed < size) {
        if (size_ >= maximum_header_bytes_) {
            return Fail(HttpRequestParseStatus::HeaderTooLarge);
        }

        const uint8_t value = data[*consumed];
        if (value == 0U) return Fail(HttpRequestParseStatus::InvalidHeader);
        if ((previous_was_cr_ && value != '\n') ||
            (!previous_was_cr_ && value == '\n')) {
            return Fail(HttpRequestParseStatus::InvalidLineEnding);
        }

        buffer_[size_++] = value;
        ++*consumed;
        previous_was_cr_ = value == '\r';

        if (size_ >= 4U && buffer_[size_ - 4U] == '\r' &&
            buffer_[size_ - 3U] == '\n' &&
            buffer_[size_ - 2U] == '\r' &&
            buffer_[size_ - 1U] == '\n') {
            return ParseCompleteHead();
        }
    }

    if (size_ == maximum_header_bytes_) {
        return Fail(HttpRequestParseStatus::HeaderTooLarge);
    }
    return status_;
}

HttpRequestParseStatus HttpRequestParser::ParseCompleteHead() {
    const uint8_t *const begin = buffer_;
    const uint8_t *const end = buffer_ + size_;
    const uint8_t *const request_line_end = FindCrlf(begin, end);
    if (request_line_end == 0 || request_line_end == begin) {
        return Fail(HttpRequestParseStatus::InvalidRequestLine);
    }

    const uint8_t *first_space = static_cast<const uint8_t *>(
        memchr(begin, ' ', static_cast<size_t>(request_line_end - begin)));
    if (first_space == 0 || first_space == begin) {
        return Fail(HttpRequestParseStatus::InvalidRequestLine);
    }
    const uint8_t *second_space = static_cast<const uint8_t *>(memchr(
        first_space + 1U, ' ',
        static_cast<size_t>(request_line_end - (first_space + 1U))));
    if (second_space == 0 || second_space == first_space + 1U ||
        second_space + 1U == request_line_end ||
        memchr(second_space + 1U, ' ',
               static_cast<size_t>(request_line_end - (second_space + 1U))) !=
            0) {
        return Fail(HttpRequestParseStatus::InvalidRequestLine);
    }

    HttpRequestParseStatus method_status = ParseMethod(
        begin, static_cast<size_t>(first_space - begin), &request_.method);
    if (method_status != HttpRequestParseStatus::Complete) {
        return Fail(method_status);
    }

    const uint8_t *target = first_space + 1U;
    const size_t target_size = static_cast<size_t>(second_space - target);
    if (target_size > kHttpMaximumRequestTargetBytes) {
        return Fail(HttpRequestParseStatus::RequestTargetTooLong);
    }
    if (target_size == 0U || target[0] != '/') {
        return Fail(HttpRequestParseStatus::InvalidRequestTarget);
    }
    for (size_t i = 0U; i < target_size; ++i) {
        if (target[i] <= 0x20U || target[i] > 0x7eU || target[i] == '#') {
            return Fail(HttpRequestParseStatus::InvalidRequestTarget);
        }
    }

    const uint8_t *version = second_space + 1U;
    const size_t version_size =
        static_cast<size_t>(request_line_end - version);
    if (version_size != 8U || memcmp(version, "HTTP/1.1", 8U) != 0) {
        return Fail(HttpRequestParseStatus::UnsupportedVersion);
    }

    request_.raw_target = HttpStringView(
        reinterpret_cast<const char *>(target), target_size);
    const uint8_t *query = static_cast<const uint8_t *>(
        memchr(target, '?', target_size));
    if (query == 0) {
        request_.raw_path = request_.raw_target;
        request_.raw_query = HttpStringView();
        request_.has_query = false;
    } else {
        request_.raw_path = HttpStringView(
            reinterpret_cast<const char *>(target),
            static_cast<size_t>(query - target));
        request_.raw_query = HttpStringView(
            reinterpret_cast<const char *>(query + 1U),
            static_cast<size_t>(target + target_size - (query + 1U)));
        request_.has_query = true;
    }

    bool saw_transfer_encoding = false;
    const uint8_t *line = request_line_end + 2U;
    while (line < end) {
        const uint8_t *line_end = FindCrlf(line, end);
        if (line_end == 0) return Fail(HttpRequestParseStatus::InvalidHeader);
        if (line_end == line) {
            line += 2U;
            break;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            return Fail(HttpRequestParseStatus::InvalidHeader);
        }

        const uint8_t *colon = static_cast<const uint8_t *>(
            memchr(line, ':', static_cast<size_t>(line_end - line)));
        if (colon == 0 || colon == line) {
            return Fail(HttpRequestParseStatus::InvalidHeader);
        }
        for (const uint8_t *cursor = line; cursor < colon; ++cursor) {
            if (!IsTokenCharacter(*cursor)) {
                return Fail(HttpRequestParseStatus::InvalidHeader);
            }
        }

        const uint8_t *value = colon + 1U;
        while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
        const uint8_t *value_end = line_end;
        while (value_end > value &&
               (value_end[-1] == ' ' || value_end[-1] == '\t')) {
            --value_end;
        }
        for (const uint8_t *cursor = value; cursor < value_end; ++cursor) {
            if ((*cursor < 0x20U && *cursor != '\t') || *cursor == 0x7fU) {
                return Fail(HttpRequestParseStatus::InvalidHeader);
            }
        }

        if (request_.header_count >= kHttpMaximumRequestHeaders) {
            return Fail(HttpRequestParseStatus::TooManyHeaders);
        }
        HttpHeaderView &header = request_.headers[request_.header_count++];
        header.name = HttpStringView(
            reinterpret_cast<const char *>(line),
            static_cast<size_t>(colon - line));
        header.value = HttpStringView(
            reinterpret_cast<const char *>(value),
            static_cast<size_t>(value_end - value));

        if (HttpStringEqualsInsensitive(header.name, "Content-Length")) {
            uint64_t parsed_length = 0U;
            if (!ParseDecimal(header.value, &parsed_length)) {
                return Fail(HttpRequestParseStatus::InvalidContentLength);
            }
            if (request_.has_content_length &&
                request_.content_length != parsed_length) {
                return Fail(HttpRequestParseStatus::ConflictingContentLength);
            }
            request_.has_content_length = true;
            request_.content_length = parsed_length;
            ++request_.content_length_count;
        } else if (HttpStringEqualsInsensitive(header.name,
                                                "Transfer-Encoding")) {
            saw_transfer_encoding = true;
        }
        line = line_end + 2U;
    }

    if (line != end) return Fail(HttpRequestParseStatus::InvalidHeader);
    if (saw_transfer_encoding && request_.has_content_length) {
        return Fail(HttpRequestParseStatus::AmbiguousBodyLength);
    }
    if (saw_transfer_encoding) {
        return Fail(HttpRequestParseStatus::UnsupportedTransferEncoding);
    }

    status_ = HttpRequestParseStatus::Complete;
    return status_;
}

const char *HttpRequestParseStatusString(HttpRequestParseStatus status) {
    switch (status) {
    case HttpRequestParseStatus::NeedMoreData:
        return "need more data";
    case HttpRequestParseStatus::Complete:
        return "complete";
    case HttpRequestParseStatus::InvalidArgument:
        return "invalid argument";
    case HttpRequestParseStatus::HeaderTooLarge:
        return "header too large";
    case HttpRequestParseStatus::InvalidLineEnding:
        return "invalid line ending";
    case HttpRequestParseStatus::InvalidRequestLine:
        return "invalid request line";
    case HttpRequestParseStatus::UnsupportedMethod:
        return "unsupported method";
    case HttpRequestParseStatus::UnsupportedVersion:
        return "unsupported HTTP version";
    case HttpRequestParseStatus::InvalidRequestTarget:
        return "invalid request target";
    case HttpRequestParseStatus::RequestTargetTooLong:
        return "request target too long";
    case HttpRequestParseStatus::InvalidHeader:
        return "invalid header";
    case HttpRequestParseStatus::TooManyHeaders:
        return "too many headers";
    case HttpRequestParseStatus::InvalidContentLength:
        return "invalid Content-Length";
    case HttpRequestParseStatus::ConflictingContentLength:
        return "conflicting Content-Length";
    case HttpRequestParseStatus::UnsupportedTransferEncoding:
        return "unsupported Transfer-Encoding";
    case HttpRequestParseStatus::AmbiguousBodyLength:
        return "ambiguous body length";
    }
    return "unknown";
}

}  // namespace remote
}  // namespace bmx
