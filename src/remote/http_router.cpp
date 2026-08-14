#include "remote/http_router.h"

#include <limits.h>
#include <string.h>

namespace bmx {
namespace remote {

namespace {

void SetTextResponse(HttpResponse *response, unsigned status,
                     const char *body) {
    response->Reset(status);
    response->AddHeader("Content-Type", "text/plain; charset=utf-8");
    response->SetFixedText(body);
}

bool ValidMountPrefix(HttpStringView prefix) {
    if (prefix.data == 0 || prefix.size == 0U || prefix.data[0] != '/' ||
        prefix.size > kHttpMaximumRequestTargetBytes) {
        return false;
    }
    for (size_t i = 0U; i < prefix.size; ++i) {
        const unsigned char value = static_cast<unsigned char>(prefix.data[i]);
        if (value <= 0x20U || value > 0x7eU || value == '?' || value == '#') {
            return false;
        }
    }
    return true;
}

bool PrefixMatches(HttpStringView path, HttpStringView prefix) {
    if (!HttpStringStartsWith(path, prefix)) return false;
    if (path.size == prefix.size || prefix.data[prefix.size - 1U] == '/') {
        return true;
    }
    return path.data[prefix.size] == '/';
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

}  // namespace

HttpRouteResult::HttpRouteResult()
    : action(HttpRouteAction::Invalid),
      response(),
      body_sink(0),
      maximum_body_bytes(0U) {}

void HttpRouteResult::Respond(const HttpResponse &route_response) {
    action = HttpRouteAction::Respond;
    response = route_response;
    body_sink = 0;
    maximum_body_bytes = 0U;
}

void HttpRouteResult::ReceiveBody(HttpBodySink *sink,
                                  uint64_t maximum_bytes) {
    action = HttpRouteAction::ReceiveBody;
    response = HttpResponse();
    body_sink = sink;
    maximum_body_bytes = maximum_bytes;
}

void SetJsonErrorResponse(unsigned status, const char *message,
                          HttpResponse *response) {
    if (response == 0) return;
    response->Reset(status);
    response->AddHeader("Content-Type", "application/json");
    response->SetFixedText(message);
}

void RespondJsonError(unsigned status, const char *message,
                      HttpRouteResult *result) {
    if (result == 0) return;
    HttpResponse response;
    SetJsonErrorResponse(status, message, &response);
    result->Respond(response);
}

bool ConstantTimePasswordMatches(const HttpRequestHead &request,
                                 const char *password) {
    if (password == 0 || password[0] == '\0') return true;
    if (request.HeaderCount("X-Password") != 1U) return false;
    HttpStringView supplied;
    if (!request.Header("X-Password", &supplied)) return false;
    const size_t expected = strlen(password);
    size_t difference = supplied.size ^ expected;
    const size_t maximum = supplied.size > expected ? supplied.size : expected;
    for (size_t i = 0U; i < maximum; ++i) {
        const unsigned char left = i < supplied.size
                                       ? static_cast<unsigned char>(supplied.data[i])
                                       : 0U;
        const unsigned char right = i < expected
                                        ? static_cast<unsigned char>(password[i])
                                        : 0U;
        difference |= static_cast<size_t>(left ^ right);
    }
    return difference == 0U;
}

bool DecodePercent(HttpStringView source, char *destination, size_t capacity) {
    if (destination == 0 || capacity == 0U) return false;
    size_t output = 0U;
    for (size_t input = 0U; input < source.size; ++input) {
        unsigned char value = static_cast<unsigned char>(source.data[input]);
        if (value == '%') {
            if (input + 2U >= source.size) return false;
            const int high = HexValue(source.data[input + 1U]);
            const int low = HexValue(source.data[input + 2U]);
            if (high < 0 || low < 0) return false;
            value = static_cast<unsigned char>((high << 4) | low);
            input += 2U;
        }
        if (value == 0U || output + 1U >= capacity) return false;
        destination[output++] = static_cast<char>(value);
    }
    destination[output] = '\0';
    return true;
}

bool ParseUnsignedDecimal(HttpStringView value, uint64_t *result) {
    if (result == 0 || value.data == 0 || value.size == 0U) return false;
    uint64_t number = 0U;
    for (size_t i = 0U; i < value.size; ++i) {
        const char digit = value.data[i];
        if (digit < '0' || digit > '9') return false;
        const uint64_t add = static_cast<uint64_t>(digit - '0');
        if (number > (UINT64_MAX - add) / 10U) return false;
        number = number * 10U + add;
    }
    *result = number;
    return true;
}

bool ParseUnsignedDecimal(HttpStringView value, uint32_t minimum,
                          uint32_t maximum, uint32_t *result) {
    uint64_t parsed = 0U;
    if (result == 0 || !ParseUnsignedDecimal(value, &parsed) ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    *result = static_cast<uint32_t>(parsed);
    return true;
}

bool QueryValue(HttpStringView query, const char *name,
                HttpStringView *value, unsigned *count) {
    if (name == 0 || value == 0 || count == 0) return false;
    *value = HttpStringView();
    *count = 0U;
    const size_t name_size = strlen(name);
    size_t begin = 0U;
    while (begin <= query.size) {
        size_t end = begin;
        while (end < query.size && query.data[end] != '&') ++end;
        size_t equal = begin;
        while (equal < end && query.data[equal] != '=') ++equal;
        if (equal - begin == name_size && equal < end &&
            memcmp(query.data + begin, name, name_size) == 0) {
            ++*count;
            *value = HttpStringView(query.data + equal + 1U,
                                    end - equal - 1U);
        }
        if (end == query.size) break;
        begin = end + 1U;
    }
    return true;
}

bool OnlyQueryNames(HttpStringView query, const char *first,
                    const char *second, const char *third,
                    const char *fourth) {
    if (first == 0 || query.data == 0 || query.size == 0U ||
        query.data[query.size - 1U] == '&') {
        return false;
    }
    size_t begin = 0U;
    while (begin < query.size) {
        size_t end = begin;
        while (end < query.size && query.data[end] != '&') ++end;
        size_t equal = begin;
        while (equal < end && query.data[equal] != '=') ++equal;
        if (equal == begin || equal == end) return false;
        const HttpStringView name(query.data + begin, equal - begin);
        if (!HttpStringEquals(name, first) &&
            (second == 0 || !HttpStringEquals(name, second)) &&
            (third == 0 || !HttpStringEquals(name, third)) &&
            (fourth == 0 || !HttpStringEquals(name, fourth))) {
            return false;
        }
        begin = end + 1U;
    }
    return true;
}

void HttpRouter::ErrorResponse(HttpServerError error,
                               const HttpRequestHead *,
                               HttpResponse *response) {
    if (response == 0) return;
    switch (error) {
    case HttpServerError::BadRequest:
        SetTextResponse(response, 400U, "bad request\n");
        break;
    case HttpServerError::HeaderTooLarge:
        SetTextResponse(response, 431U, "request header too large\n");
        break;
    case HttpServerError::MethodNotAllowed:
        SetTextResponse(response, 405U, "method not allowed\n");
        response->AddHeader("Allow", "GET, HEAD, PUT, POST");
        break;
    case HttpServerError::VersionNotSupported:
        SetTextResponse(response, 505U, "HTTP version not supported\n");
        break;
    case HttpServerError::RequestTimeout:
        SetTextResponse(response, 408U, "request timeout\n");
        break;
    case HttpServerError::LengthRequired:
        SetTextResponse(response, 411U, "Content-Length required\n");
        break;
    case HttpServerError::PayloadTooLarge:
        SetTextResponse(response, 413U, "request body too large\n");
        break;
    case HttpServerError::UnexpectedBodyData:
        SetTextResponse(response, 400U, "data after request body\n");
        break;
    case HttpServerError::BodyRejected:
        SetTextResponse(response, 400U, "request body rejected\n");
        break;
    case HttpServerError::InternalError:
        SetTextResponse(response, 500U, "internal server error\n");
        break;
    }
}

CompositeHttpRouter::CompositeHttpRouter() : mount_count_(0U) {}

bool CompositeHttpRouter::Mount(HttpStringView raw_path_prefix,
                                HttpRouter *router) {
    if (router == 0 || !ValidMountPrefix(raw_path_prefix) ||
        mount_count_ >= kHttpMaximumRouterMounts) {
        return false;
    }
    for (size_t i = 0U; i < mount_count_; ++i) {
        if (mounts_[i].prefix.size == raw_path_prefix.size &&
            HttpStringStartsWith(mounts_[i].prefix, raw_path_prefix)) {
            return false;
        }
    }
    mounts_[mount_count_].prefix = raw_path_prefix;
    mounts_[mount_count_].router = router;
    ++mount_count_;
    return true;
}

bool CompositeHttpRouter::Mount(const char *raw_path_prefix,
                                HttpRouter *router) {
    return raw_path_prefix != 0 && Mount(HttpString(raw_path_prefix), router);
}

HttpRouter *CompositeHttpRouter::Match(HttpStringView raw_path) const {
    HttpRouter *matched = 0;
    size_t matched_size = 0U;
    for (size_t i = 0U; i < mount_count_; ++i) {
        if (mounts_[i].prefix.size >= matched_size &&
            PrefixMatches(raw_path, mounts_[i].prefix)) {
            matched = mounts_[i].router;
            matched_size = mounts_[i].prefix.size;
        }
    }
    return matched;
}

void CompositeHttpRouter::Route(const HttpRequestHead &request,
                                HttpRouteResult *result) {
    if (result == 0) return;
    HttpRouter *router = Match(request.raw_path);
    if (router != 0) {
        router->Route(request, result);
        return;
    }
    HttpResponse response;
    SetTextResponse(&response, 404U, "not found\n");
    result->Respond(response);
}

void CompositeHttpRouter::ErrorResponse(HttpServerError error,
                                        const HttpRequestHead *request,
                                        HttpResponse *response) {
    HttpRouter *router = request == 0 ? 0 : Match(request->raw_path);
    if (router != 0) {
        router->ErrorResponse(error, request, response);
    } else {
        HttpRouter::ErrorResponse(error, request, response);
    }
}

}  // namespace remote
}  // namespace bmx
