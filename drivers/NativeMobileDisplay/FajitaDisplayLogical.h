// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita: the display size userland is told about and where its frames land on the real panel.
// With fajita-display-logical=<W>x<H> NativeLegacyDisplay reports W x H to iOS (IOMFB, swap validation) and scales each
// frame into the image rectangle of the panel, centered with a black letterbox. Without it the logical size is the panel
// and the image rectangle is the whole panel, so the transform below is the identity.
// Readers (touch): use the mapping only once `valid` is 1; it is written once, before the display registers.
#pragma once
#include <stdint.h>

struct fajita_display_logical {
    uint32_t valid;                          // 1 once the fields below describe the running display
    uint32_t logical_width, logical_height;  // the size userland sees (pixels)
    uint32_t panel_width, panel_height;      // the real scanout (pixels)
    uint32_t image_x, image_y;               // top-left of the scaled image on the panel
    uint32_t image_width, image_height;      // size of the scaled image on the panel
};

// Defined by NativeLegacyDisplay.cpp (libNativeLegacyDisplay.late.a, always linked on fajita).
#ifdef __cplusplus
extern "C" {
#endif
extern volatile struct fajita_display_logical fajita_display_logical;
#ifdef __cplusplus
}
#endif

// Panel pixel -> logical pixel. Returns 0 for a point in the letterbox (or before the display is up); the
// coordinates are then clamped to the nearest image edge in *lx, *ly so a drag that leaves the image stays usable.
static inline int fajita_display_panel_to_logical(uint32_t px, uint32_t py, uint32_t *lx, uint32_t *ly) {
    const volatile struct fajita_display_logical *m = &fajita_display_logical;
    if (!m->valid || !m->image_width || !m->image_height) { *lx = px; *ly = py; return 0; }
    int inside = px >= m->image_x && py >= m->image_y &&
                 px < m->image_x + m->image_width && py < m->image_y + m->image_height;
    uint32_t x = px < m->image_x ? 0 : px - m->image_x, y = py < m->image_y ? 0 : py - m->image_y;
    if (x >= m->image_width) x = m->image_width - 1;
    if (y >= m->image_height) y = m->image_height - 1;
    // Pixel centers: logical = (panel + 0.5) * L / I, truncated.
    *lx = (uint32_t)(((uint64_t)x * 2 + 1) * m->logical_width / ((uint64_t)m->image_width * 2));
    *ly = (uint32_t)(((uint64_t)y * 2 + 1) * m->logical_height / ((uint64_t)m->image_height * 2));
    return inside;
}
