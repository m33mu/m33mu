/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "rw612/rw612_mmio.h"
#include "rw612/rw612_flexspi.h"
#include "rw612/rw612_romapi.h"
#include "rw612/rw612_secure.h"
#include "rw612/cpu_config.h"
#include "m33mu/memmap.h"
#include "m33mu/mmio.h"
#include "m33mu/gpio_pads.h"
#include "m33mu/nvic.h"

/* -------------------------------------------------------------------------
 * RSTCTL / CLKCTL (paired controllers, two domains)
 *
 *   Domain 0:  RSTCTL0 @ 0x40000000   CLKCTL0 @ 0x40001000
 *   Domain 1:  RSTCTL1 @ 0x40020000   CLKCTL1 @ 0x40021000
 *
 * Each block is 0x1000 wide.  Within each block the per-peripheral arrays
 * (PSCCTL0/1/2 in CLKCTLn, PRSTCTL0/1/2 in RSTCTLn) live at offsets
 * 0x10/0x14/0x18 with SET/CLR/TOG aliases at +0x40/+0x70/+0xA0:
 *
 *   PSCCTLn       0x10 + 4*n
 *   PSCCTLn_SET   0x40 + 4*n   write 1 = set bit in PSCCTLn
 *   PSCCTLn_CLR   0x70 + 4*n   write 1 = clear bit in PSCCTLn
 *   PSCCTLn_TOG   0xA0 + 4*n   write 1 = toggle bit in PSCCTLn
 *
 * Reset semantics are *active high*: a 1 in PRSTCTLn keeps the peripheral
 * held in reset.  Firmware clears the bit (PRSTCTLn_CLR) to release it.
 *
 * mm_rw612_clkctl_periph_active(domain, pscctl_offset, bit) returns MM_TRUE
 * iff the bit is set in CLKCTLn[pscctl_offset] AND clear in RSTCTLn[same].
 * ------------------------------------------------------------------------- */
#define CLKRST_BLOCK_SIZE    0x1000u
#define CLKRST_PCTL_BASE     0x10u   /* PSCCTL0 / PRSTCTL0 */
#define CLKRST_PCTL_LAST     0x18u   /* PSCCTL2 / PRSTCTL2 */
#define CLKRST_PCTL_SET      0x40u
#define CLKRST_PCTL_CLR      0x70u
#define CLKRST_PCTL_TOG      0xA0u

#define CLKRST_DOMAIN_COUNT  2u

struct clkrst_state {
    /* CLKCTL = clk[]; RSTCTL = rst[].  Each is one register-file per domain. */
    mm_u32 clk[CLKRST_DOMAIN_COUNT][CLKRST_BLOCK_SIZE / 4u];
    mm_u32 rst[CLKRST_DOMAIN_COUNT][CLKRST_BLOCK_SIZE / 4u];
};

static struct clkrst_state clkrst;

