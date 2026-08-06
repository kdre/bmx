#include "remote/http_router.h"

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
