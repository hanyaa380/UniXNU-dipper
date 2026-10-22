// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test: Rmi4.c (PDT, register descriptors, F01, F12) against FakeRmi, a register model of a genuine-style S3706B and of a
// clone controller. Build: c++ -std=c++17 rmi4-test.cpp ../Rmi4.c (scripts/check-fajita-touch-host.py does it).
#include "FakeRmi.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static int busRead(void *ctx, uint8_t reg, uint8_t *buf, size_t len) {return static_cast<FakeRmi *>(ctx)->read(reg, buf, len);}
static int busWrite(void *ctx, uint8_t reg, const uint8_t *buf, size_t len) {return static_cast<FakeRmi *>(ctx)->write(reg, buf, len);}
static unsigned slept;
static void busDelay(void *, unsigned ms) {slept += ms;}

static void open(FakeRmi &m, rmi_dev &d) {
    const rmi_bus bus = {&m, busRead, busWrite, busDelay};
    rmi_init(&d, &bus);
}

static void testDescriptorParse() {
    rmi_reg_desc desc;
    // One-byte structure size; registers 0, 1, 5, 15.
    const uint8_t p1[] = {9, 0x23, 0x80};
    const uint8_t s1[] = {1, 0x01, 80, 0xff, 0x07, 8, 0x01, 2, 0x01};
    assert(rmi_parse_reg_desc(p1, sizeof p1, s1, sizeof s1, &desc) == 0);
    assert(desc.count == 4 && desc.struct_size == 9);
    const rmi_reg_item *d1 = rmi_desc_item(&desc, 1);
    assert(d1 && d1->size == 80 && d1->num_subpackets == 10 && d1->subpackets == 0x3ff && d1->index == 1 && d1->offset == 1);
    const rmi_reg_item *d15 = rmi_desc_item(&desc, 15);
    assert(d15 && d15->index == 3 && d15->offset == 89 && d15->size == 2);
    assert(!rmi_desc_item(&desc, 2));
    // Two-byte structure size (presence byte 0 = 0), a 2-byte register size and a 4-byte one, a zero-size register skipped
    // (it takes no address), register numbers above 7.
    const uint8_t p2[] = {0, 13, 0, 0x0b, 0x01};   // registers 0, 1, 3 and 8
    const uint8_t s2[] = {0, 0x2c, 0x01, 0x01,     // reg 0: size 0 -> le16 300
                          0, 0, 0, 0x10, 0, 0, 0, 0x01,   // reg 1: size 0 -> 0 -> le32 0x10
                          0, 0, 0};                // reg 3: size 0, le16 0, then the le32 is cut off
    assert(rmi_parse_reg_desc(p2, sizeof p2, s2, sizeof s2, &desc) == RMI_ERR_IO);   // the structure ends inside a size
    const uint8_t s3[] = {0, 0x2c, 0x01, 0x01, 0, 0, 0, 0x10, 0, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0, 0x00, 5, 0x83, 0x01};
    const uint8_t p3[] = {0, (uint8_t)sizeof s3, 0, 0x0b, 0x01};
    assert(rmi_parse_reg_desc(p3, sizeof p3, s3, sizeof s3, &desc) == 0);
    assert(desc.count == 3);                      // register 3 had size 0 and is dropped
    assert(desc.items[0].reg == 0 && desc.items[0].size == 300 && desc.items[0].index == 0);
    assert(desc.items[1].reg == 1 && desc.items[1].size == 0x10 && desc.items[1].index == 1 && desc.items[1].offset == 300);
    assert(desc.items[2].reg == 8 && desc.items[2].size == 5 && desc.items[2].index == 2);
    // Subpacket bits continue over bytes: 0x83 = bits 0,1 and more, 0x01 = bit 7.
    assert(desc.items[2].subpackets == ((1ull << 0) | (1ull << 1) | (1ull << 7)) && desc.items[2].num_subpackets == 3);
    // Bad presence lengths.
    assert(rmi_parse_reg_desc(p1, 0, s1, sizeof s1, &desc) == RMI_ERR_IO);
    uint8_t big[36] = {1};
    assert(rmi_parse_reg_desc(big, sizeof big, s1, sizeof s1, &desc) == RMI_ERR_IO);
    const uint8_t p4[] = {0, 9};
    assert(rmi_parse_reg_desc(p4, sizeof p4, s1, sizeof s1, &desc) == RMI_ERR_IO);
    // An item whose subpacket bytes run past the structure.
    const uint8_t s5[] = {4, 0x80};
    const uint8_t p5[] = {2, 0x01};
    assert(rmi_parse_reg_desc(p5, sizeof p5, s5, sizeof s5, &desc) == RMI_ERR_IO);
}

