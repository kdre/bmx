#ifndef BMX_REMOTE_COMMAND_MAILBOX_H
#define BMX_REMOTE_COMMAND_MAILBOX_H

#include <stdint.h>

#include "remote/bmx_api_types.h"

#if defined(RASPI_COMPILE)
#include <circle/spinlock.h>
#else
#include <mutex>
#endif

namespace bmx {
namespace remote {

enum class RemoteCommand : uint8_t {
    None = 0,
    SystemReboot
};

enum class ControlPollStatus : uint8_t {
    Pending = 0,
    Complete,
    Missing
};

// Fixed single-slot handoff from a transport task to the emulator safe point.
// No VICE, FatFs or shutdown call is ever made while this lock is held.
class CommandMailbox {
public:
    CommandMailbox();

    bool Post(RemoteCommand command);
    bool Take(RemoteCommand *command);
    bool PostControl(const BmxApiRequest &request, uint32_t *token);
    bool TakeControl(BmxApiRequest *request, uint32_t *token);
    bool CompleteControl(uint32_t token, const BmxApiResponse &response);
    ControlPollStatus PollControl(uint32_t token, BmxApiResponse *response);
    bool CancelControl(uint32_t token, BmxApiResponse *abandoned);
    bool Pending() const;

private:
    CommandMailbox(const CommandMailbox &);
    CommandMailbox &operator=(const CommandMailbox &);

    void Lock() const;
    void Unlock() const;
    void ResetControlLocked();

#if defined(RASPI_COMPILE)
    mutable CSpinLock lock_;
#else
    mutable std::mutex lock_;
#endif
    RemoteCommand command_;
    enum ControlState : uint8_t {
        ControlEmpty = 0,
        ControlPosted,
        ControlRunning,
        ControlAbandoned,
        ControlComplete
    };
    ControlState control_state_;
    uint32_t next_control_token_;
    uint32_t control_token_;
    BmxApiRequest control_request_;
    BmxApiResponse control_response_;
    char control_text_[kBmxApiMaximumTextBytes + 1U];
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_COMMAND_MAILBOX_H
