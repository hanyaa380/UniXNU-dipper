// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of FajitaTouchBoard.h: pin addresses and configuration words, the DFS level choice for the 19.2 MHz I2C source, and
// the coordinate mapping to the units NativeHidInput expects.
#include "../FajitaTouchBoard.h"
#include <cassert>
#include <cstdio>

static void testPins() {
    // The per-pin register blocks of the research table: TLMM 0x03400000 + tile + 0x1000 * gpio.
    assert(FTB_TLMM_BASE + ftb_pin_offset(FTB_GPIO_SDA) == 0x03931000ULL);
    assert(FTB_TLMM_BASE + ftb_pin_offset(FTB_GPIO_SCL) == 0x03932000ULL);
    assert(FTB_TLMM_BASE + ftb_pin_offset(FTB_GPIO_VIO) == 0x03558000ULL);
    assert(FTB_TLMM_BASE + ftb_pin_offset(FTB_GPIO_RST) == 0x03963000ULL);
    assert(FTB_TLMM_BASE + ftb_pin_offset(FTB_GPIO_INT) == 0x0357d000ULL);
    assert(ftb_pin_offset(81) == 0);   // a TZ-reserved pin (gpio-reserved-ranges <81 4>) is never addressed
    // qup12 at 2 mA without bias; reset at 16 mA, pull-up, output; interrupt input with pull-up.
    assert(ftb_pin_cfg(FTB_FUNC_QUP12, 2, FTB_PULL_NONE, 0) == (1u << 2));
    assert(ftb_pin_cfg(FTB_FUNC_GPIO, 16, FTB_PULL_UP, 1) == (3u | (7u << 6) | (1u << 9)));
    assert(ftb_pin_cfg(FTB_FUNC_GPIO, 16, FTB_PULL_UP, 0) == (3u | (7u << 6)));
    assert(ftb_pin_cfg(FTB_FUNC_GPIO, 40, FTB_PULL_NONE, 0) == (7u << 6));   // clamped to 16 mA
    assert(ftb_pin_func(ftb_pin_cfg(FTB_FUNC_QUP12, 2, 0, 0)) == 1 && ftb_pin_drive_ma(ftb_pin_cfg(0, 16, 0, 0)) == 16);
    // Falling edge to the application processor: (3 << 5) | raw status | (2 << 2), polarity and enable clear (enabled last).
    assert(ftb_intr_cfg_falling(0xffffffffu) == ((0xffffffffu & ~0xffu) | (3u << 5) | (1u << 4) | (2u << 2) | 0x00u));
    assert(ftb_intr_cfg_falling(0) == ((3u << 5) | (1u << 4) | (2u << 2)));
    assert(FTB_TLMM_SUMMARY_INTID == 240);
}

static void testClocks() {
    assert(ftb_branch_running(0x00000001) && !ftb_branch_running(0x80000000u) && ftb_branch_running(0x80000002u));
    // The SDM845 table ftbl_gcc_qupv3_wrap0_s0_clk_src as DFS levels: 7.3728 MHz (GPLL0 even M/N), 14.7456, 19.2 (TCXO), 29.49, ...
    uint32_t cfg[8], m[8], n[8];
    auto level = [&](unsigned l, unsigned src, unsigned hid, unsigned mval, unsigned nval) {
        cfg[l] = (src << 8) | hid | (mval ? (2u << 12) : 0);
        m[l] = mval;
        n[l] = mval ? (~(nval - mval)) & 0xffff : 0xffff;
    };
    level(0, 6, 0, 384, 15625);
    level(1, 6, 0, 768, 15625);
    level(2, 0, 0, 0, 0);
    level(3, 6, 0, 1536, 15625);
    level(4, 6, 0, 8, 75);
    level(5, 6, 0, 4, 25);
    level(6, 6, 0, 16, 75);
    level(7, 6, 0, 4, 15);
    assert(ftb_rcg_hz(cfg[0], m[0], n[0]) == 7372800);
    assert(ftb_rcg_hz(cfg[4], m[4], n[4]) == 32000000);
    assert(ftb_rcg_hz(cfg[2], m[2], n[2]) == 19200000);
    assert(ftb_rcg_hz((6u << 8) | 5u, 0, 0xffff) == 100000000);   // GPLL0 even / 3 (hid 5 = divide by 3)
    assert(ftb_xo_level(1, cfg, m, n, 8, 0) == 2);
    // TCXO with hid 1 (divide by 1) also counts.
    level(5, 0, 1, 0, 0);
    level(2, 6, 0, 8, 75);
    assert(ftb_xo_level(1, cfg, m, n, 8, 0) == 5);
    level(5, 6, 0, 4, 25);
    assert(ftb_xo_level(1, cfg, m, n, 8, 0) == -1);   // no 19.2 MHz level: refuse (no RCG reprogramming)
    // DFS off: the current configuration decides; select 0.
    assert(ftb_xo_level(0, cfg, m, n, 8, 0x0) == 0);
    assert(ftb_xo_level(0, cfg, m, n, 8, (6u << 8)) == -1);
    assert(ftb_xo_level(0, cfg, m, n, 8, (2u << 12)) == -1);   // M/N mode on the TCXO: not the plain 19.2 MHz
}

