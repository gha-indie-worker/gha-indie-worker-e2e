#pragma once
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <limits.h>

/*
 * Supervisor-owned LOCAL actor execution authority.
 *
 * No expiry-based preemption: an execution owner is released only after
 * the physical guest callback has completely returned. This protects
 * against overlapping carrier execution and stale completion ABA.
 * It is NOT distributed consensus or external-effect fencing.
 *
 * All functions synchronize on the authority's own mutex and never invoke
 * guest callbacks. Lock order for NativeActorScheduler: scheduler -> authority.
 */
typedef enum {
    ORES_EXEC_READY = 1,
    ORES_EXEC_RUNNING = 2,
    ORES_EXEC_SUSPENDED = 3,
    ORES_EXEC_STOPPING = 4,
    ORES_EXEC_TERMINATED = 5
} OresExecutionPhase;
typedef struct {
    uint64_t generation;
    uint64_t sequence;
    uint64_t carrier;
} OresExecutionClaim;
typedef struct {
    pthread_mutex_t mutex;
    uint64_t generation;
    uint64_t sequence;
    uint64_t owner;
    /* The token/sequence identify the turn; only its physical OS thread
     * may query or relinquish the authority. A copied claim alone is not
     * sufficient to authorize a foreign-thread release. */
    pthread_t owner_thread;
    OresExecutionPhase phase;
    int priority_inheritance_enabled;
} OresNativeExecutionAuthority;

static inline int ores_execution_init(OresNativeExecutionAuthority* a) {
    if (!a) return 0;
    a->priority_inheritance_enabled = 0;

    int initialized = 0;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT >= 0
        if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0
                && pthread_mutex_init(&a->mutex, &attr) == 0) {
            a->priority_inheritance_enabled = 1;
            initialized = 1;
        }
#endif
        if (pthread_mutexattr_destroy(&attr) != 0) __builtin_trap();
    }
    if (!initialized && pthread_mutex_init(&a->mutex, NULL) != 0) return 0;

    a->generation = 1;
    a->sequence = 0;
    a->owner = 0;
    a->phase = ORES_EXEC_READY;
    return 1;
}
static inline int ores_execution_priority_inheritance_enabled(
        OresNativeExecutionAuthority* a) {
    return a && a->priority_inheritance_enabled;
}
/* A mutex is unlocked during guest execution. pthread_mutex_destroy()
 * alone therefore does not detect a live owner or suspended continuation.
 * The embedding host must still fence concurrent callers before destruction. */
static inline int ores_execution_destroy(OresNativeExecutionAuthority* a) {
    if (!a || pthread_mutex_lock(&a->mutex) != 0) return 0;
    int quiescent = !a->owner
        && (a->phase == ORES_EXEC_READY || a->phase == ORES_EXEC_TERMINATED);
    if (pthread_mutex_unlock(&a->mutex) != 0) return 0;
    return quiescent && pthread_mutex_destroy(&a->mutex) == 0;
}
static inline void ores_execution_lock(OresNativeExecutionAuthority* a) {
    if (pthread_mutex_lock(&a->mutex) != 0) __builtin_trap();
}
static inline void ores_execution_unlock(OresNativeExecutionAuthority* a) {
    if (pthread_mutex_unlock(&a->mutex) != 0) __builtin_trap();
}

/* Only the scheduler may allow STOPPING turns that were already ADMITTED
 * before stop linearized. Java cancellation does not permit such draining. */
static inline int ores_execution_enter(OresNativeExecutionAuthority* a,
        uint64_t generation, uint64_t carrier, int already_admitted,
        OresExecutionClaim* out) {
    if (!a || !out || !carrier) return 0;
    ores_execution_lock(a);
    int ok = generation == a->generation && !a->owner
        && a->sequence != UINT64_MAX
        && (a->phase == ORES_EXEC_READY || a->phase == ORES_EXEC_SUSPENDED
            || (a->phase == ORES_EXEC_STOPPING && already_admitted));
    if (ok) {
        a->owner = carrier;
        a->owner_thread = pthread_self();
        ++a->sequence;
        if (a->phase != ORES_EXEC_STOPPING) a->phase = ORES_EXEC_RUNNING;
        *out = (OresExecutionClaim){generation, a->sequence, carrier};
    }
    ores_execution_unlock(a);
    return ok;
}
static inline int ores_execution_owns(OresNativeExecutionAuthority* a,
        OresExecutionClaim claim) {
    if (!a) return 0;
    ores_execution_lock(a);
    int ok = claim.carrier && a->owner == claim.carrier
        && claim.generation == a->generation
        && claim.sequence == a->sequence
        && pthread_equal(a->owner_thread, pthread_self());
    ores_execution_unlock(a);
    return ok;
}
/* Called only after guest code has unwound, not at the point where a Future
 * is produced. SUSPENDED never means two carriers may execute concurrently. */
static inline int ores_execution_leave(OresNativeExecutionAuthority* a,
        OresExecutionClaim claim, int suspended) {
    if (!a) return 0;
    ores_execution_lock(a);
    int ok = claim.carrier && claim.carrier == a->owner
        && claim.generation == a->generation
        && claim.sequence == a->sequence
        && (a->phase == ORES_EXEC_RUNNING || a->phase == ORES_EXEC_STOPPING)
        && pthread_equal(a->owner_thread, pthread_self());
    if (ok) {
        a->owner = 0;
        if (a->phase != ORES_EXEC_STOPPING)
            a->phase = suspended ? ORES_EXEC_SUSPENDED : ORES_EXEC_READY;
    }
    ores_execution_unlock(a);
    return ok;
}
static inline int ores_execution_stop(OresNativeExecutionAuthority* a,
        uint64_t generation) {
    if (!a) return 0;
    ores_execution_lock(a);
    int ok = generation == a->generation
        && a->phase != ORES_EXEC_STOPPING && a->phase != ORES_EXEC_TERMINATED;
    if (ok) a->phase = ORES_EXEC_STOPPING;
    ores_execution_unlock(a);
    return ok;
}
/* Trusted supervisor calls this only when every previously admitted turn
 * is completed AND its actual carrier has exited the guest execution path. */
static inline int ores_execution_retired(OresNativeExecutionAuthority* a,
        uint64_t generation) {
    if (!a) return 0;
    ores_execution_lock(a);
    int ok = generation == a->generation && !a->owner
        && a->phase == ORES_EXEC_STOPPING;
    if (ok) a->phase = ORES_EXEC_TERMINATED;
    ores_execution_unlock(a);
    return ok;
}
static inline uint64_t ores_execution_restart(OresNativeExecutionAuthority* a) {
    if (!a) return 0;
    ores_execution_lock(a);
    uint64_t next = 0;
    if (a->phase == ORES_EXEC_TERMINATED && !a->owner
            && a->generation != UINT64_MAX && a->sequence != UINT64_MAX) {
        next = ++a->generation;
        a->phase = ORES_EXEC_READY;
    }
    ores_execution_unlock(a);
    return next;
}
static inline OresExecutionPhase ores_execution_phase(
        OresNativeExecutionAuthority* a) {
    if (!a) return ORES_EXEC_TERMINATED;
    ores_execution_lock(a);
    OresExecutionPhase phase = a->phase;
    ores_execution_unlock(a);
    return phase;
}
