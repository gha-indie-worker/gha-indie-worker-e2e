#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Logical execution identity is deliberately distinct from allocation/heap
 * identity and from physical carrier identity.
 *
 * The structure is fixed-width and pointer-free so large runtime populations
 * can be stored as AoS today and migrated to SoA/AoSoA stores without changing
 * the ABI or leaking native pointers into guest-visible state.
 */
typedef enum {
    ORES_ALLOCATION_DOMAIN_NONE = 0,
    ORES_ALLOCATION_DOMAIN_ACTOR = 1,
    ORES_ALLOCATION_DOMAIN_TASK = 2,
    ORES_ALLOCATION_DOMAIN_RUNTIME = 3
} OresNativeAllocationDomainKind;

typedef struct {
    uint64_t kind;
    uint64_t domain;
    uint64_t owner;
    uint64_t generation;
} OresNativeAllocationDomainRef;

typedef struct {
    uint64_t runtime_domain;
    uint64_t execution;
} OresNativeExecutionId;

/*
 * Store handles are compact, generation-checked capabilities. Slot zero and
 * generation zero are permanently invalid. A store retires a slot rather than
 * wrapping its generation, so stale handles cannot become valid through ABA.
 */
typedef struct {
    uint32_t slot;
    uint32_t generation;
} OresNativeStoreHandle;

typedef OresNativeStoreHandle OresNativeExecutionHandle;
typedef OresNativeStoreHandle OresNativeContinuationHandle;

static inline int ores_native_store_handle_valid(OresNativeStoreHandle handle) {
    return handle.slot != 0 && handle.generation != 0;
}

static inline uint64_t ores_native_store_handle_pack(OresNativeStoreHandle handle) {
    return ((uint64_t)handle.generation << 32) | (uint64_t)handle.slot;
}

static inline OresNativeStoreHandle ores_native_store_handle_unpack(uint64_t packed) {
    OresNativeStoreHandle handle = {
        (uint32_t)(packed & UINT64_C(0xffffffff)),
        (uint32_t)(packed >> 32)
    };
    if (!ores_native_store_handle_valid(handle)) {
        OresNativeStoreHandle empty = {0, 0};
        return empty;
    }
    return handle;
}

typedef struct {
    OresNativeExecutionId id;
    OresNativeAllocationDomainRef allocation;
    /*
     * Logical ancestry is store-backed and generation checked. These are never
     * raw native stack or actor-heap pointers. continuation is the weak OUTGOING
     * suspension created by this execution; an incoming continuation belongs to
     * parent_execution and is also carried explicitly by OresNativeSuspensionLink.
     * The ContinuationStore pins its originating execution while live.
     */
    OresNativeExecutionHandle parent_execution;
    OresNativeContinuationHandle continuation;
} OresNativeExecutionContext;

/*
 * The compiler/runtime hand-off persisted at an await boundary. The parent is
 * the suspended execution handle that becomes parent_execution on resumption.
 * The record is pointer free and safe to keep after the worker stack vanishes.
 */
typedef struct {
    OresNativeExecutionId execution_id;
    OresNativeContinuationHandle continuation;
    OresNativeExecutionHandle parent;
} OresNativeSuspensionLink;

#ifdef __cplusplus
}
static_assert(sizeof(OresNativeStoreHandle) == 8,
              "store handle ABI must remain 8 bytes");
static_assert(sizeof(OresNativeExecutionContext) == 64,
              "execution context ABI must remain fixed-width and pointer-free");
static_assert(sizeof(OresNativeSuspensionLink) == 32,
              "suspension link ABI must remain fixed-width and pointer-free");
#else
_Static_assert(sizeof(OresNativeStoreHandle) == 8,
               "store handle ABI must remain 8 bytes");
_Static_assert(sizeof(OresNativeExecutionContext) == 64,
               "execution context ABI must remain fixed-width and pointer-free");
_Static_assert(sizeof(OresNativeSuspensionLink) == 32,
               "suspension link ABI must remain fixed-width and pointer-free");
#endif
