/* PUBLIC SOURCE-FREE REFERENCE: independently authored count-based index.
 * Does not reproduce the private Oreslang scheduler's bitmap implementation.
 * No private source, Git trees, credentials or cross-org checkout. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
enum { ACTORS = 65536, BLOCK = 64, BLOCKS = 1024,
       REGION = 4096, REGIONS = 16 };
typedef struct {
    unsigned char present[ACTORS];
    uint16_t by_block[BLOCKS], by_region[REGIONS];
} Index;
static void assign(Index* x, size_t slot, int set) {
    assert(slot < ACTORS);
    unsigned char now = (unsigned char)!!set;
    if (x->present[slot] == now) return;
    uint16_t* block = &x->by_block[slot / BLOCK];
    uint16_t* region = &x->by_region[slot / REGION];
    if (now) {
        assert(*block < BLOCK && *region < REGION);
        ++*block; ++*region;
    } else {
        assert(*block && *region);
        --*block; --*region;
    }
    x->present[slot] = now;
}
static size_t next(const Index* x, size_t cursor) {
    assert(cursor < ACTORS);
    /* Count-based skipping is intentionally different from the private
     * two-level bit index. Model the observable first-ready semantics. */
    for (int pass = 0; pass != 2; ++pass) {
        size_t low = pass ? 0 : cursor;
        size_t high = pass ? cursor : ACTORS;
        if (low >= high) continue;
        size_t first_region = low / REGION;
        size_t last_region = (high - 1) / REGION;
        for (size_t region = first_region; region <= last_region; ++region) {
            if (!x->by_region[region]) continue;
            size_t rlo = region * REGION;
            size_t rhi = rlo + REGION;
            if (rlo < low) rlo = low;
            if (rhi > high) rhi = high;
            for (size_t block = rlo / BLOCK; block <= (rhi - 1) / BLOCK; ++block) {
                if (!x->by_block[block]) continue;
                size_t blo = block * BLOCK;
                size_t bhi = blo + BLOCK;
                if (blo < rlo) blo = rlo;
                if (bhi > rhi) bhi = rhi;
                for (size_t slot = blo; slot < bhi; ++slot)
                    if (x->present[slot]) return slot;
            }
        }
    }
    return ACTORS;
}
static size_t independent_oracle(const Index* x, size_t cursor) {
    for (size_t offset = 0; offset < ACTORS; ++offset) {
        size_t slot = (cursor + offset) % ACTORS;
        if (x->present[slot]) return slot;
    }
    return ACTORS;
}
static void invariant(const Index* x) {
    for (size_t region = 0; region < REGIONS; ++region) {
        size_t region_total = 0;
        for (size_t block = region * (REGION / BLOCK);
             block < (region + 1) * (REGION / BLOCK); ++block) {
            size_t block_total = 0;
            for (size_t i = block * BLOCK; i < (block + 1) * BLOCK; ++i) {
                assert(x->present[i] <= 1);
                block_total += x->present[i];
            }
            assert(block_total == x->by_block[block]);
            region_total += block_total;
        }
        assert(region_total == x->by_region[region]);
    }
}
static uint32_t deterministic(uint32_t* seed) {
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *seed = x;
}
/* Synthetic at-most-once accounting across the same sparse boundary
 * slots. This is a separate reference, not the Oreslang runtime implementation. */
static int reference_deliver_once(uint_fast64_t* observed, size_t slot) {
    const size_t names[] = {0, 63, 64, 4095, 4096, 65535};
    size_t index = 0;
    for (; index < sizeof names / sizeof names[0]; ++index)
        if (names[index] == slot) break;
    if (index == sizeof names / sizeof names[0]) return 0;
    uint_fast64_t bit = UINT64_C(1) << index;
    if (*observed & bit) return 0;
    *observed |= bit;
    return 1;
}
static void delivery_accounting_contract(void) {
    uint_fast64_t mask = 0;
    const size_t slots[] = {0, 63, 64, 4095, 4096, 65535};
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; ++i)
        assert(reference_deliver_once(&mask, slots[i]));
    assert(mask == UINT64_C(0x3f));
    assert(!reference_deliver_once(&mask, 65535)); /* duplicate */
    assert(!reference_deliver_once(&mask, 42));    /* unexpected slot */
    assert(mask == UINT64_C(0x3f)); /* rejection does not corrupt account */
}
int main(void) {
    static Index data, control;
    const size_t boundary[] =
        {0,1,62,63,64,65,4094,4095,4096,4097,65534,65535};
    const size_t count = sizeof boundary / sizeof boundary[0];
    for (size_t i = 0; i < count; ++i) assign(&data, boundary[i], 1);
    invariant(&data);
    for (size_t i = 0; i < count; ++i) {
        assert(next(&data, boundary[i]) ==
               independent_oracle(&data, boundary[i]));
        size_t cursor = (boundary[i] + 1) % ACTORS;
        assert(next(&data, cursor) == independent_oracle(&data, cursor));
    }
    for (size_t i = 0; i < count; ++i) {
        assign(&data, boundary[i], 0);
        invariant(&data);
    }
    assert(next(&data, 0) == ACTORS);
    assert(next(&data, 65535) == ACTORS);
    uint32_t seed = UINT32_C(0x9e3779b9);
    for (size_t step = 0; step < 18000; ++step) {
        size_t slot = deterministic(&seed) % ACTORS;
        assign(&data, slot, (int)(deterministic(&seed) & 1));
        if (step % 11 == 0) {
            size_t cursor = deterministic(&seed) % ACTORS;
            assert(next(&data, cursor) == independent_oracle(&data, cursor));
        }
        if (step % 113 == 0) invariant(&data);
    }
    invariant(&data);
    memset(&data, 0, sizeof data);
    for (size_t i = 0; i < count; ++i) assign(&data, boundary[i], 1);
    assign(&control, 65535, 1);
    /* Separate priority plane always wins over lower-index DATA. */
    assert(next(&control, 0) == 65535);
    assign(&control, 65535, 0);
    assert(next(&data, 0) == 0);
    assign(&data, 0, 0);
    assert(next(&data, 0) == 1);
    memset(&data, 0, sizeof data);
    assign(&data, 0, 1);
    assign(&data, 65535, 1);
    size_t cursor = 0;
    unsigned high_served = 0;
    for (unsigned turn = 0; turn < 2000; ++turn) {
        size_t chosen = next(&data, cursor);
        assert(chosen == 0 || chosen == 65535);
        if (chosen == 65535) ++high_served;
        cursor = (chosen + 1) % ACTORS;
    }
    assert(high_served == 1000);
    delivery_accounting_contract();
    puts("SOURCEFREE_NATIVE_INDEX_PASS actors=65536 mixed=18000 "
         "block=64 region=4096 high_slot=65535 "
         "priority=pass round_robin=pass native_oreslang=false");
    return 0;
}
