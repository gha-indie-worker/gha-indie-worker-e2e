/* Public independent C11 reference: NO private Oreslang source or Git blobs. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
enum { ACTORS = 65536, WORDS = 1024, GROUPS = 16 };
typedef struct {
    uint64_t words[WORDS], summary[GROUPS];
    unsigned char present[ACTORS];
} Index;
static unsigned low_bit(uint64_t x) {
    assert(x);
    unsigned n = 0;
    while (!(x & UINT64_C(1))) { ++n; x >>= 1; }
    return n;
}
static void assign(Index* b, size_t slot, int set) {
    assert(slot < ACTORS);
    size_t w = slot / 64;
    uint64_t bit = UINT64_C(1) << (slot % 64);
    if (set) b->words[w] |= bit;
    else b->words[w] &= ~bit;
    uint64_t group_bit = UINT64_C(1) << (w % 64);
    if (b->words[w]) b->summary[w / 64] |= group_bit;
    else b->summary[w / 64] &= ~group_bit;
    b->present[slot] = (unsigned char)!!set;
}
static size_t first_span(const Index* b, size_t low, size_t high) {
    assert(low <= high && high <= ACTORS);
    if (low == high) return ACTORS;
    size_t start_w = low / 64, end_w = (high - 1) / 64;
    for (size_t g = start_w / 64; g <= end_w / 64; ++g) {
        uint64_t available = b->summary[g];
        if (g == start_w / 64)
            available &= UINT64_MAX << (start_w % 64);
        if (g == end_w / 64 && end_w % 64 != 63)
            available &= (UINT64_C(1) << (end_w % 64 + 1)) - 1;
        while (available) {
            size_t w = g * 64 + low_bit(available);
            assert(w < WORDS);
            uint64_t bits = b->words[w];
            if (w == start_w) bits &= UINT64_MAX << (low % 64);
            if (w == end_w && high % 64)
                bits &= (UINT64_C(1) << (high % 64)) - 1;
            if (bits) return w * 64 + low_bit(bits);
            available &= available - 1;
        }
    }
    return ACTORS;
}
static size_t next(const Index* b, size_t cursor) {
    assert(cursor < ACTORS);
    size_t result = first_span(b, cursor, ACTORS);
    return result != ACTORS ? result : first_span(b, 0, cursor);
}
static size_t expected(const Index* b, size_t cursor) {
    for (size_t k = 0; k < ACTORS; ++k) {
        size_t i = (cursor + k) % ACTORS;
        if (b->present[i]) return i;
    }
    return ACTORS;
}
static void invariant(const Index* b) {
    for (size_t w = 0; w < WORDS; ++w) {
        uint64_t word = 0;
        for (unsigned bit = 0; bit < 64; ++bit)
            if (b->present[64 * w + bit]) word |= UINT64_C(1) << bit;
        assert(b->words[w] == word);
        assert(!!(b->summary[w / 64] & (UINT64_C(1) << (w % 64))) == !!word);
    }
}
static uint32_t rnd(uint32_t* seed) {
    uint32_t x = *seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *seed = x;
}
int main(void) {
    static Index data, control;
    const size_t boundary[] = {0,1,62,63,64,65,4094,4095,4096,4097,65534,65535};
    const size_t count = sizeof boundary / sizeof boundary[0];
    for (size_t i = 0; i < count; ++i) assign(&data, boundary[i], 1);
    invariant(&data);
    for (size_t i = 0; i < count; ++i) {
        assert(next(&data, boundary[i]) == expected(&data, boundary[i]));
        size_t cursor = (boundary[i] + 1) % ACTORS;
        assert(next(&data, cursor) == expected(&data, cursor));
    }
    for (size_t i = 0; i < count; ++i) {
        assign(&data, boundary[i], 0);
        invariant(&data);
    }
    assert(next(&data, 0) == ACTORS);
    assert(next(&data, 65535) == ACTORS);
    uint32_t seed = UINT32_C(0x9e3779b9);
    for (size_t k = 0; k < 18000; ++k) {
        size_t slot = rnd(&seed) % ACTORS;
        assign(&data, slot, (int)(rnd(&seed) & 1));
        if (k % 11 == 0) {
            size_t cursor = rnd(&seed) % ACTORS;
            assert(next(&data, cursor) == expected(&data, cursor));
        }
        if (k % 113 == 0) invariant(&data);
    }
    invariant(&data);
    memset(&data, 0, sizeof data);
    for (size_t i = 0; i < count; ++i) assign(&data, boundary[i], 1);
    assign(&control, 65535, 1);
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
    puts("SOURCEFREE_NATIVE_INDEX_PASS actors=65536 mixed=18000 "
         "word_boundary=64 summary_boundary=4096 high_slot=65535 "
         "priority=pass round_robin=pass native_oreslang=false");
    return 0;
}
