// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the whole touch read path without hardware: Rmi4.c over the board's RMI-over-I2C link (FajitaTouchBoard.h) over
// GeniI2c.c over the GENI register model (FakeGeniI2c), with the RMI controller model (FakeRmi) as the I2C slave at 0x20.
// It checks the bytes on the wire for the RMI register protocol (address write, repeated START, read; page select as a plain
// write) and that a genuine and a clone controller both come up and report contacts through the board's coordinate mapping.
#include "../FajitaTouchBoard.h"
#include "../../drivers/NativeGeniI2c/tests/FakeGeniI2c.h"
#include "../../drivers/NativeRmi4Touch/tests/FakeRmi.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// FakeRmi behind an I2C address: the first written byte is the register, further bytes are written to it when the message ends
// (STOP or a repeated START); a read streams from the register.
struct RmiSlave : I2cSlave {
    FakeRmi &rmi;
    bool present = true;
    uint8_t reg = 0;
    std::vector<uint8_t> pending, readBuf;
    bool writing = false;
    explicit RmiSlave(FakeRmi &r) : rmi(r) {}
    void commit() {
        if (writing && !pending.empty()) rmi.write(reg, pending.data(), pending.size());
        pending.clear();
        writing = false;
    }
    bool ack(uint8_t a) override {return present && a == FTB_I2C_ADDR;}
    void start(bool read, size_t len) override {
        commit();
        if (read) {
            readBuf.assign(len, 0);
            if (rmi.read(reg, readBuf.data(), len)) std::fill(readBuf.begin(), readBuf.end(), 0xff);
        } else writing = true;
    }
    bool writeByte(uint8_t v, size_t i) override {
        if (i == 0) reg = v; else pending.push_back(v);
        return true;
    }
    uint8_t readByte(size_t i) override {return readBuf[i];}
    void stop() override {commit();}
};

struct Rig {
    FakeGeniI2c engine;
    FakeRmi rmi;
    RmiSlave slave{rmi};
    geni_i2c i2c{};
    ftb_rmi_link link{};
    rmi_dev dev{};
    explicit Rig(bool clone) {
        if (clone) rmi.cloneLayout(); else rmi.genuine();
        engine.device = &slave;
        engine.attach(i2c);
        assert(geni_i2c_init(&i2c) == 0);
        assert(geni_i2c_set_clock(&i2c, 0, FTB_TCXO_HZ, FTB_I2C_HZ) == 0);
        link = ftb_rmi_link{&i2c, FTB_I2C_ADDR, 0};
        const rmi_bus bus = {&link, ftb_rmi_read, ftb_rmi_write, nullptr};
        rmi_init(&dev, &bus);
    }
};

static const ftb_view kPanel = {0, 0, 0, 0, 0, 0, 0};   // no logical display: the panel itself

static void testGenuineChain() {
    Rig r(false);
    assert(rmi_scan_pdt(&r.dev) == 0);
    // The first bytes on the wire: page 0 selected (write ff 00), then the first PDT entry read with a repeated START.
    assert(r.engine.bus.rfind("S40 ff 00 P S40 e9 S41 r6 P", 0) == 0);
    assert(rmi_f01_read_info(&r.dev) == 0 && std::string(r.dev.product_id) == "s3706");
    assert(rmi_f01_configure(&r.dev, 1) == 0 && r.rmi.at(0x12)[0] == 0x84 && r.rmi.at(0x13)[0] == 0x06);
    assert(rmi_f12_probe(&r.dev) == 0 && r.dev.max_x == 1079 && r.dev.max_y == 2339 && r.dev.read_size == 81);
    r.rmi.object(1, RMI_F12_OBJ_FINGER, 0, 0);
    r.rmi.object(4, RMI_F12_OBJ_FINGER, 1079, 2339);
    r.rmi.raise(0x04);
    uint8_t st; uint32_t irq;
    assert(rmi_read_irq(&r.dev, &st, &irq) == 0 && (irq & r.dev.abs_mask));
    r.engine.bus.clear();
    rmi_contact c[RMI_MAX_OBJECTS];
    assert(rmi_f12_read_contacts(&r.dev, c, RMI_MAX_OBJECTS) == 2);
    assert(r.engine.bus == "S40 06 S41 r81 P");   // one 81-byte read of data 0 and data 1
    const ftb_map map = {r.dev.max_x, r.dev.max_y, 0};
    int x, y;
    ftb_map_point(&map, &kPanel, c[0].x, c[0].y, &x, &y); assert(x == 0 && y == 0 && c[0].id == 1);
    ftb_map_point(&map, &kPanel, c[1].x, c[1].y, &x, &y); assert(x == FTB_PUBLISH_X - 1 && y == FTB_PUBLISH_Y - 1 && c[1].id == 4);
    assert(r.i2c.nacks == 0 && r.i2c.errors == 0 && r.i2c.timeouts == 0);
}

static void testCloneChain() {
    Rig r(true);
    assert(rmi_scan_pdt(&r.dev) == 0 && r.dev.pdt_mode == RMI_PDT_FALLBACK);
    assert(rmi_f01_read_info(&r.dev) == 0 && rmi_f01_configure(&r.dev, 1) == 0);
    assert(rmi_f12_probe(&r.dev) == 0 && r.dev.f12_hardcoded && r.dev.read_size == 88);
    r.rmi.object(0, RMI_F12_OBJ_FINGER, 540, 1170);
    rmi_contact c[RMI_MAX_OBJECTS];
    assert(rmi_f12_read_contacts(&r.dev, c, RMI_MAX_OBJECTS) == 1);
    const ftb_map map = {r.dev.max_x, r.dev.max_y, 0};   // no maxima reported: the 1080 x 2340 default
    int x, y;
    ftb_map_point(&map, &kPanel, c[0].x, c[0].y, &x, &y);
    const unsigned px = 540u * (FTB_PANEL_W - 1) / 1080, py = 1170u * (FTB_PANEL_H - 1) / 2340;   // panel pixel first
    assert(x == (int)(px * (FTB_PUBLISH_X - 1ull) / (FTB_PANEL_W - 1)) && y == (int)(py * (FTB_PUBLISH_Y - 1ull) / (FTB_PANEL_H - 1)));
}

static void testAbsentController() {
    Rig r(false);
    r.slave.present = false;   // VDD (LDO28) off: nothing acknowledges 0x20
    assert(rmi_scan_pdt(&r.dev) == GI2C_ERR_ADDR_NACK);
    assert(r.i2c.nacks == 1 && r.engine.cancels == 0);
    // An address-only probe the way the stage 2 probe does it: the PDT pointer write, with STOP.
    const uint8_t reg = RMI_PDT_START;
    assert(geni_i2c_write(&r.i2c, FTB_I2C_ADDR, &reg, 1, 1) == GI2C_ERR_ADDR_NACK);
    r.slave.present = true;
    assert(geni_i2c_write(&r.i2c, FTB_I2C_ADDR, &reg, 1, 1) == 0);
    assert(rmi_scan_pdt(&r.dev) == 0);
}

int main() {
    testGenuineChain();
    testCloneChain();
    testAbsentController();
    std::puts("PASS: RMI4 over GENI I2C register path (genuine, clone, absent controller)");
    return 0;
}
