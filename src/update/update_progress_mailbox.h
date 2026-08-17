#ifndef BMX_UPDATE_UPDATE_PROGRESS_MAILBOX_H
#define BMX_UPDATE_UPDATE_PROGRESS_MAILBOX_H

#include "update/update_foreground_progress.h"

#include <stddef.h>

namespace bmx {
namespace update {

// Fixed-storage handoff for a fast producer and a slower foreground UI.
// Callers provide synchronization. Numeric progress may be coalesced in the
// latest snapshot, while phase transitions are retained in display order.
class UpdateProgressMailbox {
public:
    UpdateProgressMailbox()
        : latest_(), transitions_(), latest_valid_(false), head_(0U), count_(0U)
    {
    }

    void Reset()
    {
        latest_valid_ = false;
        head_ = 0U;
        count_ = 0U;
    }

    void Present(const UpdateForegroundUiEvent &event)
    {
        const bool phase_changed =
            !latest_valid_ || latest_.phase != event.phase;
        latest_ = event;
        latest_valid_ = true;
        if (!phase_changed) return;

        if (count_ == kTransitionCapacity) {
            // Retain the newest safety-relevant transitions if a future flow
            // ever exceeds the current six-phase protocol.
            head_ = (head_ + 1U) % kTransitionCapacity;
            --count_;
        }
        const size_t tail = (head_ + count_) % kTransitionCapacity;
        transitions_[tail] = event;
        ++count_;
    }

    bool PopTransition(UpdateForegroundUiEvent *event)
    {
        if (event == 0 || count_ == 0U) return false;
        *event = transitions_[head_];
        head_ = (head_ + 1U) % kTransitionCapacity;
        --count_;
        return true;
    }

    bool Latest(UpdateForegroundUiEvent *event) const
    {
        if (event == 0 || !latest_valid_) return false;
        *event = latest_;
        return true;
    }

private:
    static const size_t kTransitionCapacity = 16U;

    UpdateForegroundUiEvent latest_;
    UpdateForegroundUiEvent transitions_[kTransitionCapacity];
    bool latest_valid_;
    size_t head_;
    size_t count_;
};

}  // namespace update
}  // namespace bmx

#endif  // BMX_UPDATE_UPDATE_PROGRESS_MAILBOX_H
