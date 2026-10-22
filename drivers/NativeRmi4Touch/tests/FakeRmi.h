// SPDX-License-Identifier: BSD-2-Clause-Patent
// A model of an RMI4 touch controller's register space for host tests: packet registers (one address, any number of bytes) that
// a block read streams through in address order, the page select register 0xff, F01 interrupt status cleared on read, and the
// F01 reset command. Two layouts: a genuine-style S3706B with register descriptors, and a clone that leaves the PDT function
// bytes empty, has no F12 descriptors and refuses reads that touch the unimplemented PDT bytes. Not hardware.
#ifndef FAKE_RMI_H
#define FAKE_RMI_H
#include "../Rmi4.h"
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

struct FakeRmi {
    std::map<uint16_t, std::vector<uint8_t>> regs;   // page-qualified address -> packet bytes
    int page = 0;
    bool clone = false;
    bool failNext = false;          // the next transfer fails (bus error)
    unsigned pageWrites = 0, reads = 0, writes = 0, resets = 0;
    uint16_t f01Data = 0x04, f01Ctrl = 0x12, f01Cmd = 0x2f, f01Query = 0x30;
    uint16_t f12Data = 0x06, f12Ctrl = 0x14, f12Query = 0x20;
    std::vector<uint16_t> readLog;  // page-qualified start address of every read

    void set(uint16_t addr, std::vector<uint8_t> bytes) {regs[addr] = std::move(bytes);}
    std::vector<uint8_t> &at(uint16_t addr) {auto &v = regs[addr]; if (v.empty()) v.resize(1); return v;}

    // Data1 object slot: type, x, y, z, wx, wy (F12 data1 is the packet at f12Data + data1 index).
    uint16_t data1Addr() const {return clone ? f12Data : (uint16_t)(f12Data + 1);}
    void object(unsigned slot, uint8_t type, uint16_t x, uint16_t y, uint8_t z = 40) {
        auto &d = at(data1Addr());
        if (d.size() < 80) d.resize(80);
        uint8_t *p = &d[slot * 8];
        p[0] = type; p[1] = (uint8_t)x; p[2] = (uint8_t)(x >> 8); p[3] = (uint8_t)y; p[4] = (uint8_t)(y >> 8); p[5] = z; p[6] = 3; p[7] = 4;
    }
    void clearObjects() {auto &d = at(data1Addr()); std::fill(d.begin(), d.end(), 0);}
    void raise(uint8_t irqBits) {at(f01Data + 1)[0] |= irqBits;}