static mm_bool clkrst_apply_alias(mm_u32 *array, mm_u32 offset, mm_u32 value)
{
    /* Returns MM_TRUE if `offset` was a SET/CLR/TOG alias and the access has
     * been handled; `array` is updated in place.  The alias targets the base
     * register at  offset - alias_base + CLKRST_PCTL_BASE. */
    mm_u32 base_off, idx;

    if (offset >= CLKRST_PCTL_SET &&
        offset <= CLKRST_PCTL_SET + (CLKRST_PCTL_LAST - CLKRST_PCTL_BASE)) {
        base_off = (offset - CLKRST_PCTL_SET) + CLKRST_PCTL_BASE;
        idx = base_off / 4u;
        array[idx] |= value;
        return MM_TRUE;
    }
    if (offset >= CLKRST_PCTL_CLR &&
        offset <= CLKRST_PCTL_CLR + (CLKRST_PCTL_LAST - CLKRST_PCTL_BASE)) {
        base_off = (offset - CLKRST_PCTL_CLR) + CLKRST_PCTL_BASE;
        idx = base_off / 4u;
        array[idx] &= ~value;
        return MM_TRUE;
    }
    if (offset >= CLKRST_PCTL_TOG &&
        offset <= CLKRST_PCTL_TOG + (CLKRST_PCTL_LAST - CLKRST_PCTL_BASE)) {
        base_off = (offset - CLKRST_PCTL_TOG) + CLKRST_PCTL_BASE;
        idx = base_off / 4u;
        array[idx] ^= value;
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool clkctl_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                           mm_u32 *value_out)
{
    mm_u32 domain = (mm_u32)(intptr_t)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > CLKRST_BLOCK_SIZE) return MM_FALSE;
    if (domain >= CLKRST_DOMAIN_COUNT) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)clkrst.clk[domain] + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool clkctl_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                            mm_u32 value)
{
    mm_u32 domain = (mm_u32)(intptr_t)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > CLKRST_BLOCK_SIZE) return MM_FALSE;
    if (domain >= CLKRST_DOMAIN_COUNT) return MM_FALSE;
    if (size_bytes == 4u && clkrst_apply_alias(clkrst.clk[domain], offset, value)) {
        return MM_TRUE;
    }
    memcpy((mm_u8 *)clkrst.clk[domain] + offset, &value, size_bytes);
    return MM_TRUE;
}

static mm_bool rstctl_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                           mm_u32 *value_out)
{
    mm_u32 domain = (mm_u32)(intptr_t)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > CLKRST_BLOCK_SIZE) return MM_FALSE;
    if (domain >= CLKRST_DOMAIN_COUNT) return MM_FALSE;
    memcpy(value_out, (mm_u8 *)clkrst.rst[domain] + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool rstctl_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                            mm_u32 value)
{
    mm_u32 domain = (mm_u32)(intptr_t)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > CLKRST_BLOCK_SIZE) return MM_FALSE;
    if (domain >= CLKRST_DOMAIN_COUNT) return MM_FALSE;
    if (size_bytes == 4u && clkrst_apply_alias(clkrst.rst[domain], offset, value)) {
        return MM_TRUE;
    }
    memcpy((mm_u8 *)clkrst.rst[domain] + offset, &value, size_bytes);
    return MM_TRUE;
}

mm_bool mm_rw612_clkctl_periph_active(mm_u32 domain, mm_u32 pscctl_offset, mm_u32 bit)
{
    mm_u32 mask;
    if (domain >= CLKRST_DOMAIN_COUNT) return MM_FALSE;
    if (pscctl_offset < CLKRST_PCTL_BASE || pscctl_offset > CLKRST_PCTL_LAST) {
        return MM_FALSE;
    }
    mask = (1u << bit);
    return ((clkrst.clk[domain][pscctl_offset / 4u] & mask) != 0u &&
            (clkrst.rst[domain][pscctl_offset / 4u] & mask) == 0u)
           ? MM_TRUE : MM_FALSE;
}

/* -------------------------------------------------------------------------
 * Generic register-file stub (read returns last write, no side effects).
 * Used for the long tail of peripherals that the test firmware does not
 * exercise but the SDK probes during init.
 * ------------------------------------------------------------------------- */
struct periph_stub {
    mm_u32  size;
    mm_u32 *regs;
};

static mm_bool stub_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    const struct periph_stub *ps = (const struct periph_stub *)opaque;
    if (ps == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > ps->size) return MM_FALSE;
    memcpy(value_out, (const mm_u8 *)ps->regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool stub_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    struct periph_stub *ps = (struct periph_stub *)opaque;
    if (ps == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > ps->size) return MM_FALSE;
    memcpy((mm_u8 *)ps->regs + offset, &value, size_bytes);
    return MM_TRUE;
}

#define DECL_STUB(NAME, BYTES)                              \
    static mm_u32 NAME##_regs[(BYTES) / 4u];                \
    static struct periph_stub NAME##_stub = { (BYTES), NAME##_regs }