static void testMapping() {
    int x, y;
    const ftb_view panel = {0, 0, 0, 0, 0, 0, 0};   // no logical display: identity
    ftb_map map = {1079, 2339, 0};
    assert(ftb_map_point(&map, &panel, 0, 0, &x, &y) && x == 0 && y == 0);
    assert(ftb_map_point(&map, &panel, 1079, 2339, &x, &y) && x == 30479 && y == 20319);
    ftb_map_point(&map, &panel, 5000, 9000, &x, &y); assert(x == 30479 && y == 20319);   // clamped
    ftb_map_point(&map, &panel, 540, 1170, &x, &y);
    assert(x == (int)(540ull * 30479 / 1079) && y == (int)(1170ull * 20319 / 2339));
    map.orient = FTB_ORIENT_MIRROR_X | FTB_ORIENT_MIRROR_Y;
    ftb_map_point(&map, &panel, 0, 0, &x, &y); assert(x == 30479 && y == 20319);
    map = {2339, 1079, FTB_ORIENT_SWAP};   // a controller whose X runs along the long edge
    ftb_map_point(&map, &panel, 2339, 0, &x, &y); assert(x == 0 && y == 20319);
    ftb_map_point(&map, &panel, 0, 1079, &x, &y); assert(x == 30479 && y == 0);
    ftb_map defaults = {0, 0, 0};
    ftb_map_point(&defaults, &panel, 1080, 2340, &x, &y); assert(x == 30479 && y == 20319);
    // A controller with finer units than the panel still spans it.
    ftb_map fine = {1079 * 4, 2339 * 4, 0};
    unsigned px, py;
    ftb_map_panel(&fine, 1079 * 2, 2339 * 2, &px, &py); assert(px == 539 && py == 1169);
}

static void testLogical() {
    // fajita-display-logical=2048x2732: the image is 1080 x 1441 at y = 449 (2732 * 1080 / 2048 = 1440.7).
    const ftb_view v = {1, 2048, 2732, 0, 449, 1080, 1441};
    assert(ftb_view_usable(&v) && ftb_view_w(&v) == 2048 && ftb_view_h(&v) == 2732);
    unsigned lx, ly;
    assert(ftb_panel_to_logical(&v, 0, 449, &lx, &ly) && lx == 0 && ly == 0);
    assert(ftb_panel_to_logical(&v, 1079, 449 + 1440, &lx, &ly) && lx == 2047 && ly == 2731);
    assert(ftb_panel_to_logical(&v, 540, 449 + 720, &lx, &ly) && lx == 1024 && ly == 1366);
    // Letterbox: above and below the image, clamped to its top and bottom rows.
    assert(!ftb_panel_to_logical(&v, 540, 100, &lx, &ly) && lx == 1024 && ly == 0);
    assert(!ftb_panel_to_logical(&v, 540, 2339, &lx, &ly) && lx == 1024 && ly == 2731);
    int x, y;
    ftb_logical_to_published(&v, 2047, 2731, &x, &y); assert(x == 30479 && y == 20319);
    ftb_logical_to_published(&v, 0, 0, &x, &y); assert(x == 0 && y == 0);
    // End to end: a controller reporting 0..1079 x 0..2339.
    const ftb_map map = {1079, 2339, 0};
    assert(!ftb_map_point(&map, &v, 540, 2300, &x, &y) && y == 20319);
    assert(ftb_map_point(&map, &v, 540, 1169, &x, &y) && x == (int)(1024ull * 30479 / 2047) && y == (int)(1366ull * 20319 / 2731));
    // A rectangle that does not fit the panel is ignored (identity), as is valid 0.
    const ftb_view bad = {1, 2048, 2732, 0, 1000, 1080, 1441};
    assert(!ftb_view_usable(&bad) && ftb_view_w(&bad) == FTB_PANEL_W);
    assert(ftb_panel_to_logical(&bad, 540, 2300, &lx, &ly) && lx == 540 && ly == 2300);
    const ftb_view off = {0, 2048, 2732, 0, 449, 1080, 1441};
    assert(!ftb_view_usable(&off));
}

int main() {
    testPins();
    testClocks();
    testMapping();
    testLogical();
    std::puts("PASS: fajita touch board constants, clock level choice, coordinate mapping and the logical display transform");
    return 0;
}
