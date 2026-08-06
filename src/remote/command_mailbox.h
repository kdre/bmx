#ifndef BMX_REMOTE_COMMAND_MAILBOX_H
#define BMX_REMOTE_COMMAND_MAILBOX_H

#include <stdint.h>

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

// Fixed single-slot handoff from a transport task to the emulator safe point.
// No VICE, FatFs or shutdown call is ever made while this lock is held.
class CommandMailbox {
public:
    CommandMailbox();

    bool Post(RemoteCommand command);
    bool Take(RemoteCommand *command);
    bool Pending() const;

private:
    CommandMailbox(const CommandMailbox &);
    CommandMailbox &operator=(const CommandMailbox &);

    void Lock() const;
    void Unlock() const;

#if defined(RASPI_COMPILE)
    mutable CSpinLock lock_;
#else
    mutable std::mutex lock_;
#endif
    RemoteCommand command_;
};

}  // namespace remote
}  // namespace bmx

#endif  // BMX_REMOTE_COMMAND_MAILBOX_H
