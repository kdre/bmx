#include "remote/command_mailbox.h"

namespace bmx {
namespace remote {

CommandMailbox::CommandMailbox() : lock_(), command_(RemoteCommand::None) {}

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

bool CommandMailbox::Post(RemoteCommand command)
{
    if (command == RemoteCommand::None) return false;
    Lock();
    const bool accepted = command_ == RemoteCommand::None;
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

bool CommandMailbox::Pending() const
{
    Lock();
    const bool pending = command_ != RemoteCommand::None;
    Unlock();
    return pending;
}

}  // namespace remote
}  // namespace bmx
