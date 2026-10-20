#include "oreslang_rt.h"
/* Core wasm32 ABI *prototype*. Single-threaded; not a complete actor runtime. */
__attribute__((import_module("ores_host"),import_name("clock_ms")))
extern double ores_host_clock_ms(void);
static uint32_t mailbox[ORES_MAILBOX_CAPACITY];
static uint32_t head = 0, tail = 0, count = 0, last = 0;
static uint32_t granted = 0;
static uint32_t initialized = 0;
uint32_t ores_rt_abi_version(void) { return ORES_WASM_ABI_VERSION; }
uint32_t ores_rt_initialize(uint32_t capabilities) {
  /* Unknown bits fail closed. No capability is granted by a header. */
  /* Reset first, including when the request is invalid: fail closed. */
  granted = 0;
  initialized = 0;
  head = tail = count = last = 0;
  if (capabilities & ~ORES_CAP_CLOCK) return 0;
  granted = capabilities;
  initialized = 1;
  return 1;
}
uint32_t ores_rt_add(uint32_t a, uint32_t b) { return a + b; }
uint32_t ores_rt_enqueue(uint32_t item) {
  if (!initialized || count >= ORES_MAILBOX_CAPACITY) return 0;
  mailbox[tail] = item;
  tail = (tail + 1) % ORES_MAILBOX_CAPACITY;
  count++;
  return 1;
}
uint32_t ores_rt_dequeue(void) {
  if (!initialized || !count) return 0;
  last = mailbox[head];
  head = (head + 1) % ORES_MAILBOX_CAPACITY;
  count--;
  return 1;
}
uint32_t ores_rt_last_message(void) { return last; }
double ores_rt_clock_ms(void) {
  if (!initialized || !(granted & ORES_CAP_CLOCK)) return -1.0;
  return ores_host_clock_ms();
}