static mm_bool stub_reg_pair(struct mmio_bus *bus,
                             struct periph_stub *ps,
                             mm_u32 ns_base)
{
    struct mmio_region reg;
    memset(&reg, 0, sizeof(reg));
    reg.size   = ps->size;
    reg.opaque = ps;
    reg.read   = stub_read;
    reg.write  = stub_write;
    reg.base   = ns_base;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base   = ns_base + 0x10000000u;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * GPIO  (0x40100000 NS / 0x50100000 S)
 * SECGPIO (0x40154000 NS / 0x50154000 S)
 *
 * Single 32-pin port stub.  Writes to PIN/SET/CLR/NOT/DIR* update local
 * state; nothing is wired to a real GPIO backend.  Same shape as the
 * LPC55S69 driver so the NXP fsl_gpio HAL works unmodified against it.
 * ------------------------------------------------------------------------- */
#define RW612_GPIO_BASE      0x40100000u
#define RW612_SECGPIO_BASE   0x40154000u
#define RW612_GPIO_SIZE      0x2490u

#define GPIO_OFF_BYTE   0x000u
#define GPIO_OFF_WORD   0x080u
#define GPIO_OFF_DIR    0x2000u
#define GPIO_OFF_PIN    0x2100u
#define GPIO_OFF_SET    0x2200u
#define GPIO_OFF_CLR    0x2280u
#define GPIO_OFF_NOT    0x2300u
#define GPIO_OFF_DIRSET 0x2380u
#define GPIO_OFF_DIRCLR 0x2400u
#define GPIO_OFF_DIRNOT 0x2480u

#define GPIO_PORTS 2u

/* Two 32-pin ports (GPIO0-31, GPIO32-63): DIR/PIN/SET/... at +4 per port,
 * BYTE/WORD per pin 0..63. Pads are reported through m33mu/gpio_pads.h; the
 * IO_MUX is a stub, so a pin with DIR set is taken as a driven GPIO. */
struct gpio_state {
    mm_u32 dir[GPIO_PORTS];
    mm_u32 pin[GPIO_PORTS];
    struct mm_gpio_pads pads[GPIO_PORTS];
};

static struct gpio_state gpio0;

static mm_u32 gpio_input(const struct gpio_state *g, mm_u32 port)
{
    return mm_gpio_pads_input(&g->pads[port], g->dir[port], g->pin[port], 0u, 0u, g->pin[port]);
}

static void gpio_publish(struct gpio_state *g)
{
    mm_u32 port;
    for (port = 0; port < GPIO_PORTS; ++port) {
        mm_gpio_pads_publish(&g->pads[port], g->dir[port], g->pin[port], 0, 0);
    }
}

static mm_bool gpio_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                         mm_u32 *value_out)
{
    struct gpio_state *g = (struct gpio_state *)opaque;
    if (g == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > RW612_GPIO_SIZE) return MM_FALSE;
    *value_out = 0;

    if (offset < GPIO_OFF_WORD) {
        if (offset < 32u * GPIO_PORTS) *value_out = (gpio_input(g, offset / 32u) >> (offset % 32u)) & 1u;
        return MM_TRUE;
    }
    if (offset >= GPIO_OFF_WORD && offset < GPIO_OFF_DIR) {
        mm_u32 bit = (offset - GPIO_OFF_WORD) / 4u;
        if (bit < 32u * GPIO_PORTS) *value_out = (gpio_input(g, bit / 32u) >> (bit % 32u)) & 1u;
        return MM_TRUE;
    }
    if (size_bytes == 4u && offset >= GPIO_OFF_DIR && offset < GPIO_OFF_DIR + 4u * GPIO_PORTS) {
        *value_out = g->dir[(offset - GPIO_OFF_DIR) / 4u];
    } else if (size_bytes == 4u && offset >= GPIO_OFF_PIN && offset < GPIO_OFF_PIN + 4u * GPIO_PORTS) {
        *value_out = gpio_input(g, (offset - GPIO_OFF_PIN) / 4u);
    }
    return MM_TRUE;
}

