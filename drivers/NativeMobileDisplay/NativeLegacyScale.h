// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita: scale a logical frame (the size userland was told, fajita-display-logical) onto the real panel.
// The image is fitted (aspect kept), centered, with a black letterbox that is written once, not per frame.
// Each panel pixel of the image averages a 2x2 source block (2-tap box per axis) when that axis shrinks,
// or takes the nearest source pixel when it grows. The taps are precomputed per column and per row, so a
// frame costs four cached 32-bit loads and three byte-wise averages per image pixel, and only the image
// rectangle is stored to the device mapping (64-bit stores; optionally only the 64-byte blocks that changed).
#pragma once
#include "NativeLegacyRequest.h"

struct native_legacy_scale_map {
    uint32_t lw, lh;            // logical source
    uint32_t pw, ph;            // panel
    uint32_t ox, oy, ow, oh;    // image rectangle on the panel (ox and ow even)
    uint32_t *col0, *col1;      // per image column: byte offsets of the two source pixels in a row
    uint32_t *row0, *row1;      // per image row: the two source rows
};

// The fitted, centered image rectangle. 0 when the geometry is unusable.
static inline int native_legacy_scale_fit(uint32_t lw, uint32_t lh, uint32_t pw, uint32_t ph,
    uint32_t *ox, uint32_t *oy, uint32_t *ow, uint32_t *oh) {
    if (!lw || !lh || !pw || !ph || lw > 16384 || lh > 16384 || pw > 16384 || ph > 16384) return 0;
    uint64_t w, h;
    if ((uint64_t)pw * lh <= (uint64_t)ph * lw) { w = pw; h = ((uint64_t)lh * pw * 2 + lw) / ((uint64_t)lw * 2); }
    else { h = ph; w = ((uint64_t)lw * ph * 2 + lh) / ((uint64_t)lh * 2); }
    if (w > pw) w = pw;
    if (h > ph) h = ph;
    w &= ~1ull;                 // 64-bit stores: two pixels at a time from an even column
    if (!w || !h) return 0;
    *ow = (uint32_t)w; *oh = (uint32_t)h;
    *ox = ((pw - (uint32_t)w) / 2) & ~1u; *oy = (ph - (uint32_t)h) / 2;
    return 1;
}

// Taps for `out` destination samples over `in` source samples: two neighbours around each box center when
// shrinking (in > out), the nearest sample twice otherwise. Values are sample indices times `unit`.
static inline void native_legacy_scale_taps(uint32_t *a, uint32_t *b, uint32_t out, uint32_t in, uint32_t unit) {
    for (uint32_t i = 0; i < out; ++i) {
        uint64_t twice = (uint64_t)(2 * i + 1) * in;          // 2 * box center * out
        uint32_t first, second;
        if (in > out) {
            uint64_t left = twice > out ? (twice - out) / (2ull * out) : 0;   // floor(center - 0.5)
            if (left > in - 2) left = in - 2;
            first = (uint32_t)left; second = first + 1;
        } else {
            uint64_t nearest = twice / (2ull * out);
            if (nearest > in - 1) nearest = in - 1;
            first = second = (uint32_t)nearest;
        }
        a[i] = first * unit; b[i] = second * unit;
    }
}

// Byte-wise average of two BGRA pixels (rounds down).
static inline uint32_t native_legacy_avg2(uint32_t a, uint32_t b) {
    return (a & b) + (((a ^ b) & 0xfefefefeU) >> 1);
}

static inline void native_legacy_scale_row(uint32_t *out, const unsigned char *r0, const unsigned char *r1,
    const uint32_t *c0, const uint32_t *c1, uint32_t ow) {
    for (uint32_t x = 0; x < ow; ++x) {
        uint32_t p, q, s, t;
        __builtin_memcpy(&p, r0 + c0[x], 4); __builtin_memcpy(&q, r0 + c1[x], 4);
        __builtin_memcpy(&s, r1 + c0[x], 4); __builtin_memcpy(&t, r1 + c1[x], 4);
        out[x] = native_legacy_avg2(native_legacy_avg2(p, q), native_legacy_avg2(s, t));
    }
}

