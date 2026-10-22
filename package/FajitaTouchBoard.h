// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita (OnePlus 6T, SDM845) touchscreen board facts and the pure helpers around them. Every constant states its source; the
// sources are the mainline sdm845.dtsi / sdm845-oneplus-common.dtsi / sdm845-oneplus-fajita.dts, gcc-sdm845.c, pinctrl-sdm845.c,
// the downstream fajita.dts and synaptics_driver_s3320.c (see the touch research notes). Nothing here touches hardware;
// FajitaTouchProbe.cpp owns the MMIO. Host-tested by tests/fajita-touch-board-test.cpp.
#ifndef FAJITA_TOUCH_BOARD_H
#define FAJITA_TOUCH_BOARD_H
#include <stdint.h>
#include <stddef.h>
#include "GeniI2c.h"
#include "Rmi4.h"

// sdm845.dtsi: qupv3_id_1 geniqup@ac0000 (size 0x6000); i2c12 = i2c@a90000 (size 0x4000), interrupts GIC SPI 357 (unused: polled).
#define FTB_QUP_WRAPPER_BASE 0x00ac0000ULL
#define FTB_SE_BASE 0x00a90000ULL
#define FTB_SE_SIZE 0x4000ULL
// sdm845-oneplus-common.dtsi &i2c12 { clock-frequency = <400000>; touchscreen@20 { compatible = "syna,rmi4-s3706b" } }.
#define FTB_I2C_ADDR 0x20
#define FTB_I2C_HZ 400000u
// gcc-sdm845.c (GCC at 0x100000, downstream qcom,gcc@100000).
#define FTB_GCC_BASE 0x00100000ULL
#define FTB_GCC_GPLL0_MODE 0x0                  // gpll0 .offset 0x0; bit 31 = locked (read only here)
#define FTB_GCC_GPLL0_VOTE 0x52000              // gpll0 enable_reg, bit 0 (read only here: the TCXO level needs no PLL)
#define FTB_GCC_VOTE 0x5200c                    // enable_reg of the three QUP wrapper 1 branches below (BRANCH_HALT_VOTED)
#define FTB_VOTE_M_AHB (1u << 20)               // gcc_qupv3_wrap_1_m_ahb_clk, halt 0x1800c
#define FTB_VOTE_S_AHB (1u << 21)               // gcc_qupv3_wrap_1_s_ahb_clk, halt 0x18010, hwcg 0x18010 bit 1
#define FTB_VOTE_S4 (1u << 26)                  // gcc_qupv3_wrap1_s4_clk, halt 0x184d4
#define FTB_VOTE_ALL (FTB_VOTE_M_AHB | FTB_VOTE_S_AHB | FTB_VOTE_S4)
#define FTB_HALT_M_AHB 0x1800c
#define FTB_HALT_S_AHB 0x18010
#define FTB_HALT_S4 0x184d4
#define FTB_CBCR_CLK_OFF (1u << 31)
#define FTB_CBCR_HW_CTL (1u << 1)
// gcc_qupv3_wrap1_s4_clk_src: cmd_rcgr 0x184d8, mnd_width 16, hid_width 5, parent map 0 (0 TCXO, 1 GPLL0, 6 GPLL0 even),
// DFS-managed (clk-rcg2.c: DFSR at +0x14, per-level PERF_CFG +0x1c, M +0x5c, N +0x9c).
#define FTB_S4_RCG 0x184d8
#define FTB_RCG_CMD 0x0
#define FTB_RCG_CFG 0x4
#define FTB_RCG_DFSR 0x14                       // bit 0 DFS enabled
#define FTB_RCG_PERF_CFG(l) (0x1c + 4 * (l))
#define FTB_RCG_PERF_M(l) (0x5c + 4 * (l))
#define FTB_RCG_PERF_N(l) (0x9c + 4 * (l))
#define FTB_DFS_LEVELS 8
#define FTB_TCXO_HZ 19200000u
// TLMM (sdm845.dtsi tlmm: pinctrl@3400000, size 0xc00000, summary interrupt GIC SPI 208; pinctrl-sdm845.c: tiles NORTH 0x500000,
// SOUTH 0x900000, EAST 0x100000; per pin at tile + 0x1000 * gpio: +0 CTL, +4 IO, +8 INTR_CFG, +0xc INTR_STATUS).
#define FTB_TLMM_BASE 0x03400000ULL
#define FTB_TILE_NORTH 0x00500000u
#define FTB_TILE_EAST 0x00100000u
#define FTB_PIN_STRIDE 0x1000u
#define FTB_GPIO_SDA 49                         // PINGROUP(49, NORTH, qup12, ...): function 1
#define FTB_GPIO_SCL 50                         // PINGROUP(50, NORTH, qup12, ...)
#define FTB_GPIO_VIO 88                         // ts_1p8_supply: regulator-fixed, gpio 88 active high (EAST)
#define FTB_GPIO_RST 99                         // syna,reset-gpios = <&tlmm 99 GPIO_ACTIVE_LOW>; ts_default_pins (NORTH)
#define FTB_GPIO_INT 125                        // interrupts-extended = <&tlmm 125 IRQ_TYPE_EDGE_FALLING> (EAST)
#define FTB_FUNC_GPIO 0
#define FTB_FUNC_QUP12 1
#define FTB_TLMM_SUMMARY_INTID (32 + 208)
// CTL fields (pinctrl-sdm845.c PINGROUP): pull 1:0 (0 none, 1 down, 3 up), function 5:2, drive 8:6 ((mA / 2) - 1), OE bit 9.
#define FTB_PULL_NONE 0u
#define FTB_PULL_DOWN 1u
#define FTB_PULL_UP 3u
#define FTB_CTL_OE (1u << 9)
#define FTB_IO_IN (1u << 0)
#define FTB_IO_OUT (1u << 1)
// INTR_CFG fields (pinctrl-msm.c with the sdm845 group bits): enable 0, polarity 1, detection 3:2 (2 = falling), raw status 4,
// target 7:5 (3 = KPSS, the application processor). INTR_STATUS bit 0, acknowledged by writing 0.
#define FTB_INTR_ENABLE (1u << 0)
#define FTB_INTR_POL_HIGH (1u << 1)
#define FTB_INTR_DET_SHIFT 2
#define FTB_INTR_DET_FALLING 2u
#define FTB_INTR_RAW_STATUS (1u << 4)
#define FTB_INTR_TARGET_SHIFT 5
#define FTB_INTR_TARGET_KPSS 3u
// sdm845-oneplus-common.dtsi rmi4: syna,reset-delay-ms = <200>, syna,startup-delay-ms = <200>; synaptics_driver_s3320.c
// tpd_hw_pwron: 10 ms after the 1.8 V enable, reset high 10 ms, low 10 ms, high.
#define FTB_VIO_SETTLE_MS 10
#define FTB_RESET_STEP_MS 10
#define FTB_STARTUP_MS 200
// NativeHidInput (UniXNU drivers/NativeHidInput) normalises published coordinates with sheng's Novatek axis maxima, 30480 x 20320,
// to 0..32767 on each axis. The probe publishes in those units, so the digitizer needs no change: X 0..30479 spans the width of
// the display userland sees (the panel, or the logical size of fajita-display-logical) and Y 0..20319 its height.
#define FTB_PUBLISH_X 30480
#define FTB_PUBLISH_Y 20320
// Default F12 coordinate range when the controller reports none (the hardcoded clone layout): the 1080 x 2340 panel.
#define FTB_DEFAULT_MAX_X 1080
#define FTB_DEFAULT_MAX_Y 2340

