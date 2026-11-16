/* SPDX-License-Identifier: MIT */
#include "usb/Dwc3.h"
#include "usb/Dwc3Regs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A register/DMA model supplies completion events rather than a real PHY.
 * These tests check USB-visible descriptors and host request sequencing. */
_Alignas(4096) static uint8_t dma[DWC3_DMA_BYTES];
_Alignas(8) static uint8_t context[20480];
static uint32_t regs[0xd000 / 4], producer;
static unsigned syncs, from_syncs, delays, writes, last_in_fifo;
static void *last_from_pointer;
static size_t last_from_bytes;
static int timeout_commands;
static const uint64_t dma_bus = UINT64_C(0x123400000);
static uint32_t read32(void *c, uint32_t reg) { (void)c; assert(reg < sizeof(regs)); return regs[reg / 4]; }
static void write32(void *c, uint32_t reg, uint32_t value)
{
    (void)c; writes++;
    assert(reg < sizeof(regs));
    if (reg == DWC3_GEVNTCOUNT(0)) { regs[reg / 4] -= value; return; }
    regs[reg / 4] = value;
    if (reg == DWC3_DCTL) {
        regs[reg / 4] &= ~DWC3_DCTL_CSFTRST;
        /* Model the DWC3 core soft reset clearing its event DMA offset. This
         * is not a USB bus/protocol reset and does not change core behavior. */
        if (value & DWC3_DCTL_CSFTRST) producer = 0;
        if (value & DWC3_DCTL_RUN_STOP) regs[DWC3_DSTS / 4] &= ~DWC3_DSTS_DEVCTRLHLT;
        else regs[DWC3_DSTS / 4] |= DWC3_DSTS_DEVCTRLHLT;
    }
    if (reg >= DWC3_DEPCMD(0) && reg <= DWC3_DEPCMD(5) && (reg & 15) == 12) {
        assert((regs[DWC3_GUSB2PHYCFG(0) / 4] & ((1u << 6) | (1u << 8))) == 0);
        if (timeout_commands) return;
        unsigned ep = (reg - DWC3_DEPCMD(0)) / 16;
        unsigned cmd = value & 15;
        regs[reg / 4] = cmd;
        if (cmd == DWC3_DEPCMD_STARTTRANSFER) {
            uint64_t trb_bus = (uint64_t)regs[DWC3_DEPCMDPAR0(ep) / 4] << 32 |
                              regs[DWC3_DEPCMDPAR1(ep) / 4];
            assert(trb_bus == dma_bus + 4096 + ep * 64);
            const struct dwc3_trb *t = (void *)(dma + 4096 + ep * 64);
            uint64_t data_bus = (uint64_t)t->bph << 32 | t->bpl;
            assert(data_bus == dma_bus + 8192 + ep * 4096);
            regs[reg / 4] |= 1u << 16;
        }
        if (cmd == DWC3_DEPCMD_SETEPCONFIG && ep == 5)
            last_in_fifo = (regs[DWC3_DEPCMDPAR0(ep) / 4] >> 17) & 15;
    }
}
static void delay(void *c, uint32_t n) { (void)c; delays += n; }
static void dma_sync(void *c, void *p, size_t n, enum dwc3_sync_direction dir)
{
    uint8_t *base = c ? c : dma;
    assert((uint8_t *)p >= base && (uint8_t *)p + n <= base + sizeof(dma)); syncs++;
    if (dir == DWC3_FROM_DEVICE) {
        from_syncs++; last_from_pointer = p; last_from_bytes = n;
    }
}
static void barrier(void *c) { (void)c; }
static const struct dwc3_platform platform = {0, read32, write32, delay, dma_sync, barrier, 0};
static struct dwc3 *dev = (void *)context;
static const struct dwc3_identity identity = {0x1209, 0x316d, "Core test", "USB debug", "GenericDwc3"};
static struct dwc3_trb *eptrb(unsigned ep) { return (void *)(dma + 4096 + ep * 64); }
static void event(uint32_t e)
{
    memcpy(dma + producer, &e, 4); producer = (producer + 4) % 4096;
    regs[DWC3_GEVNTCOUNT(0) / 4] += 4;
}
static void complete(unsigned ep, unsigned residual)
{
    eptrb(ep)->size = residual; eptrb(ep)->ctrl &= ~DWC3_TRB_CTRL_HWO;
    event((ep << 1) | (DWC3_DEPEVT_XFERCOMPLETE << 6));
    assert(dwc3_poll(dev, 32) >= 1);
}
static void not_ready(unsigned ep, unsigned phase)
{
    event((ep << 1) | (DWC3_DEPEVT_XFERNOTREADY << 6) | (phase << 12));
    assert(dwc3_poll(dev, 32) >= 1);
}
static void setup(uint8_t type, uint8_t request, uint16_t val, uint16_t index, uint16_t len)
{
    uint8_t packet[8] = {type, request, val & 255, val >> 8, index & 255, index >> 8, len & 255, len >> 8};
    memcpy(dma + 8192, packet, 8); complete(0, 0);
}
static void status_in(unsigned trbtype)
{
    not_ready(1, DEPEVT_STATUS_CONTROL_STATUS);
    assert((eptrb(1)->ctrl & (63u << 4)) == trbtype); complete(1, 0);
}
static void descriptor_tests(void)
{
    size_t n, offset;
    const uint8_t *p = dwc3_descriptor(dev, 2, 0, 0, &n);
    assert(n == 67 && p[2] == 67 && p[4] == 2);
    unsigned interfaces = 0, endpoints = 0, unions = 0, acms = 0;
    for (offset = 0; offset < n; offset += p[offset]) {
        assert(p[offset] >= 2 && offset + p[offset] <= n);
        if (p[offset + 1] == 4) interfaces++;
        if (p[offset + 1] == 5) {
            endpoints++;
            if (p[offset + 2] == 2 || p[offset + 2] == 0x82)
                assert(p[offset + 4] == 0 && p[offset + 5] == 2);
        }
        if (p[offset + 1] == 0x24 && p[offset + 2] == 6) {
            unions++; assert(p[offset + 3] == 0 && p[offset + 4] == 1);
        }
        if (p[offset + 1] == 0x24 && p[offset + 2] == 2) acms++;
    }
    assert(offset == n && interfaces == 2 && endpoints == 3 && unions == 1 && acms == 1);
    p = dwc3_descriptor(dev, 3, 3, 0, &n);
    assert(n == 2 + strlen(identity.serial) * 2 && p[0] == n);
    for (offset = 0; offset < strlen(identity.serial); offset++) {
        assert(p[2 + offset * 2] == identity.serial[offset] && p[3 + offset * 2] == 0);
    }
    for (unsigned s = 0; s < 4; s++) {
        p = dwc3_descriptor(dev, 3, s, 0, &n); assert(p && p[0] == n);
    }
    assert(!dwc3_descriptor(dev, 3, 99, 0, &n) && n == 0);
    puts("PASS: one CDC ACM function, functional descriptors, endpoint sizes and exact serial");
}
int main(void)
{
    assert(sizeof(context) == 20480);
    assert(dwc3_context_bytes() < sizeof(context));
    assert(dwc3_context_bytes() <= sizeof(context));
    assert(dwc3_init(context, sizeof(context), &platform, &identity, dma + 1, dma_bus, sizeof(dma)) == -1);
    {
        struct dwc3_identity invalid = identity;
        size_t length = 99;
        assert(!dwc3_descriptor(0, 1, 0, 0, &length) && length == 0);
        assert(dwc3_init(context, sizeof(context), &platform, 0, dma, dma_bus, sizeof(dma)) == -1);
        invalid.vid = 0;
        assert(dwc3_init(context, sizeof(context), &platform, &invalid, dma, dma_bus, sizeof(dma)) == -1);
        invalid = identity; invalid.product = "";
        assert(dwc3_init(context, sizeof(context), &platform, &invalid, dma, dma_bus, sizeof(dma)) == -1);
        invalid.product = "12345678901234567890123456789012";
        assert(dwc3_init(context, sizeof(context), &platform, &invalid, dma, dma_bus, sizeof(dma)) == -1);
        invalid.product = "bad\ntext";
        assert(dwc3_init(context, sizeof(context), &platform, &invalid, dma, dma_bus, sizeof(dma)) == -1);
        invalid.product = 0;
        assert(dwc3_init(context, sizeof(context), &platform, &invalid, dma, dma_bus, sizeof(dma)) == -1);
    }
    assert(writes == 0);
    regs[DWC3_GSNPSID / 4] = 0x3331310a;
    regs[DWC3_GUSB2PHYCFG(0) / 4] = (1u << 6) | (1u << 8);
    regs[DWC3_GHWPARAMS1 / 4] = 1u << 15;
    regs[DWC3_GHWPARAMS3 / 4] = (6 << 12) | (3 << 18);
    regs[DWC3_DSTS / 4] = DWC3_DSTS_DEVCTRLHLT;
    assert(dwc3_init(context, sizeof(context), &platform, &identity, dma, dma_bus, sizeof(dma)) == 0);
    {
        _Alignas(4096) static uint8_t other_dma[DWC3_DMA_BYTES];
        _Alignas(8) static uint8_t other_context[DWC3_CONTEXT_BYTES];
        struct dwc3_platform other_platform = platform;
        char serial_text[] = "IndependentCore";
        struct dwc3_identity other_id = {0x1234, 0x5678, "Other platform", "Other CDC", serial_text};
        const uint8_t *desc;
        size_t length;
        other_platform.cookie = other_dma;
        assert(dwc3_init(other_context, sizeof(other_context), &other_platform, &other_id,
                         other_dma, dma_bus + DWC3_DMA_BYTES, sizeof(other_dma)) == 0);
        desc = dwc3_descriptor((void *)other_context, 1, 0, 0, &length);
        assert(length == 18 && desc[8] == 0x34 && desc[9] == 0x12 && desc[10] == 0x78 && desc[11] == 0x56);
        serial_text[0] = 'X';
        desc = dwc3_descriptor((void *)other_context, 3, 3, 0, &length);
        assert(desc[2] == 'I'); /* Caller string was copied. */
        desc = dwc3_descriptor(dev, 1, 0, 0, &length);
        assert(desc[8] == 0x09 && desc[9] == 0x12 && desc[10] == 0x6d && desc[11] == 0x31);
    }
    descriptor_tests();
    assert(last_in_fifo == 2);
    assert(regs[DWC3_GUSB2PHYCFG(0) / 4] == ((1u << 6) | (1u << 8)));
    assert(dwc3_start(dev) == 0);
    assert(dwc3_queue(dev, "early log\n", 10) == 10);
    event(1u | (DWC3_DEVT_USBRST << 8));
    event(1u | (DWC3_DEVT_CONNECTDONE << 8));
    assert(dwc3_poll(dev, 1) == 1 && regs[DWC3_GEVNTCOUNT(0) / 4] == 4);
    assert(dwc3_poll(dev, 32) == 1);
    assert(eptrb(0)->size == 8 && !dwc3_ready(dev));
    setup(0x80, 6, 0x0100, 0, 18);
    not_ready(1, DEPEVT_STATUS_CONTROL_DATA);
    assert(eptrb(1)->size == 18 && dma[8192 + 4096] == 18);
    complete(1, 0); not_ready(0, DEPEVT_STATUS_CONTROL_STATUS);
    assert((eptrb(0)->ctrl & (63u << 4)) == DWC3_TRBCTL_CONTROL_STATUS3);
    complete(0, 0);
    setup(0, 5, 42, 0, 0); status_in(DWC3_TRBCTL_CONTROL_STATUS2);
    assert((regs[DWC3_DCFG / 4] & DWC3_DCFG_DEVADDR_MASK) == DWC3_DCFG_DEVADDR(42));
    setup(0, 9, 1, 0, 0); status_in(DWC3_TRBCTL_CONTROL_STATUS2);
    assert(!dwc3_ready(dev));
    setup(0x21, 0x22, 3, 0, 0); status_in(DWC3_TRBCTL_CONTROL_STATUS2);
    assert(dwc3_ready(dev));
    assert(eptrb(4)->size == 4096u);
    {
        static const char first_rx[] = "SXNU1 partial\r\n";
        uint8_t received[32];
        unsigned before_from = from_syncs;
        memcpy(dma + 8192 + 4 * 4096, first_rx, sizeof(first_rx) - 1);
        complete(4, 4096u - (sizeof(first_rx) - 1));
        assert(from_syncs > before_from);
        assert(last_from_pointer == dma + 8192 + 4 * 4096 && last_from_bytes == 4096u);
        assert(dwc3_rx_pending(dev) == sizeof(first_rx) - 1);
        assert(dwc3_read(dev, received, 4) == 4 && !memcmp(received, "SXNU", 4));
        assert(dwc3_rx_pending(dev) == sizeof(first_rx) - 1 - 4);
        assert(dwc3_read(dev, received + 4, sizeof(received)) == sizeof(first_rx) - 1 - 4);
        assert(!memcmp(received, first_rx, sizeof(first_rx) - 1));
        assert(dwc3_read(dev, received, sizeof(received)) == 0);
        assert(dwc3_rx_overflow(dev) == 0);
    }
    {
        uint8_t overflow[600];
        uint8_t drained[DWC3_RX_BYTES + 1];
        memset(overflow, 'R', sizeof(overflow));
        memcpy(dma + 8192 + 4 * 4096, overflow, sizeof(overflow));
        complete(4, 4096u - sizeof(overflow));
        assert(dwc3_rx_pending(dev) == DWC3_RX_BYTES);
        assert(dwc3_rx_overflow(dev) == sizeof(overflow) - DWC3_RX_BYTES);
        assert(dwc3_read(dev, drained, sizeof(drained)) == DWC3_RX_BYTES);
        assert(drained[0] == 'R' && drained[DWC3_RX_BYTES - 1] == 'R');
        assert(dwc3_rx_pending(dev) == 0);
    }
    assert(eptrb(5)->size == 10 && !memcmp(dma + 8192 + 5 * 4096, "early log\n", 10));
    assert(dwc3_queued(dev) == 10);
    complete(5, 0); assert(dwc3_queued(dev) == 0);
    puts("PASS: reset/connect, bounded event budget, GET_DESCRIPTOR, 3-stage and 2-stage control, DTR, queued early log");
    uint8_t payload[512]; memset(payload, 'X', sizeof(payload));
    assert(dwc3_queue(dev, payload, sizeof(payload)) == 512);
    assert(dwc3_poll(dev, 32) == 0 && eptrb(5)->size == 512);
    complete(5, 0); assert(eptrb(5)->size == 0 && dwc3_queued(dev) == 0);
    complete(5, 0);
    puts("PASS: explicit DMA addresses above 4GiB, separate TRB lines, correct IN FIFO and packet-aligned ZLP");
    setup(0x21, 0x20, 0, 99, 7); /* Invalid interface must stall, never index an array. */
    assert((eptrb(0)->ctrl & (63u << 4)) == DWC3_TRBCTL_CONTROL_SETUP);
    setup(0x21, 0x20, 0, 0, 7);
    not_ready(0, DEPEVT_STATUS_CONTROL_DATA); memcpy(dma + 8192, "1234567", 7); complete(0, 57);
    status_in(DWC3_TRBCTL_CONTROL_STATUS3);
    setup(0xa1, 0x21, 0, 0, 7); not_ready(1, DEPEVT_STATUS_CONTROL_DATA);
    assert(!memcmp(dma + 8192 + 4096, "1234567", 7));
    complete(1, 0); not_ready(0, DEPEVT_STATUS_CONTROL_STATUS); complete(0, 0);
    puts("PASS: invalid CDC interface rejection and exact 7-byte line-coding OUT/IN");
    assert(dwc3_queue(dev, payload, sizeof(payload)) == 512);
    assert(dwc3_poll(dev, 32) == 0);
    unsigned before_end_delay = delays;
    setup(0, 9, 0, 0, 0); status_in(DWC3_TRBCTL_CONTROL_STATUS2);
    assert(delays == before_end_delay + 2000 && !dwc3_ready(dev));
    assert(dwc3_queued(dev) == 512);
    setup(0, 9, 1, 0, 0); status_in(DWC3_TRBCTL_CONTROL_STATUS2);
    setup(0x21, 0x22, 1, 0, 0); status_in(DWC3_TRBCTL_CONTROL_STATUS2);
    assert(dwc3_ready(dev) && eptrb(5)->size == 512);
    complete(5, 0); complete(5, 0);
    assert(dwc3_queue(dev, "reset log\n", 10) == 10);
    {
        static const char stale[] = "stale RX";
        memcpy(dma + 8192 + 4 * 4096, stale, sizeof(stale) - 1);
        complete(4, 4096u - (sizeof(stale) - 1));
        assert(dwc3_rx_pending(dev) == sizeof(stale) - 1);
    }
    assert(dwc3_poll(dev, 32) == 0);
    before_end_delay = delays;
    {
        uint32_t generation_before_reset = dwc3_rx_generation(dev);
    event(1u | (DWC3_DEVT_USBRST << 8));
    event(1u | (DWC3_DEVT_CONNECTDONE << 8));
    assert(dwc3_poll(dev, 32) == 2 && delays == before_end_delay + 2000);
    assert(!dwc3_ready(dev) && dwc3_queued(dev) == 10);
        assert(dwc3_rx_pending(dev) == 0);
        assert(dwc3_rx_generation(dev) != generation_before_reset);
    }
    setup(0, 9, 1, 0, 0); status_in(DWC3_TRBCTL_CONTROL_STATUS2);
    setup(0x21, 0x22, 1, 0, 0); status_in(DWC3_TRBCTL_CONTROL_STATUS2);
    assert(eptrb(5)->size == 10); complete(5, 0);
    setup(0x80, 6, 0x0100, 0, 18);
    not_ready(1, DEPEVT_STATUS_CONTROL_STATUS); /* Wrong phase must not start DATA. */
    struct dwc3_snapshot snapshot;
    dwc3_snapshot(dev, &snapshot);
    assert((snapshot.active_endpoints & 2u) == 0);
    not_ready(1, DEPEVT_STATUS_CONTROL_DATA);
    complete(1, (DWC3_TRBSTS_SETUP_PENDING << 28));
    assert((eptrb(0)->ctrl & (63u << 4)) == DWC3_TRBCTL_CONTROL_SETUP);
    setup(0x80, 6, 0x0100, 0, 18);
    not_ready(1, DEPEVT_STATUS_CONTROL_DATA);
    before_end_delay = delays;
    event(1u | (DWC3_DEVT_USBRST << 8));
    event(1u | (DWC3_DEVT_CONNECTDONE << 8));
    assert(dwc3_poll(dev, 32) == 2 && delays == before_end_delay + 2000);
    assert((eptrb(0)->ctrl & (63u << 4)) == DWC3_TRBCTL_CONTROL_SETUP);
    puts("PASS: suspend bits cleared/restored; deconfigure/reset end active DMA with USB31 delay; aborted control restarts SETUP");
    event((31u << 1) | (DWC3_DEPEVT_XFERCOMPLETE << 6));
    assert(dwc3_poll(dev, 32) == 1);
    uint8_t large[17000]; memset(large, 'L', sizeof(large));
    assert(dwc3_queue(dev, large, sizeof(large)) == 16384 && dwc3_dropped(dev) == 616);
    assert(dwc3_stop(dev) == 0);
    assert(syncs > 0 && delays >= 5000);
    puts("PASS: malformed event bounds, finite queue with dropped counter and controller stop");
    printf("context=%zu DMA=%u syncs=%u writes=%u\n", dwc3_context_bytes(), DWC3_DMA_BYTES, syncs, writes);
    assert(dwc3_init(context, sizeof(context), &platform, &identity, dma, dma_bus, sizeof(dma)) == 0);
    assert(dwc3_start(dev) == 0);
    event(1u | (DWC3_DEVT_USBRST << 8));
    event(1u | (DWC3_DEVT_CONNECTDONE << 8));
    assert(dwc3_poll(dev, 32) == 2);
    setup(0, 9, 1, 0, 0); status_in(DWC3_TRBCTL_CONTROL_STATUS2);
    assert(eptrb(4)->size == 4096u);
    eptrb(4)->size = 4097u;
    eptrb(4)->ctrl &= ~DWC3_TRB_CTRL_HWO;
    event((4u << 1) | (DWC3_DEPEVT_XFERCOMPLETE << 6));
    assert(dwc3_poll(dev, 32) == -8);
    assert(dwc3_rx_pending(dev) == 0);
    puts("PASS: OUT residual/cache-sync ordering, finite RX overflow, reset clearing and strict length rejection");
    unsigned previous_writes = writes;
    regs[DWC3_GSNPSID / 4] = 0;
    assert(dwc3_init(context, sizeof(context), &platform, &identity, dma, dma_bus, sizeof(dma)) == -2);
    assert(writes == previous_writes);
    regs[DWC3_GSNPSID / 4] = 0x3331310a;
    timeout_commands = 1;
    unsigned before_timeout = delays;
    assert(dwc3_init(context, sizeof(context), &platform, &identity, dma, dma_bus, sizeof(dma)) == -4);
    assert(delays == before_timeout + 10000 && regs[DWC3_DEVTEN / 4] == 0 && regs[DWC3_DALEPENA / 4] == 0);
    assert(regs[DWC3_GUSB2PHYCFG(0) / 4] == ((1u << 6) | (1u << 8)));
    puts("PASS: unknown core refuses before writes; stuck command exits within 10000 one-microsecond polls");
    return 0;
}