static void testPdtEntry() {
    rmi_function f;
    const uint8_t e[6] = {0x20, 0x00, 0x14, 0x06, 0x42, 0x12};
    assert(rmi_parse_pdt_entry(e, 0x01dd, &f) == 1);
    assert(f.query == 0x0120 && f.control == 0x0114 && f.data == 0x0106 && f.number == 0x12 && f.irq_count == 2 && f.version == 2);
    const uint8_t end0[6] = {1, 2, 3, 4, 5, 0x00}, endff[6] = {1, 2, 3, 4, 5, 0xff};
    assert(rmi_parse_pdt_entry(end0, 0xe9, &f) == 0 && rmi_parse_pdt_entry(endff, 0xe9, &f) == 0);
}

static void testGenuine() {
    FakeRmi m; m.genuine();
    rmi_dev d; open(m, d);
    assert(rmi_scan_pdt(&d) == 0);
    assert(d.pdt_mode == RMI_PDT_SCANNED && d.nfn == 3 && d.irq_count == 4 && d.irq_regs == 1);
    assert(d.fn[d.f01].data == 0x04 && d.fn[d.f01].control == 0x12 && d.fn[d.f01].irq_pos == 1);
    assert(d.fn[d.f12].data == 0x06 && d.fn[d.f12].query == 0x20 && d.fn[d.f12].irq_pos == 2 && d.abs_mask == 0x04);
    assert(m.pageWrites == 1);                  // page 0 selected once, never again
    assert(rmi_f01_read_info(&d) == 0);
    assert(!std::strcmp(d.product_id, "s3706") && d.manufacturer == 0x01 && (d.status & RMI_F01_STATUS_UNCONFIGURED));
    assert(rmi_f01_configure(&d, 1) == 0);
    assert(d.ctrl0_before == 0x01 && d.ctrl0 == 0x84);                  // sleep mode normal, NoSleep, Configured
    assert(m.at(0x12)[0] == 0x84 && m.at(0x13)[0] == 0x06);              // interrupts: F12 absolute and F01 status
    assert(rmi_f12_probe(&d) == 0);
    assert(!d.f12_hardcoded && d.objects == 10 && d.data1_size == 80 && d.data1_offset == 1 && d.read_size == 81);
    assert(d.max_x == 1079 && d.max_y == 2339);
    // A frame: two fingers, a palm (not delivered) and a gloved finger.
    m.object(0, RMI_F12_OBJ_FINGER, 100, 200);
    m.object(3, RMI_F12_OBJ_PALM, 500, 500);
    m.object(5, RMI_F12_OBJ_FINGER, 1000, 2300, 77);
    m.object(9, RMI_F12_OBJ_GLOVED, 1079, 2339);
    m.raise(0x04);
    uint8_t status; uint32_t irq;
    assert(rmi_read_irq(&d, &status, &irq) == 0 && irq == 0x04 && (status & 0x80));
    assert(rmi_read_irq(&d, &status, &irq) == 0 && irq == 0);           // cleared on read
    rmi_contact c[10];
    int n = rmi_f12_read_contacts(&d, c, 10);
    assert(n == 3);
    assert(c[0].id == 0 && c[0].x == 100 && c[0].y == 200 && c[0].type == RMI_F12_OBJ_FINGER);
    assert(c[1].id == 5 && c[1].x == 1000 && c[1].y == 2300 && c[1].z == 77 && c[1].wx == 3 && c[1].wy == 4);
    assert(c[2].id == 9 && c[2].type == RMI_F12_OBJ_GLOVED);
    assert(rmi_f12_read_contacts(&d, c, 2) == 2);                        // the caller's limit
    m.clearObjects();
    assert(rmi_f12_read_contacts(&d, c, 10) == 0);                       // all lifted
    // The read covers data 0 and data 1 from the data base: one read of 81 bytes at 0x06.
    assert(m.readLog.back() == 0x06);
    // Reset: the command register, then the wait.
    slept = 0;
    assert(rmi_f01_reset(&d, 200) == 0 && m.resets == 1 && slept == 200);
    assert(rmi_f01_read_info(&d) == 0 && (d.status & RMI_F01_STATUS_UNCONFIGURED));
    // Bootloader mode is reported.
    m.at(0x04)[0] = 0x40;
    assert(rmi_f01_read_info(&d) == RMI_ERR_BOOTLOADER);
}

