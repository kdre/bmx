#include "semaphore.h"

_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0),
               "uint32_t atomics must be lock-free");

static inline void sem_wait_for_event(void) {
#if defined(__arm__) || defined(__aarch64__)
    __asm__ volatile ("wfe" ::: "memory");
#else
    /* Host tests use a portable spin-wait when WFE is unavailable. */
    __atomic_signal_fence(__ATOMIC_ACQUIRE);
#endif
}

static inline void sem_send_event(void) {
#if defined(__arm__) || defined(__aarch64__)
    __asm__ volatile ("sev" ::: "memory");
#endif
}

void sem_dec(uint32_t* semaphore) {
    uint32_t value = __atomic_load_n(semaphore, __ATOMIC_RELAXED);

    for (;;) {
        while (value != 0) {
            if (__atomic_compare_exchange_n(semaphore, &value, value - 1, 0,
                                            __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return;
            }
        }

        sem_wait_for_event();
        value = __atomic_load_n(semaphore, __ATOMIC_RELAXED);
    }
}

void sem_inc(uint32_t* semaphore) {
    uint32_t old = __atomic_fetch_add(semaphore, 1, __ATOMIC_RELEASE);

    if (old == 0) {
        /* Publish the 0 -> 1 transition before waking sleeping cores. */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        sem_send_event();
    }
}
