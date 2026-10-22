// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test: GeniI2c.c against a register model of a GENI serial engine running I2C firmware in FIFO mode, with an I2C slave
// behind it. The model follows the documented register behaviour (and how the Linux driver reacts to it); it is not hardware.
// Build: c++ -std=c++17 geni-i2c-test.cpp FakeGeniI2c.cpp ../GeniI2c.c (scripts/check-fajita-touch-host.py does it).
#include "FakeGeniI2c.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static FakeGeniI2c *fresh(FakeGeniI2c &m, geni_i2c &g) {
    m = FakeGeniI2c();
    g = geni_i2c();
    m.attach(g);
    return &m;
}

static void testInitAndClock() {
    FakeGeniI2c m; geni_i2c g;
    fresh(m, g);
    assert(geni_i2c_init(&g) == 0);
    assert(g.info.protocol == GI2C_PROTO_I2C && g.info.tx_fifo_depth == 16 && g.info.fifo_width_bits == 32);
    assert(g.tx_wm == 15);
    assert(m.reg(GI2C_RX_WATERMARK) == 15 && m.reg(GI2C_RX_RFR_WATERMARK) == 16);
    // The I2C packing the Linux driver programs (8 bits, four per word, MSB first).
    assert(m.reg(GI2C_TX_PACKING_CFG0) == 0x7f8fe && m.reg(GI2C_TX_PACKING_CFG1) == 0xffefe);
    assert(m.reg(GI2C_RX_PACKING_CFG0) == 0x7f8fe && m.reg(GI2C_RX_PACKING_CFG1) == 0xffefe);
    assert(m.reg(GI2C_BYTE_GRAN) == 0);
    assert((m.reg(GI2C_DMA_MODE_EN) & 1) == 0);
    assert((m.reg(GI2C_CGC_CTRL) & 0x7f) == 0x7f && (m.reg(GI2C_DMA_GENERAL_CFG) & 0xf) == 0xf);
    assert(m.reg(GI2C_OUTPUT_CTRL) == 0x7f && m.reg(GI2C_FORCE_DEFAULT_REG) == 1);
    assert((m.reg(GI2C_M_IRQ_EN) & GI2C_M_FIFO_IRQ_EN) == GI2C_M_FIFO_IRQ_EN);
    assert((m.reg(GI2C_M_IRQ_EN) & GI2C_M_COMMON_IRQ_EN) == GI2C_M_COMMON_IRQ_EN);
    assert(m.reg(GI2C_GSI_EVENT_EN) == 0);
    assert(m.reg(GI2C_IRQ_EN) != 0);   // the top-level enable stays on unless asked
    // No transfer before the clock is programmed.
    uint8_t b = 0;
    assert(geni_i2c_read(&g, 0x20, &b, 1) == GI2C_ERR_PARAM);
    // 19.2 MHz source, 400 kHz: {div 2, high 5, low 11, cycle 22}.
    assert(geni_i2c_set_clock(&g, 0, 19200000, 400000) == 0);
    assert(m.reg(GI2C_CLK_SEL) == 0 && m.reg(GI2C_SER_M_CLK_CFG) == ((2u << 4) | 1u));
    assert(m.reg(GI2C_SCL_COUNTERS) == ((5u << 20) | (11u << 10) | 22u));
    assert(geni_i2c_set_clock(&g, 3, 32000000, 100000) == 0);
    assert(m.reg(GI2C_CLK_SEL) == 3 && m.reg(GI2C_SER_M_CLK_CFG) == ((12u << 4) | 1u));
    assert(m.reg(GI2C_SCL_COUNTERS) == ((9u << 20) | (10u << 10) | 26u));
    assert(geni_i2c_set_clock(&g, 0, 19200000, 3400000) == GI2C_ERR_PARAM);
    assert(geni_i2c_set_clock(&g, 0, 24000000, 400000) == GI2C_ERR_PARAM);
    assert(geni_i2c_set_clock(&g, 16, 19200000, 400000) == GI2C_ERR_PARAM);
    // irq_outputs_off clears the top-level enable after init.
    fresh(m, g);
    g.irq_outputs_off = 1;
    assert(geni_i2c_init(&g) == 0 && m.reg(GI2C_IRQ_EN) == 0);
}

