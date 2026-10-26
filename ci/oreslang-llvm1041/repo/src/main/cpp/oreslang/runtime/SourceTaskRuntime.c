#include "oreslang/runtime/SourceTaskRuntime.h"

#include "oreslang/runtime/NativeActorScheduler.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SOURCE_TASK_MAGIC UINT64_C(0x4f5245535441534b)

typedef struct {
    uint64_t magic;
    pthread_mutex_t mutex;
    pthread_cond_t completed;
    OresSourceTaskI64Producer producer;
    int64_t result;
    int done;
    int consuming;
    int consumed;
    int producer_on_carrier;
} SourceTaskI64;

typedef struct {
    pthread_mutex_t mutex;
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    SourceTaskI64* task;
} SourceTaskRuntime;

static SourceTaskRuntime runtime = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .scheduler = NULL,
    .actor = {0, 0, 0},
    .task = NULL
};

static void task_worker(void* context, size_t actor_index, uint64_t payload) {
    (void)context;
    (void)actor_index;
    if (payload != UINT64_C(1)) abort();

    if (pthread_mutex_lock(&runtime.mutex) != 0) abort();
    SourceTaskI64* task = runtime.task;
    OresNativeActorScheduler* scheduler = runtime.scheduler;
    if (!task || task->magic != SOURCE_TASK_MAGIC || !scheduler) abort();
    if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();

    if (!ores_native_actor_scheduler_is_current_carrier(scheduler)) abort();
    const int64_t result = task->producer();

    if (pthread_mutex_lock(&task->mutex) != 0) abort();
    if (task->done) abort();
    task->result = result;
    task->producer_on_carrier = 1;
    task->done = 1;
    if (pthread_cond_broadcast(&task->completed) != 0) abort();
    if (pthread_mutex_unlock(&task->mutex) != 0) abort();
}

int oreslang_source_task_start_i64_checked(
        OresSourceTaskI64Producer producer, void** out_task) {
    if (!producer || !out_task) return ORES_SOURCE_TASK_INVALID;
    *out_task = NULL;

    SourceTaskI64* task = (SourceTaskI64*)calloc(1, sizeof(*task));
    if (!task) return ORES_SOURCE_TASK_RESOURCE_EXHAUSTED;
    if (pthread_mutex_init(&task->mutex, NULL) != 0) {
        free(task);
        return ORES_SOURCE_TASK_RESOURCE_EXHAUSTED;
    }
    if (pthread_cond_init(&task->completed, NULL) != 0) {
        (void)pthread_mutex_destroy(&task->mutex);
        free(task);
        return ORES_SOURCE_TASK_RESOURCE_EXHAUSTED;
    }
    task->magic = SOURCE_TASK_MAGIC;
    task->producer = producer;

    if (pthread_mutex_lock(&runtime.mutex) != 0) abort();
    if (runtime.task != NULL) {
        if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();
        (void)pthread_cond_destroy(&task->completed);
        (void)pthread_mutex_destroy(&task->mutex);
        free(task);
        return ORES_SOURCE_TASK_RESOURCE_EXHAUSTED;
    }

    if (!runtime.scheduler) {
        runtime.scheduler = ores_native_actor_scheduler_open(
            1, 1, 1, task_worker, NULL);
        if (!runtime.scheduler) {
            if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();
            (void)pthread_cond_destroy(&task->completed);
            (void)pthread_mutex_destroy(&task->mutex);
            free(task);
            return ORES_SOURCE_TASK_RESOURCE_EXHAUSTED;
        }
        runtime.actor = ores_native_actor_scheduler_spawn(runtime.scheduler, 0);
        if (runtime.actor.scheduler_domain == 0) abort();
    }

    runtime.task = task;
    const OresNativeSendStatus admitted =
        ores_native_actor_scheduler_try_send(
            runtime.scheduler, runtime.actor, UINT64_C(1));
    if (admitted != ORES_NATIVE_SEND_ACCEPTED) {
        OresNativeActorScheduler* failed_scheduler = runtime.scheduler;
        OresNativeActorRef failed_actor = runtime.actor;
        runtime.task = NULL;
        runtime.scheduler = NULL;
        runtime.actor = (OresNativeActorRef){0, 0, 0};
        if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();

        /*
         * Admission failure must not strand a scheduler/carrier resource.
         * No DATA was accepted, so sealing and draining the empty actor is a
         * trusted host cleanup path, never a guest callback.
         */
        (void)ores_native_actor_scheduler_stop(failed_scheduler, failed_actor);
        (void)ores_native_actor_scheduler_wait_done(
            failed_scheduler, failed_actor);
        ores_native_actor_scheduler_retire(failed_scheduler);
        (void)ores_native_actor_scheduler_drain(failed_scheduler);
        (void)ores_native_actor_scheduler_try_destroy(failed_scheduler);

        (void)pthread_cond_destroy(&task->completed);
        (void)pthread_mutex_destroy(&task->mutex);
        free(task);
        return ORES_SOURCE_TASK_RESOURCE_EXHAUSTED;
    }
    if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();

    *out_task = task;
    return ORES_SOURCE_TASK_OK;
}

