// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm GENI serial-engine I2C master, FIFO (PIO) mode, polled. Register layout and sequences follow the study of the Linux
// i2c-qcom-geni / qcom-geni-se drivers and include/linux/soc/qcom/geni-se.h (no code copied), and the GENI SPI driver of UniXNU
// (drivers/NativeGeniSpi). No board addresses, clocks or pins here; the caller supplies register accessors and owns the clocks.
// Host-tested by tests/geni-i2c-test.cpp against a register model.
#ifndef GENI_I2C_H
#define GENI_I2C_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Serial-engine registers (offsets from the SE base); the common ones match GeniSpi.h.
#define GI2C_FORCE_DEFAULT_REG 0x20
#define GI2C_OUTPUT_CTRL 0x24
#define GI2C_CGC_CTRL 0x28
#define GI2C_GENI_STATUS 0x40
#define GI2C_SER_M_CLK_CFG 0x48
#define GI2C_IF_DISABLE_RO 0x64
#define GI2C_FW_REVISION_RO 0x68
#define GI2C_CLK_SEL 0x7c
#define GI2C_BYTE_GRAN 0x254
#define GI2C_DMA_MODE_EN 0x258
#define GI2C_TX_PACKING_CFG0 0x260
#define GI2C_TX_PACKING_CFG1 0x264
#define GI2C_TX_TRANS_LEN 0x26c         // SE_I2C_TX_TRANS_LEN
#define GI2C_RX_TRANS_LEN 0x270         // SE_I2C_RX_TRANS_LEN
#define GI2C_SCL_COUNTERS 0x278         // SE_I2C_SCL_COUNTERS
#define GI2C_RX_PACKING_CFG0 0x284
#define GI2C_RX_PACKING_CFG1 0x288
#define GI2C_M_CMD0 0x600
#define GI2C_M_CMD_CTRL 0x604
#define GI2C_M_IRQ_STATUS 0x610
#define GI2C_M_IRQ_EN 0x614
#define GI2C_M_IRQ_CLEAR 0x618
#define GI2C_S_IRQ_EN 0x644
#define GI2C_S_IRQ_CLEAR 0x648
#define GI2C_TX_FIFOn 0x700
#define GI2C_RX_FIFOn 0x780
#define GI2C_TX_FIFO_STATUS 0x800
#define GI2C_RX_FIFO_STATUS 0x804
#define GI2C_TX_WATERMARK 0x80c
#define GI2C_RX_WATERMARK 0x810
#define GI2C_RX_RFR_WATERMARK 0x814
#define GI2C_IOS 0x908                  // SE_GENI_IOS: bit 0 RX_DATA_IN, bit 1 IO2_DATA_IN (the line levels)
#define GI2C_M_GP_LENGTH 0x910          // bytes the engine moved before a NACK: 0 = the address was not acknowledged
#define GI2C_DMA_TX_IRQ_CLR 0xc44
#define GI2C_DMA_RX_IRQ_CLR 0xd44
#define GI2C_GSI_EVENT_EN 0xe18
#define GI2C_IRQ_EN 0xe1c
#define GI2C_HW_PARAM_0 0xe24
#define GI2C_DMA_GENERAL_CFG 0xe30
// QUP wrapper register (offset from the wrapper base).
#define GI2C_QUP_HW_VER 0x4

#define GI2C_PROTO_I2C 3                // GENI_FW_REVISION_RO[15:8]
#define GI2C_FIFO_IF_DISABLE (1u<<0)

// M_CMD0: opcode in 31:27, parameters in 26:0. I2C parameters: bit 2 STOP_STRETCH (no STOP, the next command starts with a
// repeated START), slave address in 15:9.
#define GI2C_OP_WRITE 1u
#define GI2C_OP_READ 2u
#define GI2C_OP_ADDR_ONLY 4u
#define GI2C_OP_BUS_CLEAR 6u
#define GI2C_OPCODE_SHIFT 27
#define GI2C_STOP_STRETCH (1u<<2)
#define GI2C_ADDR_SHIFT 9
#define GI2C_ADDR_MASK (0x7fu<<9)
#define GI2C_CMD_ABORT (1u<<1)          // M_CMD_CTRL
#define GI2C_CMD_CANCEL (1u<<2)

