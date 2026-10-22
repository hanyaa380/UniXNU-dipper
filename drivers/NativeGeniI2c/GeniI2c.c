// SPDX-License-Identifier: BSD-2-Clause-Patent
// GENI I2C master in FIFO mode, polled. The sequences are those of the Linux i2c-qcom-geni.c FIFO path (probe: geni_se_init with
// RX watermark depth-1 and RFR depth, 8-bit packing four per word MSB first; transfer: TRANS_LEN, M_CMD0, TX watermark 1 and a
// refill of depth-1 words per watermark event, RX drained on watermark/last; errors classified only together with CMD_FAILURE or
// CMD_ABORT, and every error but an address NACK cancelled), with the interrupt handler replaced by a poll of M_IRQ_STATUS.
#include "GeniI2c.h"

static uint32_t rd(struct geni_i2c *g, uint32_t reg) {return g->se.rd(g->se.ctx, reg);}
static void wr(struct geni_i2c *g, uint32_t reg, uint32_t v) {g->se.wr(g->se.ctx, reg, v);}
static void rmw_set(struct geni_i2c *g, uint32_t reg, uint32_t bits) {wr(g, reg, rd(g, reg) | bits);}
static void pause_us(struct geni_i2c *g, unsigned us) {if (g->se.delay_us) g->se.delay_us(g->se.ctx, us);}

int geni_i2c_read_info(struct geni_i2c *g) {
    if (!g || !g->se.rd || !g->se.wr) return GI2C_ERR_PARAM;
    struct geni_i2c_info *i = &g->info;
    i->fw_revision = rd(g, GI2C_FW_REVISION_RO);
    i->protocol = (i->fw_revision >> 8) & 0xff;
    i->if_disable = rd(g, GI2C_IF_DISABLE_RO);
    i->fifo_disabled = (i->if_disable & GI2C_FIFO_IF_DISABLE) != 0;
    i->geni_status = rd(g, GI2C_GENI_STATUS);
    i->hw_param0 = rd(g, GI2C_HW_PARAM_0);
    i->qup_hw_version = g->wrapper.rd ? g->wrapper.rd(g->wrapper.ctx, GI2C_QUP_HW_VER) : 0;
    // geni_se_get_tx_fifo_depth: QUP 3.10 and later widen the depth field to bits 23:16.
    uint32_t major = i->qup_hw_version >> 28, minor = (i->qup_hw_version >> 16) & 0xfff;
    uint32_t mask = ((major == 3 && minor >= 10) || major > 3) ? 0xff0000u : 0x3f0000u;
    i->tx_fifo_depth = (i->hw_param0 & mask) >> 16;
    i->fifo_width_bits = (i->hw_param0 >> 24) & 0x3f;
    return 0;
}

// geni_se_config_packing(se, 8, 4, msb_to_lsb = true): four 8-bit vectors, start bit 7, 15, 23, 31, direction MSB first,
// length 7, stop on the last.
void geni_i2c_packing(uint32_t *cfg0, uint32_t *cfg1) {
    uint32_t cfg[4];
    for (unsigned i = 0; i < 4; i++) cfg[i] = ((i * 8u + 7u) << 5) | (1u << 4) | (7u << 1);
    cfg[3] |= 1u;
    *cfg0 = cfg[0] | (cfg[1] << 10);
    *cfg1 = cfg[2] | (cfg[3] << 10);
}

static void irq_clear(struct geni_i2c *g) {   // geni_se_irq_clear
    wr(g, GI2C_GSI_EVENT_EN, 0);
    wr(g, GI2C_M_IRQ_CLEAR, 0xffffffffu);
    wr(g, GI2C_S_IRQ_CLEAR, 0xffffffffu);
    wr(g, GI2C_DMA_TX_IRQ_CLR, 0xffffffffu);
    wr(g, GI2C_DMA_RX_IRQ_CLR, 0xffffffffu);
    wr(g, GI2C_IRQ_EN, 0xffffffffu);
}

