#pragma once
/* Trusted native host VM shared physical-turn admission, C11.
 * This gate is NOT a guest capability, a thread affinity token, or a
 * substitute for per-actor generation-checked execution ownership. */
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct OresNativePhysicalTurnGate OresNativePhysicalTurnGate;
struct OresNativePhysicalTurnGate {
    _Atomic size_t admitted;
    _Atomic size_t waiting;
    size_t maximum;
    void (*notify_free)(void*);
    void* context;
};

static inline int ores_native_turn_gate_try_acquire(
        OresNativePhysicalTurnGate* gate) {
    if (!gate) return 1;
    size_t current = atomic_load_explicit(&gate->admitted, memory_order_acquire);
    while (current < gate->maximum) {
        if (atomic_compare_exchange_weak_explicit(
                &gate->admitted, &current, current + 1,
                memory_order_acq_rel, memory_order_acquire))
            return 1;
    }
    return 0;
}

static inline void ores_native_turn_gate_release(
        OresNativePhysicalTurnGate* gate) {
    if (!gate) return;
    size_t before = atomic_fetch_sub_explicit(
            &gate->admitted, 1, memory_order_acq_rel);
    if (before == 0) abort();
    /* Caller must not hold its pool scheduler mutex: cross-pool notification
     * acquires other scheduler locks. All VM schedulers must stay alive until
     * every admitted callback has released its permit. */
    if (gate->notify_free
        && atomic_load_explicit(&gate->waiting, memory_order_acquire) > 0)
        gate->notify_free(gate->context);
}

static inline size_t ores_native_turn_gate_active(
        const OresNativePhysicalTurnGate* gate) {
    return atomic_load_explicit(&gate->admitted, memory_order_acquire);
}