static mm_bool gpio_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                          mm_u32 value)
{
    struct gpio_state *g = (struct gpio_state *)opaque;
    mm_u32 base;
    mm_u32 port;
    if (g == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > RW612_GPIO_SIZE) return MM_FALSE;

    if (offset < GPIO_OFF_DIR) {
        mm_u32 bit = offset < GPIO_OFF_WORD ? offset : (offset - GPIO_OFF_WORD) / 4u;
        if (bit < 32u * GPIO_PORTS) {
            if (value & 1u) g->pin[bit / 32u] |=  (1u << (bit % 32u));
            else            g->pin[bit / 32u] &= ~(1u << (bit % 32u));
            gpio_publish(g);
        }
        return MM_TRUE;
    }
    if (size_bytes != 4u) return MM_TRUE;
    port = (offset & 0x7Fu) / 4u;
    base = offset - port * 4u;
    if (port >= GPIO_PORTS) return MM_TRUE;
    switch (base) {
    case GPIO_OFF_DIR:    g->dir[port] = value;      break;
    case GPIO_OFF_PIN:    g->pin[port] = value;      break;
    case GPIO_OFF_SET:    g->pin[port] |=  value;    break;
    case GPIO_OFF_CLR:    g->pin[port] &= ~value;    break;
    case GPIO_OFF_NOT:    g->pin[port] ^=  value;    break;
    case GPIO_OFF_DIRSET: g->dir[port] |=  value;    break;
    case GPIO_OFF_DIRCLR: g->dir[port] &= ~value;    break;
    case GPIO_OFF_DIRNOT: g->dir[port] ^=  value;    break;
    default: return MM_TRUE;
    }
    gpio_publish(g);
    return MM_TRUE;
}

static mm_bool gpio_set_external_input_cb(void *opaque, int bank, int pin, mm_bool level,
                                          mm_u64 late_cycles)
{
    (void)opaque;
    (void)late_cycles;
    if (bank < 0 || bank >= (int)GPIO_PORTS) return MM_FALSE;
    return mm_gpio_pads_set_external(&gpio0.pads[bank], pin, level);
}

/* Banks are named "GPIO<first>-<last>": pads are GPIO0..GPIO63. */
static mm_bool gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank >= (int)GPIO_PORTS) return MM_FALSE;
    if (name_out != 0 && name_len > 0u) snprintf(name_out, name_len, "GPIO%d-%d", bank * 32, bank * 32 + 31);
    if (pins_out != 0) *pins_out = 32;
    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * Long tail of peripheral stubs.  Sized just large enough to swallow SDK
 * probes; behaviour added later only if a test exposes a real fault.
 * ------------------------------------------------------------------------- */
DECL_STUB(io_mux,   0x1000u);   /* MCI_IO_MUX  0x40004000 */
DECL_STUB(ahbsc,    0x1000u);   /* AHB SC      0x40148000 */
DECL_STUB(wwdt,     0x1Cu);     /*             0x4000E000 */
DECL_STUB(utick,    0x20u);     /*             0x4000F000 */
DECL_STUB(rtc,      0x60u);     /*             0x40030000 */
DECL_STUB(ostimer,  0x40u);     /*             0x4013B000 */
DECL_STUB(sctimer,  0x550u);    /*             0x40146000 */
DECL_STUB(dma0,     0x1000u);   /*             0x40104000 */
DECL_STUB(dma1,     0x1000u);   /*             0x40105000 */
DECL_STUB(gdma,     0x1000u);   /*             0x4014E000 */
DECL_STUB(pint,     0x40u);     /*             0x40025000 */
DECL_STUB(usbh,     0x100u);    /*             0x40114000 */

/* PMU / power / aon */
DECL_STUB(pmu_ns,   0x1000u);   /* PMU         0x40135000 */
DECL_STUB(aon,      0x1000u);   /* AON_DOMAIN  0x4013C000 */
DECL_STUB(aon_ciu,  0x800u);    /* AON_SOC_CIU 0x45000800 (pad pulls, MCI IO mux) */