int geni_i2c_init(struct geni_i2c *g) {
    int r = geni_i2c_read_info(g);
    if (r) return r;
    if (g->info.protocol != GI2C_PROTO_I2C) return GI2C_ERR_PROTO;
    if (g->info.fifo_disabled) return GI2C_ERR_NOTSUP;
    if (g->info.tx_fifo_depth < 4 || g->info.fifo_width_bits != 32) return GI2C_ERR_NODEV;
    g->tx_wm = g->info.tx_fifo_depth - 1;
    // geni_se_init(se, tx_wm, depth)
    irq_clear(g);
    rmw_set(g, GI2C_CGC_CTRL, GI2C_DEFAULT_CGC_EN);
    rmw_set(g, GI2C_DMA_GENERAL_CFG, GI2C_DMA_CGC_ALL);
    wr(g, GI2C_OUTPUT_CTRL, GI2C_DEFAULT_IO_OUTPUT_CTRL);
    wr(g, GI2C_FORCE_DEFAULT_REG, 1);
    rmw_set(g, GI2C_IRQ_EN, GI2C_IRQ_EN_ALL);
    wr(g, GI2C_DMA_MODE_EN, rd(g, GI2C_DMA_MODE_EN) & ~1u);
    wr(g, GI2C_GSI_EVENT_EN, 0);
    wr(g, GI2C_RX_WATERMARK, g->tx_wm);
    wr(g, GI2C_RX_RFR_WATERMARK, g->info.tx_fifo_depth);
    rmw_set(g, GI2C_M_IRQ_EN, GI2C_M_COMMON_IRQ_EN);
    rmw_set(g, GI2C_S_IRQ_EN, GI2C_S_COMMON_IRQ_EN);
    // geni_se_config_packing(se, BITS_PER_BYTE, 4, true, true, true)
    uint32_t cfg0, cfg1;
    geni_i2c_packing(&cfg0, &cfg1);
    wr(g, GI2C_TX_PACKING_CFG0, cfg0);
    wr(g, GI2C_TX_PACKING_CFG1, cfg1);
    wr(g, GI2C_RX_PACKING_CFG0, cfg0);
    wr(g, GI2C_RX_PACKING_CFG1, cfg1);
    wr(g, GI2C_BYTE_GRAN, 0);
    // geni_se_select_mode(se, GENI_SE_FIFO); Linux repeats it per message, once is enough here.
    irq_clear(g);
    rmw_set(g, GI2C_M_IRQ_EN, GI2C_M_FIFO_IRQ_EN);
    wr(g, GI2C_DMA_MODE_EN, rd(g, GI2C_DMA_MODE_EN) & ~1u);
    if (g->irq_outputs_off) wr(g, GI2C_IRQ_EN, 0);
    if (!g->poll_limit) g->poll_limit = 20000;   // 20 ms at 1 us per poll: about 800 bytes at 400 kHz
    g->ready = 0;
    return 0;
}

// i2c-qcom-geni.c geni_i2c_clk_map_19p2mhz / _32mhz.
static const struct geni_i2c_timing timing_19p2[] = {{100000, 7, 10, 12, 26}, {400000, 2, 5, 11, 22}, {1000000, 1, 2, 8, 18}};
static const struct geni_i2c_timing timing_32[] = {{100000, 12, 9, 10, 26}, {400000, 4, 3, 9, 19}, {1000000, 2, 3, 5, 15}};

const struct geni_i2c_timing *geni_i2c_find_timing(unsigned src_hz, unsigned bus_hz) {
    const struct geni_i2c_timing *t = src_hz == 19200000u ? timing_19p2 : src_hz == 32000000u ? timing_32 : 0;
    if (!t) return 0;
    for (unsigned i = 0; i < 3; i++) if (t[i].bus_hz == bus_hz) return &t[i];
    return 0;
}

int geni_i2c_set_clock(struct geni_i2c *g, unsigned clk_sel, unsigned src_hz, unsigned bus_hz) {
    const struct geni_i2c_timing *t = geni_i2c_find_timing(src_hz, bus_hz);
    if (!g || !t || clk_sel > 15) return GI2C_ERR_PARAM;
    // qcom_geni_i2c_conf
    wr(g, GI2C_CLK_SEL, clk_sel);
    wr(g, GI2C_SER_M_CLK_CFG, (t->div << GI2C_CLK_DIV_SHIFT) | GI2C_SER_CLK_EN);
    wr(g, GI2C_SCL_COUNTERS, (t->t_high << 20) | (t->t_low << 10) | t->t_cycle);
    g->timing = *t;
    g->clk_sel = clk_sel;
    g->ready = 1;
    return 0;
}

