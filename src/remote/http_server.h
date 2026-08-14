#ifndef BMX_REMOTE_HTTP_SERVER_H
#define BMX_REMOTE_HTTP_SERVER_H

#include "remote/http_parser.h"
#include "remote/http_response_writer.h"
#include "remote/http_router.h"

namespace bmx {
namespace remote {

struct HttpServerConfig {
    size_t maximum_connections;
    uint64_t header_timeout_ms;
    uint64_t body_timeout_ms;
    uint64_t idle_timeout_ms;
    uint64_t write_timeout_ms;
    // Zero keeps a quiet response stream open indefinitely.  A non-zero value
    // limits time without bytes produced or sent by that stream.
    uint64_t stream_idle_timeout_ms;

    HttpServerConfig();
    bool Valid() const;
};

enum class HttpAcceptStatus : uint8_t {
    Accepted = 0,
    WouldBlock,
    Error
};

class HttpListener {
 public:
    virtual ~HttpListener() {}
    virtual HttpAcceptStatus Accept(HttpTransport **transport) = 0;
    // Releases the adapter object after the server has called Close().
    virtual void Release(HttpTransport *transport) = 0;
};

class HttpConnection {
 public:
    HttpConnection();

    bool Begin(HttpTransport *transport, HttpRouter *router,
               const HttpServerConfig &config, uint64_t now_ms);
    // Performs bounded work and at most one transport read or write.  The
    // optional flag reports accepted, received or transmitted bytes.
    bool Poll(uint64_t now_ms, bool *made_progress = 0);
    void Stop();

    bool active() const { return active_; }
    bool writing_response() const {
        return active_ && state_ == kWritingResponse;
    }
    HttpTransport *TakeFinishedTransport();

 private:
    enum State : uint8_t {
        kIdle = 0,
        kReadingHead,
        kReadingBody,
        kWritingResponse
    };

    static bool Expired(uint64_t now_ms, uint64_t start_ms,
                        uint64_t duration_ms);
    const HttpRequestHead *ErrorRequest() const;
    HttpServerError ParseError(HttpRequestParseStatus status) const;
    void RouteRequest(uint64_t now_ms);
    void ProcessBufferedInput(uint64_t now_ms);
    void BeginError(HttpServerError error, uint64_t now_ms);
    void RejectBody(uint64_t now_ms);
    bool BeginResponse(const HttpResponse &response, uint64_t now_ms);
    void AbortBody(HttpBodyAbortReason reason);
    void FinishBody(uint64_t now_ms);
    void PollResponse(uint64_t now_ms, bool *made_progress);
    void Terminate(HttpCompletionReason reason,
                   HttpBodyAbortReason body_reason);
    void NotifyCompletion(HttpCompletionReason reason);

    bool active_;
    State state_;
    HttpTransport *transport_;
    HttpRouter *router_;
    HttpServerConfig config_;
    HttpRequestParser parser_;
    HttpBodySink *body_sink_;
    bool body_sink_ended_;
    uint64_t body_remaining_;
    uint8_t read_buffer_[kHttpConnectionReadBufferBytes];
    size_t read_size_;
    size_t read_offset_;
    HttpResponseWriter writer_;
    HttpCompletion *response_completion_;
    bool completion_notified_;
    bool phase_timing_pending_;
    uint64_t phase_started_ms_;
    uint64_t last_progress_ms_;
};

enum class HttpServerPollStatus : uint8_t {
    Ok = 0,
    ListenerError,
    InvalidConfiguration
};

// Allocation-free connection owner.  Poll accepts at most one client and
// advances each active slot once, so the surrounding Circle task remains
// cooperative.  A default configuration permits a log follower and an upload
// concurrently.
class HttpServer {
 public:
    HttpServer(HttpListener *listener, HttpRouter *router,
               const HttpServerConfig &config = HttpServerConfig());
    ~HttpServer();

    // made_progress allows a cooperative owner to remain runnable only while
    // the non-blocking transport is actively moving a request or response.
    HttpServerPollStatus Poll(uint64_t now_ms, bool *made_progress = 0);
    // Advances only connections which are already writing a response.  This
    // is safe to call from a router's bounded cooperative callback: it keeps
    // an established log stream moving without re-entering request parsing,
    // routing, body sinks or filesystem work on another connection.
    void PollResponsesCooperatively(uint64_t now_ms);
    void Stop();

    bool valid() const { return valid_; }
    size_t active_connections() const;

 private:
    void ReleaseFinished();

    HttpListener *listener_;
    HttpRouter *router_;
    HttpServerConfig config_;
    bool valid_;
    bool polling_[kHttpMaximumConnections];
    HttpConnection connections_[kHttpMaximumConnections];
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_HTTP_SERVER_H