// M_IRQ_STATUS / M_IRQ_EN bits.
#define GI2C_M_CMD_DONE (1u<<0)
#define GI2C_M_CMD_OVERRUN (1u<<1)
#define GI2C_M_ILLEGAL_CMD (1u<<2)
#define GI2C_M_CMD_FAILURE (1u<<3)
#define GI2C_M_CMD_CANCEL (1u<<4)
#define GI2C_M_CMD_ABORT (1u<<5)
#define GI2C_M_GP_IRQ0 (1u<<9)
#define GI2C_M_GP_IRQ1 (1u<<10)         // NACK (address or data, told apart by M_GP_LENGTH)
#define GI2C_M_GP_IRQ3 (1u<<12)         // bus protocol error
#define GI2C_M_GP_IRQ4 (1u<<13)         // arbitration lost
#define GI2C_M_GP_IRQS (0x3fu<<9)
#define GI2C_M_IO_DATA_DEASSERT (1u<<22)
#define GI2C_M_IO_DATA_ASSERT (1u<<23)
#define GI2C_M_RX_FIFO_RD_ERR (1u<<24)
#define GI2C_M_RX_FIFO_WR_ERR (1u<<25)
#define GI2C_M_RX_FIFO_WATERMARK (1u<<26)
#define GI2C_M_RX_FIFO_LAST (1u<<27)
#define GI2C_M_TX_FIFO_RD_ERR (1u<<28)
#define GI2C_M_TX_FIFO_WR_ERR (1u<<29)
#define GI2C_M_TX_FIFO_WATERMARK (1u<<30)
#define GI2C_M_COMMON_IRQ_EN (0x7eu | GI2C_M_IO_DATA_DEASSERT | GI2C_M_IO_DATA_ASSERT | GI2C_M_RX_FIFO_RD_ERR | \
                              GI2C_M_RX_FIFO_WR_ERR | GI2C_M_TX_FIFO_RD_ERR | GI2C_M_TX_FIFO_WR_ERR)
#define GI2C_S_COMMON_IRQ_EN (0x3eu | 0x3e00u | (1u<<24) | (1u<<25))
#define GI2C_M_FIFO_IRQ_EN (GI2C_M_CMD_DONE | GI2C_M_TX_FIFO_WATERMARK | GI2C_M_RX_FIFO_WATERMARK | GI2C_M_RX_FIFO_LAST)
#define GI2C_M_ERRORS (GI2C_M_CMD_OVERRUN | GI2C_M_ILLEGAL_CMD | GI2C_M_CMD_FAILURE | GI2C_M_CMD_ABORT | GI2C_M_GP_IRQ0 | \
                       GI2C_M_GP_IRQ1 | GI2C_M_GP_IRQ3 | GI2C_M_GP_IRQ4 | GI2C_M_RX_FIFO_RD_ERR | GI2C_M_RX_FIFO_WR_ERR | \
                       GI2C_M_TX_FIFO_RD_ERR | GI2C_M_TX_FIFO_WR_ERR)
// RX_FIFO_STATUS
#define GI2C_RX_WC_MASK 0x1ffffffu
// SE_IRQ_EN: DMA RX, DMA TX, M, S
#define GI2C_IRQ_EN_ALL 0xfu
#define GI2C_DMA_CGC_ALL 0xfu           // SE_DMA_GENERAL_CFG: RX, TX, AHB slave and AHB secure slave clock gating on
#define GI2C_DEFAULT_CGC_EN 0x7fu
#define GI2C_DEFAULT_IO_OUTPUT_CTRL 0x7fu
#define GI2C_SER_CLK_EN (1u<<0)
#define GI2C_CLK_DIV_SHIFT 4

