// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register model of a GENI serial engine with I2C firmware in FIFO mode, and the I2C slave behind it, for host tests. It models the
// behaviour the Linux i2c-qcom-geni driver relies on: TRANS_LEN plus M_CMD0 start a message; TX words are consumed, and the TX
// watermark event raised while the FIFO is at or below TX_WATERMARK; RX words are produced into a 16-word FIFO with the RX
// watermark and RX_LAST events; CMD_DONE ends every message; a NACK raises GP_IRQ1 with CMD_FAILURE and CMD_DONE and records the
// bytes moved in M_GP_LENGTH; CANCEL and ABORT confirm with their own status bits. It is not hardware.
#ifndef FAKE_GENI_I2C_H
#define FAKE_GENI_I2C_H
#include "../GeniI2c.h"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

struct I2cSlave {
    virtual ~I2cSlave() {}
    virtual bool ack(uint8_t addr) = 0;                    // the address phase
    virtual void start(bool read, size_t len) = 0;         // after an acknowledged address; len = TRANS_LEN
    virtual bool writeByte(uint8_t value, size_t index) = 0;   // false: NACK
    virtual uint8_t readByte(size_t index) = 0;
    virtual void stop() = 0;
};

// A plain register-pointer slave: the first written byte sets the pointer, later bytes store and reads load from it.
struct RegSlave : I2cSlave {
    uint8_t addr = 0x20;
    bool present = true;
    int nackAfter = -1;          // index of the first written byte that is not acknowledged (-1: none)
    uint8_t mem[256] = {};
    uint8_t ptr = 0;
    bool ack(uint8_t a) override {return present && a == addr;}
    void start(bool, size_t) override {}
    bool writeByte(uint8_t v, size_t i) override {
        if (nackAfter >= 0 && i >= (size_t)nackAfter) return false;
        if (i == 0) ptr = v; else mem[ptr++] = v;
        return true;
    }
    uint8_t readByte(size_t) override {return mem[ptr++];}
    void stop() override {}
};

struct FakeGeniI2c {
    uint32_t regs[0x1000 / 4] = {};
    uint32_t fwRevision = 0x0300 | 0x27;          // protocol 3 (I2C)
    uint32_t ifDisable = 0;
    uint32_t hwParam0 = (32u << 24) | (16u << 16);  // 32-bit FIFO words, depth 16
    uint32_t wrapperVersion = 0x20000000u;        // QUP 2.0
    RegSlave slave;
    I2cSlave *device = nullptr;                   // null: the built-in RegSlave
    // Engine state.
    uint32_t irq = 0, gpLen = 0;
    bool active = false, inTransaction = false, readOp = false, stopStretch = false;
    size_t len = 0, moved = 0;
    std::deque<uint32_t> tx, rx;
    uint32_t partial = 0; unsigned partialBytes = 0;
    // Fault injection.
    bool stuck = false, cancelIgnored = false;
    uint32_t injectGp = 0, noiseBits = 0;
    unsigned leftoverWords = 0;
    // Observations.
    std::string bus;
    unsigned commands = 0, cancels = 0, aborts = 0, stopStretchWrites = 0;
    unsigned txWatermarkEvents = 0, rxWatermarkEvents = 0;
    size_t maxTxWords = 0;

    void attach(geni_i2c &g);
    uint32_t reg(uint32_t offset) const {return regs[offset / 4];}
    size_t rxWords() const {return rx.size();}
    uint32_t read(uint32_t offset);
    void write(uint32_t offset, uint32_t value);
private:
    I2cSlave &dev() {return device ? *device : slave;}
    void startCommand(uint32_t cmd);
    void tick();
    void finish(bool stop);
};
#endif
