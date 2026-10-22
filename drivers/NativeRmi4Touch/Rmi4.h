// SPDX-License-Identifier: BSD-2-Clause-Patent
// Synaptics RMI4 subset for a 2-D touch sensor: the page description table, F01 (device control) and F12 (2-D sensor) as the
// Linux rmi4 core (rmi_driver.c, rmi_f01.c, rmi_f12.c) and the postmarketOS "quirks for third party touchscreen controllers"
// series use them (studied, no code copied). Transport-independent: the caller supplies byte-register read and write on the
// current page; page selection (register 0xff) is done here. No allocation, no kernel calls. Host-tested by tests/rmi4-test.cpp.
#ifndef RMI4_H
#define RMI4_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RMI_PAGE_SELECT 0xff
#define RMI_PDT_START 0xe9              // first PDT entry of a page; entries are 6 bytes, scanned downwards
#define RMI_PDT_END 0x05
#define RMI_PDT_ENTRY 6
#define RMI_MAX_FUNCTIONS 12
#define RMI_MAX_PAGES 4                 // touch controllers keep F01/F12 on page 0; scan stops after an empty page anyway
#define RMI_DESC_ITEMS 32               // packet registers per descriptor kept (F12 uses numbers below 32)
#define RMI_STRUCT_MAX 256              // largest register descriptor structure read
#define RMI_MAX_OBJECTS 10              // contacts reported (F12 Data1 subpackets)
#define RMI_OBJ_BYTES 8                 // F12 Data1: type, x lo/hi, y lo/hi, z, wx, wy

// F01
#define RMI_F01_STATUS_CODE(s) ((s) & 0x0f)
#define RMI_F01_STATUS_BOOTLOADER 0x40
#define RMI_F01_STATUS_UNCONFIGURED 0x80
#define RMI_F01_CTRL0_SLEEP_MASK 0x03
#define RMI_F01_CTRL0_NOSLEEP 0x04
#define RMI_F01_CTRL0_CONFIGURED 0x80
#define RMI_F01_CMD_RESET 0x01
// F12 object types (rmi_f12.c)
#define RMI_F12_OBJ_NONE 0x00
#define RMI_F12_OBJ_FINGER 0x01
#define RMI_F12_OBJ_STYLUS 0x02
#define RMI_F12_OBJ_PALM 0x03
#define RMI_F12_OBJ_UNCLASSIFIED 0x04
#define RMI_F12_OBJ_GLOVED 0x06

#define RMI_ERR_PARAM (-22)
#define RMI_ERR_IO (-5)                 // a malformed descriptor or an out-of-range value
#define RMI_ERR_NODEV (-19)             // F01 or F12 missing
#define RMI_ERR_BOOTLOADER (-16)        // EBUSY: the controller is in its bootloader (no touch firmware running)

struct rmi_bus {
    void *ctx;
    // Read or write len bytes starting at register reg of the current page. Return 0 or a negative error.
    int (*read)(void *ctx, uint8_t reg, uint8_t *buf, size_t len);
    int (*write)(void *ctx, uint8_t reg, const uint8_t *buf, size_t len);
    void (*delay_ms)(void *ctx, unsigned ms);
};

struct rmi_function {
    uint16_t entry;                     // address of its PDT entry
    uint16_t query, command, control, data;   // page-qualified base addresses
    uint8_t number, version, irq_count, irq_pos;
};

struct rmi_reg_item {
    uint8_t reg;                        // packet register number
    uint8_t index;                      // address offset from the function's base (one address per present register)
    uint16_t offset;                    // byte offset in the packet read from the base
    uint32_t size;
    uint64_t subpackets;                // subpacket presence bits 0..63
    uint16_t num_subpackets;
};

struct rmi_reg_desc {
    unsigned count;
    uint32_t struct_size;
    struct rmi_reg_item items[RMI_DESC_ITEMS];
};

enum rmi_pdt_mode {RMI_PDT_SCANNED = 0, RMI_PDT_FALLBACK = 1};

struct rmi_contact {uint8_t id, type, z, wx, wy; uint16_t x, y;};

