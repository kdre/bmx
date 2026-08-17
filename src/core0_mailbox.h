//
// core0_mailbox.h
//
// Single-producer/single-consumer request hand-off for a secondary CPU core
// calling platform services which must run on Circle's primary core.
//

#ifndef BMX_CORE0_MAILBOX_H
#define BMX_CORE0_MAILBOX_H

#include <assert.h>
#include <stdint.h>

namespace bmx {

class Core0Mailbox {
public:
  enum State : uint32_t {
    Idle = 0,
    Pending = 1,
    Completed = 2,
  };

  Core0Mailbox() : mState(Idle) {}

  void RequestAndWait() {
    const uint32_t initialState =
        __atomic_load_n(&mState, __ATOMIC_RELAXED);
    assert(initialState == Idle);
    (void) initialState;

    // The caller fills the request payload before publishing Pending.
    __atomic_store_n(&mState, Pending, __ATOMIC_RELEASE);
    Signal();

    while (__atomic_load_n(&mState, __ATOMIC_ACQUIRE) != Completed) {
      WaitForSignal();
    }

    // The result has been consumed before the mailbox becomes reusable.
    __atomic_store_n(&mState, Idle, __ATOMIC_RELEASE);
  }

  bool HasPendingRequest() const {
    return __atomic_load_n(&mState, __ATOMIC_ACQUIRE) == Pending;
  }

  void CompleteRequest() {
    const uint32_t initialState =
        __atomic_load_n(&mState, __ATOMIC_RELAXED);
    assert(initialState == Pending);
    (void) initialState;

    // The consumer fills the result before publishing Completed.
    __atomic_store_n(&mState, Completed, __ATOMIC_RELEASE);
    Signal();
  }

  bool IsIdle() const {
    return __atomic_load_n(&mState, __ATOMIC_ACQUIRE) == Idle;
  }

  static void WaitForSignal() {
#if defined(__aarch64__) || defined(__arm__)
    asm volatile("wfe" ::: "memory");
#else
    // Host tests deliberately spin on the atomic state transition.
    asm volatile("" ::: "memory");
#endif
  }

  static void Signal() {
#if defined(__aarch64__)
    asm volatile("dsb sy\n\tsev" ::: "memory");
#elif defined(__arm__)
    asm volatile("dsb\n\tsev" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
  }

private:
  static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr),
                "Core0 mailbox state must be lock-free");

  uint32_t mState;
};

} // namespace bmx

#endif
