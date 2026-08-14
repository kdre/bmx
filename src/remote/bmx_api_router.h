#ifndef BMX_REMOTE_BMX_API_ROUTER_H
#define BMX_REMOTE_BMX_API_ROUTER_H

#include "remote/bmx_api_types.h"
#include "remote/http_router.h"
#include "update/update_filesystem.h"

namespace bmx {
namespace remote {

enum class BmxApiExchangeStatus : uint8_t {
    Ok = 0,
    Busy,
    Timeout,
    Unavailable
};

class BmxApiBackend {
public:
    virtual ~BmxApiBackend() {}
    virtual BmxApiExchangeStatus Exchange(const BmxApiRequest &request,
                                          BmxApiResponse *response,
                                          uint32_t timeout_ms) = 0;
    virtual bmx::update::UpdateFileSystem *OpenMediaVolume(
        const char *volume) = 0;
    virtual void CloseMediaVolume(
        bmx::update::UpdateFileSystem *file_system) = 0;
    virtual void YieldMediaIo() = 0;
};

class BmxApiRouter : public HttpRouter {
public:
    BmxApiRouter(BmxApiBackend *backend, const char *password);

    void Route(const HttpRequestHead &request,
               HttpRouteResult *result) override;
    void ErrorResponse(HttpServerError error,
                       const HttpRequestHead *request,
                       HttpResponse *response) override;

private:
    enum { kMaximumEventStreams = 2 };

    class EventStream;
    class JsonSink;
    class TextSink;
    class MediaUploadSink;

    bool Authenticated(const HttpRequestHead &request) const;
    void RouteState(const HttpRequestHead &request, HttpRouteResult *result);
    void RouteMenu(const HttpRequestHead &request, HttpRouteResult *result,
                   BmxMenuAction action);
    void RouteEvents(const HttpRequestHead &request, HttpRouteResult *result);
    void RouteStorage(const HttpRequestHead &request, HttpRouteResult *result);
    void RouteFiles(const HttpRequestHead &request, HttpRouteResult *result);
    void RouteFile(const HttpRequestHead &request, HttpRouteResult *result);
    void RouteDirectory(const HttpRequestHead &request,
                        HttpRouteResult *result);
    void RouteMedia(const HttpRequestHead &request, HttpRouteResult *result);
    void RouteControls(const HttpRequestHead &request, HttpRouteResult *result);
    void RouteActions(const HttpRequestHead &request, HttpRouteResult *result);
    void RouteMenuPage(const HttpRequestHead &request, HttpRouteResult *result,
                       BmxApiOperation operation);
    void RouteControl(const HttpRequestHead &request, HttpRouteResult *result,
                      HttpStringView key);
    void RouteAction(const HttpRequestHead &request, HttpRouteResult *result,
                     HttpStringView key);
    void RouteInput(const HttpRequestHead &request, HttpRouteResult *result);
    void RouteTextInput(const HttpRequestHead &request,
                        HttpRouteResult *result);
    void RouteFileRename(const HttpRequestHead &request,
                         HttpRouteResult *result);
    void RouteDirectoryRename(const HttpRequestHead &request,
                              HttpRouteResult *result);
    void RouteScreenshot(const HttpRequestHead &request,
                         HttpRouteResult *result);
    void RouteAudio(const HttpRequestHead &request, HttpRouteResult *result,
                    bool wav);
    bool MediaPathAvailable(const char *path, const char *other_path,
                            HttpRouteResult *result,
                            bool include_children = false);
    bool Exchange(const BmxApiRequest &request, uint32_t timeout_ms,
                  HttpRouteResult *result);
    bool BuildResponse(const BmxApiResponse &api_response,
                       HttpResponse *response);
    void Invalidate(uint32_t resources);
    void InvalidateSuccessfulOperation(const BmxApiRequest &request);
    void PollMenuInvalidation();
    void EventStreamReleased(EventStream *stream);
    void UploadReleased(MediaUploadSink *sink);
    static void CooperativeYield(void *context);

    BmxApiBackend *backend_;
    const char *password_;
    MediaUploadSink *active_upload_;
    EventStream *active_event_streams_[kMaximumEventStreams];
    uint32_t request_token_;
    uint32_t event_revision_;
    uint32_t menu_revision_;
};

}  // namespace remote
}  // namespace bmx

#endif