    void genuine() {
        clone = false;
        regs.clear();
        // PDT, page 0, from 0xe9 down: {query, command, control, data, irq count | version << 5, function}.
        set(0xe9, {0x50}); set(0xea, {0x4f}); set(0xeb, {0x4e}); set(0xec, {0x00}); set(0xed, {0x41}); set(0xee, {0x34});
        set(0xe3, {(uint8_t)f01Query}); set(0xe4, {(uint8_t)f01Cmd}); set(0xe5, {(uint8_t)f01Ctrl}); set(0xe6, {(uint8_t)f01Data});
        set(0xe7, {0x01}); set(0xe8, {0x01});
        set(0xdd, {(uint8_t)f12Query}); set(0xde, {0x00}); set(0xdf, {(uint8_t)f12Ctrl}); set(0xe0, {(uint8_t)f12Data});
        set(0xe1, {0x02}); set(0xe2, {0x12});
        set(0xd7, {0}); set(0xd8, {0}); set(0xd9, {0}); set(0xda, {0}); set(0xdb, {0}); set(0xdc, {0});   // end of table
        // F01: query 0 manufacturer, query 1 properties, product id at query 11; data: status, interrupt status; control 0, 1.
        set(f01Query, {0x01}); set(f01Query + 1, {0x07});
        const char *pid = "s3706";
        for (unsigned i = 0; i < 10; i++) set((uint16_t)(f01Query + 11 + i), {(uint8_t)(i < 5 ? pid[i] : 0)});
        set(f01Data, {0x80}); set(f01Data + 1, {0x00});   // unconfigured after power-on
        set(f01Ctrl, {0x01}); set(f01Ctrl + 1, {0x00});   // sensor sleep, no interrupts enabled
        // F12 query 0: has register descriptors. Query descriptor (query + 1..3): one register.
        set(f12Query, {0x01});
        set(f12Query + 1, {2}); set(f12Query + 2, {2, 0x01}); set(f12Query + 3, {1, 0x01});
        // Control descriptor (query + 4..6): control 8 (12 bytes, subpackets 0..2) and control 20 (3 bytes).
        set(f12Query + 4, {4}); set(f12Query + 5, {4, 0x00, 0x01, 0x10}); set(f12Query + 6, {12, 0x07, 3, 0x01});
        // Data descriptor (query + 7..9): data 0 (1 byte), data 1 (80 bytes, ten objects), data 5 (8), data 15 (2).
        set(f12Query + 7, {3}); set(f12Query + 8, {9, 0x23, 0x80});
        set(f12Query + 9, {1, 0x01, 80, 0xff, 0x07, 8, 0x01, 2, 0x01});
        // Control 8: max x 1079, max y 2339, pitch, border; control 20.
        set(f12Ctrl, {0x37, 0x04, 0x23, 0x09, 0, 0, 0, 0, 0, 0, 0, 0}); set(f12Ctrl + 1, {0, 0, 0});
        set(f12Data, {0xaa}); set(f12Data + 1, std::vector<uint8_t>(80, 0)); set(f12Data + 2, std::vector<uint8_t>(8, 0));
        set(f12Data + 3, {0, 0});
    }

    void cloneLayout() {
        genuine();
        clone = true;
        // The function and interrupt bytes of every PDT entry are zero; F12 query 0 says "no register descriptors".
        for (uint16_t e : {0xe9, 0xe3, 0xdd}) {set((uint16_t)(e + 4), {0}); set((uint16_t)(e + 5), {0});}
        set(f12Query, {0x00});
        regs.erase(f12Data + 1); regs.erase(f12Data + 2); regs.erase(f12Data + 3);
        set(f12Data, std::vector<uint8_t>(88, 0));   // the 88-byte packet: ten objects, then 8 more bytes
    }

    // Bytes of a block read from addr: each packet register in turn.
    int read(uint8_t reg, uint8_t *buf, size_t len) {
        ++reads;
        if (failNext) {failNext = false; return -5;}
        const uint16_t start = (uint16_t)(page << 8 | reg);
        readLog.push_back(start);
        if (clone && page == 0) {
            // Unaligned reads into the unimplemented PDT bytes are refused (only the 6-byte read of the first entry works).
            for (uint16_t e : {0xe3, 0xdd}) if (start <= e + 4 && start + len > (size_t)e + 4) return -6;
        }
        size_t n = 0;
        uint16_t addr = start;
        while (n < len) {
            auto it = regs.find(addr);
            if (it == regs.end()) {buf[n++] = 0; addr++; continue;}
            for (size_t k = 0; k < it->second.size() && n < len; k++) buf[n++] = it->second[k];
            if (addr == f01Data + 1 && page == 0) std::fill(it->second.begin(), it->second.end(), 0);   // cleared on read
            addr++;
        }
        return 0;
    }
    int write(uint8_t reg, const uint8_t *buf, size_t len) {
        ++writes;
        if (failNext) {failNext = false; return -5;}
        if (reg == RMI_PAGE_SELECT) {page = buf[0]; ++pageWrites; return 0;}
        const uint16_t addr = (uint16_t)(page << 8 | reg);
        if (addr == f01Cmd && (buf[0] & 1)) {   // F01 reset
            ++resets;
            at(f01Data)[0] = 0x80; at(f01Ctrl)[0] = 0x00; at(f01Ctrl + 1)[0] = 0; page = 0;
            return 0;
        }
        auto &v = at(addr);
        if (v.size() < len) v.resize(len);
        std::memcpy(v.data(), buf, len);
        return 0;
    }
};
#endif
