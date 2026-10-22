// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "FakeGeniI2c.h"
#include <cassert>
#include <cstdio>

static uint32_t seRead(void *ctx, uint32_t reg) {return static_cast<FakeGeniI2c *>(ctx)->read(reg);}
static void seWrite(void *ctx, uint32_t reg, uint32_t value) {static_cast<FakeGeniI2c *>(ctx)->write(reg, value);}
static uint32_t wrapRead(void *ctx, uint32_t reg) {
    assert(reg == GI2C_QUP_HW_VER);
    return static_cast<FakeGeniI2c *>(ctx)->wrapperVersion;
}
static void noDelay(void *, unsigned) {}

void FakeGeniI2c::attach(geni_i2c &g) {
    g.se = geni_i2c_io{this, seRead, seWrite, noDelay};
    g.wrapper = geni_i2c_io{this, wrapRead, nullptr, noDelay};
}

static void hex(std::string &s, unsigned v) {char b[8]; std::snprintf(b, sizeof b, "%02x", v); s += b;}

void FakeGeniI2c::finish(bool stop) {
    irq |= GI2C_M_CMD_DONE;
    active = false;
    if (stop) {bus += " P"; inTransaction = false; dev().stop();}
    else inTransaction = true;
}

void FakeGeniI2c::startCommand(uint32_t cmd) {
    ++commands;
    assert(!active);
    const unsigned op = cmd >> GI2C_OPCODE_SHIFT;
    const uint8_t addr = (uint8_t)((cmd >> GI2C_ADDR_SHIFT) & 0x7f);
    assert(op == GI2C_OP_WRITE || op == GI2C_OP_READ);
    readOp = op == GI2C_OP_READ;
    stopStretch = (cmd & GI2C_STOP_STRETCH) != 0;
    assert(!(readOp && stopStretch));   // the driver always ends a read with STOP
    if (stopStretch) ++stopStretchWrites;
    len = regs[(readOp ? GI2C_RX_TRANS_LEN : GI2C_TX_TRANS_LEN) / 4];
    assert(len > 0);
    moved = 0; partial = 0; partialBytes = 0; gpLen = 0;
    if (!bus.empty()) bus += ' ';
    bus += 'S';
    hex(bus, (unsigned)(addr << 1 | (readOp ? 1 : 0)));
    for (unsigned i = 0; i < leftoverWords; i++) rx.push_back(0xdead0000u + i);
    leftoverWords = 0;
    if (injectGp) {irq |= injectGp | GI2C_M_CMD_FAILURE | GI2C_M_CMD_DONE; bus += " E P"; inTransaction = false; return;}
    if (!dev().ack(addr)) {
        gpLen = 0;
        irq |= GI2C_M_GP_IRQ1 | GI2C_M_CMD_FAILURE | GI2C_M_CMD_DONE;
        bus += " N P";
        inTransaction = false;
        return;
    }
    dev().start(readOp, len);
    active = true;
}

void FakeGeniI2c::tick() {
    if (!active || stuck) return;
    for (unsigned k = 0; k < 8 && active; k++) {
        if (!readOp) {
            if (tx.empty()) break;   // underrun: wait for the driver
            const unsigned inWord = moved % 4;
            const uint8_t b = (uint8_t)(tx.front() >> (8 * inWord));
            if (!dev().writeByte(b, moved)) {
                gpLen = (uint32_t)moved;
                irq |= GI2C_M_GP_IRQ1 | GI2C_M_CMD_FAILURE | GI2C_M_CMD_DONE;
                bus += " N P";
                active = false; inTransaction = false;
                return;
            }
            bus += ' '; hex(bus, b);
            ++moved;
            if (inWord == 3 || moved == len) tx.pop_front();
            if (moved == len) {finish(!stopStretch); break;}
        } else {
            if (rx.size() >= 16) break;   // RX FIFO full: wait for the driver to drain
            partial |= (uint32_t)dev().readByte(moved) << (8 * partialBytes);
            ++moved;
            if (++partialBytes == 4) {rx.push_back(partial); partial = 0; partialBytes = 0;}
            if (moved == len) {
                if (partialBytes) {rx.push_back(partial); partial = 0; partialBytes = 0;}
                bus += " r" + std::to_string(len);
                irq |= GI2C_M_RX_FIFO_LAST;
                finish(true);
                break;
            }
        }
    }
    const uint32_t txWm = regs[GI2C_TX_WATERMARK / 4];
    if (!readOp && txWm && tx.size() <= txWm && !(irq & GI2C_M_TX_FIFO_WATERMARK)) {irq |= GI2C_M_TX_FIFO_WATERMARK; ++txWatermarkEvents;}
    const uint32_t rxWm = regs[GI2C_RX_WATERMARK / 4];
    if (readOp && rxWm && rx.size() >= rxWm && !(irq & GI2C_M_RX_FIFO_WATERMARK)) {irq |= GI2C_M_RX_FIFO_WATERMARK; ++rxWatermarkEvents;}
}

uint32_t FakeGeniI2c::read(uint32_t offset) {
    assert(offset < 0x1000 && !(offset & 3));
    switch (offset) {
    case GI2C_FW_REVISION_RO: return fwRevision;
    case GI2C_IF_DISABLE_RO: return ifDisable;
    case GI2C_HW_PARAM_0: return hwParam0;
    case GI2C_GENI_STATUS: return active ? 1u : 0u;
    case GI2C_IOS: return 3;
    case GI2C_M_GP_LENGTH: return gpLen;
    case GI2C_M_IRQ_STATUS: {
        tick();
        // A noise bit appears once per message without CMD_FAILURE.
        uint32_t v = irq;
        if (noiseBits && active) v |= noiseBits;
        return v;
    }
    case GI2C_TX_FIFO_STATUS: return (uint32_t)tx.size();
    case GI2C_RX_FIFO_STATUS: return (uint32_t)rx.size() | (!active && !rx.empty() ? 0x80000000u : 0);
    case GI2C_RX_FIFOn: {
        assert(!rx.empty());   // never read an empty FIFO
        const uint32_t v = rx.front();
        rx.pop_front();
        return v;
    }
    default: return regs[offset / 4];
    }
}

void FakeGeniI2c::write(uint32_t offset, uint32_t value) {
    assert(offset < 0x1000 && !(offset & 3));
    switch (offset) {
    case GI2C_M_IRQ_CLEAR: irq &= ~value; return;
    case GI2C_M_CMD0: regs[offset / 4] = value; startCommand(value); return;
    case GI2C_TX_FIFOn:
        assert(tx.size() < 16);   // never overfill the FIFO
        tx.push_back(value);
        if (tx.size() > maxTxWords) maxTxWords = tx.size();
        return;
    case GI2C_M_CMD_CTRL:
        if (value & GI2C_CMD_CANCEL) {
            ++cancels;
            if (!cancelIgnored) {irq |= GI2C_M_CMD_CANCEL; active = false; tx.clear(); inTransaction = false;}
        }
        if (value & GI2C_CMD_ABORT) {++aborts; irq |= GI2C_M_CMD_ABORT; active = false; tx.clear(); inTransaction = false;}
        return;
    default: regs[offset / 4] = value; return;
    }
}