static int wait_bit(struct geni_i2c *g, uint32_t bits, unsigned limit) {
    for (unsigned n = 0; n < limit; n++) {
        uint32_t st = rd(g, GI2C_M_IRQ_STATUS);
        if (st) wr(g, GI2C_M_IRQ_CLEAR, st);
        if (st & bits) return 0;
        pause_us(g, 1);
    }
    return GI2C_ERR_TIMEOUT;
}

void geni_i2c_recover(struct geni_i2c *g) {
    g->recoveries++;
    wr(g, GI2C_TX_WATERMARK, 0);
    const unsigned limit = g->poll_limit > 1000 ? g->poll_limit / 4 : 1000;
    wr(g, GI2C_M_CMD_CTRL, GI2C_CMD_CANCEL);                  // geni_se_cancel_m_cmd
    if (wait_bit(g, GI2C_M_CMD_CANCEL | GI2C_M_CMD_DONE, limit)) {
        wr(g, GI2C_M_CMD_CTRL, GI2C_CMD_ABORT);               // geni_se_abort_m_cmd
        (void)wait_bit(g, GI2C_M_CMD_ABORT | GI2C_M_CMD_DONE, limit);
    }
    // Words a cancelled read left behind must not reach the next message.
    for (unsigned n = 0; n < 256; n++) {
        uint32_t words = rd(g, GI2C_RX_FIFO_STATUS) & GI2C_RX_WC_MASK;
        if (!words) break;
        (void)rd(g, GI2C_RX_FIFOn);
    }
    wr(g, GI2C_M_IRQ_CLEAR, 0xffffffffu);
}

static void snapshot(struct geni_i2c *g) {
    g->snap_irq = rd(g, GI2C_M_IRQ_STATUS);
    g->snap_status = rd(g, GI2C_GENI_STATUS);
    g->snap_gp_len = rd(g, GI2C_M_GP_LENGTH);
    g->snap_ios = rd(g, GI2C_IOS);
    g->snap_txfs = rd(g, GI2C_TX_FIFO_STATUS);
    g->snap_rxfs = rd(g, GI2C_RX_FIFO_STATUS);
    g->snap_cmd = rd(g, GI2C_M_CMD0);
    g->snap_valid = 1;
}

// geni_i2c_irq's error branch: only a failed or aborted command carries an error; the general-purpose bits tell which.
static int classify(struct geni_i2c *g, uint32_t st, unsigned op) {
    if (!(st & (GI2C_M_CMD_FAILURE | GI2C_M_CMD_ABORT))) return 0;
    if (st & GI2C_M_GP_IRQ1) {
        uint32_t moved = rd(g, GI2C_M_GP_LENGTH);
        g->snap_gp_len = moved;
        if (!moved) return GI2C_ERR_ADDR_NACK;
        if (op == GI2C_OP_WRITE) return GI2C_ERR_DATA_NACK;
    }
    if (st & GI2C_M_GP_IRQ3) return GI2C_ERR_BUS_PROTO;
    if (st & GI2C_M_GP_IRQ4) return GI2C_ERR_ARB_LOST;
    return GI2C_ERR_IO;   // overrun, illegal command, abort, GP_IRQ0 or an unexplained failure
}

struct cursor {const uint8_t *tx; uint8_t *rx; size_t len, done;};

static void push(struct geni_i2c *g, struct cursor *c) {
    for (unsigned w = 0; w < g->tx_wm && c->done < c->len; w++) {
        uint32_t word = 0;
        for (unsigned b = 0; b < 4 && c->done < c->len; b++) word |= (uint32_t)c->tx[c->done++] << (8 * b);
        wr(g, GI2C_TX_FIFOn, word);
    }
    if (c->done == c->len) wr(g, GI2C_TX_WATERMARK, 0);   // all queued: no further refill events
}

