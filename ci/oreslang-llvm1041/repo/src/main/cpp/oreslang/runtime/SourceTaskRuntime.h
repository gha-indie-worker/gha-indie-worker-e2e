#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t (*OresSourceTaskI64Producer)(void);

typedef enum {
    ORES_SOURCE_TASK_OK = 0,
    ORES_SOURCE_TASK_INVALID = 1,
    ORES_SOURCE_TASK_WOULD_DEADLOCK = 2,
    ORES_SOURCE_TASK_ALREADY_CONSUMED = 3,
    ORES_SOURCE_TASK_RESOURCE_EXHAUSTED = 4
} OresSourceTaskStatus;

/*
 * First source Task<int> runtime slice.
 *
 * A producer is submitted to one shared native scheduler pool. start() never
 * runs guest producer code inline. await() is external-host glue only and
 * rejects every scheduler carrier; no guest carrier may park on a Task.
 *
 * V1 deliberately permits one live capture-free Task per source process.
 * This is a narrow compiler/runtime bridge, not the final generic Task ABI.
 */
int oreslang_source_task_start_i64_checked(
    OresSourceTaskI64Producer producer, void** out_task);
void* oreslang_source_task_start_i64(OresSourceTaskI64Producer producer);

int oreslang_source_task_await_i64_checked(void* task, int64_t* out_result);
int64_t oreslang_source_task_await_i64(void* task);

/*
 * Called by the compiler-owned native main wrapper after the guest main has
 * consumed its Task. Physically drains/stops the scheduler before releasing
 * the Task object. Returns 1 on clean retirement, 0 on contract violation.
 */
int oreslang_source_task_runtime_shutdown(void);

#ifdef __cplusplus
}
#endif