// Errors (negative errno values, as the Linux driver reports them).
#define GI2C_ERR_PARAM (-22)            // EINVAL
#define GI2C_ERR_ADDR_NACK (-6)         // ENXIO: nobody acknowledged the address
#define GI2C_ERR_DATA_NACK (-121)       // EREMOTEIO: a data byte was not acknowledged
#define GI2C_ERR_BUS_PROTO (-71)        // EPROTO
#define GI2C_ERR_ARB_LOST (-11)         // EAGAIN
#define GI2C_ERR_IO (-5)                // overrun, illegal command, FIFO error, short transfer
#define GI2C_ERR_TIMEOUT (-110)
#define GI2C_ERR_NODEV (-19)            // FIFO geometry unusable
#define GI2C_ERR_PROTO (-93)            // EPROTONOSUPPORT: the engine is not loaded with I2C firmware
#define GI2C_ERR_NOTSUP (-95)           // FIFO interface fused off: only GPI DMA would work

struct geni_i2c_io {
    void *ctx;
    uint32_t (*rd)(void *ctx, uint32_t reg);
    void (*wr)(void *ctx, uint32_t reg, uint32_t value);
    void (*delay_us)(void *ctx, unsigned us);
};

struct geni_i2c_info {
    uint32_t fw_revision, protocol, if_disable, hw_param0, geni_status, qup_hw_version;
    unsigned tx_fifo_depth, fifo_width_bits;
    int fifo_disabled;
};

// One SCL timing set (i2c-qcom-geni.c geni_i2c_clk_map_*): the serial clock divider and the high, low and cycle counts.
struct geni_i2c_timing {unsigned bus_hz, div, t_high, t_low, t_cycle;};

struct geni_i2c {
    struct geni_i2c_io se;              // serial engine registers
    struct geni_i2c_io wrapper;         // QUP wrapper registers (hardware version only); rd may be null
    struct geni_i2c_info info;
    unsigned tx_wm;                     // TX words pushed per refill and the RX watermark (depth - 1)
    unsigned poll_limit;                // polls (1 us apart) of one transfer before it is cancelled
    int irq_outputs_off;                // 1: SE_IRQ_EN = 0 after init (completion is polled either way)
    struct geni_i2c_timing timing;
    unsigned clk_sel;
    int ready;                          // init and clock done
    // Counters and the register state of the first failure (read just before recovery).
    uint32_t xfers, nacks, timeouts, errors, recoveries;
    int first_err, last_err;
    uint32_t snap_irq, snap_status, snap_gp_len, snap_ios, snap_txfs, snap_rxfs, snap_cmd;
    int snap_valid;
};

// Read-only: protocol, FIFO geometry, FIFO-disable flag, versions. Only valid while the SE and wrapper AHB clocks run.
int geni_i2c_read_info(struct geni_i2c *g);
// geni_se_init + FIFO mode + 8-bit packing, as the Linux driver's probe. Refuses an engine without I2C firmware, with the FIFO
// interface fused off, or with an unusable FIFO.
int geni_i2c_init(struct geni_i2c *g);
// Timing for bus_hz from an SE source clock of src_hz (19.2 MHz or 32 MHz tables); clk_sel is the DFS level of that source.
const struct geni_i2c_timing *geni_i2c_find_timing(unsigned src_hz, unsigned bus_hz);
int geni_i2c_set_clock(struct geni_i2c *g, unsigned clk_sel, unsigned src_hz, unsigned bus_hz);
// One message of 1..0xffffff bytes. write: len bytes from tx; stop = 0 keeps the bus for a repeated START (STOP_STRETCH).
// read: len bytes into rx, then STOP. An unanswered address is GI2C_ERR_ADDR_NACK and needs no recovery.
int geni_i2c_write(struct geni_i2c *g, uint8_t addr, const uint8_t *tx, size_t len, int stop);
int geni_i2c_read(struct geni_i2c *g, uint8_t addr, uint8_t *rx, size_t len);
// Write tx (the register address), repeated START, read rx: the RMI register read.
int geni_i2c_write_read(struct geni_i2c *g, uint8_t addr, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen);
// Cancel, then abort, the current command (used after a timeout or an error other than an address NACK).
void geni_i2c_recover(struct geni_i2c *g);
// Packing vectors of 8-bit words, four per FIFO word, MSB first (exposed for the host test).
void geni_i2c_packing(uint32_t *cfg0, uint32_t *cfg1);

#ifdef __cplusplus
}
#endif
#endif