// TLMM offset (from FTB_TLMM_BASE) of a touch pin's CTL register; 0 for a pin this board does not use.
static inline uint32_t ftb_pin_offset(unsigned gpio) {
    switch (gpio) {
    case FTB_GPIO_SDA: case FTB_GPIO_SCL: case FTB_GPIO_RST: return FTB_TILE_NORTH + FTB_PIN_STRIDE * gpio;
    case FTB_GPIO_VIO: case FTB_GPIO_INT: return FTB_TILE_EAST + FTB_PIN_STRIDE * gpio;
    default: return 0;
    }
}
static inline uint32_t ftb_pin_cfg(unsigned func, unsigned drive_ma, unsigned pull, int output) {
    unsigned code = drive_ma >= 2 ? drive_ma / 2 - 1 : 0;
    if (code > 7) code = 7;
    return (pull & 3u) | ((func & 15u) << 2) | (code << 6) | (output ? FTB_CTL_OE : 0);
}
static inline unsigned ftb_pin_func(uint32_t ctl) {return (ctl >> 2) & 15u;}
static inline unsigned ftb_pin_drive_ma(uint32_t ctl) {return (((ctl >> 6) & 7u) + 1) * 2;}
static inline uint32_t ftb_intr_cfg_falling(uint32_t cfg) {
    cfg &= ~((7u << FTB_INTR_TARGET_SHIFT) | (3u << FTB_INTR_DET_SHIFT) | FTB_INTR_POL_HIGH | FTB_INTR_ENABLE);
    return cfg | (FTB_INTR_TARGET_KPSS << FTB_INTR_TARGET_SHIFT) | FTB_INTR_RAW_STATUS | (FTB_INTR_DET_FALLING << FTB_INTR_DET_SHIFT);
}

