#include "oreslang/runtime/SourceTaskRuntime.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>

static _Atomic(uintptr_t) published_task = 0;
static _Atomic(int) carrier_await_status = -1;

typedef struct {
    void* task;
    _Atomic(int)* go;
    int status;
} DuplicateAwaitContext;

static int64_t simple_producer(void) {
    return 7;
}

static void* duplicate_await(void* opaque) {
    DuplicateAwaitContext* context = (DuplicateAwaitContext*)opaque;
    while (!atomic_load_explicit(context->go, memory_order_acquire))
        sched_yield();
    int64_t ignored = 0;
    context->status = oreslang_source_task_await_i64_checked(
        context->task, &ignored);
    return NULL;
}

static int64_t producer(void) {
    uintptr_t raw = 0;
    while ((raw = atomic_load_explicit(
                &published_task, memory_order_acquire)) == 0)
        sched_yield();

    int64_t impossible = 0;
    const int status = oreslang_source_task_await_i64_checked(
        (void*)raw, &impossible);
    atomic_store_explicit(
        &carrier_await_status, status, memory_order_release);
    return 41;
}

int main(void) {
    void* task = 0;
    assert(oreslang_source_task_start_i64_checked(producer, &task)
        == ORES_SOURCE_TASK_OK);
    assert(task != 0);
    atomic_store_explicit(
        &published_task, (uintptr_t)task, memory_order_release);

    int64_t result = 0;
    assert(oreslang_source_task_await_i64_checked(task, &result)
        == ORES_SOURCE_TASK_OK);
    assert(result == 41);
    assert(atomic_load_explicit(
        &carrier_await_status, memory_order_acquire)
        == ORES_SOURCE_TASK_WOULD_DEADLOCK);

    int64_t duplicate = 0;
    assert(oreslang_source_task_await_i64_checked(task, &duplicate)
        == ORES_SOURCE_TASK_ALREADY_CONSUMED);

    assert(oreslang_source_task_runtime_shutdown() == 1);
    assert(oreslang_source_task_runtime_shutdown() == 1);

    /*
     * Race duplicate post-consumption await admission against physical
     * shutdown/reclamation. The duplicate may enter before the closing fence
     * (ALREADY_CONSUMED) or after it (INVALID), but it must never touch freed
     * task synchronization state.
     */
    for (int i = 0; i < 64; ++i) {
        void* raced_task = NULL;
        assert(oreslang_source_task_start_i64_checked(
            simple_producer, &raced_task) == ORES_SOURCE_TASK_OK);
        int64_t raced_result = 0;
        assert(oreslang_source_task_await_i64_checked(
            raced_task, &raced_result) == ORES_SOURCE_TASK_OK);
        assert(raced_result == 7);

        _Atomic(int) go = 0;
        DuplicateAwaitContext context = {
            .task = raced_task,
            .go = &go,
            .status = -1
        };
        pthread_t duplicate;
        assert(pthread_create(
            &duplicate, NULL, duplicate_await, &context) == 0);
        atomic_store_explicit(&go, 1, memory_order_release);

        assert(oreslang_source_task_runtime_shutdown() == 1);
        assert(pthread_join(duplicate, NULL) == 0);
        assert(context.status == ORES_SOURCE_TASK_ALREADY_CONSUMED ||
            context.status == ORES_SOURCE_TASK_INVALID);
    }

    assert(oreslang_source_task_runtime_shutdown() == 1);
    return 0;
}