static void testClone() {
    FakeRmi m; m.cloneLayout();
    rmi_dev d; open(m, d);
    assert(rmi_scan_pdt(&d) == 0);
    assert(d.pdt_mode == RMI_PDT_FALLBACK && d.nfn == 3);
    assert(d.fn[d.f01].data == 0x04 && d.fn[d.f12].data == 0x06 && d.fn[d.f12].irq_pos == 2 && d.abs_mask == 0x04);
    assert(rmi_f01_read_info(&d) == 0);
    assert(rmi_f01_configure(&d, 1) == 0);
    assert(rmi_f12_probe(&d) == 0);
    assert(d.f12_hardcoded && d.objects == 10 && d.data1_offset == 0 && d.data1_size == 80 && d.read_size == 88);
    assert(d.max_x == 0 && d.max_y == 0);
    m.object(2, RMI_F12_OBJ_FINGER, 540, 1170);
    rmi_contact c[10];
    assert(rmi_f12_read_contacts(&d, c, 10) == 1 && c[0].id == 2 && c[0].x == 540 && c[0].y == 1170);
    // Forcing the fixed table works on a genuine controller as well.
    FakeRmi g; g.genuine();
    rmi_dev e; open(g, e);
    e.force_fallback = 1;
    assert(rmi_scan_pdt(&e) == 0 && e.pdt_mode == RMI_PDT_FALLBACK && e.fn[e.f12].data == 0x06);
}

static void testPagingAndErrors() {
    FakeRmi m; m.genuine();
    rmi_dev d; open(m, d);
    assert(rmi_scan_pdt(&d) == 0 && m.pageWrites == 1);
    uint8_t b;
    m.set(0x0110, {0x5a});
    assert(rmi_read(&d, 0x0110, &b, 1) == 0 && b == 0x5a && m.pageWrites == 2 && m.page == 1);
    assert(rmi_read(&d, 0x0110, &b, 1) == 0 && m.pageWrites == 2);
    assert(rmi_read(&d, 0x0004, &b, 1) == 0 && m.pageWrites == 3 && m.page == 0);
    // After a failed transfer the page is selected again.
    m.failNext = true;
    assert(rmi_read(&d, 0x0004, &b, 1) == -5 && d.last_err == -5);
    assert(rmi_read(&d, 0x0004, &b, 1) == 0 && m.pageWrites == 4);
    // A controller without F12.
    FakeRmi n; n.genuine();
    n.set(0xe2, {0x11});
    rmi_dev e; open(n, e);
    assert(rmi_scan_pdt(&e) == RMI_ERR_NODEV);
    // Unknown functions are skipped but still count interrupt sources before F12.
    FakeRmi u; u.genuine();
    u.set(0xee, {0x51});   // the first entry becomes an OEM function with one interrupt source
    rmi_dev f; open(u, f);
    assert(rmi_scan_pdt(&f) == 0 && f.nfn == 2 && f.fn[f.f12].irq_pos == 2 && f.abs_mask == 0x04);
    // Decode limits: no more objects than the packet holds.
    uint8_t data1[16] = {1, 1, 0, 2, 0, 9, 9, 9, 1, 3, 0, 4, 0, 9, 9, 9};
    rmi_contact c[4];
    assert(rmi_f12_decode(data1, sizeof data1, 10, c, 4) == 2);
    assert(rmi_f12_decode(data1, 15, 10, c, 4) == 1);
    assert(rmi_read(&d, 0, nullptr, 1) == RMI_ERR_PARAM && rmi_read(&d, 0, &b, 0) == RMI_ERR_PARAM);
}

int main() {
    testDescriptorParse();
    testPdtEntry();
    testGenuine();
    testClone();
    testPagingAndErrors();
    std::puts("PASS: RMI4 PDT, register descriptors, F01 and F12 against the controller model");
    return 0;
}