// A branch is running when CLK_OFF is clear; a branch with hardware clock gating (bit 1) may read CLK_OFF while idle.
static inline int ftb_branch_running(uint32_t cbcr) {return !(cbcr & FTB_CBCR_CLK_OFF) || (cbcr & FTB_CBCR_HW_CTL);}

// Source rates by RCG source select (gcc_parent_map_0); GPLL0 600 MHz, GPLL0 even 300 MHz (the 100 MHz = even / 3 entry).
static inline uint64_t ftb_src_hz(unsigned sel) {return sel == 0 ? FTB_TCXO_HZ : sel == 1 ? 600000000ULL : sel == 6 ? 300000000ULL : 0;}
// Rate of one DFS level (clk-rcg2.c calc_rate: half-integer divider hid, then M/N with N stored as ~(N - M)).
static inline uint64_t ftb_rcg_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    const unsigned src = (cfg >> 8) & 7, mode = (cfg >> 12) & 3, hid = cfg & 0x1f;
    uint64_t rate = ftb_src_hz(src);
    if (hid) rate = rate * 2 / (hid + 1);
    if (mode) {
        const uint32_t m = mreg & 0xffff, n = ((~nreg) & 0xffff) + m;
        if (n) rate = rate * m / n;
    }
    return rate;
}
// The DFS level the I2C clock tables need (19.2 MHz from TCXO), as geni_se_clk_freq_match(se, 19200000, exact) finds it: the
// first level with exactly that rate. Without DFS (dfsr bit 0 clear) the RCG's current configuration must already be 19.2 MHz,
// and the select is 0. Returns the level or -1.
static inline int ftb_xo_level(uint32_t dfsr, const uint32_t *cfg, const uint32_t *m, const uint32_t *n, unsigned levels,
                               uint32_t cfg_now) {
    if (!(dfsr & 1)) return ftb_rcg_hz(cfg_now, 0, 0xffff) == FTB_TCXO_HZ && !((cfg_now >> 12) & 3) ? 0 : -1;
    for (unsigned l = 0; l < levels; l++) if (ftb_rcg_hz(cfg[l], m[l], n[l]) == FTB_TCXO_HZ) return (int)l;
    return -1;
}

// Panel orientation (fajita-touch-orient): bit 0 swaps the controller's axes, bit 1 mirrors X, bit 2 mirrors Y (after the swap).
#define FTB_ORIENT_SWAP 1u
#define FTB_ORIENT_MIRROR_X 2u
#define FTB_ORIENT_MIRROR_Y 4u
// The panel: the 1080 x 2340 portrait GOP scanout (sdm845-oneplus-fajita.dts, the s6e3fc2x01 mode).
#define FTB_PANEL_W 1080u
#define FTB_PANEL_H 2340u
struct ftb_map {unsigned max_x, max_y, orient;};
// Controller coordinates (0..max inclusive) to panel pixels (0..FTB_PANEL_W-1, 0..FTB_PANEL_H-1).
static inline void ftb_map_panel(const struct ftb_map *map, unsigned x, unsigned y, unsigned *px, unsigned *py) {
    unsigned mx = map->max_x ? map->max_x : FTB_DEFAULT_MAX_X, my = map->max_y ? map->max_y : FTB_DEFAULT_MAX_Y;
    if (map->orient & FTB_ORIENT_SWAP) {unsigned t = x; x = y; y = t; t = mx; mx = my; my = t;}
    if (x > mx) x = mx;
    if (y > my) y = my;
    unsigned ox = (unsigned)((uint64_t)x * (FTB_PANEL_W - 1) / mx), oy = (unsigned)((uint64_t)y * (FTB_PANEL_H - 1) / my);
    if (map->orient & FTB_ORIENT_MIRROR_X) ox = FTB_PANEL_W - 1 - ox;
    if (map->orient & FTB_ORIENT_MIRROR_Y) oy = FTB_PANEL_H - 1 - oy;
    *px = ox; *py = oy;
}