/* WWDT non-zero reset */
#define WWDT_OFF_TC     0x04u
#define WWDT_OFF_TV     0x0Cu
#define WWDT_OFF_WINDOW 0x18u

/* -------------------------------------------------------------------------
 * CACHE64  (CTRL0/POLSEL0 0x40033000, CTRL1/POLSEL1 0x40034000)
 *
 * The FlexSPI XIP cache.  The policy selector (REG0_TOP/REG1_TOP/POLSEL at
 * 0x14..0x1C) and the cache controller (CCR/CLCR/CSAR/CCVR at 0x800..0x80C)
 * share one 4 KB block.  The model has no cache array, so commands complete
 * immediately: CCR.GO and CLCR.LGO read back clear once written.
 * ------------------------------------------------------------------------- */
#define CACHE64_BLOCK_SIZE   0x1000u
#define CACHE64_COUNT        2u
#define CACHE64_OFF_CCR      0x800u
#define CACHE64_OFF_CLCR     0x804u
#define CACHE64_CCR_GO       (1u << 31)
#define CACHE64_CLCR_LGO     (1u << 0)

static mm_u32 cache64_regs[CACHE64_COUNT][CACHE64_BLOCK_SIZE / 4u];

static mm_bool cache64_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                            mm_u32 *value_out)
{
    mm_u32 idx = (mm_u32)(intptr_t)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > CACHE64_BLOCK_SIZE || idx >= CACHE64_COUNT) return MM_FALSE;
    *value_out = 0u;
    memcpy(value_out, (const mm_u8 *)cache64_regs[idx] + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool cache64_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                             mm_u32 value)
{
    mm_u32 idx = (mm_u32)(intptr_t)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > CACHE64_BLOCK_SIZE || idx >= CACHE64_COUNT) return MM_FALSE;
    if (size_bytes == 4u && offset == CACHE64_OFF_CCR) {
        value &= ~CACHE64_CCR_GO;
    } else if (size_bytes == 4u && offset == CACHE64_OFF_CLCR) {
        value &= ~CACHE64_CLCR_LGO;
    }
    memcpy((mm_u8 *)cache64_regs[idx] + offset, &value, size_bytes);
    return MM_TRUE;
}

/* -------------------------------------------------------------------------
 * SOCCTRL (SOC_CTRL CIU, 0x45001000 / 0x55001000)
 *
 * CHIP_INFO[7:0] is the silicon revision: the SDK and the boot ROM API
 * pick the ROM API tree from it (0 = A0, anything else = A1 tree).  The
 * model reports A2 (RW612ETA2I); M33MU_RW612_CHIP_REV overrides it.
 * ------------------------------------------------------------------------- */
#define SOCCTRL_BASE_NS      0x45001000u
#define SOCCTRL_SIZE         0x1000u
#define SOCCTRL_OFF_CHIP_INFO 0x114u
#define RW612_CHIP_REV_A2    2u

static mm_u32 socctrl_regs[SOCCTRL_SIZE / 4u];

mm_u32 mm_rw612_chip_rev(void)
{
    static int cached = -1;
    const char *env;
    if (cached < 0) {
        env = getenv("M33MU_RW612_CHIP_REV");
        cached = (env != 0 && env[0] != '\0') ? (int)(strtoul(env, 0, 0) & 0xFFu)
                                              : (int)RW612_CHIP_REV_A2;
    }
    return (mm_u32)cached;
}

static mm_bool socctrl_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                            mm_u32 *value_out)
{
    (void)opaque;
    if (value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SOCCTRL_SIZE) return MM_FALSE;
    *value_out = 0u;
    memcpy(value_out, (const mm_u8 *)socctrl_regs + offset, size_bytes);
    return MM_TRUE;
}

