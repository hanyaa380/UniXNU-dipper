// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Rmi4.h"

#define PRESENCE_REGS_MAX 35            // rmi_driver.h RMI_REG_DESC_PRESENCE_REGS_MAX: 3 + 256 bits / 8
#define PRESENCE_BITS 256
#define SUBPACKET_BITS 296

static void zero(void *p, size_t n) {uint8_t *b = (uint8_t *)p; while (n--) *b++ = 0;}
static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | (p[1] << 8));}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);}

void rmi_init(struct rmi_dev *d, const struct rmi_bus *bus) {
    zero(d, sizeof *d);
    d->bus = *bus;
    d->page = -1;
    d->f01 = d->f12 = -1;
}

static int select_page(struct rmi_dev *d, uint16_t addr) {
    const int page = addr >> 8;
    if (page == d->page) return 0;
    const uint8_t p = (uint8_t)page;
    int r = d->bus.write(d->bus.ctx, RMI_PAGE_SELECT, &p, 1);
    d->page_writes++;
    d->page = r ? -1 : page;
    return r;
}

int rmi_read(struct rmi_dev *d, uint16_t addr, uint8_t *buf, size_t len) {
    if (!d || !buf || !len) return RMI_ERR_PARAM;
    int r = select_page(d, addr);
    if (!r) r = d->bus.read(d->bus.ctx, (uint8_t)addr, buf, len);
    d->reads++;
    if (r) {d->page = -1; d->last_err = r;}   // after a failed transfer the controller's page is not trusted
    return r;
}

int rmi_write(struct rmi_dev *d, uint16_t addr, const uint8_t *buf, size_t len) {
    if (!d || !buf || !len) return RMI_ERR_PARAM;
    int r = select_page(d, addr);
    if (!r) r = d->bus.write(d->bus.ctx, (uint8_t)addr, buf, len);
    d->writes++;
    if (r) {d->page = -1; d->last_err = r;}
    return r;
}

static void sleep_ms(struct rmi_dev *d, unsigned ms) {if (d->bus.delay_ms) d->bus.delay_ms(d->bus.ctx, ms);}

int rmi_parse_pdt_entry(const uint8_t *e, uint16_t addr, struct rmi_function *out) {
    if (e[5] == 0x00 || e[5] == 0xff) return 0;   // RMI4_END_OF_PDT
    const uint16_t page = (uint16_t)(addr & 0xff00);
    zero(out, sizeof *out);
    out->entry = addr;
    out->query = (uint16_t)(page + e[0]);
    out->command = (uint16_t)(page + e[1]);
    out->control = (uint16_t)(page + e[2]);
    out->data = (uint16_t)(page + e[3]);
    out->irq_count = e[4] & 0x07;
    out->version = (e[4] >> 5) & 0x03;
    out->number = e[5];
    return 1;
}

// The function numbers the postmarketOS quirk series accepts (rmi_pdt_entry_is_valid); a garbled entry of a clone controller
// is recorded nowhere else.
static int known_function(uint8_t fn) {
    switch (fn) {
    case 0x01: case 0x03: case 0x11: case 0x12: case 0x30: case 0x34: case 0x3a: case 0x54: case 0x55: return 1;
    default: return 0;
    }
}

static void add_function(struct rmi_dev *d, const struct rmi_function *f) {
    for (unsigned i = 0; i < d->nfn; i++) if (d->fn[i].number == f->number) return;   // a duplicate entry
    if (d->nfn >= RMI_MAX_FUNCTIONS) return;
    d->fn[d->nfn] = *f;
    if (f->number == 0x01 && d->f01 < 0) d->f01 = (int)d->nfn;
    if (f->number == 0x12 && d->f12 < 0) d->f12 = (int)d->nfn;
    d->nfn++;
}

// postmarketOS rmi_s3706_fallback_pdt: {function, interrupt count | version << 5} for the entries at 0xe9, 0xe3 and 0xdd.
static const uint8_t s3706_fallback[3][2] = {{0x34, 0x41}, {0x01, 0x01}, {0x12, 0x01}};

static int scan_fallback(struct rmi_dev *d) {
    d->pdt_mode = RMI_PDT_FALLBACK;
    for (unsigned i = 0; i < 3; i++) {
        const uint16_t addr = (uint16_t)(RMI_PDT_START - RMI_PDT_ENTRY * i);
        uint8_t e[RMI_PDT_ENTRY];
        int r = rmi_read(d, addr, e, 4);   // only the four base addresses: the clones do not implement the rest
        if (r) return r;
        e[4] = s3706_fallback[i][1];
        e[5] = s3706_fallback[i][0];
        struct rmi_function f;
        rmi_parse_pdt_entry(e, addr, &f);
        f.irq_pos = (uint8_t)d->irq_count;
        d->irq_count += f.irq_count;
        add_function(d, &f);
    }
    return 0;
}

