#include "remote/command_mailbox.h"

#include <string.h>

namespace bmx {
namespace remote {

CommandMailbox::CommandMailbox()
    : lock_(), command_(RemoteCommand::None), control_state_(ControlEmpty),
      next_control_token_(0U), control_token_(0U), control_request_(),
      control_response_(), control_text_() {}

void CommandMailbox::Lock() const
{
#if defined(RASPI_COMPILE)
    lock_.Acquire();
#else
    lock_.lock();
#endif
}

void CommandMailbox::Unlock() const
{
#if defined(RASPI_COMPILE)
    lock_.Release();
#else
    lock_.unlock();
#endif
}

void CommandMailbox::ResetControlLocked()
{
    control_request_ = BmxApiRequest();
    control_response_ = BmxApiResponse();
    control_text_[0] = '\0';
    control_token_ = 0U;
    control_state_ = ControlEmpty;
}

bool CommandMailbox::Post(RemoteCommand command)
{
    if (command == RemoteCommand::None) return false;
    Lock();
    const bool accepted = command_ == RemoteCommand::None &&
                          control_state_ == ControlEmpty;
    if (accepted) command_ = command;
    Unlock();
    return accepted;
}

bool CommandMailbox::Take(RemoteCommand *command)
{
    if (command == 0) return false;
    Lock();
    const bool available = command_ != RemoteCommand::None;
    *command = command_;
    command_ = RemoteCommand::None;
    Unlock();
    return available;
}

bool CommandMailbox::PostControl(const BmxApiRequest &request,
                                 uint32_t *token)
{
    if (request.operation == BmxApiOperation::None || token == 0 ||
        (request.operation == BmxApiOperation::TextInput &&
         (request.text == 0 || request.text_size == 0U ||
          request.text_size > kBmxApiMaximumTextBytes))) return false;
    Lock();
    const bool accepted = command_ == RemoteCommand::None &&
                          control_state_ == ControlEmpty;
    if (accepted) {
        if (++next_control_token_ == 0U) ++next_control_token_;
        control_token_ = next_control_token_;
        control_request_ = request;
        if (request.operation == BmxApiOperation::TextInput) {
            memcpy(control_text_, request.text, request.text_size);
            control_text_[request.text_size] = '\0';
            control_request_.text = control_text_;
        }
        control_response_ = BmxApiResponse();
        control_state_ = ControlPosted;
        *token = control_token_;
    }
    Unlock();
    return accepted;
}

bool CommandMailbox::TakeControl(BmxApiRequest *request, uint32_t *token)
{
    if (request == 0 || token == 0) return false;
    Lock();
    const bool available = control_state_ == ControlPosted;
    if (available) {
        *request = control_request_;
        *token = control_token_;
        control_state_ = ControlRunning;
    }
    Unlock();
    return available;
}

bool CommandMailbox::CompleteControl(uint32_t token,
                                     const BmxApiResponse &response)
{
    Lock();
    const bool accepted = control_state_ == ControlRunning &&
                          control_token_ == token;
    if (accepted) {
        control_response_ = response;
        control_state_ = ControlComplete;
    } else if (control_state_ == ControlAbandoned &&
               control_token_ == token) {
        ResetControlLocked();
    }
    Unlock();
    return accepted;
}

ControlPollStatus CommandMailbox::PollControl(uint32_t token,
                                              BmxApiResponse *response)
{
    if (response == 0) return ControlPollStatus::Missing;
    Lock();
    ControlPollStatus status = ControlPollStatus::Missing;
    if (control_state_ != ControlEmpty && control_token_ == token) {
        status = ControlPollStatus::Pending;
        if (control_state_ == ControlComplete) {
            *response = control_response_;
            ResetControlLocked();
            status = ControlPollStatus::Complete;
        }
    }
    Unlock();
    return status;
}

bool CommandMailbox::CancelControl(uint32_t token,
                                   BmxApiResponse *abandoned)
{
    Lock();
    const bool cancelled = control_state_ != ControlEmpty &&
                           control_state_ != ControlAbandoned &&
                           control_token_ == token;
    if (cancelled) {
        if (abandoned != 0 && control_state_ == ControlComplete) {
            *abandoned = control_response_;
        }
        if (control_state_ == ControlRunning) {
            // Keep pointer-backed request data alive until the safe-point
            // consumer finishes. CompleteControl() will release the slot.
            control_state_ = ControlAbandoned;
        } else {
            ResetControlLocked();
        }
    }
    Unlock();
    return cancelled;
}

bool CommandMailbox::Pending() const
{
    Lock();
    const bool pending = command_ != RemoteCommand::None ||
                         control_state_ != ControlEmpty;
    Unlock();
    return pending;
}

}  // namespace remote
}  // namespace bmx
