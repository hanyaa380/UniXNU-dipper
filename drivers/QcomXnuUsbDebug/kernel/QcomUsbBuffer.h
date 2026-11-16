/* SPDX-License-Identifier: MIT */
#ifndef SOCRATES_USB_BUFFER_H
#define SOCRATES_USB_BUFFER_H
#include <stddef.h>
#include <stdint.h>

/* CPU-only backlog: never DMA mapped. Callers mask local IRQ/FIQ before
 * taking this nonspinning guard, including for drain/snapshot operations.
 * A recursive/competing producer returns immediately and counts lost bytes.
 * Preserve oldest bytes: an unavailable host must not overwrite early panic
 * or startup evidence. Finite capacity is reported, never called lossless. */
#define SOC_USB_LOG_BUFFER_BYTES (4u * 1024u * 1024u)
#define SOC_USB_LOG_WRITE_BUDGET 16384u
#define SOC_USB_LOG_DRAIN_BUDGET 4096u
struct soc_usb_buffer {
    uint8_t *data;
    size_t capacity, head, tail, used, high_water;
    unsigned int busy;
    uint64_t accepted, overflow, contended;
};
typedef size_t (*soc_usb_buffer_sink)(void *, const void *, size_t);

/* Retryable framed producer: enqueue the whole record or leave the FIFO
 * unchanged. The caller retains rejected data, so backpressure is not loss. */
static inline size_t soc_usb_buffer_try_record(struct soc_usb_buffer *b,
                                              const void *bytes, size_t count)
{
    const uint8_t *p = (const uint8_t *)bytes;
    if (!bytes || !count || count > SOC_USB_LOG_WRITE_BUDGET || count > b->capacity) return 0;
    if (__atomic_exchange_n(&b->busy, 1u, __ATOMIC_ACQUIRE)) return 0;
    if (count > b->capacity - b->used) {
        __atomic_store_n(&b->busy, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        b->data[b->head] = p[i];
        if (++b->head == b->capacity) b->head = 0;
    }
    __atomic_store_n(&b->used, b->used + count, __ATOMIC_RELAXED);
    if (b->used > b->high_water) __atomic_store_n(&b->high_water, b->used, __ATOMIC_RELAXED);
    __atomic_fetch_add(&b->accepted, count, __ATOMIC_RELAXED);
    __atomic_store_n(&b->busy, 0u, __ATOMIC_RELEASE);
    return count;
}

static inline size_t soc_usb_buffer_write(struct soc_usb_buffer *b,
                                          const void *bytes, size_t count)
{
    const uint8_t *p = (const uint8_t *)bytes;
    size_t n, i;
    if (!bytes || !count) return 0;
    if (__atomic_exchange_n(&b->busy, 1u, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&b->contended, count, __ATOMIC_RELAXED);
        return 0;
    }
    n = count;
    if (n > SOC_USB_LOG_WRITE_BUDGET) n = SOC_USB_LOG_WRITE_BUDGET;
    if (n > b->capacity - b->used) n = b->capacity - b->used;
    for (i = 0; i < n; i++) {
        b->data[b->head] = p[i];
        if (++b->head == b->capacity) b->head = 0;
    }
    __atomic_store_n(&b->used, b->used + n, __ATOMIC_RELAXED);
    if (b->used > b->high_water) __atomic_store_n(&b->high_water, b->used, __ATOMIC_RELAXED);
    __atomic_fetch_add(&b->accepted, n, __ATOMIC_RELAXED);
    __atomic_fetch_add(&b->overflow, count - n, __ATOMIC_RELAXED);
    __atomic_store_n(&b->busy, 0u, __ATOMIC_RELEASE);
    return n;
}

/* The sink only copies CPU memory; it must never poll hardware, print, wait,
 * allocate or invoke a scheduler. Consume only the accepted prefix so USB
 * backpressure and partial copies preserve the exact FIFO order. */
static inline size_t soc_usb_buffer_drain(struct soc_usb_buffer *b,
                                          soc_usb_buffer_sink sink, void *cookie,
                                          size_t budget)
{
    size_t n, accepted;
    if (__atomic_exchange_n(&b->busy, 1u, __ATOMIC_ACQUIRE)) return 0;
    n = b->used;
    if (n > budget) n = budget;
    if (n > SOC_USB_LOG_DRAIN_BUDGET) n = SOC_USB_LOG_DRAIN_BUDGET;
    if (n > b->capacity - b->tail) n = b->capacity - b->tail;
    accepted = n ? sink(cookie, b->data + b->tail, n) : 0;
    /* Defensive sink contract: never advance outside initialized bytes. */
    if (accepted > n) accepted = 0;
    b->tail = (b->tail + accepted) % b->capacity;
    __atomic_store_n(&b->used, b->used - accepted, __ATOMIC_RELAXED);
    __atomic_store_n(&b->busy, 0u, __ATOMIC_RELEASE);
    return accepted;
}
static inline size_t soc_usb_buffer_pending(const struct soc_usb_buffer *b)
{
    return __atomic_load_n(&b->used, __ATOMIC_RELAXED);
}
static inline uint64_t soc_usb_buffer_dropped(const struct soc_usb_buffer *b)
{
    return __atomic_load_n(&b->overflow, __ATOMIC_RELAXED) +
           __atomic_load_n(&b->contended, __ATOMIC_RELAXED);
}
#endif