int rmi_scan_pdt(struct rmi_dev *d) {
    if (!d) return RMI_ERR_PARAM;
    d->nfn = 0; d->irq_count = 0; d->f01 = d->f12 = -1; d->pdt_mode = RMI_PDT_SCANNED;
    int r = rmi_read(d, RMI_PDT_START, d->first_entry, RMI_PDT_ENTRY);
    if (r) return r;
    if (d->force_fallback || d->first_entry[5] == 0x00 || d->first_entry[5] == 0xff) r = scan_fallback(d);
    else {
        // Page 0 only: F01 and F12 live there on these controllers, and every extra page select is one more register a clone
        // may not implement. Interrupt positions count every listed function, known or not, as the Linux core does.
        for (uint16_t addr = RMI_PDT_START; addr >= RMI_PDT_END; addr = (uint16_t)(addr - RMI_PDT_ENTRY)) {
            uint8_t e[RMI_PDT_ENTRY];
            if (addr == RMI_PDT_START) for (unsigned k = 0; k < RMI_PDT_ENTRY; k++) e[k] = d->first_entry[k];
            else if ((r = rmi_read(d, addr, e, RMI_PDT_ENTRY))) return r;
            struct rmi_function f;
            if (!rmi_parse_pdt_entry(e, addr, &f)) break;
            f.irq_pos = (uint8_t)d->irq_count;
            d->irq_count += f.irq_count;
            if (known_function(f.number)) add_function(d, &f);
        }
    }
    if (r) return r;
    d->irq_regs = (d->irq_count + 7) / 8;
    if (d->irq_regs > 4) d->irq_regs = 4;
    if (d->f01 < 0 || d->f12 < 0) return RMI_ERR_NODEV;
    if (d->irq_regs == 0) d->irq_regs = 1;
    const struct rmi_function *f12 = &d->fn[d->f12];
    d->abs_mask = f12->irq_pos < 32 ? 1u << f12->irq_pos : 0;
    return 0;
}

int rmi_parse_reg_desc(const uint8_t *presence, size_t presence_len, const uint8_t *structure, size_t structure_len,
                       struct rmi_reg_desc *out) {
    zero(out, sizeof *out);
    if (!presence || presence_len < 1 || presence_len > PRESENCE_REGS_MAX) return RMI_ERR_IO;
    size_t first;
    if (presence[0] == 0) {
        if (presence_len < 3) return RMI_ERR_IO;
        out->struct_size = le16(&presence[1]);
        first = 3;
    } else {
        out->struct_size = presence[0];
        first = 1;
    }
    // Registers in number order, each item: size (1 byte; 0 = 2-byte size follows; 0 again = 4-byte size follows), then
    // subpacket presence bytes, seven bits each, bit 7 = another byte follows.
    size_t at = 0, available = structure_len < out->struct_size ? structure_len : out->struct_size;
    unsigned index = 0, offset = 0;
    for (size_t i = first; i < presence_len; i++) {
        for (unsigned b = 0; b < 8; b++) {
            if (!(presence[i] & (1u << b))) continue;
            const unsigned reg = (unsigned)(i - first) * 8 + b;
            if (reg >= PRESENCE_BITS) return RMI_ERR_IO;
            if (at >= available) return 0;   // the structure ended: the registers so far are kept (rmi_read_register_desc)
            uint32_t size = structure[at++];
            if (size == 0) {
                if (available - at < 2) return RMI_ERR_IO;
                size = le16(&structure[at]); at += 2;
                if (size == 0) {
                    if (available - at < 4) return RMI_ERR_IO;
                    size = le32(&structure[at]); at += 4;
                }
            }
            uint64_t map = 0;
            unsigned bit = 0, count = 0;
            for (;;) {
                if (at >= available) return RMI_ERR_IO;
                const uint8_t v = structure[at++];
                for (unsigned k = 0; k < 7; k++, bit++) {
                    if (!(v & (1u << k))) continue;
                    if (bit >= SUBPACKET_BITS) return RMI_ERR_IO;
                    if (bit < 64) map |= 1ull << bit;
                    count++;
                }
                if (!(v & 0x80)) break;
            }
            if (size == 0) continue;   // a zero-size register is ignored, and takes no address
            if (out->count < RMI_DESC_ITEMS) {
                struct rmi_reg_item *it = &out->items[out->count++];
                it->reg = (uint8_t)reg;
                it->index = (uint8_t)index;
                it->offset = (uint16_t)(offset > 0xffff ? 0xffff : offset);
                it->size = size;
                it->subpackets = map;
                it->num_subpackets = (uint16_t)count;
            }
            index++;
            offset += size;
        }
    }
    return 0;
}

