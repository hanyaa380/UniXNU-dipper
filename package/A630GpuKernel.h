// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The Stage 30 A630 backend. These calls are made with the GPU service lock
 * held: the address-space allocator, page-table writer and command ring are
 * shared by every client of the one hardware context.
 */
bool native_a630_gpu_kmd_ready(void);
bool native_a630_gpu_kmd_parameter(uint32_t which, uint64_t *value);
uint64_t native_a630_gpu_kmd_address(uint64_t size);
void native_a630_gpu_kmd_address_free(uint64_t address, uint64_t size);
bool native_a630_gpu_kmd_map(uint64_t address, uint64_t phys, bool read_only);
void native_a630_gpu_kmd_mapped(void);
void native_a630_gpu_kmd_unmap(uint64_t address, uint64_t size);
void native_a630_gpu_kmd_to_memory(void *start, uint64_t size);
uint32_t native_a630_gpu_kmd_submit(const uint64_t *addresses,
    const uint32_t *words, uint32_t count);
bool native_a630_gpu_kmd_done(uint32_t fence);
uint32_t native_a630_gpu_kmd_last(void);
void native_a630_gpu_kmd_report(const char *why);
void native_a630_gpu_kmd_stop(void);

#ifdef __cplusplus
}
#endif
