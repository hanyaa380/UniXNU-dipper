/*
 * The touch screen of the Xiaomi Mi 8 (dipper, SDM845): an ST FTS at address 0x49 on the I2C of
 * serial engine 14 (F-026). Only with boot-arg fts=1; boot-arg fts_steps=N stops after N steps.
 *
 * Steps 1 and 2 only read (nothing is written to a clock, a pin, a GENI register or a PMIC; the only
 * write is the command word that asks the PMIC arbiter for a read, as in redwood_buttons.c).
 * Steps 3 and 4 WRITE to pins, to the RPMh (one request for one regulator) and to the clock
 * controller (the engine's clock); each is read back before the next one is made, and a step that
 * does not read back stops the run.
 *   1  look at the clocks of the engine (GCC): reading the clock controller is safe whatever its state;
 *      only if the two AHB clocks of the QUP wrapper are on is the engine itself read (an unclocked
 *      access to the QUP can hang the bus), and then its firmware revision says which protocol it runs
 *   2  the state of what the chip needs: the pins (TLMM: interrupt 31, reset 32, SDA 33, SCL 34, the
 *      I/O supply switch 23) and the analog supply (PM8998 L28, through the PMIC arbiter)
 *   3  power the chip: reset low, I/O supply switch (GPIO23) high, L28 on (3.3 V, through RPMh, as
 *      the device tree's "ldoa28"), interrupt pin to an input, then reset released
 *   4  the engine's clock (crystal, undivided) and the two I2C pins muxed to the engine ("qup14")
 *   5  I2C: an address-only probe of 0x49 (ST FTS) and 0x4a (Atmel mXT), the chip's identity, and the first
 *      events of the ST chip's FIFO. The engine's FIFO interface is switched off by the boot firmware
 *      (GENI_IF_DISABLE_RO = 1, EXP-0068: words written to its TX FIFO vanish; only address-only commands
 *      work), so the bus is driven by hand on the two pins (open drain: low = drive, high = let go; the
 *      board has the pull-ups), as redwood_goodix.c does for its SPI when the engine cannot be used.
 *   6  instead of 5's probes: the chip as the FTM5 firmware it runs (EXP-0069): reset, wait for "controller
 *      ready", read its id and system info, switch scanning on, then watch the interrupt pin and post the
 *      touches to /dev/redwood-touch. Boot-args: fts_watch=S stops after S seconds (0, the default: never),
 *      fts_poll_us=N (default 4000), fts_raw=N raw events printed in full (default 40), fts_orient=B (bit 0
 *      mirrors x, bit 1 mirrors y, bit 2 swaps them, before the mirroring), fts_i2c=dma: try the engine's
 *      own DMA after "controller ready" instead of the pins by hand (TASK-001; falls back to them on any error).
 *
 * Addresses and layouts: the phone's own device tree (i2c@a98000, pinctrl@3400000, gcc@100000), Linux
 * drivers/clk/qcom/gcc-sdm845.c, drivers/pinctrl/qcom/pinctrl-sdm845.c, drivers/soc/qcom/qcom-geni-se.c,
 * include/linux/soc/qcom/geni-se.h, and bsd/dev/redwood_buttons.c for the arbiter.
 */
#include <arm/caches_internal.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern int printf(const char *, ...);
extern int vsnprintf(char *, size_t, const char *, va_list);
extern size_t strlcpy(char *, const char *, size_t);
extern size_t strlen(const char *);
extern int strcmp(const char *, const char *);
extern int memcmp(const void *, const void *, size_t);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);

/* Publication contract consumed by the original IOHIDDevice in NativeHidInput. */
#define MI8_TOUCH_SLOTS 12
volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[MI8_TOUCH_SLOTS], native_touch_ys[MI8_TOUCH_SLOTS], native_touch_ids[MI8_TOUCH_SLOTS];
static void (*native_touch_sink)(void *);
static void *native_touch_sink_context;
void native_touch_set_sink(void (*sink)(void *), void *context);

enum {
	REDWOOD_TOUCH_DOWN = 1,
	REDWOOD_TOUCH_MOVE = 2,
	REDWOOD_TOUCH_UP = 3
};
void redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	return printf("%s", line);
}
#define printf say

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

/* ---- the clock controller ------------------------------------------------------------------------ */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x001f0000UL
#define GCC_VOTE_APPS           0x5200c         /* the apps processor's enables of several branches */
#define VOTE_WRAP1_M_AHB        (1u << 20)
#define VOTE_WRAP1_S_AHB        (1u << 21)
#define VOTE_WRAP1_S6           (1u << 28)      /* the engine: SE14 is the 7th of the second wrapper */
#define GCC_WRAP1_M_AHB_HALT    0x1800c
#define GCC_WRAP1_S_AHB_HALT    0x18010
#define GCC_WRAP1_S6_HALT       0x18734
#define GCC_WRAP1_S6_RCG        0x18738         /* [31] root off; CFG at +4, M, N, D after */
#define CBCR_OFF                (1u << 31)      /* the branch is off (for the voted ones: not running) */

static vm_offset_t gcc;

static bool
branch_on(const char *name, uint32_t halt_reg)
{
	uint32_t v = rd(gcc + halt_reg);

	printf("fts: clock %-26s halt register %#07x = %#010x: %s\n", name, halt_reg, v, (v & CBCR_OFF) ? "OFF" : "on");
	return !(v & CBCR_OFF);
}

/* ---- the engine ----------------------------------------------------------------------------------- */

#define SE14_PHYS               0x00a98000UL
#define SE14_SIZE               0x4000UL
#define QUP1_PHYS               0x00ac0000UL    /* the wrapper: its hardware version at 4 */
#define QUP1_SIZE               0x6000UL
#define GENI_STATUS             0x40
#define GENI_FW_REVISION_RO     0x68            /* [15:8] protocol (1 spi, 2 uart, 3 i2c, 4 i3c), [7:0] version */
#define GENI_CLK_SEL            0x7c
#define GENI_DMA_MODE_EN        0x258
#define HW_PARAM_0              0xe24
#define HW_PARAM_1              0xe28

/* ---- the pins ------------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x03400000UL
#define TLMM_NORTH              0x00500000UL    /* pins 31 to 34; each pin has 0x1000 of registers in its tile */
#define TLMM_SOUTH              0x00900000UL    /* pin 23 */

/* Only the pages of the pins that are looked at: the whole block is 12 MB and the kernel cannot map that at once. */
static vm_offset_t pins_north, pins_south;

static void
pin_look(const char *what, unsigned pin, vm_offset_t tile)
{
	vm_offset_t base = (tile == TLMM_NORTH ? pins_north + 0x1000UL * (pin - 31) : pins_south + 0x1000UL * (pin - 23));
	uint32_t cfg = rd(base), io = rd(base + 4);

	printf("fts: pin %2u (%s): config %#05x = pull %u, function %u, drive %u mA, %s; level in %u, out %u\n", pin, what, cfg,
	    cfg & 3, (cfg >> 2) & 15, 2 * (((cfg >> 6) & 7) + 1), (cfg & (1u << 9)) ? "output" : "input", io & 1, (io >> 1) & 1);
}

/* ---- the PMIC arbiter (reads only) ---------------------------------------------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_VERSION             0x000
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18
#define STATUS_DONE             0x1
#define COMMAND_READ            (1u << 27)

static vm_offset_t arb_core, arb_cnfg, arb_observer;

/* The channel of a peripheral (slave << 8 | peripheral), as redwood_buttons.c chooses; -1 = none. */
static int
find_channel(uint16_t id, uint32_t channels)
{
	int found = -1;

	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		if (map != 0 && ((map >> 8) & 0xfff) == id && (found < 0 || (rd(arb_cnfg + ARB_OWNER(n)) & 7) == 0)) {
			found = (int)n;
		}
	}
	return found;
}

/* This kernel has no other PMIC-arbiter client, so the Mi 8 probe owns the observer.
 * Keep the calls named after the reference lock to make the shared-block assumption explicit. */
static void redwood_spmi_lock(void) {}
static void redwood_spmi_unlock(void) {}