static mm_bool socctrl_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                             mm_u32 value)
{
    (void)opaque;
    if (size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > SOCCTRL_SIZE) return MM_FALSE;
    if (offset >= SOCCTRL_OFF_CHIP_INFO && offset < SOCCTRL_OFF_CHIP_INFO + 4u) {
        return MM_TRUE; /* read-only */
    }
    memcpy((mm_u8 *)socctrl_regs + offset, &value, size_bytes);
    return MM_TRUE;
}

static void rw612_clkrst_apply_resets(void)
{
    mm_u32 d;
    for (d = 0; d < CLKRST_DOMAIN_COUNT; ++d) {
        memset(clkrst.clk[d], 0, sizeof(clkrst.clk[d]));
        /* RSTCTL reset: all bits set (= active reset, peripherals held off). */
        memset(clkrst.rst[d], 0xFFu, sizeof(clkrst.rst[d]));
    }
}

void mm_rw612_mmio_reset(void)
{
    rw612_clkrst_apply_resets();
    memset(&gpio0, 0, sizeof(gpio0));
    mm_gpio_pads_init(&gpio0.pads[0], 0, 32);
    mm_gpio_pads_init(&gpio0.pads[1], 1, 32);
    memset(io_mux_regs,   0, sizeof(io_mux_regs));
    memset(ahbsc_regs,    0, sizeof(ahbsc_regs));
    memset(wwdt_regs,     0, sizeof(wwdt_regs));
    memset(utick_regs,    0, sizeof(utick_regs));
    memset(rtc_regs,      0, sizeof(rtc_regs));
    memset(ostimer_regs,  0, sizeof(ostimer_regs));
    memset(sctimer_regs,  0, sizeof(sctimer_regs));
    memset(dma0_regs,     0, sizeof(dma0_regs));
    memset(dma1_regs,     0, sizeof(dma1_regs));
    memset(gdma_regs,     0, sizeof(gdma_regs));
    memset(pint_regs,     0, sizeof(pint_regs));
    memset(usbh_regs,     0, sizeof(usbh_regs));
    memset(pmu_ns_regs,   0, sizeof(pmu_ns_regs));
    memset(aon_regs,      0, sizeof(aon_regs));
    memset(aon_ciu_regs,  0, sizeof(aon_ciu_regs));

    wwdt_regs[WWDT_OFF_TC     / 4u] = 0xFFu;
    wwdt_regs[WWDT_OFF_TV     / 4u] = 0xFFu;
    wwdt_regs[WWDT_OFF_WINDOW / 4u] = 0xFFFFFFu;

    memset(cache64_regs,  0, sizeof(cache64_regs));
    memset(socctrl_regs,  0, sizeof(socctrl_regs));
    socctrl_regs[SOCCTRL_OFF_CHIP_INFO / 4u] = mm_rw612_chip_rev();

    mm_rw612_flexspi_reset();
    mm_rw612_secure_reset();
    mm_rw612_romapi_reset();
}

static mm_bool reg_pair(struct mmio_bus *bus, struct mmio_region *r,
                        mm_u32 ns_base, mm_u32 s_base)
{
    r->base = ns_base;
    if (!mmio_bus_register_region(bus, r)) return MM_FALSE;
    r->base = s_base;
    if (!mmio_bus_register_region(bus, r)) return MM_FALSE;
    return MM_TRUE;
}

