/* SPDX-License-Identifier: MIT */
#ifndef QCOM_USER_LOG_WIRE_H
#define QCOM_USER_LOG_WIRE_H
#include <stdint.h>
#include <stddef.h>
#define SOC_ULOG_MAGIC 0x534c4731u
#define SOC_ULOG_CHUNK 1024u
#define SOC_ULOG_MAX (4u * 1024u * 1024u)
#define SOC_ULOG_STATUS 1u
#define SOC_ULOG_GZIP 2u
/* Fixed little-endian arm64 ABI; no pointers, packed fields or native sizes. */
struct QcomUserLogWire {
    uint32_t magic, version, sessionLo, sessionHi, stream, sequence;
    uint32_t total, offset, bytes, crc, flags, reserved;
    uint8_t data[SOC_ULOG_CHUNK];
};
#if defined(__cplusplus)
static_assert(offsetof(QcomUserLogWire, data) == 48 && sizeof(QcomUserLogWire) == 1072,
              "User log wire ABI must match the kernel and host decoder");
#else
_Static_assert(offsetof(struct QcomUserLogWire, data) == 48 && sizeof(struct QcomUserLogWire) == 1072,
               "User log wire ABI must match the kernel and host decoder");
#endif
static inline uint32_t qcom_ulog_crc(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~0u;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= p[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static inline int qcom_ulog_valid(const struct QcomUserLogWire *w, size_t size)
{
    return size >= offsetof(struct QcomUserLogWire, data) &&
        w->magic == SOC_ULOG_MAGIC && w->version == 1 && !w->reserved &&
        w->stream && w->bytes && w->bytes <= SOC_ULOG_CHUNK &&
        size == offsetof(struct QcomUserLogWire, data) + w->bytes &&
        w->total && w->total <= SOC_ULOG_MAX && w->offset <= w->total &&
        w->bytes <= w->total - w->offset && w->offset % SOC_ULOG_CHUNK == 0 &&
        w->sequence == w->offset / SOC_ULOG_CHUNK &&
        (w->bytes == SOC_ULOG_CHUNK || w->offset + w->bytes == w->total) &&
        (w->flags == SOC_ULOG_STATUS || w->flags == SOC_ULOG_GZIP) &&
        qcom_ulog_crc(w->data, w->bytes) == w->crc;
}
#endif