static void drain(struct geni_i2c *g, struct cursor *c) {
    uint32_t words = rd(g, GI2C_RX_FIFO_STATUS) & GI2C_RX_WC_MASK;
    for (uint32_t w = 0; w < words; w++) {
        uint32_t val = rd(g, GI2C_RX_FIFOn);
        for (unsigned b = 0; b < 4 && c->done < c->len; b++) {c->rx[c->done++] = (uint8_t)val; val >>= 8;}
        if (c->done == c->len) break;
    }
}

static int message(struct geni_i2c *g, unsigned op, uint8_t addr, const uint8_t *tx, uint8_t *rx, size_t len, int stop) {
    if (!g || !g->ready || !len || len > 0xffffffu || addr > 0x7f || (op == GI2C_OP_WRITE ? !tx : !rx)) return GI2C_ERR_PARAM;
    g->xfers++;
    struct cursor c = {tx, rx, len, 0};
    wr(g, GI2C_M_IRQ_CLEAR, 0xffffffffu);
    wr(g, op == GI2C_OP_WRITE ? GI2C_TX_TRANS_LEN : GI2C_RX_TRANS_LEN, (uint32_t)len);
    wr(g, GI2C_M_CMD0, (op << GI2C_OPCODE_SHIFT) | (((uint32_t)addr << GI2C_ADDR_SHIFT) & GI2C_ADDR_MASK) |
                       (stop ? 0 : GI2C_STOP_STRETCH));
    if (op == GI2C_OP_WRITE) wr(g, GI2C_TX_WATERMARK, 1);   // the FIFO refill is driven by the watermark event
    int err = 0, done = 0;
    for (unsigned n = 0; n < g->poll_limit; n++) {
        uint32_t st = rd(g, GI2C_M_IRQ_STATUS);
        if (!st) {pause_us(g, 1); continue;}
        if (!err) err = classify(g, st, op);
        if (err) {
            if (op == GI2C_OP_WRITE) wr(g, GI2C_TX_WATERMARK, 0);   // stop feeding a failed write
        } else if (op == GI2C_OP_READ && (st & (GI2C_M_RX_FIFO_WATERMARK | GI2C_M_RX_FIFO_LAST))) {
            drain(g, &c);
        } else if (op == GI2C_OP_WRITE && (st & GI2C_M_TX_FIFO_WATERMARK)) {
            push(g, &c);
        }
        wr(g, GI2C_M_IRQ_CLEAR, st);
        if (st & GI2C_M_CMD_DONE) {done = 1; break;}
        // An error without CMD_DONE is left to the timeout, as in the Linux driver.
    }
    if (!done) {
        snapshot(g);
        if (!err) err = GI2C_ERR_TIMEOUT;
        g->timeouts++;
        geni_i2c_recover(g);
    } else {
        if (!err && op == GI2C_OP_READ && c.done < len) drain(g, &c);
        if (!err && c.done < len) err = GI2C_ERR_IO;   // the engine finished early
        if (err) {
            snapshot(g);
            if (err != GI2C_ERR_ADDR_NACK) geni_i2c_recover(g);
        }
    }
    if (err) {
        if (err == GI2C_ERR_ADDR_NACK || err == GI2C_ERR_DATA_NACK) g->nacks++;
        else g->errors++;
        if (!g->first_err) g->first_err = err;
        g->last_err = err;
    }
    return err;
}

int geni_i2c_write(struct geni_i2c *g, uint8_t addr, const uint8_t *tx, size_t len, int stop) {
    return message(g, GI2C_OP_WRITE, addr, tx, 0, len, stop);
}

int geni_i2c_read(struct geni_i2c *g, uint8_t addr, uint8_t *rx, size_t len) {
    return message(g, GI2C_OP_READ, addr, 0, rx, len, 1);
}

int geni_i2c_write_read(struct geni_i2c *g, uint8_t addr, const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxlen) {
    int r = message(g, GI2C_OP_WRITE, addr, tx, 0, txlen, 0);
    return r ? r : message(g, GI2C_OP_READ, addr, 0, rx, rxlen, 1);
}
