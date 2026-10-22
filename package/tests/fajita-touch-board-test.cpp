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
    ftb_map map = {1079, 2339, 0};
    ftb_map_point(&map, 0, 0, &x, &y); assert(x == 0 && y == 0);
    ftb_map_point(&map, 1079, 2339, &x, &y); assert(x == 30479 && y == 20319);
    ftb_map_point(&map, 5000, 9000, &x, &y); assert(x == 30479 && y == 20319);   // clamped
    ftb_map_point(&map, 540, 1170, &x, &y); assert(x == (int)(540ull * 30479 / 1079) && y == (int)(1170ull * 20319 / 2339));
    map.orient = FTB_ORIENT_MIRROR_X | FTB_ORIENT_MIRROR_Y;
    ftb_map_point(&map, 0, 0, &x, &y); assert(x == 30479 && y == 20319);
    map = {2339, 1079, FTB_ORIENT_SWAP};   // a controller whose X runs along the long edge
    ftb_map_point(&map, 2339, 0, &x, &y); assert(x == 0 && y == 20319);
    ftb_map_point(&map, 0, 1079, &x, &y); assert(x == 30479 && y == 0);
    ftb_map defaults = {0, 0, 0};
    ftb_map_point(&defaults, 1080, 2340, &x, &y); assert(x == 30479 && y == 20319);
}

int main() {
    testPins();
    testClocks();
    testMapping();
    std::puts("PASS: fajita touch board constants, clock level choice and coordinate mapping");
    return 0;
}