struct rmi_dev {
    struct rmi_bus bus;
    int page;                           // selected page, -1 unknown
    // PDT
    struct rmi_function fn[RMI_MAX_FUNCTIONS];
    unsigned nfn, irq_count, irq_regs;
    int f01, f12;                       // indexes into fn, -1 missing
    enum rmi_pdt_mode pdt_mode;
    int force_fallback;                 // use the S3706B fixed table without scanning
    uint8_t first_entry[RMI_PDT_ENTRY]; // the raw entry at 0xe9 (clone controllers often leave it empty)
    // F01
    uint8_t status, ctrl0_before, ctrl0, irq_enable;
    char product_id[11];
    uint8_t manufacturer, query1;
    // F12
    uint8_t f12_query0;
    int f12_hardcoded;                  // no register descriptors: the postmarketOS fixed layout
    struct rmi_reg_desc f12_query, f12_control, f12_data;
    uint16_t data1_offset;              // byte offset of Data1 in the packet read from the F12 data base
    uint32_t data1_size, read_size;     // Data1 bytes; bytes read per frame (data1_offset + data1_size)
    unsigned objects;                   // Data1 subpackets
    uint16_t max_x, max_y;              // F12 Control 8 (0 when not reported)
    uint32_t abs_mask;                  // F12's first interrupt bit in the status registers
    uint8_t pkt[RMI_STRUCT_MAX];
    // Diagnostics
    int last_err;
    uint32_t reads, writes, page_writes;
};

void rmi_init(struct rmi_dev *d, const struct rmi_bus *bus);
// Read or write len bytes at a page-qualified address (page = addr >> 8), selecting the page first when it changes.
int rmi_read(struct rmi_dev *d, uint16_t addr, uint8_t *buf, size_t len);
int rmi_write(struct rmi_dev *d, uint16_t addr, const uint8_t *buf, size_t len);
// Scan the PDT (pages 0..RMI_MAX_PAGES-1, stop after an empty page). Functions are known from the PostmarketOS list; duplicates
// and unknown numbers are skipped. When the first entry of page 0 is empty, or force_fallback is set, the S3706B fixed table is
// used instead: 0xe9 F34, 0xe3 F01, 0xdd F12, reading only the four base-address bytes of each entry.
int rmi_scan_pdt(struct rmi_dev *d);
// Pure: one 6-byte PDT entry; returns 0 for an end-of-table marker (function 0x00 or 0xff), 1 otherwise.
int rmi_parse_pdt_entry(const uint8_t *e, uint16_t addr, struct rmi_function *out);
// Pure: a register descriptor from its presence register and structure (rmi_read_register_desc).
int rmi_parse_reg_desc(const uint8_t *presence, size_t presence_len, const uint8_t *structure, size_t structure_len,
                       struct rmi_reg_desc *out);
const struct rmi_reg_item *rmi_desc_item(const struct rmi_reg_desc *desc, unsigned reg);
// Read a register descriptor at addr (size, presence and structure at addr, addr + 1, addr + 2).
int rmi_read_reg_desc(struct rmi_dev *d, uint16_t addr, struct rmi_reg_desc *out);
// F01: status, query 0..1 and the product id (query 11..20 for controllers that garble query 3..).
int rmi_f01_read_info(struct rmi_dev *d);
// F01 software reset (command 1) and the wait after it.
int rmi_f01_reset(struct rmi_dev *d, unsigned wait_ms);
// F01: normal sleep mode, NoSleep (as the fajita DT asks), Configured; interrupt enable = F12 absolute (and F01 status).
int rmi_f01_configure(struct rmi_dev *d, int nosleep);
// F12: register descriptors (or the hardcoded layout), Data1 geometry, Control 8 axis maxima.
int rmi_f12_probe(struct rmi_dev *d);
// F01 data: device status and the interrupt status registers in one read from the F01 data base (the postmarketOS order: some
// aftermarket controllers mishandle the interrupt request otherwise). irq gets up to 32 status bits.
int rmi_read_irq(struct rmi_dev *d, uint8_t *status, uint32_t *irq);
// Read Data1 and decode it: every object slot that holds a contact (finger, stylus, unclassified, gloved) is returned with its
// slot index as id. Returns the number of contacts or a negative error.
int rmi_f12_read_contacts(struct rmi_dev *d, struct rmi_contact *out, unsigned max);
// Pure: decode objects from a Data1 buffer.
unsigned rmi_f12_decode(const uint8_t *data1, size_t size, unsigned objects, struct rmi_contact *out, unsigned max);

#ifdef __cplusplus
}
#endif
#endif