static bool
spmi_read(int channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t ch = arb_observer + 0x80UL * (uint32_t)channel;
	uint64_t start = mach_absolute_time(), now, interval;
	uint32_t status, data[2];

	nanoseconds_to_absolutetime(2000 * (uint64_t)NSEC_PER_USEC, &interval);
	redwood_spmi_lock();
	*(volatile uint32_t *)(ch + OBSERVER_COMMAND) = COMMAND_READ | (uint32_t)reg << 4 | (count - 1);
	do {
		status = rd(ch + OBSERVER_STATUS);
		now = mach_absolute_time();
	} while (!(status & STATUS_DONE) && now - start < interval);
	data[0] = rd(ch + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(ch + 0x1c) : 0;
	redwood_spmi_unlock();
	if ((status & 0xf) != STATUS_DONE) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return true;
}

/* An LDO of the PM8998 (slave 0): its type and subtype, the status, and the enable register. */
static int
ldo_look(const char *name, uint8_t peripheral, uint32_t channels)
{
	uint16_t id = peripheral;       /* slave 0 */
	int channel = find_channel(id, channels);
	uint8_t type[2], status[8], enable;

	if (channel < 0) {
		printf("fts: %s: no channel of the arbiter is slave 0 peripheral %#x\n", name, peripheral);
		return -1;
	}
	if (!spmi_read(channel, 0x04, type, 2) || !spmi_read(channel, 0x08, status, 8) || !spmi_read(channel, 0x46, &enable, 1)) {
		printf("fts: %s (peripheral %#x, channel %d): a read failed\n", name, peripheral, channel);
		return -1;
	}
	printf("fts: %s (peripheral %#x, channel %d): type %#x subtype %#x; registers 0x08.. = %02x %02x %02x %02x %02x %02x %02x %02x; "
	    "enable (0x46) %#x\n", name, peripheral, channel, type[0], type[1], status[0], status[1], status[2], status[3], status[4],
	    status[5], status[6], status[7], enable);
	return enable;
}

/* ---- writes (steps 3 and 4) ----------------------------------------------------------------------- */

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static vm_offset_t
pin_base(unsigned pin)
{
	return pin == 23 ? pins_south : pins_north + 0x1000UL * (pin - 31);
}

/* config word: [1:0] pull (3 up), [5:2] function, [8:6] drive (mA / 2 - 1), [9] output; level: IO register bit 1 */
static void
pin_set(unsigned pin, uint32_t cfg, int level)
{
	vm_offset_t base = pin_base(pin);

	if (level >= 0) {
		wr(base + 4, level ? 2 : 0);    /* the level first, so that the pin never drives the wrong one */
	}
	wr(base, cfg);
}

/* The RPMh: the application processor's "DRV 2" and the command database the boot loader left. */
#define RSC_PHYS                0x179e0000UL
#define RSC_SIZE                0x10000UL
#define CMD_DB_PHYS             0x85fe0000UL    /* mainline sdm845.dtsi; inside the DT's removed_region@85fc0000; 0x861e0000 (the DT node name) is empty (EXP-0067) */
#define CMD_DB_SIZE             0x20000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))   /* command slot set 0 (an "active" one: qcom,tcs-config) */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

static vm_offset_t rsc, cmd_db;

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database; 0 = none (as in redwood_goodix.c). */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("fts: no command database at %#lx (magic %02x %02x %02x %02x)\n", CMD_DB_PHYS, db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || (uint32_t)slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	printf("fts: RPMh: timeout (register %#x = %#x)\n", reg, rd(TCS(reg)));
	return false;
}

/* One request of `count` commands through the first command slot set, as redwood_goodix.c does. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("fts: RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1);
	for (unsigned i = 0; i < count; i++) {
		printf("fts: RPMh: command %u: address %#x data %u, status %#x, response %#x\n", i, rd(TCS(TCS_CMD_ADDR(i))),
		    rd(TCS(TCS_CMD_DATA(i))), rd(TCS(TCS_CMD_ADDR(i) + 8)), rd(TCS(TCS_CMD_ADDR(i) + 12)));
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

#define PIN_IRQ                 31
#define PIN_RESET               32
#define PIN_SDA                 33
#define PIN_SCL                 34
#define PIN_VDDIO               23
#define CFG_INPUT_PULLUP_16MA   0x1c3           /* the device tree's ts_active: gpio, pull up, 16 mA */
#define CFG_OUTPUT_PULLUP_16MA  0x3c3
#define CFG_QUP14_2MA           0x008           /* function 2 ("qup14"), 2 mA, no pull */

static bool
pin_level(unsigned pin)
{
	return rd(pin_base(pin) + 4) & 1;
}


/* Step 3. */
static bool
power_on(uint32_t channels)
{
	uint32_t base, address[3], data[3] = { 3300, 1, VRM_MODE_LDO_HPM };

	(void)channels;
	printf("fts: step 3: power. reset low, interrupt pin an input\n");
	pin_set(PIN_RESET, CFG_OUTPUT_PULLUP_16MA, 0);
	pin_set(PIN_IRQ, CFG_INPUT_PULLUP_16MA, -1);
	sleep_ms(1);
	pin_look("touch interrupt", PIN_IRQ, TLMM_NORTH);
	pin_look("touch reset", PIN_RESET, TLMM_NORTH);

	printf("fts: the I/O supply switch (GPIO23) high\n");
	pin_set(PIN_VDDIO, 0x203 | 0, 1);
	sleep_ms(5);
	pin_look("I/O supply switch", PIN_VDDIO, TLMM_SOUTH);
	if (!pin_level(PIN_VDDIO)) {
		printf("fts: GPIO23 does not read back high: stopping\n");
		return false;
	}

	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	base = rpmh_address("ldoa28");
	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("fts: RPMh: no regulator \"ldoa28\" (address %#x): stopping\n", base);
		return false;
	}
	address[0] = base + VRM_VOLTAGE;
	address[1] = base + VRM_ENABLE;
	address[2] = base + VRM_MODE;
	printf("fts: RPMh: ldoa28 is at %#x; slot status %#x, control %#x, irq enable %#x status %#x\n", base, rd(TCS(TCS_STATUS)),
	    rd(TCS(TCS_CONTROL)), rd(TCS(TCS_IRQ_ENABLE)), rd(TCS(TCS_IRQ_STATUS)));
	if (!rpmh_write(address, data, 3)) {
		printf("fts: RPMh: the request for L28 did not complete: stopping\n");
		return false;
	}
	printf("fts: RPMh: L28 requested on, 3300 mV\n");
	sleep_ms(20);
	/*
	 * No read back: the regulators the RPMh manages belong to the always-on processor, and the peripheral
	 * guessed for L28 (slave 0, 0x5b) is not it (EXP-0067: nothing at 0x50-0x62 looks like a 3.3 V LDO, and no
	 * peripheral of slaves 0 and 1 changed). The proof is the chip answering on the bus (step 5).
	 */

	sleep_ms(10);
	printf("fts: reset released\n");
	pin_set(PIN_RESET, CFG_OUTPUT_PULLUP_16MA, 1);
	sleep_ms(100);
	pin_look("touch interrupt", PIN_IRQ, TLMM_NORTH);
	pin_look("touch reset", PIN_RESET, TLMM_NORTH);
	return true;
}

/* Step 4. */
static bool
clock_and_pins(void)
{
	uint32_t cfg;

	printf("fts: step 4: the engine's clock and the I2C pins\n");
	/* the crystal, undivided, as redwood_goodix.c does for its engine: nothing else has to run */
	cfg = rd(gcc + GCC_WRAP1_S6_RCG + 4);
	wr(gcc + GCC_WRAP1_S6_RCG + 4, (cfg & ~0x371fu) | 1);
	wr(gcc + GCC_WRAP1_S6_RCG, rd(gcc + GCC_WRAP1_S6_RCG) | 1);
	for (int i = 0; i < 500 && (rd(gcc + GCC_WRAP1_S6_RCG) & 1); i++) {
		sleep_us(10);
	}
	printf("fts: the engine's clock source: command %#010x, config %#010x\n", rd(gcc + GCC_WRAP1_S6_RCG),
	    rd(gcc + GCC_WRAP1_S6_RCG + 4));
	if (rd(gcc + GCC_WRAP1_S6_RCG) & 1) {
		printf("fts: the clock source does not take its configuration: stopping\n");
		return false;
	}
	wr(gcc + GCC_VOTE_APPS, rd(gcc + GCC_VOTE_APPS) | VOTE_WRAP1_M_AHB | VOTE_WRAP1_S_AHB | VOTE_WRAP1_S6);
	for (int i = 0; i < 1000 && (rd(gcc + GCC_WRAP1_S6_HALT) & CBCR_OFF); i++) {
		sleep_us(10);
	}
	printf("fts: votes now %#010x\n", rd(gcc + GCC_VOTE_APPS));
	if (!branch_on("QUPV3_WRAP1_S6 (SE14)", GCC_WRAP1_S6_HALT)) {
		printf("fts: the engine's clock stays off: stopping\n");
		return false;
	}
	pin_set(PIN_SDA, CFG_QUP14_2MA, -1);
	pin_set(PIN_SCL, CFG_QUP14_2MA, -1);
	sleep_ms(1);
	pin_look("I2C SDA, qup14", PIN_SDA, TLMM_NORTH);
	pin_look("I2C SCL, qup14", PIN_SCL, TLMM_NORTH);
	return true;
}

/* ---- I2C by hand on the two pins ------------------------------------------------------------------- */

#define CFG_GPIO_RELEASED       0x000           /* gpio, input, no pull */
#define CFG_GPIO_LOW            0x200           /* gpio, output (level 0 is set first) */

static bool use_bitbang;

static uint32_t bb_us = 5;      /* half a clock period */

/*
 * Busy waits: delay_for_interval() blocks for any interval here, however short, because nothing sets
 * cpu_idle_latency on this platform (ml_delay_should_spin(), osfmk/arm64/machine_routines.c), so each
 * half clock was a sleep and two thread switches: 3.4 ms per event, 30-43% of the one CPU while a
 * finger moved (EXP-0070).
 */
static void
spin_us(uint32_t us)
{
	uint64_t ticks, deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &ticks);
	deadline = mach_absolute_time() + ticks;
	while (mach_absolute_time() < deadline) {
	}
}

static void
bb_wait(void)
{
	spin_us(bb_us);
}

static void
sda_release(void)
{
	pin_set(PIN_SDA, CFG_GPIO_RELEASED, -1);
}

static void
sda_low(void)
{
	pin_set(PIN_SDA, CFG_GPIO_LOW, 0);
}

static void
scl_low(void)
{
	pin_set(PIN_SCL, CFG_GPIO_LOW, 0);
}

/* lets go of the clock and waits for the chip to let go as well (it may stretch it); counts how long and how often */
static uint32_t bb_stretch_max_us, bb_stretch_timeouts, bb_stretches;
static const char *bb_where = "idle";
static bool bb_error;

static bool
scl_release(void)
{
	uint64_t start = mach_absolute_time(), now, taken;

	pin_set(PIN_SCL, CFG_GPIO_RELEASED, -1);
	/* the line rises within microseconds unless the chip holds it: spin for 200 us, then sleep in steps */
	for (int i = 0; i < 200 && !pin_level(PIN_SCL); i++) {
		spin_us(1);
	}
	for (int i = 0; i < 10000; i++) {         /* up to 100 ms: this chip stretches for tens of milliseconds (EXP-0068) */
		if (pin_level(PIN_SCL)) {
			now = mach_absolute_time();
			absolutetime_to_nanoseconds(now - start, &taken);
			if (taken > 20000) {
				bb_stretches++;
			}
			if (taken / 1000 > bb_stretch_max_us) {
				bb_stretch_max_us = (uint32_t)(taken / 1000);
			}
			return true;
		}
		sleep_us(10);
	}
	bb_stretch_timeouts++;
	if (!bb_error) {
		printf("fts: SCL stays low for 100 ms (during %s; SDA %s)\n", bb_where, pin_level(PIN_SDA) ? "high" : "LOW");
	}
	bb_error = true;
	return false;
}