static inline int native_legacy_scale_destination(volatile void *destination, uint64_t destinationBytes,
    uint64_t destinationStride, const struct native_legacy_scale_map *m) {
    uintptr_t dst = (uintptr_t)destination;
    return m && dst && !(dst & 7) && !(destinationStride & 7) &&
        native_legacy_surface_bounds(m->pw, m->ph, destinationStride, destinationBytes, destinationBytes) &&
        destinationBytes <= UINTPTR_MAX && dst <= UINTPTR_MAX - destinationBytes &&
        m->ow && m->oh && !(m->ow & 1) && !(m->ox & 1) && m->ox + m->ow <= m->pw && m->oy + m->oh <= m->ph;
}

// Fill everything outside the image rectangle with `value`. Done once; frames never touch it.
static inline int native_legacy_scale_letterbox(volatile void *destination, uint64_t destinationBytes,
    uint64_t destinationStride, const struct native_legacy_scale_map *m, uint32_t value) {
    if (!native_legacy_scale_destination(destination, destinationBytes, destinationStride, m)) return 0;
    uintptr_t dst = (uintptr_t)destination;
    for (uint32_t row = 0; row < m->ph; ++row) {
        volatile uint32_t *out = (volatile uint32_t *)(dst + row * destinationStride);
        if (row < m->oy || row >= m->oy + m->oh) {
            for (uint32_t column = 0; column < m->pw; ++column) out[column] = value;
        } else {
            for (uint32_t column = 0; column < m->ox; ++column) out[column] = value;
            for (uint32_t column = m->ox + m->ow; column < m->pw; ++column) out[column] = value;
        }
    }
    return 1;
}

// Scale one logical frame into the image rectangle. `line` is a cached buffer of ow pixels. With a shadow
// (ow*oh pixels, rows packed, 8-byte aligned) only the 64-byte blocks that differ from it are stored (valid=0
// stores everything and fills it). Returns the bytes stored, or UINT64_MAX when the geometry is unusable.
static inline uint64_t native_legacy_scale_copy(volatile void *destination, uint64_t destinationBytes,
    uint64_t destinationStride, const void *source, uint64_t sourceBytes, uint64_t sourceStride,
    const struct native_legacy_scale_map *m, uint32_t *line, void *shadow, int valid) {
    uintptr_t src = (uintptr_t)source;
    if (!native_legacy_scale_destination(destination, destinationBytes, destinationStride, m) ||
        !src || !line || !m->col0 || !m->col1 || !m->row0 || !m->row1 || ((uintptr_t)line & 7) ||
        !native_legacy_surface_bounds(m->lw, m->lh, sourceStride, sourceBytes, sourceBytes) ||
        sourceBytes > UINTPTR_MAX || src > UINTPTR_MAX - sourceBytes || (shadow && ((uintptr_t)shadow & 7)))
        return UINT64_MAX;
    uintptr_t dst = (uintptr_t)destination;
    const uint32_t words = m->ow / 2;
    uint64_t stored = 0;
    for (uint32_t y = 0; y < m->oh; ++y) {
        // Tap tables only hold indices below lw and lh (native_legacy_scale_taps), inside the bounded source.
        native_legacy_scale_row(line, (const unsigned char *)(src + (uint64_t)m->row0[y] * sourceStride),
            (const unsigned char *)(src + (uint64_t)m->row1[y] * sourceStride), m->col0, m->col1, m->ow);
        volatile uint64_t *out = (volatile uint64_t *)(dst + (uint64_t)(m->oy + y) * destinationStride + (uint64_t)m->ox * 4);
        const uint64_t *in = (const uint64_t *)line;
        if (!shadow) {
            for (uint32_t w = 0; w < words; ++w) out[w] = in[w];
            stored += (uint64_t)words * 8;
            continue;
        }
        uint64_t *old = (uint64_t *)((unsigned char *)shadow + (uint64_t)y * m->ow * 4);
        uint32_t w = 0;
        for (; w + 8 <= words; w += 8) {
            if (valid && !((in[w] ^ old[w]) | (in[w + 1] ^ old[w + 1]) | (in[w + 2] ^ old[w + 2]) | (in[w + 3] ^ old[w + 3]) |
                           (in[w + 4] ^ old[w + 4]) | (in[w + 5] ^ old[w + 5]) | (in[w + 6] ^ old[w + 6]) | (in[w + 7] ^ old[w + 7])))
                continue;
            for (unsigned i = 0; i < 8; ++i) { out[w + i] = in[w + i]; old[w + i] = in[w + i]; }
            stored += 64;
        }
        for (; w < words; ++w) {
            if (valid && in[w] == old[w]) continue;
            out[w] = in[w]; old[w] = in[w]; stored += 8;
        }
    }
    return stored;
}