mm_bool mm_rw612_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));

    /* RSTCTL0 / CLKCTL0 */
    reg.size   = CLKRST_BLOCK_SIZE;
    reg.opaque = (void *)(intptr_t)0;
    reg.read   = rstctl_read;
    reg.write  = rstctl_write;
    if (!reg_pair(bus, &reg, 0x40000000u, 0x50000000u)) return MM_FALSE;
    reg.read   = clkctl_read;
    reg.write  = clkctl_write;
    if (!reg_pair(bus, &reg, 0x40001000u, 0x50001000u)) return MM_FALSE;

    /* RSTCTL1 / CLKCTL1 */
    reg.opaque = (void *)(intptr_t)1;
    reg.read   = rstctl_read;
    reg.write  = rstctl_write;
    if (!reg_pair(bus, &reg, 0x40020000u, 0x50020000u)) return MM_FALSE;
    reg.read   = clkctl_read;
    reg.write  = clkctl_write;
    if (!reg_pair(bus, &reg, 0x40021000u, 0x50021000u)) return MM_FALSE;

    /* GPIO + SECGPIO (alias of the same state) */
    reg.size   = RW612_GPIO_SIZE;
    reg.opaque = &gpio0;
    reg.read   = gpio_read;
    reg.write  = gpio_write;
    if (!reg_pair(bus, &reg, RW612_GPIO_BASE, RW612_GPIO_BASE | 0x10000000u)) return MM_FALSE;
    if (!reg_pair(bus, &reg, RW612_SECGPIO_BASE, RW612_SECGPIO_BASE | 0x10000000u)) return MM_FALSE;
    mm_gpio_set_external_input_writer(gpio_set_external_input_cb, 0);
    mm_gpio_set_bank_info_reader(gpio_bank_info, 0);

    /* Long tail */
    if (!stub_reg_pair(bus, &io_mux_stub,  0x40004000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &wwdt_stub,    0x4000E000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &utick_stub,   0x4000F000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &pint_stub,    0x40025000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &rtc_stub,     0x40030000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &dma0_stub,    0x40104000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &dma1_stub,    0x40105000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &usbh_stub,    0x40114000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &pmu_ns_stub,  0x40135000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &ostimer_stub, 0x4013B000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &aon_stub,     0x4013C000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &sctimer_stub, 0x40146000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &ahbsc_stub,   0x40148000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &gdma_stub,    0x4014E000u)) return MM_FALSE;
    if (!stub_reg_pair(bus, &aon_ciu_stub, 0x45000800u)) return MM_FALSE;

    /* XIP cache controllers */
    reg.size   = CACHE64_BLOCK_SIZE;
    reg.read   = cache64_read;
    reg.write  = cache64_write;
    reg.opaque = (void *)(intptr_t)0;
    if (!reg_pair(bus, &reg, 0x40033000u, 0x50033000u)) return MM_FALSE;
    reg.opaque = (void *)(intptr_t)1;
    if (!reg_pair(bus, &reg, 0x40034000u, 0x50034000u)) return MM_FALSE;

    /* SOC_CTRL CIU */
    reg.size   = SOCCTRL_SIZE;
    reg.read   = socctrl_read;
    reg.write  = socctrl_write;
    reg.opaque = 0;
    if (!reg_pair(bus, &reg, SOCCTRL_BASE_NS, SOCCTRL_BASE_NS | 0x10000000u)) return MM_FALSE;

    /* FlexSPI controller, secure subsystem and ROM API */
    if (!mm_rw612_flexspi_register_mmio(bus)) return MM_FALSE;
    if (!mm_rw612_secure_register_mmio(bus)) return MM_FALSE;
    if (!mm_rw612_romapi_register_mmio(bus)) return MM_FALSE;

    return MM_TRUE;
}

void mm_rw612_flash_bind(struct mm_memmap *map,
                         mm_u8 *flash,
                         mm_u32 flash_size,
                         const struct mm_flash_persist *persist,
                         mm_u32 flags)
{
    (void)flags;
    mm_rw612_flexspi_bind(map, flash, flash_size, persist);
}

mm_u64 mm_rw612_cpu_hz(void)
{
    /* RW612 boots from FRO 64 MHz; firmware can switch to SYS_PLL at ~260 MHz. */
    return 64000000ull;
}

/* ------------------------------------------------------------------------ */
/* Boot ROM model                                                           */
/* ------------------------------------------------------------------------ */

