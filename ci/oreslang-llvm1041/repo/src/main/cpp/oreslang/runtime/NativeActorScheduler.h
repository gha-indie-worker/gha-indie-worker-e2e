#pragma once
#include <stddef.h>
#include <stdint.h>
#include "oreslang/runtime/NativeExecutionContext.h"
typedef struct OresNativePhysicalTurnGate OresNativePhysicalTurnGate;
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OresNativeActorScheduler OresNativeActorScheduler;
typedef struct {
    uint64_t scheduler_domain;
    uint64_t actor_slot;
    uint64_t generation;
} OresNativeActorRef;

/* Native scheduler-owned continuation wake escrow. Each producer actor has a
 * finite, preallocated pool, separate from DATA and supervisor CONTROL. This
 * is a trusted host ABI: source guest programs cannot mint wake tickets.
 * Source-level send_async lowering and physical message loans are NOT supplied
 * by these scheduling primitives alone.
 */
enum { ORES_NATIVE_WAKE_CREDITS_PER_ACTOR = 8 };
typedef enum {
    ORES_NATIVE_WAKE_READY = 1,
    ORES_NATIVE_WAKE_CANCELLED = 2,
    ORES_NATIVE_WAKE_FAILED = 3
} OresNativeWakeOutcome;
typedef struct {
    OresNativeActorRef producer;
    OresNativeActorRef target;
    uint64_t slot; /* per-producer preallocated wake slot, zero-based */
    uint64_t slot_generation;
    uint64_t continuation_slot;
    uint64_t continuation_generation;
    uint64_t code_generation;
    uint64_t descriptor_generation;
} OresNativeWakeTicket;
typedef enum {
    ORES_NATIVE_WAKE_INVALID = 0,
    ORES_NATIVE_WAKE_PUBLISHED = 1,
    ORES_NATIVE_WAKE_PRODUCER_RETIRED = 2
} OresNativeWakePublishStatus;

typedef enum {
    ORES_NATIVE_TERMINATION_STOPPED = 1,
    ORES_NATIVE_TERMINATION_RUNTIME_RETIRED = 2
} OresNativeActorTerminationKind;

typedef struct {
    OresNativeActorRef actor;
    OresNativeActorTerminationKind kind;
    uint64_t accepted_user_messages;
    uint64_t processed_user_messages;
    uint64_t abandoned_user_messages;
    uint64_t abandoned_control_messages;
} OresNativeActorTerminalState;
enum { ORES_NATIVE_CONTROL_PANIC = 1, ORES_NATIVE_CONTROL_CANCELLED = 2,
       ORES_NATIVE_CONTROL_STOPPED = 3 };
typedef struct {
    uint64_t kind;
    OresNativeActorRef child;
    uint64_t failure_code;
    uint64_t correlation;
} OresNativeActorTerminalFailure;
/* Typed terminal failure is copied into the reserved parent CONTROL slot.
 * A supervisor handler runs only on the parent's single logical actor lane.
 */
typedef void (*OresNativeActorPanicFn)(
    void* ctx, size_t parent_actor, OresNativeActorTerminalFailure failure);
/* Explicit *native host* actor lifecycle, not compiler-lowered source spawn.
 * Spawning activates exactly one preallocated actor slot; no ambient cross-
 * actor mutable references, host heap allocation, or slot reincarnation.
 */
/* Trusted host-only pre-admission wiring. Must be called before the first
 * actor message/carrier starts. Gate outlives this scheduler and is released
 * after its last callback finishes. A NULL gate leaves legacy dispatch intact. */
int ores_native_actor_scheduler_configure_turn_gate(
    OresNativeActorScheduler* scheduler, OresNativePhysicalTurnGate* gate);
/* Wake idle carriers that lost a cross-domain permit race, including while
 * the scheduler is retiring and still has admitted work to drain. Trusted
 * VM-only call; not a guest CONTROL message. */
void ores_native_actor_scheduler_signal_turn_gate(OresNativeActorScheduler* scheduler);
OresNativeActorRef ores_native_actor_scheduler_spawn(
    OresNativeActorScheduler* scheduler, size_t actor_slot);
/* Send succeeds only after atomic admission to a live, generation-matched
 * actor mailbox. It does NOT promise execution or successful processing.
 * No guest callback, blocking wait, or memory allocation occurs here.
 * Cross-process admission cannot be inferred from an IPC write: callers must
 * obtain a remote mailbox receipt before reporting ACCEPTED.
 */