static void testRefusals() {
    FakeGeniI2c m; geni_i2c g;
    fresh(m, g); m.fwRevision = 0x0100;          // SPI firmware
    assert(geni_i2c_init(&g) == GI2C_ERR_PROTO);
    fresh(m, g); m.ifDisable = 1;                // FIFO fused off: GPI DMA only
    assert(geni_i2c_init(&g) == GI2C_ERR_NOTSUP);
    fresh(m, g); m.hwParam0 = 32u << 24;          // depth 0
    assert(geni_i2c_init(&g) == GI2C_ERR_NODEV);
    // QUP 3.10+: the depth field is 8 bits wide.
    fresh(m, g); m.wrapperVersion = 0x30100000u; m.hwParam0 = (32u << 24) | (0x40u << 16);
    assert(geni_i2c_init(&g) == 0 && g.info.tx_fifo_depth == 64);
    fresh(m, g); m.wrapperVersion = 0x20000000u; m.hwParam0 = (32u << 24) | (0x50u << 16);   // older: 6 bits (0x10)
    assert(geni_i2c_init(&g) == 0 && g.info.tx_fifo_depth == 16);
}

static void ready(FakeGeniI2c &m, geni_i2c &g) {
    fresh(m, g);
    assert(geni_i2c_init(&g) == 0);
    assert(geni_i2c_set_clock(&g, 0, 19200000, 400000) == 0);
}

static void testRegisterRead() {
    FakeGeniI2c m; geni_i2c g; ready(m, g);
    for (unsigned i = 0; i < 256; i++) m.slave.mem[i] = (uint8_t)(i ^ 0xa5);
    const uint8_t reg = 0xe9;
    uint8_t out[6] = {};
    assert(geni_i2c_write_read(&g, 0x20, &reg, 1, out, sizeof out) == 0);
    for (unsigned i = 0; i < 6; i++) assert(out[i] == (uint8_t)((0xe9 + i) ^ 0xa5));
    // On the wire: START 0x20 W, 0xe9, repeated START (no STOP between), 0x20 R, six bytes, STOP.
    assert(m.bus == "S40 e9 S41 r6 P");
    assert(m.stopStretchWrites == 1);
    assert(g.xfers == 2 && g.nacks == 0 && g.errors == 0 && g.timeouts == 0);
    assert(m.cancels == 0 && m.aborts == 0);
}

static void testLongRead() {
    FakeGeniI2c m; geni_i2c g; ready(m, g);
    for (unsigned i = 0; i < 256; i++) m.slave.mem[i] = (uint8_t)(i * 7 + 1);
    // 81 bytes: more than one RX watermark (15 words = 60 bytes) and a partial last word.
    const uint8_t reg = 0x06;
    std::vector<uint8_t> out(81, 0xee);
    assert(geni_i2c_write_read(&g, 0x20, &reg, 1, out.data(), out.size()) == 0);
    for (unsigned i = 0; i < out.size(); i++) assert(out[i] == (uint8_t)((6 + i) * 7 + 1));
    assert(m.rxWatermarkEvents >= 1);
    // 3 bytes and exactly one word.
    for (size_t n : {size_t(1), size_t(3), size_t(4), size_t(5), size_t(64), size_t(200)}) {
        std::vector<uint8_t> o(n, 0);
        assert(geni_i2c_read(&g, 0x20, o.data(), n) == 0);
    }
}

static void testLongWrite() {
    FakeGeniI2c m; geni_i2c g; ready(m, g);
    std::vector<uint8_t> data(1 + 100);
    data[0] = 0x10;
    for (unsigned i = 1; i < data.size(); i++) data[i] = (uint8_t)(i + 3);
    // More than one refill: 15 words (60 bytes) per watermark event.
    assert(geni_i2c_write(&g, 0x20, data.data(), data.size(), 1) == 0);
    for (unsigned i = 1; i < data.size(); i++) assert(m.slave.mem[0x10 + i - 1] == data[i]);
    assert(m.txWatermarkEvents >= 2);
    assert(m.reg(GI2C_TX_WATERMARK) == 0);   // refill events off once everything is queued
    assert(m.maxTxWords <= 16);              // never more than the FIFO holds
}

static void testAddressNack() {
    FakeGeniI2c m; geni_i2c g; ready(m, g);
    m.slave.present = false;
    const uint8_t reg = 0xe9;
    uint8_t out[6];
    assert(geni_i2c_write_read(&g, 0x20, &reg, 1, out, 6) == GI2C_ERR_ADDR_NACK);
    // An unanswered address ends the message (CMD_DONE); the Linux driver neither cancels nor sends the read.
    assert(m.cancels == 0 && m.aborts == 0);
    assert(m.bus == "S40 N P");
    assert(g.nacks == 1 && g.first_err == GI2C_ERR_ADDR_NACK && g.snap_valid && g.snap_gp_len == 0);
    // Another address on the same bus: also NACK.
    m.slave.present = true;
    assert(geni_i2c_write_read(&g, 0x21, &reg, 1, out, 6) == GI2C_ERR_ADDR_NACK);
    assert(geni_i2c_write_read(&g, 0x20, &reg, 1, out, 6) == 0);   // the bus still works afterwards
}