/*
 * FlexSPI NOR boot: the FlexSPI configuration block ("FCFB") sits at offset
 * 0x400 and the image starts at offset 0x1000.  The image header lives in the
 * reserved words of the vector table: image length at 0x20, image type at
 * 0x24 and load address at 0x34.  A load address inside the FlexSPI window
 * is an execute-in-place image; one inside SRAM is copied there first.  The
 * model does not authenticate signed image types.
 */
#define FCB_OFFSET          0x400u
#define FCB_TAG             0x42464346u /* "FCFB" */
#define IMG_OFFSET          0x1000u
#define IMG_LENGTH_OFF      0x20u
#define IMG_TYPE_OFF        0x24u
#define IMG_LOAD_ADDR_OFF   0x34u
#define FLEXSPI_WINDOW_SIZE 0x08000000u

static mm_u32 rd32(const mm_u8 *p)
{
    return (mm_u32)p[0] | ((mm_u32)p[1] << 8) | ((mm_u32)p[2] << 16) | ((mm_u32)p[3] << 24);
}

static mm_bool ram_offset(mm_u32 addr, mm_u32 *off_out)
{
    /* Any SRAM alias: code bus 0x0/0x10000000, system bus 0x20000000/0x30000000. */
    mm_u32 base = addr & 0xF0000000u;
    mm_u32 off = addr & 0x0FFFFFFFu;
    if ((base == 0x00000000u || base == 0x10000000u || base == 0x20000000u || base == 0x30000000u) &&
        off < RW612_RAM_SIZE) {
        *off_out = off;
        return MM_TRUE;
    }
    return MM_FALSE;
}

mm_bool mm_rw612_boot_resolve(struct mm_memmap *map,
                              const mm_u8 *flash,
                              mm_u32 flash_size,
                              mm_u32 *vtor_out)
{
    mm_u32 load;
    mm_u32 length;
    mm_u32 type;
    mm_u32 ram_off;
    mm_u32 i;

    if (flash == 0 || vtor_out == 0 || flash_size < IMG_OFFSET + 0x40u) {
        return MM_FALSE;
    }
    if (rd32(flash + FCB_OFFSET) != FCB_TAG) {
        return MM_FALSE; /* raw image: vector table at offset 0 */
    }
    load = rd32(flash + IMG_OFFSET + IMG_LOAD_ADDR_OFF);
    length = rd32(flash + IMG_OFFSET + IMG_LENGTH_OFF);
    type = rd32(flash + IMG_OFFSET + IMG_TYPE_OFF) & 0xFFu;
    if (type != 0u) {
        printf("[RW612] boot: image type 0x%02lx is not authenticated by the model\n",
               (unsigned long)type);
    }
    if (load == 0u || load == 0xFFFFFFFFu ||
        ((load & 0xEFFFFFFFu) >= RW612_FLASH_BASE_NS &&
         (load & 0xEFFFFFFFu) - RW612_FLASH_BASE_NS < FLEXSPI_WINDOW_SIZE)) {
        *vtor_out = (load == 0u || load == 0xFFFFFFFFu) ? (RW612_FLASH_BASE_NS + IMG_OFFSET) : load;
        printf("[RW612] boot: XIP from FlexSPI, vector table 0x%08lx\n", (unsigned long)*vtor_out);
        return MM_TRUE;
    }
    if (!ram_offset(load, &ram_off) || length == 0u || length > flash_size - IMG_OFFSET ||
        ram_off + length > RW612_RAM_SIZE) {
        fprintf(stderr, "[RW612] boot: invalid load-to-RAM image (load=0x%08lx len=0x%lx)\n",
                (unsigned long)load, (unsigned long)length);
        return MM_FALSE;
    }
    for (i = 0; i < length; ++i) {
        (void)mm_memmap_write_ram_raw(map, RW612_RAM_BASE_S + ram_off + i, 1u, flash[IMG_OFFSET + i]);
    }
    *vtor_out = load;
    printf("[RW612] boot: loaded 0x%lx bytes to RAM, vector table 0x%08lx\n",
           (unsigned long)length, (unsigned long)*vtor_out);
    return MM_TRUE;
}