typedef enum {
    ORES_NATIVE_SEND_ACCEPTED = 0,
    ORES_NATIVE_SEND_INVALID_REF = 1,
    ORES_NATIVE_SEND_TERMINATED = 2,
    ORES_NATIVE_SEND_MAILBOX_FULL = 3,
    ORES_NATIVE_SEND_RESOURCE_EXHAUSTED = 4,
    ORES_NATIVE_SEND_TRANSPORT_UNAVAILABLE = 5
} OresNativeSendStatus;
OresNativeSendStatus ores_native_actor_scheduler_try_send(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor, uint64_t payload);
/* Compatibility API: 1 iff accepted, 0 otherwise. Prefer typed try_send. */
int ores_native_actor_scheduler_send(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor, uint64_t payload);
int ores_native_actor_scheduler_stop(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Seal user DATA immediately while preserving an already-active turn and
 * lifecycle CONTROL. Queued DATA is abandoned with terminal accounting. */
int ores_native_actor_scheduler_stop_sealed(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Nonblocking host readiness/termination observations. Source-level
 * get_ready()/get_done() must return futures when compiler lowering exists.
 */
int ores_native_actor_scheduler_poll_ready(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
int ores_native_actor_scheduler_poll_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
int ores_native_actor_scheduler_terminal_state(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    OresNativeActorTerminalState* output);
/* Trusted external supervisor barrier: block only the host caller until
 * the selected stopped actor drains. This is NOT a guest await/Future;
 * never invoke it on a scheduler worker/actor carrier. */
int ores_native_actor_scheduler_wait_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Handler runs on the scheduler worker pool, never on the submitting carrier.
 * At most one callback executes per actor, across all worker threads.
 * The actor index is a scheduler-local slot, not a cross-tenant capability.
 */
typedef void (*OresNativeActorTaskFn)(void* ctx, size_t actor_index, uint64_t payload);
typedef void (*OresNativeActorWakeFn)(
    void* ctx, size_t actor_index, OresNativeWakeTicket ticket,
    OresNativeWakeOutcome outcome);
/* One reserved CONTROL message per actor, independent of normal-mail
 * capacity; only an admitted actor lane may invoke the CONTROL handler.
 * CONTROL callbacks never run on a failed child's carrier by direct call.
 */
/* Variant with a dedicated continuation wake callback. The callback is
 * dispatched under the producer's authenticated single-flight actor claim,
 * never inline from completion, admission or cancellation threads. */
OresNativeActorScheduler* ores_native_actor_scheduler_open_with_wakes(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn on_message, OresNativeActorTaskFn on_control,
    OresNativeActorPanicFn on_panic, OresNativeActorWakeFn on_wake, void* ctx);
OresNativeActorScheduler* ores_native_actor_scheduler_open_typed_control(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn on_message, OresNativeActorTaskFn on_future,
    OresNativeActorPanicFn on_panic, void* ctx);
OresNativeActorScheduler* ores_native_actor_scheduler_open_control(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn on_message, OresNativeActorTaskFn on_control, void* ctx);
/* A failed/retired child publishes a typed control envelope (encoded in
 * payload) to the parent. Returns 0 if the reserved slot is full or if the
 * target is stale/stopped; caller must retry or explicitly escalate.
 */
/* Reserve the exact future delivery credit BEFORE registering a bounded
 * pending mailbox producer. All worker pthreads are made serviceable during
 * reservation, so transfer->publish cannot fail on late worker creation.
 * target/continuation/descriptor identities are immutable ABA fences.
 * A reserved or transferred ticket blocks producer physical DONE; it does not
 * occupy the supervisor CONTROL slot or the USER mailbox.
 *
 * The mailbox-accounting lock must serialize TRANSFER with mailbox credit
 * handoff and cancellation. For same-scheduler targets, TRANSFER revalidates
 * the exact live target incarnation so STOP cannot become a false admission.
 * Wake mutation APIs fail closed when invoked on a scheduler created without
 * wake support. No guest/user callback runs in these functions.
 */
int ores_native_actor_scheduler_reserve_wake(
    OresNativeActorScheduler* scheduler, OresNativeActorRef producer,
    OresNativeActorRef target, uint64_t continuation_slot,
    uint64_t continuation_generation, uint64_t code_generation,
    uint64_t descriptor_generation, OresNativeWakeTicket* out);
int ores_native_actor_scheduler_transfer_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket);
/* Cancellation succeeds only while still RESERVED (not transferred). */
int ores_native_actor_scheduler_cancel_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket);
/* Explicit rollback for an aborted transfer before envelope publication.
 * This is NOT late producer cancellation and must settle the send as failed. */
int ores_native_actor_scheduler_abort_transferred_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket);
/* After transfer, publication cannot fail due to capacity or allocation.
 * If STOP won first it consumes the reserved ticket as a terminal failure,
 * without scheduling user code or fabricating successful delivery. */
OresNativeWakePublishStatus ores_native_actor_scheduler_publish_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket,
    OresNativeWakeOutcome outcome);
int ores_native_actor_scheduler_is_current_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket);
size_t ores_native_actor_scheduler_outstanding_wake_loans(
    OresNativeActorScheduler* scheduler, OresNativeActorRef producer);
uint64_t ores_native_actor_scheduler_wakes_completed(
    OresNativeActorScheduler* scheduler);
int ores_native_actor_scheduler_publish_control(
    OresNativeActorScheduler* scheduler, OresNativeActorRef parent, uint64_t payload);