static void
bb_start(void)
{
	bb_where = "a start";
	sda_release();
	scl_release();
	bb_wait();
	sda_low();
	bb_wait();
	scl_low();
}

static void
bb_stop(void)
{
	bb_where = "a stop";
	sda_low();
	bb_wait();
	scl_release();
	bb_wait();
	sda_release();
	bb_wait();
}

/* returns true if the chip pulled SDA low for the ninth clock */
static bool
bb_put(uint8_t byte)
{
	bool ack;

	bb_where = "a byte written";
	for (int bit = 7; bit >= 0; bit--) {
		if (byte >> bit & 1) {
			sda_release();
		} else {
			sda_low();
		}
		bb_wait();
		scl_release();
		bb_wait();
		scl_low();
	}
	sda_release();
	bb_wait();
	scl_release();
	bb_wait();
	ack = !pin_level(PIN_SDA);
	scl_low();
	return ack;
}

static uint8_t
bb_get(bool ack)
{
	uint8_t byte = 0;

	bb_where = "a byte read";
	sda_release();
	for (int bit = 0; bit < 8; bit++) {
		bb_wait();
		scl_release();
		bb_wait();
		byte = (uint8_t)(byte << 1 | pin_level(PIN_SDA));
		scl_low();
	}
	if (ack) {
		sda_low();
	} else {
		sda_release();
	}
	bb_wait();
	scl_release();
	bb_wait();
	scl_low();
	sda_release();
	return byte;
}

/* After SCL stayed low: let go of both lines and clock nine times, then a stop (the usual way to free a stuck chip). */
static void
bb_recover(void)
{
	bb_error = false;
	sda_release();
	for (int i = 0; i < 9; i++) {
		scl_low();
		bb_wait();
		scl_release();
		bb_wait();
	}
	bb_error = false;
	bb_stop();
	bb_error = false;
}

static int
bb_probe(uint8_t address)
{
	bool ack;

	bb_error = false;
	bb_start();
	ack = bb_put((uint8_t)(address << 1));
	bb_stop();
	if (bb_error) {
		bb_recover();
		return 2;
	}
	return ack ? 0 : 1;
}

static bool
bb_write(uint8_t address, const uint8_t *data, uint32_t length, bool stretch)
{
	bool ok;

	bb_error = false;
	bb_start();
	ok = bb_put((uint8_t)(address << 1));
	for (uint32_t i = 0; ok && !bb_error && i < length; i++) {
		ok = bb_put(data[i]);
	}
	if (!stretch || !ok || bb_error) {
		bb_stop();
	}
	if (bb_error) {
		bb_recover();
		return false;
	}
	return ok;      /* with stretch: no stop, the read that follows starts again */
}

static bool
bb_read(uint8_t address, uint8_t *data, uint32_t length)
{
	bool ok;

	bb_error = false;
	bb_start();
	ok = bb_put((uint8_t)(address << 1 | 1));
	for (uint32_t i = 0; ok && !bb_error && i < length; i++) {
		data[i] = bb_get(i + 1 < length);
	}
	bb_stop();
	if (bb_error) {
		bb_recover();
		return false;
	}
	return ok;
}

/* ---- the engine as an I2C master (step 5) --------------------------------------------------------- */

#define GENI_FORCE_DEFAULT      0x020
#define GENI_OUTPUT_CTRL        0x024
#define GENI_CGC_CTRL           0x028
#define GENI_SER_M_CLK_CFG      0x048           /* bit 0 enable, [15:4] divider */
#define GENI_TX_PACKING_CFG0    0x260
#define GENI_TX_PACKING_CFG1    0x264
#define GENI_RX_PACKING_CFG0    0x284
#define GENI_RX_PACKING_CFG1    0x288
#define GENI_BYTE_GRAN          0x254
#define I2C_TX_TRANS_LEN        0x26c
#define I2C_RX_TRANS_LEN        0x270
#define I2C_SCL_COUNTERS        0x278           /* [29:20] high, [19:10] low, [9:0] cycle */
#define GENI_M_CMD0             0x600           /* [31:27] operation, [15:9] address, bit 2 stop stretch */
#define GENI_M_CMD_CTRL         0x604           /* bit 2 cancel, bit 1 abort */
#define GENI_M_IRQ_STATUS       0x610
#define GENI_M_IRQ_EN           0x614
#define GENI_M_IRQ_CLEAR        0x618
#define GENI_S_IRQ_CLEAR        0x648
#define GENI_TX_FIFO            0x700
#define GENI_RX_FIFO            0x780
#define GENI_TX_FIFO_STATUS     0x800
#define GENI_RX_FIFO_STATUS     0x804           /* bit 31 last, [30:28] valid bytes of the last word, [24:0] words */
#define GENI_TX_WATERMARK       0x80c
#define GENI_RX_WATERMARK       0x810
#define GENI_RX_RFR_WATERMARK   0x814
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_DMA_GENERAL_CFG      0xe30

#define M_CMD_DONE              (1u << 0)
#define M_TX_WATERMARK          (1u << 30)
#define M_CMD_FAILURE           (1u << 3)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_NACK                  (1u << 10)      /* GP IRQ 1 */
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13))
#define I2C_WRITE               1u
#define I2C_READ                2u
#define I2C_ADDR_ONLY           4u
#define I2C_STOP_STRETCH        (1u << 2)       /* no stop after this message: the next one starts again */

static vm_offset_t se14;
static uint32_t fifo_depth;

static inline uint32_t
se_rd(uint32_t reg)
{
	return rd(se14 + reg);
}

static inline void
se_wr(uint32_t reg, uint32_t value)
{
	wr(se14 + reg, value);
}

