#ifndef BMX_REMOTE_HTTP_ROUTER_H
#define BMX_REMOTE_HTTP_ROUTER_H

#include "remote/http_body_sink.h"

namespace bmx {
namespace remote {

enum class HttpServerError : uint8_t {
    BadRequest = 0,
    HeaderTooLarge,
    MethodNotAllowed,
    VersionNotSupported,
    RequestTimeout,
    LengthRequired,
    PayloadTooLarge,
    UnexpectedBodyData,
    BodyRejected,
    InternalError
};

enum class HttpRouteAction : uint8_t {
    Invalid = 0,
    Respond,
    ReceiveBody
};

struct HttpRouteResult {
    HttpRouteAction action;
    HttpResponse response;
    HttpBodySink *body_sink;
    uint64_t maximum_body_bytes;

    HttpRouteResult();
    void Respond(const HttpResponse &route_response);
    void ReceiveBody(HttpBodySink *sink, uint64_t maximum_bytes);
};

void SetJsonErrorResponse(unsigned status, const char *message,
                          HttpResponse *response);
void RespondJsonError(unsigned status, const char *message,
                      HttpRouteResult *result);

bool ConstantTimePasswordMatches(const HttpRequestHead &request,
                                 const char *password);
bool DecodePercent(HttpStringView source, char *destination, size_t capacity);
bool ParseUnsignedDecimal(HttpStringView value, uint64_t *result);
bool ParseUnsignedDecimal(HttpStringView value, uint32_t minimum,
                          uint32_t maximum, uint32_t *result);
bool QueryValue(HttpStringView query, const char *name,
                HttpStringView *value, unsigned *count);
bool OnlyQueryNames(HttpStringView query, const char *first,
                    const char *second = 0, const char *third = 0,
                    const char *fourth = 0);

class HttpRouter {
 public:
    virtual ~HttpRouter() {}

    // Called after the complete request head and before any body byte is
    // consumed.  This is where a protocol router authenticates, validates its
    // raw target and either rejects immediately or supplies a streaming sink.
    virtual void Route(const HttpRequestHead &request,
                       HttpRouteResult *result) = 0;

    // Routers may override this to retain their own wire-format errors.  The
    // default is a small text/plain HTTP error suitable before a namespace can
    // be identified.
    virtual void ErrorResponse(HttpServerError error,
                               const HttpRequestHead *request,
                               HttpResponse *response);
};

static const size_t kHttpMaximumRouterMounts = 8U;

// A fixed, allocation-free namespace dispatcher.  Matching is performed on
// the untouched raw path; it never percent-decodes, normalizes or interprets
// route parameters.  The longest registered prefix wins.
class CompositeHttpRouter : public HttpRouter {
 public:
    CompositeHttpRouter();

    bool Mount(HttpStringView raw_path_prefix, HttpRouter *router);
    bool Mount(const char *raw_path_prefix, HttpRouter *router);
    size_t mount_count() const { return mount_count_; }

    void Route(const HttpRequestHead &request,
               HttpRouteResult *result) override;
    void ErrorResponse(HttpServerError error,
                       const HttpRequestHead *request,
                       HttpResponse *response) override;

 private:
    struct MountEntry {
        HttpStringView prefix;
        HttpRouter *router;
    };

    HttpRouter *Match(HttpStringView raw_path) const;

    MountEntry mounts_[kHttpMaximumRouterMounts];
    size_t mount_count_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_HTTP_ROUTER_H