/* Child must be a valid actor in the same scheduler, distinct from parent.
 * Returns 0 on reserved CONTROL saturation, stale refs or parent retirement.
 * Never invokes on_panic directly on the publishing child's carrier.
 */
int ores_native_actor_scheduler_publish_failure(
    OresNativeActorScheduler* scheduler, OresNativeActorRef parent,
    OresNativeActorTerminalFailure failure);
uint64_t ores_native_actor_scheduler_control_completed(
    OresNativeActorScheduler* scheduler);
OresNativeActorScheduler* ores_native_actor_scheduler_open(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn callback, void* ctx);
/* Legacy index admission requires an already-spawned host actor.\n * New hosts should use the generation-checked actor ref send API.\n * O(1), nonblocking admission. Returns 1 on accepted, 0 on capacity or retirement.
 * Does not allocate and never invokes callbacks inline. Capacity includes the
 * currently executing task. Caller retains ownership on rejection.
 */
int ores_native_actor_scheduler_submit(
    OresNativeActorScheduler* scheduler, size_t actor_index, uint64_t payload);
/* No new tasks after retirement, but previously accepted tasks drain. */
void ores_native_actor_scheduler_retire(OresNativeActorScheduler* scheduler);
/* Wait for every accepted callback to complete; may not be called from its worker. */
int ores_native_actor_scheduler_drain(OresNativeActorScheduler* scheduler);
/* Whether this OS thread is currently executing a callback from scheduler.
 * Only intended for immediate deadlock prevention in VM teardown. */
int ores_native_actor_scheduler_is_current_carrier(OresNativeActorScheduler* scheduler);
/* Exact live actor identity for trusted callback/FFI bridges only. */
int ores_native_actor_scheduler_is_current_actor(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Logical execution identity for the current guest turn. The execution id is
 * not a heap id and is not a carrier/thread id. For actor turns the allocation
 * domain names the actor-owned domain independently. Returns zero outside an
 * authenticated current turn. */
int ores_native_actor_scheduler_current_execution_context(
    OresNativeActorScheduler* scheduler, OresNativeExecutionContext* out);
/* Host CONTROL-plane wake only: signals an existing scheduler condition.
 * Does not admit actor mail, start workers, invoke guest code, or count as
 * a supervisor delivery receipt. Returns zero once retired.
 * Caller must fence the scheduler's lifetime. */
int ores_native_actor_scheduler_signal_control(OresNativeActorScheduler* scheduler);
/* Requires retirement, drained tasks AND caller-owned submitter fence.
 * Returns 0 when not ready; after success the pointer is invalid.
 */
int ores_native_actor_scheduler_try_destroy(OresNativeActorScheduler* scheduler);
/* Configure all subsequently created carriers for explicit POSIX SCHED_FIFO.
 * Bootstrap-only: must precede carrier creation and message/control admission.
 * Configuration success does not imply the process has permission to create
 * such threads; prestart fails closed if pthread_create rejects the policy. */
int ores_native_actor_scheduler_configure_fifo(
    OresNativeActorScheduler* scheduler, int priority);
/* True only after every configured SCHED_FIFO carrier has been created. */
int ores_native_actor_scheduler_fifo_carriers_ready(
    OresNativeActorScheduler* scheduler);
/* Move every pthread_create out of the future admission path. This is a
 * necessary but deliberately insufficient hard-RT primitive: it does not
 * qualify scheduler policy, priority inheritance, CPU/IRQ isolation, memory
 * residency, or WCET. Must run before any message/control admission. */
int ores_native_actor_scheduler_prestart_carriers(
    OresNativeActorScheduler* scheduler);
/* Compatibility spelling for RT VM bootstrap; delegates to the stronger
 * current prestart-carriers invariant rather than maintaining another state. */
int ores_native_actor_scheduler_prestart_realtime(
    OresNativeActorScheduler* scheduler);
int ores_native_actor_scheduler_all_carriers_started(
    OresNativeActorScheduler* scheduler);
/* True only when the scheduler mutex was successfully initialized with the
 * POSIX priority-inheritance protocol. Hard-RT admission should require this
 * (or a separately qualified synchronization protocol). */
int ores_native_actor_scheduler_priority_inheritance_enabled(
    OresNativeActorScheduler* scheduler);
/* Carriers created on demand unless explicitly prestarted; includes workers
 * until retirement. */
size_t ores_native_actor_scheduler_started_carriers(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_accepted(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_rejected(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_completed(OresNativeActorScheduler* scheduler);
#ifdef ORESLANG_NATIVE_TEST_PTHREAD_CREATE_FAILURE
/* Test-only deterministic worker-create failure; no production symbol. */
void ores_native_actor_scheduler_test_fail_next_create(void);
#endif
#ifdef ORESLANG_NATIVE_TEST_EXECUTION_EXHAUSTION
/* Test-only execution-id exhaustion control; no production symbol. */
int ores_native_actor_scheduler_test_set_next_execution(
    OresNativeActorScheduler* scheduler, uint64_t next_execution);
#endif
#ifdef __cplusplus
}
#endif
