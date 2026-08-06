#ifndef BMX_REMOTE_DEVELOPER_UI_ROUTER_H
#define BMX_REMOTE_DEVELOPER_UI_ROUTER_H

#include "remote/http_router.h"
#include "update/update_filesystem.h"

namespace bmx {
namespace remote {

// Streams the small first-party client from fixed SD-card paths. It is
// deliberately an exact asset allowlist, not a general file server.
class DeveloperUiRouter : public HttpRouter {
public:
    explicit DeveloperUiRouter(bmx::update::UpdateFileSystem *file_system);

    void Route(const HttpRequestHead &request,
               HttpRouteResult *result) override;
    void ErrorResponse(HttpServerError error,
                       const HttpRequestHead *request,
                       HttpResponse *response) override;

private:
    class AssetStream;

    bmx::update::UpdateFileSystem *file_system_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_DEVELOPER_UI_ROUTER_H