const struct rmi_reg_item *rmi_desc_item(const struct rmi_reg_desc *desc, unsigned reg) {
    for (unsigned i = 0; i < desc->count; i++) if (desc->items[i].reg == reg) return &desc->items[i];
    return 0;
}

int rmi_read_reg_desc(struct rmi_dev *d, uint16_t addr, struct rmi_reg_desc *out) {
    uint8_t size = 0;
    int r = rmi_read(d, addr, &size, 1);
    if (r) return r;
    if (size < 1 || size > PRESENCE_REGS_MAX) return RMI_ERR_IO;
    uint8_t presence[PRESENCE_REGS_MAX];
    if ((r = rmi_read(d, (uint16_t)(addr + 1), presence, size))) return r;
    uint32_t struct_size = presence[0] ? presence[0] : (size >= 3 ? le16(&presence[1]) : 0);
    if (!struct_size) return RMI_ERR_IO;
    if (struct_size > RMI_STRUCT_MAX) struct_size = RMI_STRUCT_MAX;   // the registers F12 needs come first
    if ((r = rmi_read(d, (uint16_t)(addr + 2), d->pkt, struct_size))) return r;
    return rmi_parse_reg_desc(presence, size, d->pkt, struct_size, out);
}

int rmi_f01_read_info(struct rmi_dev *d) {
    if (!d || d->f01 < 0) return RMI_ERR_NODEV;
    const struct rmi_function *f = &d->fn[d->f01];
    uint8_t q[2];
    int r = rmi_read(d, f->query, q, 2);
    if (r) return r;
    d->manufacturer = q[0];
    d->query1 = q[1];
    uint8_t id[10];
    if ((r = rmi_read(d, (uint16_t)(f->query + 11), id, sizeof id))) return r;
    for (unsigned i = 0; i < 10; i++) d->product_id[i] = (id[i] >= 0x20 && id[i] < 0x7f) ? (char)id[i] : (id[i] ? '?' : 0);
    d->product_id[10] = 0;
    if ((r = rmi_read(d, f->data, &d->status, 1))) return r;
    return (d->status & RMI_F01_STATUS_BOOTLOADER) ? RMI_ERR_BOOTLOADER : 0;
}

int rmi_f01_reset(struct rmi_dev *d, unsigned wait_ms) {
    if (!d || d->f01 < 0) return RMI_ERR_NODEV;
    const uint8_t cmd = RMI_F01_CMD_RESET;
    int r = rmi_write(d, d->fn[d->f01].command, &cmd, 1);
    d->page = -1;   // a reset returns the controller to page 0, but nothing is assumed
    sleep_ms(d, wait_ms);
    return r;
}

int rmi_f01_configure(struct rmi_dev *d, int nosleep) {
    if (!d || d->f01 < 0 || d->f12 < 0) return RMI_ERR_NODEV;
    const struct rmi_function *f = &d->fn[d->f01];
    uint8_t c = 0;
    int r = rmi_read(d, f->control, &c, 1);
    if (r) return r;
    d->ctrl0_before = c;
    c &= (uint8_t)~RMI_F01_CTRL0_SLEEP_MASK;   // normal operation
    if (nosleep) c |= RMI_F01_CTRL0_NOSLEEP;
    c |= RMI_F01_CTRL0_CONFIGURED;             // a later reset of the controller clears it again (status bit 7)
    if ((r = rmi_write(d, f->control, &c, 1))) return r;
    d->ctrl0 = c;
    uint32_t mask = d->abs_mask;
    if (f->irq_count && f->irq_pos < 32) mask |= 1u << f->irq_pos;   // F01's own status interrupt
    uint8_t en[4];
    for (unsigned i = 0; i < d->irq_regs; i++) en[i] = (uint8_t)(mask >> (8 * i));
    if ((r = rmi_write(d, (uint16_t)(f->control + 1), en, d->irq_regs))) return r;
    d->irq_enable = (uint8_t)mask;
    return 0;
}

