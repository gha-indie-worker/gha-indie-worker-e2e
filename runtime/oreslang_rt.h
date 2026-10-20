#ifndef ORESLANG_WASM_RT_H
#define ORESLANG_WASM_RT_H
#include <stdint.h>
#define ORES_WASM_ABI_VERSION 1u
#define ORES_CAP_CLOCK 1u
#define ORES_MAILBOX_CAPACITY 8u
uint32_t ores_rt_abi_version(void);
uint32_t ores_rt_initialize(uint32_t capabilities);
uint32_t ores_rt_add(uint32_t a,uint32_t b);
uint32_t ores_rt_enqueue(uint32_t item);
uint32_t ores_rt_dequeue(void);
uint32_t ores_rt_last_message(void);
double ores_rt_clock_ms(void);
#endif