// What userland sees of the display (a snapshot of NativeLegacyDisplay's fajita_display_logical, FajitaDisplayLogical.h): with
// fajita-display-logical=<W>x<H> iOS sees W x H and the frames are fitted into the image rectangle of the panel with a black
// letterbox; otherwise (valid 0, or a rectangle that does not fit the panel) the logical display is the panel itself.
struct ftb_view {unsigned valid, logical_w, logical_h, image_x, image_y, image_w, image_h;};
static inline int ftb_view_usable(const struct ftb_view *v) {
    return v->valid && v->logical_w && v->logical_h && v->image_w && v->image_h && v->image_x + v->image_w <= FTB_PANEL_W &&
           v->image_y + v->image_h <= FTB_PANEL_H;
}
static inline unsigned ftb_view_w(const struct ftb_view *v) {return ftb_view_usable(v) ? v->logical_w : FTB_PANEL_W;}
static inline unsigned ftb_view_h(const struct ftb_view *v) {return ftb_view_usable(v) ? v->logical_h : FTB_PANEL_H;}
// Panel pixel to logical pixel. Returns 1 inside the image, 0 in the letterbox; a letterbox point is clamped to the nearest image
// edge first (so a drag that leaves the image, or a swipe that starts on the black bar below it, stays on the screen's edge).
// The arithmetic is the display's (pixel centres: logical = (panel + 0.5) * L / I), so a touch lands on the pixel it covers.
static inline int ftb_panel_to_logical(const struct ftb_view *v, unsigned px, unsigned py, unsigned *lx, unsigned *ly) {
    if (!ftb_view_usable(v)) {
        *lx = px < FTB_PANEL_W ? px : FTB_PANEL_W - 1;
        *ly = py < FTB_PANEL_H ? py : FTB_PANEL_H - 1;
        return px < FTB_PANEL_W && py < FTB_PANEL_H;
    }
    const int inside = px >= v->image_x && py >= v->image_y && px < v->image_x + v->image_w && py < v->image_y + v->image_h;
    unsigned x = px < v->image_x ? 0 : px - v->image_x, y = py < v->image_y ? 0 : py - v->image_y;
    if (x >= v->image_w) x = v->image_w - 1;
    if (y >= v->image_h) y = v->image_h - 1;
    *lx = (unsigned)(((uint64_t)x * 2 + 1) * v->logical_w / ((uint64_t)v->image_w * 2));
    *ly = (unsigned)(((uint64_t)y * 2 + 1) * v->logical_h / ((uint64_t)v->image_h * 2));
    return inside;
}
// Logical pixel to the published units (0..FTB_PUBLISH_X-1 across the logical width, 0..FTB_PUBLISH_Y-1 down its height), which
// NativeHidInput scales to its 0..32767 axes; the event driver then hands backboardd 0..1 of the logical screen.
static inline void ftb_logical_to_published(const struct ftb_view *v, unsigned lx, unsigned ly, int *ox, int *oy) {
    const unsigned w = ftb_view_w(v), h = ftb_view_h(v);
    if (lx >= w) lx = w - 1;
    if (ly >= h) ly = h - 1;
    *ox = w > 1 ? (int)((uint64_t)lx * (FTB_PUBLISH_X - 1) / (w - 1)) : 0;
    *oy = h > 1 ? (int)((uint64_t)ly * (FTB_PUBLISH_Y - 1) / (h - 1)) : 0;
}
// Controller coordinates straight to the published units; returns 1 when the point is on the displayed image.
static inline int ftb_map_point(const struct ftb_map *map, const struct ftb_view *view, unsigned x, unsigned y, int *ox, int *oy) {
    unsigned px, py, lx, ly;
    ftb_map_panel(map, x, y, &px, &py);
    const int inside = ftb_panel_to_logical(view, px, py, &lx, &ly);
    ftb_logical_to_published(view, lx, ly, ox, oy);
    return inside;
}

// RMI register access over the GENI I2C engine: a read is the 1-byte register address, a repeated START, then the data
// (rmi_i2c.c); a write is the register address followed by the data in one message.
struct ftb_rmi_link {struct geni_i2c *i2c; uint8_t addr; int last;};
static inline int ftb_rmi_read(void *ctx, uint8_t reg, uint8_t *buf, size_t len) {
    struct ftb_rmi_link *l = (struct ftb_rmi_link *)ctx;
    return l->last = geni_i2c_write_read(l->i2c, l->addr, &reg, 1, buf, len);
}
static inline int ftb_rmi_write(void *ctx, uint8_t reg, const uint8_t *buf, size_t len) {
    struct ftb_rmi_link *l = (struct ftb_rmi_link *)ctx;
    uint8_t msg[64];
    if (len + 1 > sizeof msg) return l->last = GI2C_ERR_PARAM;
    msg[0] = reg;
    for (size_t i = 0; i < len; i++) msg[1 + i] = buf[i];
    return l->last = geni_i2c_write(l->i2c, l->addr, msg, len + 1, 1);
}
#endif