int rmi_f12_probe(struct rmi_dev *d) {
    if (!d || d->f12 < 0) return RMI_ERR_NODEV;
    const struct rmi_function *f = &d->fn[d->f12];
    int r = rmi_read(d, f->query, &d->f12_query0, 1);
    if (r) return r;
    d->max_x = d->max_y = 0;
    if (!(d->f12_query0 & 1)) {
        // postmarketOS rmi_f12_set_hardcoded_desc: no register descriptors, Data1 at the data base, ten 8-byte objects, and an
        // 88-byte packet read.
        d->f12_hardcoded = 1;
        d->data1_offset = 0;
        d->data1_size = RMI_MAX_OBJECTS * RMI_OBJ_BYTES;
        d->objects = RMI_MAX_OBJECTS;
        d->read_size = 88;
        return 0;
    }
    d->f12_hardcoded = 0;
    uint16_t q = (uint16_t)(f->query + 1);
    if ((r = rmi_read_reg_desc(d, q, &d->f12_query))) return r;
    if ((r = rmi_read_reg_desc(d, (uint16_t)(q + 3), &d->f12_control))) return r;
    if ((r = rmi_read_reg_desc(d, (uint16_t)(q + 6), &d->f12_data))) return r;
    const struct rmi_reg_item *data1 = rmi_desc_item(&d->f12_data, 1);
    if (!data1 || data1->size < RMI_OBJ_BYTES || !data1->num_subpackets) return RMI_ERR_NODEV;
    d->data1_offset = data1->offset;
    d->data1_size = data1->size;
    d->objects = data1->num_subpackets;
    if ((uint32_t)d->data1_offset + d->data1_size > RMI_STRUCT_MAX) return RMI_ERR_IO;
    d->read_size = d->data1_offset + d->data1_size;
    const struct rmi_reg_item *c8 = rmi_desc_item(&d->f12_control, 8);   // sensor tuning: axis maxima in subpacket 0
    if (c8 && (c8->subpackets & 1) && c8->size >= 4 && c8->size <= 15) {
        uint8_t b[15];
        if ((r = rmi_read(d, (uint16_t)(f->control + c8->index), b, c8->size))) return r;
        d->max_x = le16(&b[0]);
        d->max_y = le16(&b[2]);
    }
    return 0;
}

int rmi_read_irq(struct rmi_dev *d, uint8_t *status, uint32_t *irq) {
    if (!d || d->f01 < 0) return RMI_ERR_NODEV;
    uint8_t b[5] = {0};
    int r = rmi_read(d, d->fn[d->f01].data, b, 1 + d->irq_regs);
    if (r) return r;
    uint32_t v = 0;
    for (unsigned i = 0; i < d->irq_regs; i++) v |= (uint32_t)b[1 + i] << (8 * i);
    if (status) *status = b[0];
    if (irq) *irq = v;
    d->status = b[0];
    return 0;
}

unsigned rmi_f12_decode(const uint8_t *data1, size_t size, unsigned objects, struct rmi_contact *out, unsigned max) {
    unsigned n = objects;
    if (n > size / RMI_OBJ_BYTES) n = (unsigned)(size / RMI_OBJ_BYTES);
    unsigned count = 0;
    for (unsigned i = 0; i < n && count < max; i++) {
        const uint8_t *p = data1 + i * RMI_OBJ_BYTES;
        switch (p[0]) {
        case RMI_F12_OBJ_FINGER: case RMI_F12_OBJ_STYLUS: case RMI_F12_OBJ_UNCLASSIFIED: case RMI_F12_OBJ_GLOVED: break;
        default: continue;   // none, palm (not a touch to deliver) or a type this driver does not know
        }
        struct rmi_contact *c = &out[count++];
        c->id = (uint8_t)i;
        c->type = p[0];
        c->x = le16(&p[1]);
        c->y = le16(&p[3]);
        c->z = p[5];
        c->wx = p[6];
        c->wy = p[7];
    }
    return count;
}

int rmi_f12_read_contacts(struct rmi_dev *d, struct rmi_contact *out, unsigned max) {
    if (!d || d->f12 < 0 || !d->read_size) return RMI_ERR_NODEV;
    int r = rmi_read(d, d->fn[d->f12].data, d->pkt, d->read_size);
    if (r) return r;
    return (int)rmi_f12_decode(d->pkt + d->data1_offset, d->data1_size, d->objects, out, max);
}
