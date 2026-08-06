#include "remote/http_types.h"

#include <string.h>

namespace bmx {
namespace remote {

namespace {

char AsciiLower(char value) {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

}  // namespace

HttpStringView HttpString(const char *value) {
    return value == 0 ? HttpStringView() : HttpStringView(value, strlen(value));
}

bool HttpStringEquals(HttpStringView value, const char *expected) {
    if (expected == 0) return false;
    const size_t expected_size = strlen(expected);
    return value.size == expected_size &&
           (value.size == 0U || memcmp(value.data, expected, value.size) == 0);
}

bool HttpStringEqualsInsensitive(HttpStringView value, const char *expected) {
    if (expected == 0) return false;
    const size_t expected_size = strlen(expected);
    if (value.size != expected_size) return false;
    for (size_t i = 0U; i < value.size; ++i) {
        if (AsciiLower(value.data[i]) != AsciiLower(expected[i])) return false;
    }
    return true;
}

bool HttpStringStartsWith(HttpStringView value, HttpStringView prefix) {
    return prefix.size <= value.size &&
           (prefix.size == 0U ||
            memcmp(value.data, prefix.data, prefix.size) == 0);
}

const char *HttpMethodName(HttpMethod method) {
    switch (method) {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Head:
        return "HEAD";
    case HttpMethod::Put:
        return "PUT";
    case HttpMethod::Post:
        return "POST";
    }
    return "";
}

HttpRequestHead::HttpRequestHead()
    : method(HttpMethod::Get),
      raw_target(),
      raw_path(),
      raw_query(),
      has_query(false),
      header_count(0U),
      has_content_length(false),
      content_length(0U),
      content_length_count(0U) {}

bool HttpRequestHead::Header(const char *name, HttpStringView *value) const {
    if (name == 0 || value == 0) return false;
    for (size_t i = 0U; i < header_count; ++i) {
        if (HttpStringEqualsInsensitive(headers[i].name, name)) {
            *value = headers[i].value;
            return true;
        }
    }
    return false;
}

size_t HttpRequestHead::HeaderCount(const char *name) const {
    if (name == 0) return 0U;
    size_t count = 0U;
    for (size_t i = 0U; i < header_count; ++i) {
        if (HttpStringEqualsInsensitive(headers[i].name, name)) ++count;
    }
    return count;
}

HttpResponse::HttpResponse()
    : status_code(500U),
      reason_phrase(),
      header_count(0U),
      body_kind(HttpResponseBodyKind::Empty),
      fixed_body(0),
      fixed_body_size(0U),
      declared_content_length(0U),
      stream(0),
      completion(0) {}

void HttpResponse::Reset(unsigned status, const char *reason) {
    status_code = status;
    reason_phrase = reason == 0 ? HttpString(HttpReasonPhrase(status))
                                : HttpString(reason);
    header_count = 0U;
    body_kind = HttpResponseBodyKind::Empty;
    fixed_body = 0;
    fixed_body_size = 0U;
    declared_content_length = 0U;
    stream = 0;
    completion = 0;
}

bool HttpResponse::AddHeader(HttpStringView name, HttpStringView value) {
    if (header_count >= kHttpMaximumResponseHeaders) return false;
    headers[header_count].name = name;
    headers[header_count].value = value;
    ++header_count;
    return true;
}

bool HttpResponse::AddHeader(const char *name, const char *value) {
    return name != 0 && value != 0 && AddHeader(HttpString(name), HttpString(value));
}

void HttpResponse::SetEmptyBody() {
    body_kind = HttpResponseBodyKind::Empty;
    fixed_body = 0;
    fixed_body_size = 0U;
    declared_content_length = 0U;
    stream = 0;
}

void HttpResponse::SetFixedBody(const uint8_t *body, size_t size) {
    body_kind = HttpResponseBodyKind::Fixed;
    fixed_body = body;
    fixed_body_size = size;
    declared_content_length = static_cast<uint64_t>(size);
    stream = 0;
}

void HttpResponse::SetFixedText(const char *body) {
    if (body == 0) {
        SetEmptyBody();
        return;
    }
    SetFixedBody(reinterpret_cast<const uint8_t *>(body), strlen(body));
}

void HttpResponse::SetHeadOnly(uint64_t content_length) {
    body_kind = HttpResponseBodyKind::HeadOnly;
    fixed_body = 0;
    fixed_body_size = 0U;
    declared_content_length = content_length;
    stream = 0;
}

void HttpResponse::SetStream(HttpResponseStream *body_stream) {
    body_kind = HttpResponseBodyKind::Stream;
    fixed_body = 0;
    fixed_body_size = 0U;
    declared_content_length = 0U;
    stream = body_stream;
}

const char *HttpReasonPhrase(unsigned status_code) {
    switch (status_code) {
    case 200U:
        return "OK";
    case 201U:
        return "Created";
    case 202U:
        return "Accepted";
    case 204U:
        return "No Content";
    case 400U:
        return "Bad Request";
    case 401U:
        return "Unauthorized";
    case 403U:
        return "Forbidden";
    case 404U:
        return "Not Found";
    case 405U:
        return "Method Not Allowed";
    case 408U:
        return "Request Timeout";
    case 409U:
        return "Conflict";
    case 411U:
        return "Length Required";
    case 413U:
        return "Content Too Large";
    case 415U:
        return "Unsupported Media Type";
    case 417U:
        return "Expectation Failed";
    case 422U:
        return "Unprocessable Content";
    case 429U:
        return "Too Many Requests";
    case 431U:
        return "Request Header Fields Too Large";
    case 500U:
        return "Internal Server Error";
    case 503U:
        return "Service Unavailable";
    case 505U:
        return "HTTP Version Not Supported";
    case 507U:
        return "Insufficient Storage";
    default:
        return "Unknown";
    }
}

}  // namespace remote
}  // namespace bmx
