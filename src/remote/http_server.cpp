#include "remote/http_server.h"

namespace bmx {
namespace remote {

HttpServerConfig::HttpServerConfig()
    : maximum_connections(kHttpMaximumConnections),
      header_timeout_ms(10000U),
      body_timeout_ms(300000U),
      idle_timeout_ms(10000U),
      write_timeout_ms(10000U),
      stream_idle_timeout_ms(0U) {}

bool HttpServerConfig::Valid() const {
    return maximum_connections != 0U &&
           maximum_connections <= kHttpMaximumConnections;
}

HttpConnection::HttpConnection()
    : active_(false),
      state_(kIdle),
      transport_(0),
      router_(0),
      config_(),
      parser_(),
      body_sink_(0),
      body_sink_ended_(true),
      body_remaining_(0U),
      read_size_(0U),
      read_offset_(0U),
      writer_(),
      response_completion_(0),
      completion_notified_(false),
      phase_timing_pending_(false),
      phase_started_ms_(0U),
      last_progress_ms_(0U) {}

bool HttpConnection::Begin(HttpTransport *transport, HttpRouter *router,
                           const HttpServerConfig &config,
                           uint64_t now_ms) {
    if (active_ || transport_ != 0 || transport == 0 || router == 0 ||
        !config.Valid()) {
        return false;
    }
    active_ = true;
    state_ = kReadingHead;
    transport_ = transport;
    router_ = router;
    config_ = config;
    parser_.Reset();
    body_sink_ = 0;
    body_sink_ended_ = true;
    body_remaining_ = 0U;
    read_size_ = 0U;
    read_offset_ = 0U;
    response_completion_ = 0;
    completion_notified_ = false;
    phase_timing_pending_ = false;
    phase_started_ms_ = now_ms;
    last_progress_ms_ = now_ms;
    return true;
}

bool HttpConnection::Expired(uint64_t now_ms, uint64_t start_ms,
                             uint64_t duration_ms) {
    // Cooperative response polling may advance a connection recursively with
    // a fresher timestamp before the outer server poll reaches that slot.
    // The outer timestamp is then stale, not evidence of a huge timeout.
    return duration_ms != 0U && now_ms >= start_ms &&
           now_ms - start_ms >= duration_ms;
}

const HttpRequestHead *HttpConnection::ErrorRequest() const {
    return parser_.request().raw_path.data == 0 ? 0 : &parser_.request();
}

HttpServerError HttpConnection::ParseError(
    HttpRequestParseStatus status) const {
    switch (status) {
    case HttpRequestParseStatus::HeaderTooLarge:
    case HttpRequestParseStatus::RequestTargetTooLong:
    case HttpRequestParseStatus::TooManyHeaders:
        return HttpServerError::HeaderTooLarge;
    case HttpRequestParseStatus::UnsupportedMethod:
        return HttpServerError::MethodNotAllowed;
    case HttpRequestParseStatus::UnsupportedVersion:
        return HttpServerError::VersionNotSupported;
    default:
        return HttpServerError::BadRequest;
    }
}

void HttpConnection::RouteRequest(uint64_t now_ms) {
    HttpRouteResult route;
    router_->Route(parser_.request(), &route);
    if (route.action == HttpRouteAction::Respond) {
        BeginResponse(route.response, now_ms);
        return;
    }
    if (route.action != HttpRouteAction::ReceiveBody ||
        route.body_sink == 0) {
        BeginError(HttpServerError::InternalError, now_ms);
        return;
    }

    body_sink_ = route.body_sink;
    body_sink_ended_ = false;
    if (!parser_.request().has_content_length) {
        AbortBody(HttpBodyAbortReason::MissingLength);
        BeginError(HttpServerError::LengthRequired, now_ms);
        return;
    }
    if (parser_.request().content_length > route.maximum_body_bytes) {
        AbortBody(HttpBodyAbortReason::TooLarge);
        BeginError(HttpServerError::PayloadTooLarge, now_ms);
        return;
    }

    body_remaining_ = parser_.request().content_length;
    state_ = kReadingBody;
    // Route() may synchronously hash an existing target.  Arm the new phase
    // from the next Poll() timestamp so time spent inside the previous phase
    // cannot make this body immediately stale.
    phase_timing_pending_ = true;
    if (body_remaining_ == 0U) {
        if (read_offset_ < read_size_) {
            AbortBody(HttpBodyAbortReason::UnexpectedData);
            BeginError(HttpServerError::UnexpectedBodyData, now_ms);
        } else {
            FinishBody(now_ms);
        }
    }
}

void HttpConnection::AbortBody(HttpBodyAbortReason reason) {
    if (body_sink_ != 0 && !body_sink_ended_) {
        body_sink_->Abort(reason);
        body_sink_ended_ = true;
    }
    body_sink_ = 0;
}

void HttpConnection::FinishBody(uint64_t now_ms) {
    if (body_sink_ == 0 || body_sink_ended_) {
        BeginError(HttpServerError::InternalError, now_ms);
        return;
    }
    HttpBodySink *sink = body_sink_;
    body_sink_ended_ = true;
    body_sink_ = 0;
    HttpResponse response;
    if (!sink->Finish(&response)) {
        sink->Abort(HttpBodyAbortReason::SinkRejected);
        BeginError(HttpServerError::BodyRejected, now_ms);
        return;
    }
    BeginResponse(response, now_ms);
}

void HttpConnection::RejectBody(uint64_t now_ms) {
    if (body_sink_ == 0 || body_sink_ended_) {
        BeginError(HttpServerError::InternalError, now_ms);
        return;
    }
    HttpBodySink *sink = body_sink_;
    body_sink_ended_ = true;
    body_sink_ = 0;
    HttpResponse response;
    if (sink->Reject(&response)) {
        BeginResponse(response, now_ms);
        return;
    }
    sink->Abort(HttpBodyAbortReason::SinkRejected);
    BeginError(HttpServerError::BodyRejected, now_ms);
}

void HttpConnection::ProcessBufferedInput(uint64_t now_ms) {
    while (active_ && read_offset_ < read_size_) {
        if (state_ == kReadingHead) {
            size_t consumed = 0U;
            const HttpRequestParseStatus status = parser_.Feed(
                read_buffer_ + read_offset_, read_size_ - read_offset_,
                &consumed);
            read_offset_ += consumed;
            if (status == HttpRequestParseStatus::NeedMoreData) break;
            if (status != HttpRequestParseStatus::Complete) {
                BeginError(ParseError(status), now_ms);
                break;
            }
            RouteRequest(now_ms);
            continue;
        }

        if (state_ == kReadingBody) {
            const size_t available = read_size_ - read_offset_;
            if (static_cast<uint64_t>(available) > body_remaining_) {
                AbortBody(HttpBodyAbortReason::UnexpectedData);
                BeginError(HttpServerError::UnexpectedBodyData, now_ms);
                break;
            }
            if (available != 0U &&
                !body_sink_->Write(read_buffer_ + read_offset_, available)) {
                read_offset_ += available;
                RejectBody(now_ms);
                break;
            }
            read_offset_ += available;
            body_remaining_ -= static_cast<uint64_t>(available);
            if (body_remaining_ == 0U) FinishBody(now_ms);
            continue;
        }

        // An early response intentionally ignores an unread request body.  It
        // will be discarded when the connection closes.
        break;
    }

    if (read_offset_ == read_size_) {
        read_offset_ = 0U;
        read_size_ = 0U;
    }
}

void HttpConnection::BeginError(HttpServerError error, uint64_t now_ms) {
    HttpResponse response;
    router_->ErrorResponse(error, ErrorRequest(), &response);
    BeginResponse(response, now_ms);
}

bool HttpConnection::BeginResponse(const HttpResponse &response,
                                   uint64_t now_ms) {
    (void)now_ms;
    const bool suppress_body =
        parser_.status() == HttpRequestParseStatus::Complete &&
        parser_.request().method == HttpMethod::Head;
    HttpResponseStartStatus start = writer_.Start(response, suppress_body);
    if (start != HttpResponseStartStatus::Ok) {
        if (response.completion != 0) {
            response.completion->Complete(HttpCompletionReason::InvalidResponse);
        }
        HttpResponse fallback;
        router_->ErrorResponse(HttpServerError::InternalError, ErrorRequest(),
                               &fallback);
        fallback.completion = 0;
        start = writer_.Start(fallback, suppress_body);
        if (start != HttpResponseStartStatus::Ok) {
            Terminate(HttpCompletionReason::InvalidResponse,
                      HttpBodyAbortReason::TransportError);
            return false;
        }
    }
    state_ = kWritingResponse;
    // Finish() may synchronously sync and read back a large file.  As above,
    // begin response timing only when control returns to the poll loop.
    phase_timing_pending_ = true;
    response_completion_ = writer_.completion();
    completion_notified_ = false;
    return true;
}

void HttpConnection::NotifyCompletion(HttpCompletionReason reason) {
    if (completion_notified_) return;
    completion_notified_ = true;
    HttpCompletion *completion = response_completion_;
    response_completion_ = 0;
    if (completion != 0) completion->Complete(reason);
}

void HttpConnection::Terminate(HttpCompletionReason reason,
                               HttpBodyAbortReason body_reason) {
    if (!active_) return;
    AbortBody(body_reason);
    if (writer_.active()) writer_.Abort();
    NotifyCompletion(reason);
    if (transport_ != 0) transport_->Close();
    active_ = false;
    state_ = kIdle;
}

void HttpConnection::PollResponse(uint64_t now_ms, bool *poll_made_progress) {
    if (writer_.streaming() && read_offset_ < read_size_) {
        Terminate(HttpCompletionReason::TransportError,
                  HttpBodyAbortReason::UnexpectedData);
        return;
    }
    if (writer_.has_pending_output() &&
        Expired(now_ms, last_progress_ms_, config_.write_timeout_ms)) {
        Terminate(HttpCompletionReason::Timeout, HttpBodyAbortReason::Timeout);
        return;
    }
    if (writer_.streaming() && !writer_.has_pending_output() &&
        Expired(now_ms, last_progress_ms_,
                config_.stream_idle_timeout_ms)) {
        Terminate(HttpCompletionReason::Timeout, HttpBodyAbortReason::Timeout);
        return;
    }

    bool response_progress = false;
    const HttpResponseWriteStatus status =
        writer_.Poll(transport_, &response_progress);
    if (response_progress) {
        last_progress_ms_ = now_ms;
        if (poll_made_progress != 0) *poll_made_progress = true;
    }
    switch (status) {
    case HttpResponseWriteStatus::Pending:
        return;
    case HttpResponseWriteStatus::WaitingForStream: {
        // A connection-close stream may remain quiet indefinitely.  Probe
        // the request side while no response bytes are pending so a peer FIN
        // (or illegal data after the single request) cannot pin the limited
        // connection slots until another log byte happens to arrive.
        const HttpIoResult probe = transport_->Read(read_buffer_,
                                                    sizeof(read_buffer_));
        if (probe.status == HttpIoStatus::WouldBlock && probe.size == 0U) {
            return;
        }
        if (probe.status == HttpIoStatus::Closed && probe.size == 0U) {
            Terminate(HttpCompletionReason::ClientDisconnected,
                      HttpBodyAbortReason::ClientDisconnected);
            return;
        }
        Terminate(HttpCompletionReason::TransportError,
                  HttpBodyAbortReason::TransportError);
        return;
    }
    case HttpResponseWriteStatus::Complete:
        Terminate(HttpCompletionReason::ResponseSent,
                  HttpBodyAbortReason::ServerStopped);
        return;
    case HttpResponseWriteStatus::ClientDisconnected:
        Terminate(HttpCompletionReason::ClientDisconnected,
                  HttpBodyAbortReason::ClientDisconnected);
        return;
    case HttpResponseWriteStatus::TransportError:
    case HttpResponseWriteStatus::InvalidState:
        Terminate(HttpCompletionReason::TransportError,
                  HttpBodyAbortReason::TransportError);
        return;
    case HttpResponseWriteStatus::StreamError:
        Terminate(HttpCompletionReason::StreamError,
                  HttpBodyAbortReason::TransportError);
        return;
    }
}

bool HttpConnection::Poll(uint64_t now_ms, bool *made_progress) {
    if (made_progress != 0) *made_progress = false;
    if (!active_) return false;
    if (phase_timing_pending_) {
        phase_timing_pending_ = false;
        phase_started_ms_ = now_ms;
        last_progress_ms_ = now_ms;
    }
    if (state_ == kWritingResponse) {
        PollResponse(now_ms, made_progress);
        return active_;
    }

    if (state_ == kReadingHead &&
        (Expired(now_ms, phase_started_ms_, config_.header_timeout_ms) ||
         Expired(now_ms, last_progress_ms_, config_.idle_timeout_ms))) {
        BeginError(HttpServerError::RequestTimeout, now_ms);
        return active_;
    }
    if (state_ == kReadingBody &&
        (Expired(now_ms, phase_started_ms_, config_.body_timeout_ms) ||
         Expired(now_ms, last_progress_ms_, config_.idle_timeout_ms))) {
        AbortBody(HttpBodyAbortReason::Timeout);
        BeginError(HttpServerError::RequestTimeout, now_ms);
        return active_;
    }

    if (read_offset_ < read_size_) {
        if (made_progress != 0) *made_progress = true;
        ProcessBufferedInput(now_ms);
        return active_;
    }

    const HttpIoResult result = transport_->Read(read_buffer_,
                                                 sizeof(read_buffer_));
    switch (result.status) {
    case HttpIoStatus::Progress:
        if (result.size == 0U || result.size > sizeof(read_buffer_)) {
            Terminate(HttpCompletionReason::TransportError,
                      HttpBodyAbortReason::TransportError);
            return false;
        }
        read_size_ = result.size;
        read_offset_ = 0U;
        last_progress_ms_ = now_ms;
        if (made_progress != 0) *made_progress = true;
        ProcessBufferedInput(now_ms);
        return active_;
    case HttpIoStatus::WouldBlock:
        if (result.size != 0U) {
            Terminate(HttpCompletionReason::TransportError,
                      HttpBodyAbortReason::TransportError);
        }
        return active_;
    case HttpIoStatus::Closed:
        Terminate(HttpCompletionReason::ClientDisconnected,
                  HttpBodyAbortReason::ClientDisconnected);
        return false;
    case HttpIoStatus::Error:
        Terminate(HttpCompletionReason::TransportError,
                  HttpBodyAbortReason::TransportError);
        return false;
    }
    return active_;
}

void HttpConnection::Stop() {
    Terminate(HttpCompletionReason::ServerStopped,
              HttpBodyAbortReason::ServerStopped);
}

HttpTransport *HttpConnection::TakeFinishedTransport() {
    if (active_) return 0;
    HttpTransport *result = transport_;
    transport_ = 0;
    router_ = 0;
    return result;
}

HttpServer::HttpServer(HttpListener *listener, HttpRouter *router,
                       const HttpServerConfig &config)
    : listener_(listener),
      router_(router),
      config_(config),
      valid_(listener != 0 && router != 0 && config.Valid()),
      polling_(),
      connections_() {}

HttpServer::~HttpServer() { Stop(); }

size_t HttpServer::active_connections() const {
    size_t count = 0U;
    for (size_t i = 0U; i < config_.maximum_connections &&
                        i < kHttpMaximumConnections;
         ++i) {
        if (connections_[i].active()) ++count;
    }
    return count;
}

void HttpServer::ReleaseFinished() {
    if (listener_ == 0) return;
    for (size_t i = 0U; i < config_.maximum_connections &&
                        i < kHttpMaximumConnections;
         ++i) {
        HttpTransport *transport = connections_[i].TakeFinishedTransport();
        if (transport != 0) listener_->Release(transport);
    }
}

HttpServerPollStatus HttpServer::Poll(uint64_t now_ms, bool *made_progress) {
    if (made_progress != 0) *made_progress = false;
    if (!valid_) return HttpServerPollStatus::InvalidConfiguration;
    ReleaseFinished();

    if (active_connections() < config_.maximum_connections) {
        HttpTransport *accepted = 0;
        const HttpAcceptStatus status = listener_->Accept(&accepted);
        if (status == HttpAcceptStatus::Error) {
            return HttpServerPollStatus::ListenerError;
        }
        if (status == HttpAcceptStatus::Accepted) {
            if (accepted == 0) return HttpServerPollStatus::ListenerError;
            bool installed = false;
            for (size_t i = 0U; i < config_.maximum_connections; ++i) {
                if (!connections_[i].active()) {
                    installed = connections_[i].Begin(
                        accepted, router_, config_, now_ms);
                    if (installed) break;
                }
            }
            if (!installed) {
                accepted->Close();
                listener_->Release(accepted);
                return HttpServerPollStatus::ListenerError;
            }
            if (made_progress != 0) *made_progress = true;
        } else if (accepted != 0) {
            return HttpServerPollStatus::ListenerError;
        }
    }

    for (size_t i = 0U; i < config_.maximum_connections; ++i) {
        if (connections_[i].active() && !polling_[i]) {
            polling_[i] = true;
            bool connection_progress = false;
            connections_[i].Poll(now_ms, &connection_progress);
            polling_[i] = false;
            if (connection_progress && made_progress != 0) {
                *made_progress = true;
            }
        }
    }
    ReleaseFinished();
    return HttpServerPollStatus::Ok;
}

void HttpServer::PollResponsesCooperatively(uint64_t now_ms) {
    if (!valid_) return;
    for (size_t i = 0U; i < config_.maximum_connections; ++i) {
        if (!polling_[i] && connections_[i].writing_response()) {
            polling_[i] = true;
            connections_[i].Poll(now_ms);
            polling_[i] = false;
        }
    }
    ReleaseFinished();
}

void HttpServer::Stop() {
    if (listener_ == 0) return;
    for (size_t i = 0U; i < config_.maximum_connections &&
                        i < kHttpMaximumConnections;
         ++i) {
        connections_[i].Stop();
    }
    ReleaseFinished();
}

}  // namespace remote
}  // namespace bmx