static void
i2c_recover(void)
{
	se_wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(se_rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(se_rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		se_wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(se_rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (se_rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)se_rd(GENI_RX_FIFO);
	}
	se_wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* As geni_se_init() and qcom_geni_i2c_conf() of Linux do, at 100 kHz from the 19.2 MHz crystal (its table: 7, 10, 12, 26). */
static const char *
i2c_init(void)
{
	uint32_t fw = se_rd(GENI_FW_REVISION_RO);

	if (((fw >> 8) & 0xff) != 3) {
		return "the serial engine has no I2C firmware";
	}
	fifo_depth = se_rd(HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	se_wr(SE_GSI_EVENT_EN, 0);
	se_wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	se_wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	se_wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	se_wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	se_wr(GENI_CGC_CTRL, se_rd(GENI_CGC_CTRL) | 0x7f);
	se_wr(SE_DMA_GENERAL_CFG, se_rd(SE_DMA_GENERAL_CFG) | 0xf);
	se_wr(GENI_OUTPUT_CTRL, 0x7f);
	se_wr(GENI_FORCE_DEFAULT, 1);
	se_wr(GENI_DMA_MODE_EN, se_rd(GENI_DMA_MODE_EN) & ~1u);      /* FIFO mode */
	se_wr(GENI_RX_WATERMARK, fifo_depth - 1);       /* as geni_i2c_init(): tx_wm = depth - 1, rfr = depth */
	se_wr(GENI_RX_RFR_WATERMARK, fifo_depth);
	se_wr(GENI_TX_WATERMARK, 0);
	se_wr(GENI_BYTE_GRAN, 0);                        /* four 8-bit words to a FIFO entry */
	se_wr(SE_IRQ_EN, se_rd(SE_IRQ_EN) | 0xf);
	se_wr(GENI_M_IRQ_EN, se_rd(GENI_M_IRQ_EN) | 0x7fc0007f);
	/* 8 bits per byte, four bytes to a FIFO word, as Linux's geni_se_config_packing(8, 4, ...) */
	se_wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	se_wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	se_wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	se_wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	se_wr(GENI_CLK_SEL, 0);
	se_wr(GENI_SER_M_CLK_CFG, 7u << 4 | 1);
	se_wr(I2C_SCL_COUNTERS, 10u << 20 | 12u << 10 | 26u);
	return NULL;
}

/*
 * Wait for a command to end: returns the interrupt status (done, or an error), 0 on a timeout. A NACK
 * ends in "failure" with the done bit not set: the caller recovers.
 */
static uint32_t
i2c_wait(uint32_t timeout_us)
{
	for (uint32_t waited = 0; waited < timeout_us; waited += 20) {
		uint32_t status = se_rd(GENI_M_IRQ_STATUS);

		if (status & (M_CMD_DONE | M_ERRORS)) {
			return status;
		}
		sleep_us(20);
	}
	return 0;
}

static bool
i2c_idle(void)
{
	for (int i = 0; se_rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			i2c_recover();
			break;
		}
		sleep_us(100);
	}
	se_wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	return true;
}

/* 0 = acknowledged, 1 = nobody answers (NACK), 2 = something else went wrong */
static int
i2c_probe(uint8_t address)
{
	uint32_t status;

	if (use_bitbang) {
		return bb_probe(address);
	}

	i2c_idle();
	se_wr(GENI_M_CMD0, I2C_ADDR_ONLY << 27 | (uint32_t)address << 9);
	status = i2c_wait(50000);
	if ((status & M_CMD_DONE) && !(status & M_ERRORS)) {
		se_wr(GENI_M_IRQ_CLEAR, status);
		return 0;
	}
	printf("fts: probe %#04x: interrupt status %#010x%s\n", address, status, (status & M_NACK) ? " (NACK)" : "");
	i2c_recover();
	return (status & M_NACK) ? 1 : 2;
}

/*
 * Linux qcom-geni-se.c: geni_se_tx_init_dma()/geni_se_rx_init_dma();
 * geni-se.h supplies IRQ status/reset offsets and bits. Keep the SE IRQ output
 * gated because this driver has no interrupt handler (a guess: the raw status still latches).
 */
#define GENI_IF_DISABLE_RO      0x064
#define SE_DMA_TX_PTR_L         0xc30
#define SE_DMA_TX_PTR_H         0xc34
#define SE_DMA_TX_ATTR          0xc38
#define SE_DMA_TX_LEN           0xc3c
#define SE_DMA_TX_IRQ_STAT      0xc40
#define SE_DMA_TX_IRQ_EN_SET    0xc4c
#define SE_DMA_TX_IRQ_EN_CLR    0xc50
#define SE_DMA_TX_LEN_IN        0xc54
#define SE_DMA_TX_FSM_RST       0xc58
#define SE_DMA_RX_PTR_L         0xd30
#define SE_DMA_RX_PTR_H         0xd34
#define SE_DMA_RX_ATTR          0xd38
#define SE_DMA_RX_LEN           0xd3c
#define SE_DMA_RX_IRQ_STAT      0xd40
#define SE_DMA_RX_IRQ_EN_SET    0xd4c
#define SE_DMA_RX_IRQ_EN_CLR    0xd50
#define SE_DMA_RX_LEN_IN        0xd54
#define SE_DMA_RX_FSM_RST       0xd58
#define DMA_DONE                (1u << 0)
#define DMA_AHB_ERR             (1u << 2)
#define DMA_RESET_DONE          (1u << 3)
#define DMA_IRQ_ENABLE          7u
#define DMA_TX_EOT_BUF          1u
/* Linux i2c-qcom-geni.c: DM_I2C_CB_ERR, geni_i2c_irq(). */
#define DMA_RX_I2C_ERRORS       ((1u << 6) | (1u << 8) | (1u << 9))
#define DMA_M_ERRORS            (M_ERRORS | M_CMD_CANCEL | M_CMD_ABORT | (0xfu << 24))
#define DMA_TIMEOUT_US          50000u
#define DMA_BUFFER_SIZE         (PAGE_SIZE / 2)

static bool use_dma, dma_trace;
static vm_offset_t dma_page;
static uint64_t dma_phys;
static uint8_t dma_poll_event;

static uint64_t
i2c_dma_deadline(void)
{
	uint64_t ticks;

	nanoseconds_to_absolutetime((uint64_t)DMA_TIMEOUT_US * NSEC_PER_USEC, &ticks);
	return mach_absolute_time() + ticks;
}

static void
i2c_dma_pause(uint64_t deadline)
{
	uint64_t next;

	/* Explicitly block: delay_for_interval() may choose to spin on other platforms. */
	clock_interval_to_deadline(100, NSEC_PER_USEC, &next);
	if (next > deadline) {
		next = deadline;
	}
	if (mach_absolute_time() < next &&
	    assert_wait_deadline((event_t)&dma_poll_event, THREAD_UNINT, next) == THREAD_WAITING) {
		thread_block(THREAD_CONTINUE_NULL);
	}
}

static void
i2c_dma_dump(const char *why)
{
	printf("fts: DMA %s: GENI_STATUS=%#x GENI_M_IRQ_STATUS=%#x GENI_IF_DISABLE_RO=%#x DMA_MODE=%#x\n",
	    why, se_rd(GENI_STATUS), se_rd(GENI_M_IRQ_STATUS), se_rd(GENI_IF_DISABLE_RO), se_rd(GENI_DMA_MODE_EN));
	printf("fts: DMA TX: SE_DMA_TX_IRQ_STAT=%#x SE_DMA_TX_LEN_IN=%u LEN=%u PTR=%08x:%08x ATTR=%#x\n",
	    se_rd(SE_DMA_TX_IRQ_STAT), se_rd(SE_DMA_TX_LEN_IN), se_rd(SE_DMA_TX_LEN),
	    se_rd(SE_DMA_TX_PTR_H), se_rd(SE_DMA_TX_PTR_L), se_rd(SE_DMA_TX_ATTR));
	printf("fts: DMA RX: SE_DMA_RX_IRQ_STAT=%#x SE_DMA_RX_LEN_IN=%u LEN=%u PTR=%08x:%08x ATTR=%#x\n",
	    se_rd(SE_DMA_RX_IRQ_STAT), se_rd(SE_DMA_RX_LEN_IN), se_rd(SE_DMA_RX_LEN),
	    se_rd(SE_DMA_RX_PTR_H), se_rd(SE_DMA_RX_PTR_L), se_rd(SE_DMA_RX_ATTR));
	printf("fts: DMA CMD=%#x TX_TRANS_LEN=%u RX_TRANS_LEN=%u SE_IRQ_EN=%#x GSI_EVENT_EN=%#x SDA=%u SCL=%u\n",
	    se_rd(GENI_M_CMD0), se_rd(I2C_TX_TRANS_LEN), se_rd(I2C_RX_TRANS_LEN),
	    se_rd(SE_IRQ_EN), se_rd(SE_GSI_EVENT_EN), pin_level(PIN_SDA), pin_level(PIN_SCL));
}

static bool
i2c_dma_wait_bits(uint32_t reg, uint32_t mask)
{
	uint64_t deadline = i2c_dma_deadline();

	for (;;) {
		if ((se_rd(reg) & mask) == mask) {
			return true;
		}
		if (mach_absolute_time() >= deadline) {
			return false;
		}
		i2c_dma_pause(deadline);
	}
}

static bool
i2c_dma_scl_high(uint64_t deadline)
{
	pin_set(PIN_SCL, CFG_GPIO_RELEASED, -1);
	while (!pin_level(PIN_SCL)) {
		if (mach_absolute_time() >= deadline) {
			return false;
		}
		i2c_dma_pause(deadline);
	}
	return true;
}

static void
i2c_dma_fallback(const char *why)
{
	bool aborted, tx_reset, rx_reset;
	uint64_t deadline;

	i2c_dma_dump(why);
	use_dma = false;
	/* Linux geni_i2c_abort_xfer()/tx_fsm_rst()/rx_fsm_rst(): stop before reusing pins or memory. */
	se_wr(GENI_M_IRQ_CLEAR, M_CMD_ABORT);
	se_wr(GENI_M_CMD_CTRL, 1u << 1);
	aborted = i2c_dma_wait_bits(GENI_M_IRQ_STATUS, M_CMD_ABORT);
	se_wr(SE_DMA_TX_IRQ_CLR, DMA_RESET_DONE);
	se_wr(SE_DMA_TX_FSM_RST, 1);
	tx_reset = i2c_dma_wait_bits(SE_DMA_TX_IRQ_STAT, DMA_RESET_DONE);
	se_wr(SE_DMA_RX_IRQ_CLR, DMA_RESET_DONE);
	se_wr(SE_DMA_RX_FSM_RST, 1);
	rx_reset = i2c_dma_wait_bits(SE_DMA_RX_IRQ_STAT, DMA_RESET_DONE);
	printf("fts: DMA cleanup: abort=%u TX_RESET_DONE=%u RX_RESET_DONE=%u (each wait <=50 ms)\n",
	    aborted, tx_reset, rx_reset);
	i2c_dma_dump("after cleanup");
	se_wr(SE_IRQ_EN, 0);
	se_wr(SE_GSI_EVENT_EN, 0);
	se_wr(SE_DMA_TX_IRQ_EN_CLR, 0xffffffff);
	se_wr(SE_DMA_RX_IRQ_EN_CLR, 0xffffffff);
	se_wr(GENI_DMA_MODE_EN, se_rd(GENI_DMA_MODE_EN) & ~1u);
	/* A guess: disconnecting GENI outputs and remuxing GPIO isolates even a failed abort. */
	se_wr(GENI_FORCE_DEFAULT, 1);
	se_wr(GENI_OUTPUT_CTRL, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	use_bitbang = true;
	sda_release();
	deadline = i2c_dma_deadline();
	for (int i = 0; i < 9 && !pin_level(PIN_SDA); i++) {
		scl_low();
		bb_wait();
		if (!i2c_dma_scl_high(deadline)) {
			break;
		}
		bb_wait();
	}
	scl_low();
	sda_low();
	bb_wait();
	(void)i2c_dma_scl_high(deadline);
	bb_wait();
	sda_release();
	bb_wait();
	bb_error = false;
	/* Never free or touch this page again: a failed reset may leave DMA outstanding. */
	printf("fts: DMA disabled permanently; fallback to bitbang; DMA page retained; SDA=%u SCL=%u%s\n",
	    pin_level(PIN_SDA), pin_level(PIN_SCL),
	    pin_level(PIN_SDA) && pin_level(PIN_SCL) ? "" : " (bus still low; recovery not confirmed)");
}

static bool
i2c_dma_init(void)
{
	if ((se_rd(GENI_STATUS) & 1) || ((se_rd(GENI_FW_REVISION_RO) >> 8) & 0xff) != 3) {
		i2c_dma_fallback("init: engine busy or not I2C");
		return false;
	}
	if (kmem_alloc_contig(kernel_map, &dma_page, round_page(PAGE_SIZE), PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		i2c_dma_fallback("init: no contiguous DMA page");
		return false;
	}
	dma_phys = kvtophys(dma_page);
	/* A guess: a physical address works only with SMMU bypass or an identity mapping for this stream. */
	printf("fts: DMA init: one %lu-byte page, PA=%#llx; SMMU mapping UNKNOWN (physical address trial)\n",
	    (unsigned long)PAGE_SIZE, dma_phys);
	se_wr(SE_IRQ_EN, 0);
	se_wr(SE_GSI_EVENT_EN, 0);
	se_wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	se_wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	se_wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	se_wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	/* Linux geni_se_select_mode(GENI_SE_DMA): disable FIFO/done IRQs, then select DMA. */
	se_wr(GENI_M_IRQ_EN, se_rd(GENI_M_IRQ_EN) & ~(M_CMD_DONE | M_TX_WATERMARK | (3u << 26)));
	se_wr(SE_DMA_GENERAL_CFG, se_rd(SE_DMA_GENERAL_CFG) | 0xf);
	se_wr(GENI_DMA_MODE_EN, se_rd(GENI_DMA_MODE_EN) | 1u);
	se_wr(GENI_OUTPUT_CTRL, 0x7f);
	pin_set(PIN_SDA, CFG_QUP14_2MA, -1);
	pin_set(PIN_SCL, CFG_QUP14_2MA, -1);
	__asm__ volatile("dsb sy" ::: "memory");
	use_bitbang = false;
	use_dma = true;
	i2c_dma_dump("init (FIFO_IF_DISABLE does not prove SE DMA availability)");
	if (!(se_rd(GENI_DMA_MODE_EN) & 1)) {
		i2c_dma_fallback("init: DMA mode did not read back");
		return false;
	}
	return true;
}

static bool
i2c_dma_wait(bool receive)
{
	uint64_t deadline = i2c_dma_deadline();

	for (;;) {
		uint32_t active = se_rd(GENI_STATUS) & 1u;
		uint32_t m = se_rd(GENI_M_IRQ_STATUS);
		uint32_t tx = se_rd(SE_DMA_TX_IRQ_STAT), rx = se_rd(SE_DMA_RX_IRQ_STAT);

		if ((m & DMA_M_ERRORS) || ((tx | rx) & DMA_AHB_ERR) || (rx & DMA_RX_I2C_ERRORS)) {
			i2c_dma_fallback(((tx | rx) & DMA_AHB_ERR) ? "AHB/SBE error (SMMU/access suspect)" : "I2C/command error");
			return false;
		}
		/* A guess: DMA_DONE can precede the last ACK/STOP; require command idle even with STOP_STRETCH. */
		if (((receive ? rx : tx) & DMA_DONE) && !active) {
			__asm__ volatile("dsb sy" ::: "memory");
			if (dma_trace) {
				i2c_dma_dump(receive ? "selftest RX complete" : "selftest TX complete");
			}
			return true;
		}
		if (mach_absolute_time() >= deadline) {
			i2c_dma_fallback("timeout 50 ms (DMA path/SMMU/FIFO gate/stretch unresolved)");
			return false;
		}
		i2c_dma_pause(deadline);
	}
}

static bool
i2c_dma_transfer(uint8_t address, uint32_t length, bool receive, bool stretch)
{
	uint64_t phys = dma_phys + (receive ? DMA_BUFFER_SIZE : 0);

	if (!use_dma) {
		return false;
	}
	if (address > 0x7f || length == 0 || length > DMA_BUFFER_SIZE || (se_rd(GENI_STATUS) & 1)) {
		i2c_dma_fallback("invalid length/address or engine still active");
		return false;
	}
	se_wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	se_wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	se_wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	/* Linux geni_i2c_{rx,tx}_one_msg(): TRANS_LEN, M_CMD0, then DMA prep (LEN last). */
	se_wr(receive ? I2C_RX_TRANS_LEN : I2C_TX_TRANS_LEN, length);
	se_wr(GENI_M_CMD0, (receive ? I2C_READ : I2C_WRITE) << 27 | (uint32_t)address << 9 |
	    (stretch ? I2C_STOP_STRETCH : 0));
	se_wr(receive ? SE_DMA_RX_IRQ_EN_SET : SE_DMA_TX_IRQ_EN_SET, DMA_IRQ_ENABLE);
	se_wr(receive ? SE_DMA_RX_PTR_L : SE_DMA_TX_PTR_L, (uint32_t)phys);
	se_wr(receive ? SE_DMA_RX_PTR_H : SE_DMA_TX_PTR_H, (uint32_t)(phys >> 32));
	se_wr(receive ? SE_DMA_RX_ATTR : SE_DMA_TX_ATTR, receive ? 0 : DMA_TX_EOT_BUF);
	__asm__ volatile("dsb sy" ::: "memory");
	se_wr(receive ? SE_DMA_RX_LEN : SE_DMA_TX_LEN, length);
	__asm__ volatile("dsb sy" ::: "memory");
	return i2c_dma_wait(receive);
}

static bool
i2c_dma_write(uint8_t address, const uint8_t *data, uint32_t length, bool stretch)
{
	if (!use_dma || data == NULL || length == 0 || length > DMA_BUFFER_SIZE || (se_rd(GENI_STATUS) & 1)) {
		if (use_dma) {
			i2c_dma_fallback("invalid TX buffer/length or engine active");
		}
		return false;
	}
	memcpy((void *)dma_page, data, length);
	FlushPoC_DcacheRegion(dma_page, length);
	return i2c_dma_transfer(address, length, false, stretch);
}

static bool
i2c_dma_read(uint8_t address, uint8_t *data, uint32_t length)
{
	vm_offset_t rx = dma_page + DMA_BUFFER_SIZE;

	if (!use_dma || data == NULL || length == 0 || length > DMA_BUFFER_SIZE || (se_rd(GENI_STATUS) & 1)) {
		if (use_dma) {
			i2c_dma_fallback("invalid RX buffer/length or engine active");
		}
		return false;
	}
	/* This tree has no invalidate_dcache(); clean+invalidate before DMA prevents dirty eviction over RX. */
	FlushPoC_DcacheRegion(rx, length);
	if (!i2c_dma_transfer(address, length, true, false)) {
		return false;
	}
	/* No CPU writes between the two flushes: the post-DMA operation invalidates only clean lines. */
	FlushPoC_DcacheRegion(rx, length);
	memcpy(data, (const void *)rx, length);
	return true;
}

static bool
i2c_dma_selftest(void)
{
	static const uint8_t command[5] = { 0xfa, 0x20, 0, 0, 0 };
	uint8_t id[8] = { 0 };
	bool ok;

	dma_trace = true;
	ok = i2c_dma_write(0x49, command, sizeof(command), true) && i2c_dma_read(0x49, id, sizeof(id));
	dma_trace = false;
	printf("fts: DMA selftest: %s ID=%02x %02x %02x %02x %02x %02x %02x %02x (expected 36 39)\n",
	    ok && id[0] == 0x36 && id[1] == 0x39 ? "PASS" : "FAIL", id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
	i2c_dma_dump("selftest result");
	if (ok && (id[0] != 0x36 || id[1] != 0x39)) {
		i2c_dma_fallback("selftest ID mismatch (mapping/cache/protocol unresolved)");
		return false;
	}
	return ok;
}

static int write_variant;       /* EXP-0068: 0 as U-Boot, 1 a pause before the watermark, 2 the data in the FIFO before the command */

static bool
i2c_write(uint8_t address, const uint8_t *data, uint32_t length, bool stretch)
{
	uint32_t status = 0, sent = 0, seen = 0;
	uint64_t start = mach_absolute_time(), now, interval;

	if (use_dma) {
		return i2c_dma_write(address, data, length, stretch);
	}
	if (use_bitbang) {
		return bb_write(address, data, length, stretch);
	}
	i2c_idle();
	se_wr(I2C_TX_TRANS_LEN, length);
	if (write_variant == 2) {
		for (uint32_t i = 0; i < length; i += 4) {
			uint32_t word = 0;

			for (uint32_t j = 0; j < 4 && i + j < length; j++) {
				word |= (uint32_t)data[i + j] << (8 * j);
			}
			se_wr(GENI_TX_FIFO, word);
		}
		sent = length;
	}
	se_wr(GENI_M_CMD0, I2C_WRITE << 27 | (uint32_t)address << 9 | (stretch ? I2C_STOP_STRETCH : 0));
	if (write_variant == 1) {
		sleep_us(500);
	}
	se_wr(GENI_TX_WATERMARK, 1);    /* as Linux and U-Boot: the engine asks for data with the watermark status */
	nanoseconds_to_absolutetime(50000 * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = se_rd(GENI_M_IRQ_STATUS);
		seen |= status;
		if ((status & M_TX_WATERMARK) && sent < length) {
			for (uint32_t i = 0; i < fifo_depth - 1 && sent < length; i++) {
				uint32_t word = 0;

				for (uint32_t j = 0; j < 4 && sent < length; j++, sent++) {
					word |= (uint32_t)data[sent] << (8 * j);
				}
				se_wr(GENI_TX_FIFO, word);
			}
			if (sent == length) {
				se_wr(GENI_TX_WATERMARK, 0);
			}
		}
		if (status & ~M_CMD_DONE) {
			se_wr(GENI_M_IRQ_CLEAR, status & ~M_CMD_DONE);   /* the watermark status is cleared as it is served */
		}
		now = mach_absolute_time();
	} while (!(status & (M_CMD_DONE | M_ERRORS)) && now - start < interval);
	se_wr(GENI_TX_WATERMARK, 0);
	if ((status & M_CMD_DONE) && !(status & M_ERRORS) && sent == length) {
		se_wr(GENI_M_IRQ_CLEAR, status);
		return true;
	}
	printf("fts: write to %#04x: interrupt status %#010x (all seen %#010x)%s, sent %u of %u; engine status %#x, TX FIFO %#x, RX FIFO %#x, "
	    "IOS %#x; command %#010x, length %u, watermark %u\n", address, status, seen, (status & M_NACK) ? " (NACK)" : "", sent,
	    length, se_rd(GENI_STATUS), se_rd(GENI_TX_FIFO_STATUS), se_rd(GENI_RX_FIFO_STATUS), se_rd(0x908), se_rd(GENI_M_CMD0),
	    se_rd(I2C_TX_TRANS_LEN), se_rd(GENI_TX_WATERMARK));
	printf("fts: the lines now: SDA (GPIO33) %s, SCL (GPIO34) %s\n", pin_level(PIN_SDA) ? "high" : "LOW", pin_level(PIN_SCL) ? "high" : "LOW");
	i2c_recover();
	return false;
}

static bool
i2c_read(uint8_t address, uint8_t *data, uint32_t length)
{
	uint32_t received = 0, status = 0;
	uint64_t start = mach_absolute_time(), now, interval;

	if (use_dma) {
		return i2c_dma_read(address, data, length);
	}
	if (use_bitbang) {
		return bb_read(address, data, length);
	}
	i2c_idle();
	se_wr(I2C_RX_TRANS_LEN, length);
	se_wr(GENI_M_CMD0, I2C_READ << 27 | (uint32_t)address << 9);
	nanoseconds_to_absolutetime(50000 * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		uint32_t fifo = se_rd(GENI_RX_FIFO_STATUS), words = fifo & 0x01ffffff, bytes = words * 4;

		status = se_rd(GENI_M_IRQ_STATUS);
		if (fifo & 0x80000000u) {
			uint32_t valid = fifo >> 28 & 7;

			if (valid && valid < 4) {
				bytes -= 4 - valid;
			}
		}
		for (uint32_t i = 0; i < words; i++) {
			uint32_t word = se_rd(GENI_RX_FIFO);

			for (uint32_t j = 0; j < 4 && bytes; j++, bytes--) {
				if (received < length) {
					data[received++] = (uint8_t)(word >> (8 * j));
				}
			}
		}
		now = mach_absolute_time();
	} while (!(status & (M_CMD_DONE | M_ERRORS)) && now - start < interval);
	if ((status & M_CMD_DONE) && !(status & M_ERRORS) && received == length) {
		se_wr(GENI_M_IRQ_CLEAR, status);
		return true;
	}
	printf("fts: read from %#04x: interrupt status %#010x, %u of %u bytes; engine status %#x, IOS %#x\n", address, status, received,
	    length, se_rd(GENI_STATUS), se_rd(0x908));
	i2c_recover();
	return false;
}

static void
dump_bytes(const char *what, const uint8_t *b, uint32_t n)
{
	char line[100];
	uint32_t at = 0;

	for (uint32_t i = 0; i < n && at + 3 < sizeof(line); i++) {
		at += (uint32_t)snprintf(line + at, sizeof(line) - at, " %02x", b[i]);
	}
	line[at] = 0;
	printf("fts: %s:%s\n", what, line);
}

/* ---- the FTM5 firmware (step 6) -------------------------------------------------------------------- */

/*
 * The chip runs ST's FTM5 firmware, not the FTM4 that step 5's commands (0xb6, 0x85) are for: the stock kernel
 * has FTM5's readSysInfo and setScanMode, and the event 0x03 read in EXP-0068 is FTM5's "controller ready".
 * Commands and layouts from Xiaomi's driver for this phone (github.com/MiCode/Xiaomi_Kernel_OpenSource, branch
 * dipper-q-oss, drivers/input/touchscreen/fts_521: fts_lib/ftsHardware.h, ftsSoftware.h, ftsCore.c, fts.c), its
 * I2C variant: no dummy byte before what is read.
 */
#define FTM5_READ_EVENT         0x86            /* FIFO_CMD_READONE over I2C: one event of 8 bytes */
#define FTM5_HW_REG             0xfa            /* + 4 address bytes, then a read: a hardware register */
#define FTM5_FRAMEBUFFER        0xa6            /* + 2 address bytes, then a read: host data; after a reset the system info */
#define FTM5_SCAN_MODE          0xa0            /* + mode, settings; mode 0 active, settings 1 as fts_mode_handler() on resume */
#define FTM5_EVENT_SIZE         8
#define EVT_NONE                0x00
#define EVT_CONTROLLER_READY    0x03
#define EVT_ENTER               0x13
#define EVT_MOTION              0x23
#define EVT_LEAVE               0x33
#define EVT_STATUS              0x43            /* type 1: the echo of a command, its first bytes from byte 2 on */
#define EVT_ERROR               0xf3
#define TOUCH_TYPE_HOVER        5
#define FTM5_FINGERS            10              /* TOUCH_ID_MAX */
#define FTM5_X_MAX              1079            /* fts.h X_AXIS_MAX, Y_AXIS_MAX: the device tree has no fts,x-max */
#define FTM5_Y_MAX              2247
#define MI8_TOUCH_X_MAX         30479           /* NativeHidInput publishes 30480 x 20320 units */
#define MI8_TOUCH_Y_MAX         20319

static uint32_t fts_steps, ftm5_raw_left = 40, ftm5_orient;

/* A failed DMA read may have consumed bytes; retry only the whole FTM5 request after GPIO recovery. */
static bool
ftm5_read(const uint8_t *command, uint32_t count, uint8_t *data, uint32_t length)
{
	bool was_dma = use_dma;
	bool ok = i2c_write(0x49, command, count, true) && i2c_read(0x49, data, length);

	if (!ok && was_dma && !use_dma) {
		ok = i2c_write(0x49, command, count, true) && i2c_read(0x49, data, length);
	}
	return ok;
}

static bool
ftm5_event(uint8_t *event)
{
	static const uint8_t command = FTM5_READ_EVENT;

	return ftm5_read(&command, 1, event, FTM5_EVENT_SIZE);
}

/* Reads events until one starts with these `count` bytes; prints the others. */
static bool
ftm5_wait(const uint8_t *want, uint32_t count, uint32_t timeout_ms, uint8_t *event)
{
	for (uint32_t waited = 0; waited < timeout_ms; waited += 10) {
		if (ftm5_event(event)) {
			if (memcmp(event, want, count) == 0) {
				return true;
			}
			if (event[0] != EVT_NONE) {
				dump_bytes("FTM5 event while waiting", event, FTM5_EVENT_SIZE);
			}
		}
		sleep_ms(10);
	}
	return false;
}

/* A command whose echo comes back as a status event (checkEcho() of the driver). */
static bool
ftm5_command(const uint8_t *command, uint32_t length)
{
	uint8_t want[FTM5_EVENT_SIZE] = { EVT_STATUS, 1 }, event[FTM5_EVENT_SIZE];
	bool was_dma = use_dma;
	bool written = i2c_write(0x49, command, length, false);

	/* FTM5 set-scan-mode is idempotent; retry it once if DMA just failed. */
	if (!written && was_dma && !use_dma) {
		written = i2c_write(0x49, command, length, false);
	}
	if (!written) {
		printf("fts: FTM5: command %#04x not acknowledged\n", command[0]);
		return false;
	}
	memcpy(want + 2, command, length > 5 ? 5 : length);
	if (!ftm5_wait(want, 2 + (length > 5 ? 5 : length), 500, event)) {
		printf("fts: FTM5: no echo of command %#04x\n", command[0]);
		return false;
	}
	return true;
}

static bool
ftm5_start(void)
{
	static const uint8_t ready[1] = { EVT_CONTROLLER_READY };
	static const uint8_t chip_id[5] = { FTM5_HW_REG, 0x20, 0x00, 0x00, 0x00 };
	static const uint8_t sys_info[3] = { FTM5_FRAMEBUFFER, 0x00, 0x00 };
	static const uint8_t scan_on[3] = { FTM5_SCAN_MODE, 0x00, 0x01 };
	uint8_t event[FTM5_EVENT_SIZE], b[32];

	/* fts_system_reset() with a reset pin: low for 10 ms, then wait for "controller ready" */
	printf("fts: step 6: FTM5. reset\n");
	pin_set(PIN_RESET, CFG_OUTPUT_PULLUP_16MA, 0);
	sleep_ms(10);
	pin_set(PIN_RESET, CFG_OUTPUT_PULLUP_16MA, 1);
	if (!ftm5_wait(ready, 1, 2000, event)) {
		printf("fts: FTM5: no \"controller ready\" within 2 s of the reset: stopping\n");
		return false;
	}
	dump_bytes("FTM5 controller ready", event, FTM5_EVENT_SIZE);

	{
		char mode[16] = { 0 };

		PE_parse_boot_argn("fts_i2c", mode, sizeof(mode));
		mode[sizeof(mode) - 1] = 0;
		if (strcmp(mode, "dma") == 0) {
			if (i2c_dma_init() && i2c_dma_selftest()) {
				printf("fts: step 6: transport=SE DMA, polling 100 us, timeout 50 ms\n");
			}
		} else {
			printf("fts: step 6: transport=bitbang (fts_i2c=dma opts in)\n");
		}
	}
	if (ftm5_read(chip_id, sizeof(chip_id), b, 8)) {
		dump_bytes("FTM5 chip id at 0x20000000 (36 39 expected), then the firmware version", b, 8);
	}
	if (ftm5_read(sys_info, sizeof(sys_info), b, 32)) {
		dump_bytes("FTM5 system info 0-15", b, 16);
		dump_bytes("FTM5 system info 16-31", b + 16, 16);
		/* readSysInfo(): from byte 4: api rev (2), minor, major, chip0 ver (2), chip0 id (2), chip1 ver, chip1 id, fw ver (2) */
		printf("fts: FTM5: type %#x, api %u.%u, chip0 id %#06x, firmware version %#06x, config version %#06x\n", b[1], b[7], b[6],
		    b[10] | b[11] << 8, b[16] | b[17] << 8, b[20] | b[21] << 8);
	}
	if (!ftm5_command(scan_on, sizeof(scan_on))) {
		printf("fts: FTM5: scanning not confirmed; watching anyway\n");
	} else {
		printf("fts: FTM5: scanning on (active mode)\n");
	}
	return true;
}

static struct {
	bool down;
	uint32_t x, y;
} ftm5_finger[FTM5_FINGERS];
static struct {
	uint32_t wakes, events, downs, ups, moves, errors, others;
} ftm5_seen;

void
native_touch_set_sink(void (*sink)(void *), void *context)
{
	native_touch_sink_context = context;
	__sync_synchronize();
	native_touch_sink = sink;
	__sync_synchronize();
}

/* Publish the complete finger set through the seqlock NativeHidInput reads. The event type
 * is deliberately not interpreted here: posting any transition publishes DOWN/MOVE/UP by
 * comparing this set with the HID side's previous set. */
void
redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	uint32_t count = 0;

	(void)type;
	(void)finger;
	(void)x;
	(void)y;
	native_touch_seq = native_touch_seq + 1;
	__sync_synchronize();
	for (uint32_t i = 0; i < FTM5_FINGERS && count < MI8_TOUCH_SLOTS; i++) {
		if (!ftm5_finger[i].down) {
			continue;
		}
		native_touch_ids[count] = (int)i;
		native_touch_xs[count] = (int)ftm5_finger[i].x;
		native_touch_ys[count] = (int)ftm5_finger[i].y;
		count++;
	}
	native_touch_count = count;
	__sync_synchronize();
	native_touch_seq = native_touch_seq + 1;
	if (native_touch_sink) {
		native_touch_sink(native_touch_sink_context);
	}
}

static void
ftm5_release_all(void)
{
	for (uint32_t i = 0; i < FTM5_FINGERS; i++) {
		if (ftm5_finger[i].down) {
			ftm5_finger[i].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, i, ftm5_finger[i].x, ftm5_finger[i].y);
		}
	}
}

/* Chip coordinates to the NativeHidInput publication range. */
static void
ftm5_scale(uint32_t cx, uint32_t cy, uint32_t *x, uint32_t *y)
{
	uint32_t sx = cx > FTM5_X_MAX ? FTM5_X_MAX : cx, sy = cy > FTM5_Y_MAX ? FTM5_Y_MAX : cy;
	uint64_t nx = (uint64_t)sx * MI8_TOUCH_X_MAX / FTM5_X_MAX;
	uint64_t ny = (uint64_t)sy * MI8_TOUCH_Y_MAX / FTM5_Y_MAX;

	if (ftm5_orient & 4) {
		uint64_t t = nx;

		nx = ny;
		ny = t;
	}
	if (ftm5_orient & 1) {
		nx = MI8_TOUCH_X_MAX - nx;
	}
	if (ftm5_orient & 2) {
		ny = MI8_TOUCH_Y_MAX - ny;
	}
	*x = (uint32_t)nx;
	*y = (uint32_t)ny;
}

static void
ftm5_handle(const uint8_t *e)
{
	static const uint8_t scan_on[3] = { FTM5_SCAN_MODE, 0x00, 0x01 };
	uint32_t finger = e[1] >> 4, type = e[1] & 0xf, cx, cy, x, y;

	ftm5_seen.events++;
	if (ftm5_raw_left > 0) {
		ftm5_raw_left--;
		dump_bytes("FTM5 raw event", e, FTM5_EVENT_SIZE);
	}
	switch (e[0]) {
	case EVT_ENTER:
	case EVT_MOTION:
		/* fts_enter_pointer_event_handler(), which also serves motion */
		cx = (uint32_t)(e[3] & 0x0f) << 8 | e[2];
		cy = (uint32_t)e[4] << 4 | e[3] >> 4;
		if (finger >= FTM5_FINGERS) {
			break;
		}
		if (type == TOUCH_TYPE_HOVER) {
			if (ftm5_finger[finger].down) {
				ftm5_finger[finger].down = false;
				ftm5_seen.ups++;
				redwood_touch_post(REDWOOD_TOUCH_UP, finger, ftm5_finger[finger].x, ftm5_finger[finger].y);
			}
			break;
		}
		ftm5_scale(cx, cy, &x, &y);
		ftm5_finger[finger].x = x;
		ftm5_finger[finger].y = y;
		if (!ftm5_finger[finger].down) {
			ftm5_finger[finger].down = true;
			ftm5_seen.downs++;
			printf("fts: finger %u down at %u,%u (chip; type %u) = %u,%u\n", finger, cx, cy, type, x, y);
			redwood_touch_post(REDWOOD_TOUCH_DOWN, finger, x, y);
		} else {
			ftm5_seen.moves++;
			redwood_touch_post(REDWOOD_TOUCH_MOVE, finger, x, y);
		}
		break;
	case EVT_LEAVE:
		if (finger < FTM5_FINGERS && ftm5_finger[finger].down) {
			ftm5_finger[finger].down = false;
			ftm5_seen.ups++;
			printf("fts: finger %u up at %u,%u\n", finger, ftm5_finger[finger].x, ftm5_finger[finger].y);
			redwood_touch_post(REDWOOD_TOUCH_UP, finger, ftm5_finger[finger].x, ftm5_finger[finger].y);
		}
		break;
	case EVT_CONTROLLER_READY:
		/* the chip has reset itself (the driver's controller_ready handler restores the mode) */
		printf("fts: FTM5: the chip reset itself; scanning on again\n");
		ftm5_release_all();
		(void)i2c_write(0x49, scan_on, sizeof(scan_on), false);
		break;
	default:
		ftm5_seen.others++;
		if (ftm5_seen.others <= 50) {
			dump_bytes(e[0] == EVT_ERROR ? "FTM5 error event" : "FTM5 other event", e, FTM5_EVENT_SIZE);
		}
		break;
	}
}

/* Poll the interrupt pin (low: the chip has events) and read the events until it lets go. */
static void
ftm5_watch(uint32_t seconds, uint32_t poll_us)
{
	uint64_t start = mach_absolute_time(), last_report = start, now, ns, busy = 0, read_start, window_ns;
	uint32_t failures = 0, window_events = 0, events_before;
	uint8_t e[FTM5_EVENT_SIZE];

	printf("fts: watching the interrupt pin every %u us%s; touch the screen\n", poll_us, seconds ? "" : ", until the run ends");
	for (;;) {
		if (!pin_level(PIN_IRQ)) {
			/* EXP-0070: how much of the one CPU the reads by hand take */
			read_start = mach_absolute_time();
			events_before = ftm5_seen.events;
			ftm5_seen.wakes++;
			for (int i = 0; i < 32 && !pin_level(PIN_IRQ); i++) {
				bool was_dma = use_dma;
				bool ok = ftm5_event(e);

				/* An uncertain DMA read can lose a release event; clear held contacts before continuing. */
				if (was_dma && !use_dma) {
					ftm5_release_all();
				}
				if (!ok) {
					ftm5_seen.errors++;
					failures++;
					break;
				}
				failures = 0;
				if (e[0] == EVT_NONE) {
					break;
				}
				ftm5_handle(e);
			}
			busy += mach_absolute_time() - read_start;
			window_events += ftm5_seen.events - events_before;
			if (failures >= 100) {
				printf("fts: FTM5: 100 reads in a row failed: stopping\n");
				ftm5_release_all();
				return;
			}
		}
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - last_report, &ns);
		if (ns >= 10 * NSEC_PER_SEC) {
			window_ns = ns;
			last_report = now;
			absolutetime_to_nanoseconds(busy, &ns);
			printf("fts: last %llu ms: reading took %llu ms (%llu.%llu%%), %u events, %llu us per event\n", window_ns / NSEC_PER_MSEC,
			    ns / NSEC_PER_MSEC, ns * 100 / window_ns, ns * 1000 / window_ns % 10, window_events,
			    window_events ? ns / NSEC_PER_USEC / window_events : 0);
			busy = 0;
			window_events = 0;
			printf("fts: so far %u wakes, %u events: %u down, %u moves, %u up, %u other, %u read errors; longest stretch %u us\n",
			    ftm5_seen.wakes, ftm5_seen.events, ftm5_seen.downs, ftm5_seen.moves, ftm5_seen.ups, ftm5_seen.others,
			    ftm5_seen.errors, bb_stretch_max_us);
		}
		absolutetime_to_nanoseconds(now - start, &ns);
		if (seconds != 0 && ns >= (uint64_t)seconds * NSEC_PER_SEC) {
			break;
		}
		sleep_us(poll_us);
	}
	ftm5_release_all();
	printf("fts: watched %u s: %u wakes, %u events: %u down, %u moves, %u up, %u other, %u read errors\n", seconds,
	    ftm5_seen.wakes, ftm5_seen.events, ftm5_seen.downs, ftm5_seen.moves, ftm5_seen.ups, ftm5_seen.others, ftm5_seen.errors);
}

/* Step 5. */
static void
i2c_step(void)
{
	const char *problem;
	int st, atmel;
	uint8_t id[3], event[8];

	printf("fts: step 5: I2C; first 500 ms for the chip to start its firmware\n");
	sleep_ms(500);
	se14 = ml_io_map(SE14_PHYS, SE14_SIZE);
	problem = i2c_init();
	if (problem != NULL) {
		printf("fts: %s: stopping\n", problem);
		return;
	}
	if (se_rd(0x064) & 1) {
		use_bitbang = true;
		printf("fts: the engine's FIFO interface is disabled: the bus is driven by hand on GPIO33/34\n");
		sda_release();
		scl_release();
		bb_wait();
		printf("fts: both lines %s at rest\n", (pin_level(PIN_SDA) && pin_level(PIN_SCL)) ? "high" : "NOT high");
	}
	printf("fts: FIFO %u words; SCL counters %#010x, clock config %#x; FIFO interface disabled (0x64) = %#x, DMA mode %#x\n",
	    fifo_depth, se_rd(I2C_SCL_COUNTERS), se_rd(GENI_SER_M_CLK_CFG), se_rd(0x064), se_rd(GENI_DMA_MODE_EN));

	st = i2c_probe(0x49);
	printf("fts: probe 0x49 (ST FTS): %s; interrupt pin %s\n", st == 0 ? "ACK" : st == 1 ? "no answer" : "ERROR",
	    pin_level(PIN_IRQ) ? "high" : "low");
	if (fts_steps >= 6) {
		uint32_t seconds = 0, poll_us = 4000;

		PE_parse_boot_argn("fts_watch", &seconds, sizeof(seconds));
		PE_parse_boot_argn("fts_poll_us", &poll_us, sizeof(poll_us));
		PE_parse_boot_argn("fts_raw", &ftm5_raw_left, sizeof(ftm5_raw_left));
		PE_parse_boot_argn("fts_orient", &ftm5_orient, sizeof(ftm5_orient));
		if (poll_us < 500) {
			poll_us = 500;
		}
		/* Step 6 starts on the proven GPIO transport; only its ID selftest may enable DMA. */
		use_bitbang = true;
		sda_release();
		scl_release();
		if (st == 0 && ftm5_start()) {
			ftm5_watch(seconds, poll_us);
		}
		return;
	}
	if (st == 0) {
		/* the ST chip's identity: hardware register 0x0004 (0x0007 for FTM3), through command 0xb6; the first byte read is a dummy */
		static const uint8_t ask[2][3] = { { 0xb6, 0x00, 0x04 }, { 0xb6, 0x00, 0x07 } };

		for (write_variant = 0; write_variant < (use_bitbang ? 1 : 3); write_variant++) {
			for (int i = 0; i < 2; i++) {
				if (i2c_write(0x49, ask[i], 3, true) && i2c_read(0x49, id, 3)) {
					dump_bytes(i == 0 ? "ST chip id at 0x0004 (dummy, then id: 36 70 expected)" :
					    "ST chip id at 0x0007 (dummy, then id: 39 6c for FTM3)", id, 3);
					printf("fts: that was with write variant %d\n", write_variant);
				}
			}
		}
		write_variant = 0;
		/* the first events of the chip's FIFO, while its interrupt pin says there is one */
		for (int i = 0; i < 6 && !pin_level(PIN_IRQ); i++) {
			static const uint8_t readone = 0x85;

			if (i2c_write(0x49, &readone, 1, true) && i2c_read(0x49, event, 8)) {
				dump_bytes("ST event", event, 8);
			} else {
				break;
			}
		}
		printf("fts: interrupt pin now %s\n", pin_level(PIN_IRQ) ? "high" : "low");
	}

	if (use_bitbang) {
			static const uint8_t id_cmd[3] = { 0xb6, 0x00, 0x04 }, one_cmd[1] = { 0x86 }, info_cmd[1] = { 0x00 };
			uint8_t r[12];

			printf("fts: variants of the read (EXP-0068), interrupt pin %s:\n", pin_level(PIN_IRQ) ? "high" : "low");
			if (i2c_read(0x49, r, 8)) {
				dump_bytes("plain read of 8 bytes, no command", r, 8);
			}
			for (int slow = 0; slow < 2; slow++) {
				bb_us = slow ? 25 : 5;
				if (i2c_write(0x49, id_cmd, 3, true) && i2c_read(0x49, r, 6)) {
					dump_bytes(slow ? "b6 00 04, repeated start, 6 bytes, slow (20 kHz)" : "b6 00 04, repeated start, 6 bytes", r, 6);
				}
				if (i2c_write(0x49, id_cmd, 3, false) && i2c_read(0x49, r, 6)) {
					dump_bytes(slow ? "b6 00 04, stop then read, 6 bytes, slow" : "b6 00 04, stop then read, 6 bytes", r, 6);
				}
				if (i2c_write(0x49, one_cmd, 1, true) && i2c_read(0x49, r, 9)) {
					dump_bytes(slow ? "86, repeated start, 9 bytes, slow" : "86, repeated start, 9 bytes", r, 9);
				}
				if (i2c_write(0x49, info_cmd, 1, true) && i2c_read(0x49, r, 8)) {
					dump_bytes(slow ? "00, repeated start, 8 bytes, slow" : "00, repeated start, 8 bytes", r, 8);
				}
			}
			bb_us = 5;
			printf("fts: clock stretching: longest %u us, %u stretches over 20 us, %u times SCL stayed low for 100 ms\n",
			    bb_stretch_max_us, bb_stretches, bb_stretch_timeouts);
			printf("fts: interrupt pin now %s\n", pin_level(PIN_IRQ) ? "high" : "low");
		}
	atmel = i2c_probe(0x4a);
	printf("fts: probe 0x4a (Atmel mXT): %s\n", atmel == 0 ? "ACK" : atmel == 1 ? "no answer" : "ERROR");
	if (atmel == 0) {
		static const uint8_t zero[2] = { 0, 0 };
		uint8_t info[7];

		if (i2c_write(0x4a, zero, 2, true) && i2c_read(0x4a, info, 7)) {
			dump_bytes("Atmel info block (family, variant, version, build, x, y, objects)", info, 7);
		}
	}
}

static void
fts_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter, votes, version, channels;
	bool m_ahb, s_ahb, s6;

	fts_steps = steps;
	printf("fts: step 1: the clocks of serial engine 14 (read the clock controller at %#lx)\n", GCC_PHYS);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	votes = rd(gcc + GCC_VOTE_APPS);
	printf("fts: the apps processor's votes (%#x) = %#010x: wrapper-1 master AHB %s, slave AHB %s, engine 6 %s\n", GCC_VOTE_APPS,
	    votes, (votes & VOTE_WRAP1_M_AHB) ? "voted" : "not voted", (votes & VOTE_WRAP1_S_AHB) ? "voted" : "not voted",
	    (votes & VOTE_WRAP1_S6) ? "voted" : "not voted");
	m_ahb = branch_on("QUPV3_WRAP_1_M_AHB", GCC_WRAP1_M_AHB_HALT);
	s_ahb = branch_on("QUPV3_WRAP_1_S_AHB", GCC_WRAP1_S_AHB_HALT);
	s6 = branch_on("QUPV3_WRAP1_S6 (SE14)", GCC_WRAP1_S6_HALT);
	printf("fts: the engine's clock source (RCG at %#x): command %#010x, config %#010x\n", GCC_WRAP1_S6_RCG,
	    rd(gcc + GCC_WRAP1_S6_RCG), rd(gcc + GCC_WRAP1_S6_RCG + 4));
	if (m_ahb && s_ahb) {
		vm_offset_t se = ml_io_map(SE14_PHYS, SE14_SIZE), qup = ml_io_map(QUP1_PHYS, QUP1_SIZE);
		uint32_t fw = rd(se + GENI_FW_REVISION_RO);

		printf("fts: the AHB clocks are on: reading the wrapper and the engine\n");
		printf("fts: QUP wrapper 1 hardware version %#010x\n", rd(qup + 4));
		printf("fts: SE14 firmware revision %#010x: protocol %u (1 spi, 2 uart, 3 i2c), version %u\n", fw, (fw >> 8) & 0xff,
		    fw & 0xff);
		printf("fts: SE14 status %#010x, clock select %#x, DMA mode %#x, hw params %#x %#x\n", rd(se + GENI_STATUS),
		    rd(se + GENI_CLK_SEL), rd(se + GENI_DMA_MODE_EN), rd(se + HW_PARAM_0), rd(se + HW_PARAM_1));
	} else {
		printf("fts: an AHB clock of the QUP wrapper is off: the engine is not touched (an access could hang the bus)\n");
	}
	(void)s6;
	if (steps < 2) {
		goto out;
	}

	printf("fts: step 2: the pins and the supplies\n");
	pins_north = ml_io_map(TLMM_PHYS + TLMM_NORTH + 0x1000UL * 31, 0x4000UL);
	pins_south = ml_io_map(TLMM_PHYS + TLMM_SOUTH + 0x1000UL * 23, 0x1000UL);
	pin_look("touch interrupt", 31, TLMM_NORTH);
	pin_look("touch reset", 32, TLMM_NORTH);
	pin_look("I2C SDA, qup14", 33, TLMM_NORTH);
	pin_look("I2C SCL, qup14", 34, TLMM_NORTH);
	pin_look("I/O supply switch", 23, TLMM_SOUTH);
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (version != 0x50000000u || channels == 0 || channels > 512) {
		printf("fts: PMIC arbiter version %#x, %u channels: not read\n", version, channels);
		goto out;
	}
	/* L28 of the PM8998 would be 0x5b00 if the LDOs start at 0x4000 and are 0x100 apart; its neighbours as a check */
	ldo_look("PM8998 L27 (neighbour)", 0x5a, channels);
	ldo_look("peripheral 0x5b (guessed L28, wrong: see EXP-0067)", 0x5b, channels);
	ldo_look("PM8998 L29 (neighbour)", 0x5c, channels);
	if (steps < 3) {
		goto out;
	}
	if (!power_on(channels)) {
		goto out;
	}
	if (steps < 4) {
		goto out;
	}
	if (clock_and_pins()) {
		uint32_t se_status = 0;
		vm_offset_t se = ml_io_map(SE14_PHYS, SE14_SIZE);

		se_status = rd(se + GENI_STATUS);
		printf("fts: SE14 now: status %#010x, firmware revision %#010x, clock select %#x\n", se_status,
		    rd(se + GENI_FW_REVISION_RO), rd(se + GENI_CLK_SEL));
		if (steps >= 5) {
			i2c_step();
		}
	}
out:
	printf("fts: stopping here (fts_steps=%u)\n", steps);
	thread_terminate(current_thread());
}

void
mi8_fts_init(void)
{
	uint32_t enabled = 0, steps = 2;
	thread_t thread;

	if (!PE_parse_boot_argn("fts", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	PE_parse_boot_argn("fts_steps", &steps, sizeof(steps));
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(fts_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