void* oreslang_source_task_start_i64(OresSourceTaskI64Producer producer) {
    void* task = NULL;
    if (oreslang_source_task_start_i64_checked(producer, &task)
            != ORES_SOURCE_TASK_OK)
        abort();
    return task;
}

int oreslang_source_task_await_i64_checked(void* opaque, int64_t* out_result) {
    if (!opaque || !out_result) return ORES_SOURCE_TASK_INVALID;

    if (pthread_mutex_lock(&runtime.mutex) != 0) abort();
    SourceTaskI64* task = runtime.task;
    OresNativeActorScheduler* scheduler = runtime.scheduler;
    const int valid = task == (SourceTaskI64*)opaque &&
        task != NULL && scheduler != NULL;
    if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();
    if (!valid) return ORES_SOURCE_TASK_INVALID;

    if (ores_native_actor_scheduler_is_current_carrier(scheduler))
        return ORES_SOURCE_TASK_WOULD_DEADLOCK;

    if (pthread_mutex_lock(&task->mutex) != 0) abort();
    if (task->magic != SOURCE_TASK_MAGIC) abort();
    if (task->consuming || task->consumed) {
        if (pthread_mutex_unlock(&task->mutex) != 0) abort();
        return ORES_SOURCE_TASK_ALREADY_CONSUMED;
    }
    task->consuming = 1;
    while (!task->done) {
        if (pthread_cond_wait(&task->completed, &task->mutex) != 0) abort();
    }
    *out_result = task->result;
    task->consuming = 0;
    task->consumed = 1;
    if (pthread_mutex_unlock(&task->mutex) != 0) abort();
    return ORES_SOURCE_TASK_OK;
}

int64_t oreslang_source_task_await_i64(void* task) {
    int64_t result = 0;
    if (oreslang_source_task_await_i64_checked(task, &result)
            != ORES_SOURCE_TASK_OK)
        abort();
    return result;
}

int oreslang_source_task_runtime_shutdown(void) {
    if (pthread_mutex_lock(&runtime.mutex) != 0) abort();
    OresNativeActorScheduler* scheduler = runtime.scheduler;
    OresNativeActorRef actor = runtime.actor;
    SourceTaskI64* task = runtime.task;
    if (!scheduler && !task) {
        if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();
        return 1;
    }
    if (!scheduler || !task ||
            ores_native_actor_scheduler_is_current_carrier(scheduler)) {
        if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();
        return 0;
    }
    if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();

    if (pthread_mutex_lock(&task->mutex) != 0) abort();
    const int terminal = task->done && task->consumed &&
        task->producer_on_carrier;
    if (pthread_mutex_unlock(&task->mutex) != 0) abort();
    if (!terminal) return 0;

    if (!ores_native_actor_scheduler_stop(scheduler, actor)) return 0;
    if (!ores_native_actor_scheduler_wait_done(scheduler, actor)) return 0;
    ores_native_actor_scheduler_retire(scheduler);
    if (!ores_native_actor_scheduler_drain(scheduler)) return 0;

#ifdef ORESLANG_SOURCE_TASK_TESTING
    fprintf(stderr,
        "SOURCE_TASK_RUNTIME_PASS producer_on_carrier=%d carriers=%zu accepted=%llu completed=%llu\n",
        task->producer_on_carrier,
        ores_native_actor_scheduler_started_carriers(scheduler),
        (unsigned long long)ores_native_actor_scheduler_accepted(scheduler),
        (unsigned long long)ores_native_actor_scheduler_completed(scheduler));
#endif

    if (!ores_native_actor_scheduler_try_destroy(scheduler)) return 0;

    if (pthread_mutex_lock(&runtime.mutex) != 0) abort();
    if (runtime.scheduler != scheduler || runtime.task != task) abort();
    runtime.scheduler = NULL;
    runtime.actor = (OresNativeActorRef){0, 0, 0};
    runtime.task = NULL;
    if (pthread_mutex_unlock(&runtime.mutex) != 0) abort();

    task->magic = 0;
    if (pthread_cond_destroy(&task->completed) != 0) abort();
    if (pthread_mutex_destroy(&task->mutex) != 0) abort();
    free(task);
    return 1;
}