static void testDataNackAndErrors() {
    FakeGeniI2c m; geni_i2c g; ready(m, g);
    m.slave.nackAfter = 2;   // the third byte is not acknowledged
    const uint8_t data[4] = {0x10, 1, 2, 3};
    assert(geni_i2c_write(&g, 0x20, data, 4, 1) == GI2C_ERR_DATA_NACK);
    assert(m.cancels == 1);  // every error other than an address NACK is cancelled
    assert(g.snap_gp_len == 2);
    m.slave.nackAfter = -1;

    ready(m, g);
    m.injectGp = GI2C_M_GP_IRQ3;   // bus protocol error
    uint8_t b;
    assert(geni_i2c_read(&g, 0x20, &b, 1) == GI2C_ERR_BUS_PROTO && m.cancels == 1);
    ready(m, g);
    m.injectGp = GI2C_M_GP_IRQ4;   // arbitration lost
    assert(geni_i2c_read(&g, 0x20, &b, 1) == GI2C_ERR_ARB_LOST);
    ready(m, g);
    m.injectGp = GI2C_M_CMD_OVERRUN;
    assert(geni_i2c_read(&g, 0x20, &b, 1) == GI2C_ERR_IO);
    // A status bit without CMD_FAILURE is not an error (the Linux handler ignores it).
    ready(m, g);
    m.noiseBits = GI2C_M_IO_DATA_ASSERT;
    assert(geni_i2c_read(&g, 0x20, &b, 1) == 0);
}

static void testTimeoutRecovery() {
    FakeGeniI2c m; geni_i2c g; ready(m, g);
    g.poll_limit = 500;
    m.stuck = true;
    uint8_t out[8];
    assert(geni_i2c_read(&g, 0x20, out, 8) == GI2C_ERR_TIMEOUT);
    assert(m.cancels == 1 && m.aborts == 0 && g.timeouts == 1 && g.snap_valid);
    // A cancel the engine never confirms is followed by an abort.
    ready(m, g);
    g.poll_limit = 500;
    m.stuck = true;
    m.cancelIgnored = true;
    assert(geni_i2c_read(&g, 0x20, out, 8) == GI2C_ERR_TIMEOUT);
    assert(m.cancels == 1 && m.aborts == 1);
    // Words a cancelled read left in the RX FIFO do not leak into the next read.
    ready(m, g);
    g.poll_limit = 500;
    m.stuck = true;
    m.leftoverWords = 3;
    assert(geni_i2c_read(&g, 0x20, out, 8) == GI2C_ERR_TIMEOUT);
    assert(m.rxWords() == 0);
    m.stuck = false;
    for (unsigned i = 0; i < 256; i++) m.slave.mem[i] = (uint8_t)i;
    const uint8_t reg = 0x40;
    assert(geni_i2c_write_read(&g, 0x20, &reg, 1, out, 8) == 0 && out[0] == 0x40 && out[7] == 0x47);
}

static void testParams() {
    FakeGeniI2c m; geni_i2c g; ready(m, g);
    uint8_t b = 0;
    assert(geni_i2c_read(&g, 0x20, &b, 0) == GI2C_ERR_PARAM);
    assert(geni_i2c_read(&g, 0x80, &b, 1) == GI2C_ERR_PARAM);
    assert(geni_i2c_read(&g, 0x20, nullptr, 1) == GI2C_ERR_PARAM);
    assert(geni_i2c_write(&g, 0x20, nullptr, 1, 1) == GI2C_ERR_PARAM);
    assert(geni_i2c_write(&g, 0x20, &b, 0x1000000, 1) == GI2C_ERR_PARAM);
    assert(m.commands == 0);   // nothing reached the engine
}

int main() {
    testInitAndClock();
    testRefusals();
    testRegisterRead();
    testLongRead();
    testLongWrite();
    testAddressNack();
    testDataNackAndErrors();
    testTimeoutRecovery();
    testParams();
    std::puts("PASS: GENI I2C FIFO sequencing against the register model");
    return 0;
}
