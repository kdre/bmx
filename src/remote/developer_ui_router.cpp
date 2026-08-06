#include "remote/developer_ui_router.h"

namespace bmx {
namespace remote {
namespace {

const char kUiRoot[] = "/bmx/dev/ui";
const uint64_t kMaximumUiAssetBytes = 512U * 1024U;
const char kNotFound[] = "not found\n";
const char kBadRequest[] = "bad request\n";
const char kMethodNotAllowed[] = "method not allowed\n";
const char kInternalError[] = "internal server error\n";
const char kUnavailable[] = "developer UI asset unavailable\n";
const char kContentSecurityPolicy[] =
    "default-src 'none'; script-src 'self'; style-src 'self'; "
    "connect-src 'self'; img-src 'self' data:; base-uri 'none'; "
    "form-action 'none'; frame-ancestors 'none'";

struct AssetRoute {
    const char *url_path;
    const char *file_path;
    const char *content_type;
};

const AssetRoute kAssetRoutes[] = {
    {"/bmx/dev/ui/", "bmx/dev/ui/index.html", "text/html; charset=utf-8"},
    {"/bmx/dev/ui/app.css", "bmx/dev/ui/app.css", "text/css; charset=utf-8"},
    {"/bmx/dev/ui/core.mjs", "bmx/dev/ui/core.mjs",
     "text/javascript; charset=utf-8"},
    {"/bmx/dev/ui/app.mjs", "bmx/dev/ui/app.mjs",
     "text/javascript; charset=utf-8"},
};

void AddCommonHeaders(HttpResponse *response)
{
    response->AddHeader("Cache-Control", "no-store");
    response->AddHeader("X-Content-Type-Options", "nosniff");
    response->AddHeader("Referrer-Policy", "no-referrer");
    response->AddHeader("Content-Security-Policy", kContentSecurityPolicy);
}

void SetTextResponse(HttpResponse *response, unsigned status,
                     const char *body)
{
    response->Reset(status);
    response->AddHeader("Content-Type", "text/plain; charset=utf-8");
    AddCommonHeaders(response);
    response->SetFixedText(body);
}

const AssetRoute *FindAsset(HttpStringView raw_path)
{
    if (HttpStringEquals(raw_path, kUiRoot)) {
        raw_path = HttpString("/bmx/dev/ui/");
    }
    for (size_t index = 0U;
         index < sizeof(kAssetRoutes) / sizeof(kAssetRoutes[0]); ++index) {
        if (HttpStringEquals(raw_path, kAssetRoutes[index].url_path)) {
            return &kAssetRoutes[index];
        }
    }
    return 0;
}

}  // namespace

class DeveloperUiRouter::AssetStream : public HttpResponseStream,
                                       public HttpCompletion {
public:
    AssetStream(bmx::update::UpdateReadFile *file, uint64_t size)
        : file_(file), size_(size), offset_(0U), closed_(false)
    {
    }

    HttpStreamReadResult Read(uint8_t *output, size_t capacity,
                              size_t *size) override
    {
        if (size == 0 || output == 0 || capacity == 0U || closed_ ||
            file_ == 0) {
            return HttpStreamReadResult::Error;
        }
        if (offset_ == size_) {
            *size = 0U;
            return HttpStreamReadResult::End;
        }
        const uint64_t remaining = size_ - offset_;
        const size_t count = remaining < static_cast<uint64_t>(capacity)
                                 ? static_cast<size_t>(remaining)
                                 : capacity;
        if (!file_->ReadAt(offset_, output, count)) {
            *size = 0U;
            return HttpStreamReadResult::Error;
        }
        offset_ += static_cast<uint64_t>(count);
        *size = count;
        return HttpStreamReadResult::Data;
    }

    void Cancel() override { Close(); }

    void Complete(HttpCompletionReason) override
    {
        Close();
        delete this;
    }

private:
    void Close()
    {
        if (closed_) return;
        closed_ = true;
        if (file_ != 0) (void)file_->Close();
        file_ = 0;
    }

    bmx::update::UpdateReadFile *file_;
    uint64_t size_;
    uint64_t offset_;
    bool closed_;
};

DeveloperUiRouter::DeveloperUiRouter(
    bmx::update::UpdateFileSystem *file_system)
    : file_system_(file_system)
{
}

void DeveloperUiRouter::Route(const HttpRequestHead &request,
                              HttpRouteResult *result)
{
    if (result == 0) return;
    if (request.method != HttpMethod::Get &&
        request.method != HttpMethod::Head) {
        HttpResponse response;
        SetTextResponse(&response, 405U, kMethodNotAllowed);
        response.AddHeader("Allow", "GET, HEAD");
        result->Respond(response);
        return;
    }
    if (request.has_query ||
        (request.has_content_length && request.content_length != 0U)) {
        HttpResponse response;
        SetTextResponse(&response, 400U, kBadRequest);
        result->Respond(response);
        return;
    }
    const AssetRoute *asset = FindAsset(request.raw_path);
    if (asset == 0) {
        HttpResponse response;
        SetTextResponse(&response, 404U, kNotFound);
        result->Respond(response);
        return;
    }

    if (file_system_ == 0) {
        HttpResponse response;
        SetTextResponse(&response, 503U, kUnavailable);
        result->Respond(response);
        return;
    }

    bmx::update::UpdateFileStat stat;
    if (!file_system_->Stat(asset->file_path, &stat)) {
        HttpResponse response;
        SetTextResponse(&response, 500U, kInternalError);
        result->Respond(response);
        return;
    }
    if (stat.type != bmx::update::UpdateNodeType::RegularFile) {
        HttpResponse response;
        SetTextResponse(&response, 404U, kUnavailable);
        result->Respond(response);
        return;
    }
    if (stat.size > kMaximumUiAssetBytes) {
        HttpResponse response;
        SetTextResponse(&response, 413U, "developer UI asset too large\n");
        result->Respond(response);
        return;
    }

    if (request.method == HttpMethod::Head) {
        HttpResponse response;
        response.Reset(200U);
        response.AddHeader("Content-Type", asset->content_type);
        AddCommonHeaders(&response);
        response.SetHeadOnly(stat.size);
        result->Respond(response);
        return;
    }

    bmx::update::UpdateReadFile *file = 0;
    uint64_t size = 0U;
    if (!file_system_->OpenRead(asset->file_path, &file) || file == 0 ||
        !file->GetSize(&size)) {
        if (file != 0) (void)file->Close();
        HttpResponse response;
        SetTextResponse(&response, 503U, kUnavailable);
        result->Respond(response);
        return;
    }
    if (size != stat.size) {
        (void)file->Close();
        HttpResponse response;
        SetTextResponse(&response, 503U, kUnavailable);
        result->Respond(response);
        return;
    }
    if (size > kMaximumUiAssetBytes) {
        (void)file->Close();
        HttpResponse response;
        SetTextResponse(&response, 413U, "developer UI asset too large\n");
        result->Respond(response);
        return;
    }
    AssetStream *stream = new AssetStream(file, size);
    if (stream == 0) {
        (void)file->Close();
        HttpResponse response;
        SetTextResponse(&response, 503U, kUnavailable);
        result->Respond(response);
        return;
    }

    HttpResponse response;
    response.Reset(200U);
    response.AddHeader("Content-Type", asset->content_type);
    AddCommonHeaders(&response);
    response.SetStream(stream);
    response.completion = stream;
    result->Respond(response);
}

void DeveloperUiRouter::ErrorResponse(HttpServerError error,
                                      const HttpRequestHead *,
                                      HttpResponse *response)
{
    if (response == 0) return;
    switch (error) {
    case HttpServerError::MethodNotAllowed:
        SetTextResponse(response, 405U, kMethodNotAllowed);
        response->AddHeader("Allow", "GET, HEAD");
        return;
    case HttpServerError::HeaderTooLarge:
        SetTextResponse(response, 431U, "request header too large\n");
        return;
    case HttpServerError::RequestTimeout:
        SetTextResponse(response, 408U, "request timeout\n");
        return;
    case HttpServerError::VersionNotSupported:
        SetTextResponse(response, 505U, "HTTP version not supported\n");
        return;
    case HttpServerError::PayloadTooLarge:
        SetTextResponse(response, 413U, "request body too large\n");
        return;
    case HttpServerError::InternalError:
        SetTextResponse(response, 500U, kInternalError);
        return;
    default:
        SetTextResponse(response, 400U, kBadRequest);
        return;
    }
}

}  // namespace remote
}  // namespace bmx
